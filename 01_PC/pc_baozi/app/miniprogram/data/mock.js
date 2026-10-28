// 假数据（Mock）。后端做好后，把这里换成接口请求即可。

// 多天日程（date = 年月日 YYYY-MM-DD）
const allEvents = [
  { id: 1, date: '2026-09-26', time: '10:00', title: '开组会', type: 'meeting', done: false },
  { id: 2, date: '2026-09-26', time: '15:00', title: '交报告', type: 'custom', done: false },
  { id: 3, date: '2026-09-26', time: '19:00', title: '健身', type: 'custom', done: false },
  { id: 4, date: '2026-09-27', time: '09:00', title: '交报告截止', type: 'custom', done: false },
  { id: 5, date: '2026-09-28', time: '14:00', title: '实验室例会', type: 'meeting', done: false },
  { id: 6, date: '2026-10-05', time: '20:00', title: '英语考试', type: 'custom', done: false },
  { id: 7, date: '2026-10-15', time: '08:00', title: '交比赛材料', type: 'custom', done: false }
]

// 习惯列表（type: sleep=作息 / interval=间隔型 / point=时间点型）
// key            = 打卡记录的稳定标识（不能用 type，否则同类型第二个习惯会串台）
// confirmMode    = on_time 当场确认（窗口 windowMin 分钟）/ after_ward 事后确认（截止到次日 deadline）
// minRatio       = 多次型习惯的日达标线（当天完成率 >= 它就算这一天坚持住了）
// days 字段是后端老的展示缓存，现在天数改由 habit_log 派生，这里只作兜底
const habits = [
  {
    id: 1, key: 'sleep', name: '作息', days: 30, enabled: true, type: 'sleep',
    sleep: '23:30', wake: '07:00',
    confirmMode: 'after_ward', deadline: '12:00', minRatio: 1
  },
  {
    id: 2, key: 'water', name: '喝水', days: 5, enabled: true, type: 'interval',
    interval: 60, start: '09:00', end: '21:00',
    confirmMode: 'on_time', windowMin: 30, minRatio: 0.6
  },
  {
    id: 3, key: 'exercise', name: '锻炼', days: 12, enabled: true, type: 'point',
    time: '19:00', repeat: '每天',
    confirmMode: 'after_ward', deadline: '12:00', minRatio: 1
  }
]

// 可添加的预设习惯
const presetHabits = [
  { name: '吃药', type: 'point', time: '08:00', repeat: '每天' },
  { name: '护眼', type: 'interval', interval: 45, start: '09:00', end: '21:00' },
  { name: '久坐提醒', type: 'interval', interval: 60, start: '09:00', end: '20:00' },
  { name: '背单词', type: 'point', time: '21:00', repeat: '每天' }
]

// 其他功能设置（非习惯）
const otherSettings = {
  weather: { enabled: true, city: '南京', time: '07:30' },
  study: { pomodoro: 25, rest: 5 }
}

// 天气提醒（条件触发，一切正常时为空字符串）
const weatherAlert = '今天有雨，出门记得带伞'

module.exports = {
  allEvents,
  habits,
  presetHabits,
  otherSettings,
  weatherAlert
}
