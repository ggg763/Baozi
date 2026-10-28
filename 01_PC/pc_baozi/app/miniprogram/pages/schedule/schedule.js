// 提醒提前量选项（秒）
const api = require('../../utils/request.js')

const REMIND_OPTIONS = [
  { label: '提前 5 分钟', sec: 300 },
  { label: '提前 10 分钟', sec: 600 },
  { label: '提前 15 分钟', sec: 900 },
  { label: '提前 30 分钟', sec: 1800 },
  { label: '提前 1 小时', sec: 3600 },
  { label: '提前 2 小时', sec: 7200 },
  { label: '提前 1 天', sec: 86400 }
]

function pad(n) {
  return String(n).padStart(2, '0')
}

// Date -> YYYY-MM-DD
function fmtDate(d) {
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
}

// YYYY-MM-DD -> 「M月D日 周X」
function dateLabel(s) {
  const d = new Date(s.replace(/-/g, '/'))
  const week = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'][d.getDay()]
  return `${d.getMonth() + 1}月${d.getDate()}日 ${week}`
}

// 后端存的是秒数，picker 用的是下标：转回来时取最接近的一档。
// （用户如果在别处配了个 45 分钟，这里会就近落到 30 或 60，而不是悄悄改成 10 分钟）
function secToOption(sec) {
  let best = 0
  let bestD = Infinity
  REMIND_OPTIONS.forEach((o, i) => {
    const d = Math.abs(o.sec - sec)
    if (d < bestD) {
      bestD = d
      best = i
    }
  })
  return { index: best, label: REMIND_OPTIONS[best].label, sec: REMIND_OPTIONS[best].sec }
}

Page({
  data: {
    editing: false,          // 带 id 进来 = 改已有日程
    title: '',
    date: '',
    dateText: '',
    startDate: '',
    endDate: '',
    time: '09:00',
    remindOptions: REMIND_OPTIONS.map(o => o.label),
    remindOffsets: [{ index: 3, label: '提前 30 分钟', sec: 1800 }],
    typeOptions: ['工作', '学习', '吃药', '其他'],
    typeIndex: 0,
    repeatOptions: ['不重复', '每天', '每周', '工作日'],
    repeatIndex: 0
  },

  onLoad(options) {
    const today = new Date()
    const end = new Date(today.getTime() + 365 * 86400000)
    const date = fmtDate(today)
    this.setData({
      date,
      dateText: '今天 · ' + dateLabel(date),
      startDate: date,
      endDate: fmtDate(end)
    })

    // 首页左滑「修改」会带 id 进来。复用同一张表单，而不是再抄一份，
    // 这样"新建时能设的"和"修改时能改的"天然一致，不会两边跑偏。
    const id = options && options.id
    if (id) {
      this.editId = Number(id)
      this.setData({ editing: true })
      wx.setNavigationBarTitle({ title: '修改日程' })
      this.loadEvent(this.editId)
    }
  },

  async loadEvent(id) {
    try {
      const data = await api.request('/api/v1/events')
      const e = (data.events || []).find(x => x.id === id)
      if (!e) {
        wx.showToast({ title: '这条日程已经不在了', icon: 'none' })
        setTimeout(() => wx.navigateBack(), 800)
        return
      }
      const secs = (e.remind_offsets && e.remind_offsets.length) ? e.remind_offsets : [600]
      this.setData({
        title: e.title || '',
        date: e.date,
        dateText: dateLabel(e.date),
        time: e.time,
        remindOffsets: secs.map(secToOption),
        typeIndex: Math.max(0, this.data.typeOptions.indexOf(e.event_type)),
        repeatIndex: Math.max(0, this.data.repeatOptions.indexOf(e.repeat_rule))
      })
    } catch (err) {
      console.warn('读取日程失败', err)
      wx.showToast({ title: '读取失败，请检查后端连接', icon: 'none' })
    }
  },

  onTitle(e) {
    this.setData({ title: e.detail.value })
  },

  onDateChange(e) {
    const date = e.detail.value
    this.setData({ date, dateText: dateLabel(date) })
  },

  onTimeChange(e) {
    this.setData({ time: e.detail.value })
  },

  onTypeChange(e) {
    this.setData({ typeIndex: e.detail.value })
  },

  onRepeatChange(e) {
    this.setData({ repeatIndex: e.detail.value })
  },

  onRemindChange(e) {
    const idx = e.currentTarget.dataset.idx
    const oi = e.detail.value
    const remindOffsets = this.data.remindOffsets.slice()
    remindOffsets[idx] = { index: oi, label: REMIND_OPTIONS[oi].label, sec: REMIND_OPTIONS[oi].sec }
    this.setData({ remindOffsets })
  },

  addRemind() {
    this.setData({
      remindOffsets: this.data.remindOffsets.concat([{ index: 1, label: '提前 10 分钟', sec: 600 }])
    })
  },

  removeRemind(e) {
    const idx = e.currentTarget.dataset.idx
    this.setData({ remindOffsets: this.data.remindOffsets.filter((_, i) => i !== idx) })
  },

  voiceInput() {
    wx.showToast({ title: '语音录入待接入', icon: 'none' })
  },

  async save() {
    if (!this.data.title) {
      wx.showToast({ title: '请填写标题', icon: 'none' })
      return
    }
    const payload = {
      title: this.data.title,
      event_time: `${this.data.date} ${this.data.time}`,
      remind_offsets: this.data.remindOffsets.map(o => o.sec),
      repeat_rule: this.data.repeatOptions[this.data.repeatIndex],
      event_type: this.data.typeOptions[this.data.typeIndex]
    }
    try {
      if (this.editId) {
        await api.request(`/api/v1/events/${this.editId}`, 'PUT', payload)
        wx.showToast({ title: '修改已保存', icon: 'success' })
      } else {
        await api.request('/api/v1/events', 'POST', payload)
        wx.showToast({ title: '已保存', icon: 'success' })
      }
      setTimeout(() => wx.navigateBack(), 600)
    } catch (e) {
      console.warn('保存失败（后端未连接）', e)
      wx.showToast({ title: '保存失败，后端未连接', icon: 'none' })
    }
  }
})
