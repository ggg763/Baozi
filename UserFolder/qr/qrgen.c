/******************************************************************************
 * @file     qrgen.c
 * @brief    二维码编码器实现（见 qrgen.h）
 *
 * 结构照抄 ISO/IEC 18004 的管线顺序，每一步都能单独测：
 *
 *   文本 ──① 比特流（0100 + 长度 + 数据 + 终止符 + 补齐）──► 数据码字
 *        ──② Reed-Solomon 分块纠错 ──────────────────────► 码字块
 *        ──③ 交错（interleave）──────────────────────────► 最终码字序列
 *        ──④ 落模块：功能图形 → 码字（之字形）→ 掩模 ───► 矩阵
 *        ──⑤ 8 种掩模罚分选优 ───────────────────────────► 最终矩阵
 *
 * 与表情层一样，本文件**只做纯计算**：不 printf、不碰外设、不用浮点，
 * 于是在 _probe 里用 mingw gcc 编一下就能和 segno 逐位比对。
 ******************************************************************************/

#include <stdint.h>
#include <string.h>

#include "qrgen.h"

/* ============================ 容量表 ============================
 * 两张表都是 "版本 → 参数"，索引 0 是占位（用 -1 标记非法）。
 * 只抄到第 10 版 —— 正好够，也省 flash。                        */

static const int8_t k_ecc_per_block[4][QRGEN_MAX_VERSION + 1] = {
    /* 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10 */
    { -1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18 },  /* L */
    { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 },  /* M */
    { -1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24 },  /* Q */
    { -1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28 },  /* H */
};

static const int8_t k_num_blocks[4][QRGEN_MAX_VERSION + 1] = {
    /* 0, 1, 2, 3, 4, 5, 6, 7, 8,  9, 10 */
    { -1, 1, 1, 1, 1, 1, 2, 2, 2,  2,  4 },  /* L */
    { -1, 1, 1, 1, 2, 2, 4, 4, 4,  5,  5 },  /* M */
    { -1, 1, 1, 2, 2, 4, 4, 6, 6,  8,  8 },  /* Q */
    { -1, 1, 1, 2, 4, 4, 4, 5, 6,  8,  8 },  /* H */
};

/* 格式信息里的 ECC 编码值（L=01, M=00, Q=11, H=10）*/
static const uint8_t k_ecc_format_bits[4] = { 1, 0, 3, 2 };

/* ============================ 内部状态 ============================
 * 全部 static：板上没有 malloc 的习惯（也不该有）。最坏 57×57 = 3249 B，
 * 相对 192 KB SRAM 的余量（当前 bss 48.6 KB）可以接受。 */
#define QRGEN_MAX_BLOCKS 8
#define QRGEN_MAX_ECC    30
/* v10-L 最多 274 个数据码字；留到 320 是为了有余量，不至于因为换了版本上限
 * 就静默写越界。 */
#define QRGEN_MAX_DATA_CW 320

static uint8_t s_modules[QRGEN_MAX_SIZE][QRGEN_MAX_SIZE];
static uint8_t s_is_func[QRGEN_MAX_SIZE][QRGEN_MAX_SIZE];
static uint8_t s_data[400];                          /* 交错后的全部码字 */
static uint8_t s_block_ecc[QRGEN_MAX_BLOCKS][QRGEN_MAX_ECC];
static uint8_t s_block_dat[QRGEN_MAX_BLOCKS][QRGEN_MAX_DATA_CW];  /* 分块数据 */

/* 候选掩模的罚分比较要用同一份"干净矩阵"，所以每次重画都从头 memset，
 * 不做增量还原 —— 一帧一次，代价可以忽略，逻辑不会错。 */

static int s_size;

/* ============================ 基础工具 ============================ */

static int bit_of(unsigned x, int i)
{
    return (int)((x >> i) & 1u);
}

/* 第 ver 版除去功能图形后，可用于数据+纠错的模块总数（单位：模块 = 比特） */
static int raw_data_modules(int ver)
{
    int result = (16 * ver + 128) * ver + 64;

    if (ver >= 2) {
        int num_align = ver / 7 + 2;
        result -= (25 * num_align - 10) * num_align - 55;
        if (ver >= 7) {
            result -= 36;               /* 版本信息两块 */
        }
    }
    return result;
}

/* 该版本总共多少个码字（含纠错） */
static int total_codewords(int ver)
{
    return raw_data_modules(ver) / 8;
}

/* 该版本 ECC 档下能装多少**数据**码字 */
static int data_codewords(int ver, qrgen_ecc_t ecc)
{
    if (ver < 1 || ver > QRGEN_MAX_VERSION) {
        return 0;
    }
    return total_codewords(ver)
           - (int)k_ecc_per_block[ecc][ver] * (int)k_num_blocks[ecc][ver];
}

int qrgen_capacity(int version, qrgen_ecc_t ecc)
{
    int dcw;
    int header;

    if (ecc < 0 || ecc > 3) {
        return 0;
    }
    dcw = data_codewords(version, ecc);
    if (dcw <= 0) {
        return 0;
    }
    /* 模式(4) + 长度(8 或 16) 之后剩下的才是正文 */
    header = 4 + ((version <= 9) ? 8 : 16);
    return (dcw * 8 - header) / 8;
}

int qrgen_min_version(int byte_len, qrgen_ecc_t ecc)
{
    int v;

    if (byte_len <= 0) {
        return 0;
    }
    for (v = 1; v <= QRGEN_MAX_VERSION; v++) {
        if (byte_len <= qrgen_capacity(v, ecc)) {
            return v;
        }
    }
    return 0;
}

/* ============================ GF(256) 运算 ============================
 * 本原多项式 0x11D，生成元 2。只做乘法（加法就是异或）。 */
static uint8_t gf_mul(uint8_t x, uint8_t y)
{
    int z = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

/* 生成多项式 (x-1)(x-2)...(x-2^(deg-1))，系数降幂存放，最高项恒为 1 省略 */
static void rs_divisor(int degree, uint8_t *result)
{
    int i, j;
    uint8_t root = 1;

    for (i = 0; i < degree; i++) {
        result[i] = 0;
    }
    result[degree - 1] = 1;

    for (i = 0; i < degree; i++) {
        for (j = 0; j < degree; j++) {
            result[j] = gf_mul(result[j], root);
            if (j + 1 < degree) {
                result[j] ^= result[j + 1];
            }
        }
        root = gf_mul(root, 0x02);
    }
}

/* 多项式取余：rem = data * x^degree mod divisor */
static void rs_remainder(const uint8_t *data, int len,
                         const uint8_t *divisor, int degree, uint8_t *result)
{
    int i, j;

    for (i = 0; i < degree; i++) {
        result[i] = 0;
    }
    for (i = 0; i < len; i++) {
        uint8_t factor = (uint8_t)(data[i] ^ result[0]);
        for (j = 0; j < degree - 1; j++) {
            result[j] = result[j + 1];
        }
        result[degree - 1] = 0;
        for (j = 0; j < degree; j++) {
            result[j] ^= gf_mul(divisor[j], factor);
        }
    }
}

/* ============================ 模块落点 ============================ */

static void set_func(int x, int y, int dark)
{
    if (x < 0 || y < 0 || x >= s_size || y >= s_size) {
        return;
    }
    s_modules[y][x] = (uint8_t)(dark ? 1 : 0);
    s_is_func[y][x] = 1;
}

/* 定位图形：7×7 同心方框 + 外用一圈白（dist=4 是分隔符）*/
static void draw_finder(int cx, int cy)
{
    int dy, dx;

    for (dy = -4; dy <= 4; dy++) {
        for (dx = -4; dx <= 4; dx++) {
            int dist = (dx < 0 ? -dx : dx);
            int t;
            if ((dy < 0 ? -dy : dy) > dist) {
                dist = (dy < 0 ? -dy : dy);
            }
            t = dist;
            set_func(cx + dx, cy + dy, (t != 2) && (t != 4));
        }
    }
}

/* 校正图形：5×5，最外圈黑、次圈白、中心黑 */
static void draw_align(int cx, int cy)
{
    int dy, dx;

    for (dy = -2; dy <= 2; dy++) {
        for (dx = -2; dx <= 2; dx++) {
            int ax = (dx < 0 ? -dx : dx);
            int ay = (dy < 0 ? -dy : dy);
            int dist = (ax > ay) ? ax : ay;
            set_func(cx + dx, cy + dy, dist != 1);
        }
    }
}

/* 校正图形中心坐标（ISO 18004 Annex E 的整数算法，不用查表）*/
static int align_positions(int ver, int *pos)
{
    int num, step, i, size;

    if (ver == 1) {
        return 0;
    }
    num = ver / 7 + 2;
    size = 17 + 4 * ver;
    step = (ver == 32) ? 26 : (ver * 4 + num * 2 + 1) / (num * 2 - 2) * 2;

    pos[0] = 6;
    for (i = num - 1; i >= 1; i--) {
        pos[i] = size - 7 - (num - 1 - i) * step;
    }
    return num;
}

static void draw_format(int ecc, int mask)
{
    int data = ((int)k_ecc_format_bits[ecc] << 3) | mask;
    int rem  = data;
    int bits;

    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    bits = ((data << 10) | rem) ^ 0x5412;

    /* 第一份：左上角 */
    for (int i = 0; i <= 5; i++) {
        set_func(8, i, bit_of((unsigned)bits, i));
    }
    set_func(8, 7, bit_of((unsigned)bits, 6));
    set_func(8, 8, bit_of((unsigned)bits, 7));
    set_func(7, 8, bit_of((unsigned)bits, 8));
    for (int i = 9; i < 15; i++) {
        set_func(14 - i, 8, bit_of((unsigned)bits, i));
    }

    /* 第二份：右上 + 左下 */
    for (int i = 0; i < 8; i++) {
        set_func(s_size - 1 - i, 8, bit_of((unsigned)bits, i));
    }
    for (int i = 8; i < 15; i++) {
        set_func(8, s_size - 15 + i, bit_of((unsigned)bits, i));
    }
    set_func(8, s_size - 8, 1);           /* 恒黑模块 */
}

static void draw_version(int ver)
{
    int rem, bits;

    if (ver < 7) {
        return;
    }
    rem = ver;
    for (int i = 0; i < 12; i++) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    }
    bits = (ver << 12) | rem;

    for (int i = 0; i < 18; i++) {
        int b = bit_of((unsigned)bits, i);
        int a = s_size - 11 + i % 3;
        int c = i / 3;
        set_func(a, c, b);
        set_func(c, a, b);
    }
}

static void draw_function_patterns(int ver, int ecc, int mask)
{
    int i, j, num, pos[8];

    /* 定时图形：第 6 行/列，黑白交替（定位图形稍后会覆盖两端）*/
    for (i = 0; i < s_size; i++) {
        set_func(6, i, (i % 2) == 0);
        set_func(i, 6, (i % 2) == 0);
    }

    draw_finder(3, 3);
    draw_finder(s_size - 4, 3);
    draw_finder(3, s_size - 4);

    num = align_positions(ver, pos);
    for (i = 0; i < num; i++) {
        for (j = 0; j < num; j++) {
            /* 三个角被定位图形占了，跳过 */
            if ((i == 0 && j == 0) || (i == 0 && j == num - 1) ||
                (i == num - 1 && j == 0)) {
                continue;
            }
            draw_align(pos[i], pos[j]);
        }
    }

    draw_format(ecc, mask);
    draw_version(ver);
}

/* 掩模条件（true = 该模块取反）*/
static int mask_invert(int mask, int x, int y)
{
    switch (mask) {
    case 0:  return ((x + y) % 2) == 0;
    case 1:  return (y % 2) == 0;
    case 2:  return (x % 3) == 0;
    case 3:  return ((x + y) % 3) == 0;
    case 4:  return ((x / 3 + y / 2) % 2) == 0;
    case 5:  return ((x * y) % 2 + (x * y) % 3) == 0;
    case 6:  return (((x * y) % 2 + (x * y) % 3) % 2) == 0;
    case 7:  return (((x + y) % 2 + (x * y) % 3) % 2) == 0;
    default: return 0;
    }
}

/* 把码字按之字形落进矩阵（含掩模）*/
static void draw_codewords(const uint8_t *cw, int len, int mask)
{
    int i = 0;
    int right, vert, j;

    for (right = s_size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;                       /* 第 6 列是定时图形，跳过 */
        }
        for (vert = 0; vert < s_size; vert++) {
            for (j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? (s_size - 1 - vert) : vert;

                if (s_is_func[y][x]) {
                    continue;
                }
                if (i < len * 8) {
                    int dark = (cw[i >> 3] >> (7 - (i & 7))) & 1;
                    if (mask_invert(mask, x, y)) {
                        dark ^= 1;
                    }
                    s_modules[y][x] = (uint8_t)dark;
                    i++;
                } else {
                    /* 剩余位（"remainder bits"）按 0 处理，掩模照打 */
                    s_modules[y][x] = (uint8_t)(mask_invert(mask, x, y) ? 1 : 0);
                }
            }
        }
    }
}

/* ============================ 掩模罚分 ============================ */

static int penalty_score(void)
{
    int score = 0;
    int i, j;

    /* 规则 1：行/列里连续 >=5 个同色，每多一个 +1（起始 +3）*/
    for (i = 0; i < s_size; i++) {
        for (j = 0; j < s_size; j++) {
            int run;
            /* 水平 */
            run = 1;
            for (int k = j + 1; k < s_size && s_modules[i][k] == s_modules[i][j]; k++) {
                run++;
            }
            if (run >= 5) {
                score += 3 + (run - 5);
            }
            j += run - 1;
        }
        for (j = 0; j < s_size; j++) {
            int run = 1;
            for (int k = j + 1; k < s_size && s_modules[k][i] == s_modules[j][i]; k++) {
                run++;
            }
            if (run >= 5) {
                score += 3 + (run - 5);
            }
            j += run - 1;
        }
    }

    /* 规则 2：2×2 同色块，每个 +3 */
    for (i = 0; i < s_size - 1; i++) {
        for (j = 0; j < s_size - 1; j++) {
            uint8_t c = s_modules[i][j];
            if (c == s_modules[i][j + 1] && c == s_modules[i + 1][j] &&
                c == s_modules[i + 1][j + 1]) {
                score += 3;
            }
        }
    }

    /* 规则 3：1:1:3:1:1 且一侧留白 4 模块的"伪定位图形"，每处 +40 */
    {
        static const uint8_t pat_a[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
        static const uint8_t pat_b[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };

        for (i = 0; i < s_size; i++) {
            for (j = 0; j + 11 <= s_size; j++) {
                int match_a = 1, match_b = 1;
                int match_c = 1, match_d = 1;
                for (int k = 0; k < 11; k++) {
                    if (s_modules[i][j + k] != pat_a[k]) { match_a = 0; }
                    if (s_modules[i][j + k] != pat_b[k]) { match_b = 0; }
                    if (s_modules[j + k][i] != pat_a[k]) { match_c = 0; }
                    if (s_modules[j + k][i] != pat_b[k]) { match_d = 0; }
                }
                if (match_a) { score += 40; }
                if (match_b) { score += 40; }
                if (match_c) { score += 40; }
                if (match_d) { score += 40; }
            }
        }
    }

    /* 规则 4：黑模块占比偏离 50%，每 5% +10 */
    {
        int dark = 0, total = s_size * s_size;
        for (i = 0; i < s_size; i++) {
            for (j = 0; j < s_size; j++) {
                dark += s_modules[i][j];
            }
        }
        {
            int pct = (dark * 100) / total;
            int dev = pct - 50;
            if (dev < 0) {
                dev = -dev;
            }
            score += (dev / 5) * 10;
        }
    }
    return score;
}

/* ============================ 主流程 ============================ */

static uint8_t s_tmp_data[QRGEN_MAX_DATA_CW];

int qrgen_encode(const char *text, qrgen_ecc_t ecc, qrgen_result_t *out)
{
    int len, ver, i, j;
    int dcw, total_cw, nblocks, ecc_len, short_blocks, short_len;
    int bitpos, best_mask, best_score;

    if (text == 0 || out == 0) {
        return -3;
    }
    len = (int)strlen(text);
    if (len <= 0) {
        return -1;
    }
    if (ecc < 0 || ecc > 3) {
        return -3;
    }

    /* ---- 选版本 ---- */
    ver = qrgen_min_version(len, ecc);
    if (ver == 0) {
        return -2;
    }
    s_size = 17 + 4 * ver;
    dcw     = data_codewords(ver, ecc);
    total_cw = total_codewords(ver);

    /* ---- ① 比特流 → 数据码字 ---- */
    memset(s_tmp_data, 0, sizeof(s_tmp_data));
    bitpos = 0;
    {
        /* 模式指示 0100（Byte）*/
        for (i = 3; i >= 0; i--) {
            int bit = (0x4 >> i) & 1;
            if (bit) {
                s_tmp_data[bitpos >> 3] |= (uint8_t)(0x80 >> (bitpos & 7));
            }
            bitpos++;
        }
        /* 字符计数 */
        {
            int ccbits = (ver <= 9) ? 8 : 16;
            for (i = ccbits - 1; i >= 0; i--) {
                if ((len >> i) & 1) {
                    s_tmp_data[bitpos >> 3] |= (uint8_t)(0x80 >> (bitpos & 7));
                }
                bitpos++;
            }
        }
        /* 数据 */
        for (i = 0; i < len; i++) {
            for (j = 7; j >= 0; j--) {
                if (((uint8_t)text[i] >> j) & 1) {
                    s_tmp_data[bitpos >> 3] |= (uint8_t)(0x80 >> (bitpos & 7));
                }
                bitpos++;
            }
        }
    }
    /* 终止符：最多 4 个 0，然后把最后一个字节剩下的位补 0（memset 已经是 0）*/
    {
        int cap_bits = dcw * 8;
        if (bitpos + 4 > cap_bits) {
            bitpos = cap_bits;
        } else {
            bitpos += 4;
        }
    }
    /* 补齐字节 + 交替 0xEC / 0x11 */
    {
        int used = (bitpos + 7) / 8;
        uint8_t pad = 0xEC;
        for (i = used; i < dcw; i++) {
            s_tmp_data[i] = pad;
            pad = (uint8_t)(pad ^ 0xFD);     /* 0xEC <-> 0x11 */
        }
    }

    /* ---- ② 分块纠错 ---- */
    nblocks     = (int)k_num_blocks[ecc][ver];
    ecc_len     = (int)k_ecc_per_block[ecc][ver];
    short_blocks = nblocks - total_cw % nblocks;
    short_len    = total_cw / nblocks;

    {
        int off = 0;
        uint8_t divisor[QRGEN_MAX_ECC];

        rs_divisor(ecc_len, divisor);
        for (i = 0; i < nblocks; i++) {
            int dat_len = short_len - ecc_len + ((i < short_blocks) ? 0 : 1);
            for (j = 0; j < dat_len; j++) {
                s_block_dat[i][j] = s_tmp_data[off + j];
            }
            off += dat_len;
            rs_remainder(s_block_dat[i], dat_len, divisor, ecc_len, s_block_ecc[i]);
        }
    }

    /* ---- ③ 交错 ---- */
    {
        int k = 0;
        int max_dat = short_len - ecc_len + 1;   /* 长块的数据长度 */

        for (i = 0; i < max_dat; i++) {
            for (j = 0; j < nblocks; j++) {
                int dat_len = short_len - ecc_len + ((j < short_blocks) ? 0 : 1);
                if (i < dat_len) {
                    s_data[k++] = s_block_dat[j][i];
                }
            }
        }
        for (i = 0; i < ecc_len; i++) {
            for (j = 0; j < nblocks; j++) {
                s_data[k++] = s_block_ecc[j][i];
            }
        }
    }

    /* ---- ④⑤ 落模块 + 掩模选优 ---- */
    best_mask  = 0;
    best_score = 0x7FFFFFFF;
    for (i = 0; i < 8; i++) {
        int score;

        memset(s_is_func, 0, sizeof(s_is_func));
        memset(s_modules, 0, sizeof(s_modules));
        draw_function_patterns(ver, ecc, i);
        draw_codewords(s_data, total_cw, i);
        score = penalty_score();
        if (score < best_score) {
            best_score = score;
            best_mask  = i;
        }
    }
    /* 用最优掩模再画一遍（上面的循环结束时矩阵停在 mask=7 的状态）*/
    memset(s_is_func, 0, sizeof(s_is_func));
    memset(s_modules, 0, sizeof(s_modules));
    draw_function_patterns(ver, ecc, best_mask);
    draw_codewords(s_data, total_cw, best_mask);

    out->modules = &s_modules[0][0];
    out->stride  = QRGEN_MAX_SIZE;
    out->size    = s_size;
    out->version = ver;
    out->mask    = best_mask;
    out->ecc     = (int)ecc;
    return 0;
}
