// web_console.h — 手机浏览器网页控制台（局域网直连，无需服务器/无需 PC）
// ============================================================================
// 用途：比赛现场演示"用户在手机上操作 → 机器人变表情"。
//
// 工作原理（为什么这样就能连）：
//   ESP32 自己跑 HTTP 服务（config.h 的 HTTP_PORT = 80）。
//   手机和板子连同一个 WiFi（比如你的手机热点），浏览器打开
//       http://<板子IP>/
//   就拿到下面这张网页，点按钮 → fetch POST /api/emotion → ESP32 用
//   UART2(GPIO14) 把 0xAA + code 发给 TD1601 → 板子变脸。
//   **不经过 PC、不经过云服务器、不需要备案域名**。
//
// 编码规则（与 protocol.h / TD1601 uart_esp.c 三端一致）：
//       code = (emo_id * 8 + intent_id) * 3 + intensity_id      0..215
//   emo_id:      neutral0 happy1 sad2 angry3 surprised4 loving5 idle6 bored7 sleeping8
//   intent_id:   none0 greeting1 question2 chat3 command4 thanks5 goodbye6 unknown7
//   intensity:   low0 medium1 high2
//   ⚠️ 网页里只让用户选 emo + intensity，intent 固定为 "chat"(3)。
//      因为 intent 只影响 TD1601 上的舵机动作/心情层，与本工程的"变脸"无关。
//
// 实现约束（踩过的坑）：
//   · `WebServer::on()` 的回调类型是 **std::function<void()>——无参**。
//     绝对不能写成 `void h(WebServer&)`，编译会报 no matching function。
//     所以这里用全局 srv 指针 + 无参包装函数。
//   · 全用 String 拼字符串，避免 PROGMEM/宏拼接的坑。
//   · 网页内不引用任何外部 CDN（比赛现场可能没外网，引 CDN 会白屏）。
//   · 页面塞进一个 R"(...)" 原始字符串，ES5 语法（老手机浏览器也能跑）。
// ============================================================================
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "log.h"      /* 统一日志通道：WiFi 模式下等价于 Serial，USB 模式下走 PKT_LOG 帧 */

// ---- 表：emo 名字 → emo_id（顺序必须与 PC 侧 td1601.py 的 EMO_ID 完全一致）----
struct WebEmoEntry { const char *name; uint8_t id; };
static const WebEmoEntry k_web_emo_table[] = {
  { "neutral",   0 },
  { "happy",     1 },
  { "sad",       2 },
  { "angry",     3 },
  { "surprised", 4 },
  { "loving",    5 },
  { "idle",      6 },
  { "bored",     7 },
  { "sleeping",  8 },
};

// ---- 表：intensity 名字 → intensity_id ----
static const WebEmoEntry k_web_int_table[] = {
  { "low",    0 },
  { "medium", 1 },
  { "high",   2 },
};

#define WEB_N_EMO     ((uint8_t)(sizeof(k_web_emo_table) / sizeof(k_web_emo_table[0])))
#define WEB_N_INT     ((uint8_t)(sizeof(k_web_int_table) / sizeof(k_web_int_table[0])))
#define WEB_N_INTENT  (8)   /* 意图维数，固定 */

static int8_t web_lookup(const WebEmoEntry *tbl, uint8_t n, const String &key) {
  String k = key;
  k.trim();
  k.toLowerCase();
  for (uint8_t i = 0; i < n; i++) {
    if (k == tbl[i].name) return (int8_t)tbl[i].id;
  }
  return -1;
}

// ============================================================================
//  页面本体
// ============================================================================
static const char k_web_page[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>包子控制台</title><style>"
"body{margin:0;padding:18px;font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif;"
"background:#f5f4f0;color:#2c2c2a}"
"h1{font-size:19px;font-weight:600;margin:0 0 4px}"
".sub{font-size:13px;color:#6b6a66;margin-bottom:18px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
"button{border:1px solid #d8d6cf;background:#fff;border-radius:12px;padding:16px 8px;"
"font-size:16px;color:#2c2c2a;font-family:inherit;-webkit-tap-highlight-color:transparent}"
"button:active{background:#e8e5dc}"
".row{margin:18px 0 8px;font-size:13px;color:#6b6a66}"
".seg{display:flex;gap:8px}"
".seg button{flex:1;padding:12px 4px;font-size:14px}"
".seg button.on{background:#3b6d11;color:#fff;border-color:#3b6d11}"
"#log{margin-top:18px;padding:12px;background:#fff;border:1px solid #e2e0d9;border-radius:10px;"
"font-size:12px;line-height:1.7;min-height:56px;white-space:pre-wrap;word-break:break-all}"
".ok{color:#3b6d11}.err{color:#a32d2d}"
"</style></head><body>"
"<h1>包子控制台</h1>"
"<div class='sub'>点一下，机器人就变脸</div>"
"<div class='grid'>"
"<button onclick=\"send('neutral')\">平静</button>"
"<button onclick=\"send('happy')\">开心</button>"
"<button onclick=\"send('sad')\">难过</button>"
"<button onclick=\"send('angry')\">生气</button>"
"<button onclick=\"send('surprised')\">惊讶</button>"
"<button onclick=\"send('loving')\">喜欢</button>"
"<button onclick=\"send('idle')\">待机</button>"
"<button onclick=\"send('bored')\">无聊</button>"
"</div>"
"<div class='row'>语气强度</div>"
"<div class='seg'>"
"<button id='i0' onclick=\"lv(0)\">轻</button>"
"<button id='i1' class='on' onclick=\"lv(1)\">中</button>"
"<button id='i2' onclick=\"lv(2)\">强</button>"
"</div>"
"<div id='log'>就绪。手机和板子要在同一个 WiFi 下。</div>"
"<script>"
"var INTEN=1;"
/* 服务端期望的是名字 low/medium/high，不是数字 0/1/2。
 * 早先直接拼 INTEN(数字) 导致服务端查表失败、返回 400 unknown intensity。
 * 这里做一层映射，两边各自保持自己的表示法。 */
"var INTEN_NAME=['low','medium','high'];"
"function lv(n){INTEN=n;for(var i=0;i<3;i++){document.getElementById('i'+i).className=(i==n?'on':'');}}"
"function send(e){"
"  var l=document.getElementById('log');"
"  var x=new XMLHttpRequest();"
"  x.open('POST','/api/emotion',true);"
"  x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
"  x.onreadystatechange=function(){"
"    if(x.readyState!=4)return;"
"    if(x.status==200){l.className='ok';l.textContent='已下发  '+e+'  / 强度 '+INTEN+'\\n'+x.responseText;}"
"    else{l.className='err';l.textContent='失败  HTTP '+x.status+'\\n'+x.responseText;}"
"  };"
"  l.className='';l.textContent='发送中…';"
"  x.send('emo='+encodeURIComponent(e)+'&intensity='+INTEN_NAME[INTEN]);"
"}"
"</script></body></html>";

// ============================================================================
//  全局句柄（Arduino 单实例，够用；避免把 WebServer& 塞进回调）
// ============================================================================
static WebServer *s_web = NULL;

// GET /  →  返回网页
static void webOnRoot() {
  if (!s_web) return;
  s_web->sendHeader("Cache-Control", "no-store");
  s_web->send_P(200, "text/html; charset=utf-8", k_web_page);
}

// POST /api/emotion  参数 emo=<name>&intensity=<low|medium|high>
static void webOnEmotion() {
  if (!s_web) return;

  if (!s_web->hasArg("emo")) {
    s_web->send(400, "text/plain; charset=utf-8", "missing 'emo'");
    return;
  }
  String emoName = s_web->arg("emo");
  String intName = s_web->hasArg("intensity") ? s_web->arg("intensity") : String("medium");

  int8_t emoId = web_lookup(k_web_emo_table, WEB_N_EMO, emoName);
  if (emoId < 0) {
    s_web->send(400, "text/plain; charset=utf-8", "unknown emo: " + emoName);
    return;
  }
  int8_t intId = web_lookup(k_web_int_table, WEB_N_INT, intName);
  if (intId < 0) {
    s_web->send(400, "text/plain; charset=utf-8", "unknown intensity: " + intName);
    return;
  }

  /* intent 固定 chat(3)：只影响 TD1601 的舵机动作层，不影响变脸。
   * 要改就改这里（0..7 见 protocol.h 注释）。 */
  const uint8_t intentId = 3;

  uint16_t code = (uint16_t)(((uint16_t)emoId * WEB_N_INTENT + intentId)
                             * WEB_N_INT + (uint16_t)intId);

  /* ---- 发给 TD1601：0xAA + code（两字节，无换行，与 ino 的 CTRL_STATE 同格式）---- */
  Serial2.write(0xAA);
  Serial2.write((uint8_t)code);
  Serial2.flush();

  logPrintf("[web] %s/%s -> code=%u (emo=%u intent=%u inten=%u)\n",
            emoName.c_str(), intName.c_str(),
            (unsigned)code, (unsigned)emoId,
            (unsigned)intentId, (unsigned)intId);

  char msg[96];
  snprintf(msg, sizeof(msg), "code=%u (emo=%u intent=%u inten=%u)",
           (unsigned)code, (unsigned)emoId, (unsigned)intentId, (unsigned)intId);
  s_web->send(200, "text/plain; charset=utf-8", msg);
}

// ============================================================================
//  注册（在 server.begin() 之前调一次）
// ============================================================================
static void webConsoleBegin(WebServer &srv) {
  s_web = &srv;
  srv.on("/", webOnRoot);
  srv.on("/api/emotion", webOnEmotion);
  logPrintf("[web] 手机控制台就绪：http://<板子IP>/  (emo=%u inten=%u)\n",
            (unsigned)WEB_N_EMO, (unsigned)WEB_N_INT);
}
