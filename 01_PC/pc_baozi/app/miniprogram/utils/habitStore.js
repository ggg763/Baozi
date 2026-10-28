/**
 * 习惯打卡数据层 —— 真后端版
 *
 * 【这一版改了什么】
 * 上一版是「本地实现」：因为后端（habit_log 表 / 日粒度结算 / 超时结算）还没做，
 * 为了 UI 不是空壳，用 wx.storage 存了一份本地日志、在前端自己算状态。
 * 现在后端已经上线，本文件改成**纯接口客户端**：所有判定都在后端算，
 * 前端只负责把返回的 card 渲染出来。本地的 seed()/resetDemoData() 已删除。
 *
 * 【为什么判定不能留一份在前端】
 * 包子屏幕（TD1601）和手机读的是同一份 habit_log。如果前端自己再算一遍，
 * 迟早出现「手机显示已完成、屏幕显示未完成」。所以判定只有唯一来源：
 * 后端 baozi-backend/habit_logic.py（与包子设备端共用同一套）。
 *
 * 【接口对照】
 *   GET   /api/v1/habits                 -> loadHabits()
 *   GET   /api/v1/habits/today?date=     -> loadCards(date)   （date 省略=今天）
 *   POST  /api/v1/habits/{key}/checkin   -> checkin(key, { date, slot, source })
 *   POST  /api/v1/habits/{key}/uncheck   -> uncheck(key, { date, slot })
 *   PATCH /api/v1/habits/{id}            -> setMinRatio(id, ratio)
 *
 * 【三条铁律（实现在后端，这里只作说明，改后端时别破坏）】
 *   1. 提醒 ≠ 打卡 —— 天数只由「确认完成」产生，播报只写 pending，不加天数
 *   2. 跨天记录记在「播报那天」—— 23:30 的睡觉提醒记当天，次日确认把 date 传回来
 *   3. 今天没确认 ≠ 断签 —— 显示「已坚持 N 天 · 今天待打卡」
 */

const api = require('./request.js')

// 达标线可选项（默认 60%）
const RATIO_VALUES = [0.5, 0.6, 0.7, 0.8, 1]
const RATIO_LABELS = ['50%', '60%', '70%', '80%', '100%']

// 兜底：万一遇到没带 key 的老数据，用名字映射（新库不会走到这里，
// 因为后端 habits.habit_key 已经是稳定标识）
const NAME_KEY = { '作息': 'sleep', '喝水': 'water', '锻炼': 'exercise' }

// ---------------- 时间小工具（同步，页面选日期/算默认值要用） ----------------
function pad2(n) { return n < 10 ? '0' + n : '' + n }
function fmtDate(d) { return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) }
function todayStr(now) { return fmtDate(now || new Date()) }
function shiftDate(s, delta) {
  const p = String(s).split('-')
  const d = new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2]))
  d.setDate(d.getDate() + delta)
  return fmtDate(d)
}
function toMin(hm) {
  const p = String(hm == null ? '0:0' : hm).split(':')
  return Number(p[0]) * 60 + Number(p[1] || 0)
}
function toHM(m) { return pad2(Math.floor(m / 60)) + ':' + pad2(m % 60) }
function nowMin(now) { return now.getHours() * 60 + now.getMinutes() }

// ---------------- 习惯规格补齐（仅用于展示/默认值，不参与判定） ----------------
// 后端 /api/v1/habits 已经返回 confirmMode / windowMin / deadline / minRatio，
// 这里只是兜底，保证 picker 索引、新建草稿这些场景拿到的对象是完整的。
function normalizeHabit(h) {
  const o = Object.assign({}, h)
  if (!o.key) {
    o.key = NAME_KEY[o.name] || ('h' + (o.id === null || o.id === undefined ? Date.now().toString(36) : o.id))
  }
  if (!o.confirmMode) o.confirmMode = (o.type === 'sleep') ? 'after_ward' : 'on_time'
  if (o.confirmMode === 'on_time' && !o.windowMin) o.windowMin = 30
  if (o.confirmMode === 'after_ward' && !o.deadline) o.deadline = '12:00'
  if (o.minRatio === undefined || o.minRatio === null) o.minRatio = 0.6
  return o
}

// 达标线（读后端给的值；新建草稿还没同步时用默认 60%）
function minRatioOf(h) {
  return (h && h.minRatio !== undefined && h.minRatio !== null) ? h.minRatio : 0.6
}

// ---------------- 读 ----------------
// 习惯定义列表。返回的 days/streak 都是后端由 habit_log 现算出来的真天数。
async function loadHabits() {
  const data = await api.request('/api/v1/habits')
  return (data && data.habits) || []
}

/**
 * 某一天所有习惯的打卡卡片。
 * 返回 { date, cards: { key: card }, list: [...], pending: [...] }
 *   - cards 用 key 索引，方便按习惯取自己那张
 *   - pending 是待确认清单（如"昨晚的睡觉还没确认"），由后端算好
 * 省略 date 就是今天；传过去某天就是补卡视图。
 */
async function loadCards(date) {
  const query = date ? ('?date=' + encodeURIComponent(date)) : ''
  const data = await api.request('/api/v1/habits/today' + query)
  const map = {}
  const list = (data && data.cards) || []
  list.forEach(it => { if (it && it.key) map[it.key] = it.card })
  return {
    date: (data && data.date) || date || todayStr(),
    cards: map,
    list: list,
    pending: (data && data.pending) || []
  }
}

// ---------------- 写 ----------------
// 打卡。带 date 就是补卡/次日确认，slot 用于一天多次的习惯（如 09:00 / 10:00）。
// 后端会返回重算后的 { key, card, streak }。
function checkin(key, opts) {
  const o = opts || {}
  return api.request('/api/v1/habits/' + key + '/checkin', 'POST', {
    date: o.date || todayStr(),
    slot: o.slot || 'main',
    source: o.source || 'app'
  })
}

// 撤销打卡（后端是直接删记录，不留 undone）
function uncheck(key, opts) {
  const o = opts || {}
  return api.request('/api/v1/habits/' + key + '/uncheck', 'POST', {
    date: o.date || todayStr(),
    slot: o.slot || 'main'
  })
}

// 改达标线（传习惯的数据库 id，不是 habit_key；未同步的临时 id 直接跳过）
function setMinRatio(id, ratio) {
  if (id === null || id === undefined) return Promise.resolve(null)
  if (typeof id === 'string' && id.indexOf('tmp_') === 0) return Promise.resolve(null)
  return api.request('/api/v1/habits/' + id, 'PATCH', { minRatio: ratio })
}

// 逐日记录（日历格子用）：每天是 met / missed / pending / future
async function loadLog(key, days) {
  const data = await api.request('/api/v1/habits/' + key + '/log?days=' + (days || 30))
  return (data && data.days) || []
}

module.exports = {
  RATIO_VALUES: RATIO_VALUES,
  RATIO_LABELS: RATIO_LABELS,
  // 同步工具
  todayStr: todayStr,
  shiftDate: shiftDate,
  toMin: toMin,
  toHM: toHM,
  nowMin: nowMin,
  normalizeHabit: normalizeHabit,
  minRatioOf: minRatioOf,
  // 异步接口
  loadHabits: loadHabits,
  loadCards: loadCards,
  checkin: checkin,
  uncheck: uncheck,
  setMinRatio: setMinRatio,
  loadLog: loadLog
}
