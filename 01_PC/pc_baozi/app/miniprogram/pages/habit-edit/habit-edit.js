const api = require('../../utils/request.js')

// 提醒方式
const TYPE_OPTIONS = [
  { label: '作息（睡觉 / 起床）', value: 'sleep' },
  { label: '每隔一段时间', value: 'interval' },
  { label: '固定时间点', value: 'point' }
]

// 提前量。0 就是不提前 —— 喝水一天十几次，再提前就成骚扰了
const LEAD_OPTIONS = [
  { label: '不提前，到点才提醒', value: 0 },
  { label: '提前 5 分钟', value: 5 },
  { label: '提前 10 分钟', value: 10 },
  { label: '提前 15 分钟', value: 15 },
  { label: '提前 30 分钟', value: 30 },
  { label: '提前 1 小时', value: 60 }
]

const INTERVAL_OPTIONS = [15, 30, 45, 60, 90, 120]
const REPEAT_OPTIONS = ['每天', '每周', '工作日']
const RATIO_OPTIONS = [
  { label: '50%', value: 0.5 },
  { label: '60%', value: 0.6 },
  { label: '70%', value: 0.7 },
  { label: '80%', value: 0.8 },
  { label: '100%', value: 1 }
]

// 取最接近的一档下标：万一后端存了个列表里没有的值（比如别处配的 20 分钟），
// 就近落到相邻档，而不是悄悄变成第一项
function nearestIndex(list, v, key) {
  let best = 0
  let bestD = Infinity
  list.forEach((item, i) => {
    const cur = key ? item[key] : item
    const d = Math.abs(Number(cur) - Number(v))
    if (d < bestD) {
      bestD = d
      best = i
    }
  })
  return best
}

Page({
  data: {
    id: null,
    loading: true,
    name: '',
    type: 'point',
    typeIndex: 2,
    leadIndex: 0,
    intervalIndex: 3,
    repeatIndex: 0,
    ratioIndex: 1,
    time: '20:00',
    sleep: '23:30',
    wake: '07:00',
    start: '09:00',
    end: '21:00',
    interval: 60,
    repeat: '每天',
    typeOptions: TYPE_OPTIONS.map(o => o.label),
    leadOptions: LEAD_OPTIONS.map(o => o.label),
    intervalOptions: INTERVAL_OPTIONS,
    repeatOptions: REPEAT_OPTIONS,
    ratioOptions: RATIO_OPTIONS.map(o => o.label)
  },

  onLoad(options) {
    const id = options && options.id
    if (!id) {
      wx.showToast({ title: '缺少习惯 id', icon: 'none' })
      return
    }
    this.setData({ id: Number(id) })
    this.load()
  },

  // 习惯定义从列表接口拿（后端没有 GET /habits/{id} 这个接口，列表也就几条，够用）
  async load() {
    try {
      const data = await api.request('/api/v1/habits')
      const h = (data.habits || []).find(x => x.id === this.data.id)
      if (!h) {
        this.setData({ loading: false })
        wx.showToast({ title: '这个习惯已经不在了', icon: 'none' })
        setTimeout(() => wx.navigateBack(), 800)
        return
      }
      this.setData({
        loading: false,
        name: h.name || '',
        type: h.type || 'point',
        typeIndex: Math.max(0, TYPE_OPTIONS.findIndex(o => o.value === h.type)),
        leadIndex: nearestIndex(LEAD_OPTIONS, Number(h.lead_min) || 0, 'value'),
        intervalIndex: nearestIndex(INTERVAL_OPTIONS, Number(h.interval) || 60),
        repeatIndex: Math.max(0, REPEAT_OPTIONS.indexOf(h.repeat)),
        ratioIndex: nearestIndex(RATIO_OPTIONS, Number(h.min_ratio) || 0.6, 'value'),
        time: h.time || '20:00',
        sleep: h.sleep || '23:30',
        wake: h.wake || '07:00',
        start: h.start || '09:00',
        end: h.end || '21:00',
        interval: Number(h.interval) || 60,
        repeat: h.repeat || '每天'
      })
    } catch (err) {
      console.warn('读取习惯失败', err)
      this.setData({ loading: false })
      wx.showToast({ title: '读取失败，请检查后端连接', icon: 'none' })
    }
  },

  onName(e) { this.setData({ name: e.detail.value }) },

  onType(e) {
    const i = Number(e.detail.value)
    this.setData({ typeIndex: i, type: TYPE_OPTIONS[i].value })
  },

  onLead(e) { this.setData({ leadIndex: Number(e.detail.value) }) },

  onInterval(e) {
    const i = Number(e.detail.value)
    this.setData({ intervalIndex: i, interval: INTERVAL_OPTIONS[i] })
  },

  onRepeat(e) {
    const i = Number(e.detail.value)
    this.setData({ repeatIndex: i, repeat: REPEAT_OPTIONS[i] })
  },

  onRatio(e) { this.setData({ ratioIndex: Number(e.detail.value) }) },

  onTime(e) { this.setData({ time: e.detail.value }) },
  onSleep(e) { this.setData({ sleep: e.detail.value }) },
  onWake(e) { this.setData({ wake: e.detail.value }) },
  onStart(e) { this.setData({ start: e.detail.value }) },
  onEnd(e) { this.setData({ end: e.detail.value }) },

  async save() {
    const name = (this.data.name || '').trim()
    if (!name) {
      wx.showToast({ title: '习惯名不能为空', icon: 'none' })
      return
    }
    const type = this.data.type
    // 只发改动项。⚠️ 故意不带 confirm_mode：
    // 那是"打卡窗口怎么算"的规则，编辑名字/时间时不该被顺手改掉
    const patch = {
      name,
      type,
      lead_min: LEAD_OPTIONS[this.data.leadIndex].value
    }
    if (type === 'sleep') {
      patch.sleep = this.data.sleep
      patch.wake = this.data.wake
    } else if (type === 'interval') {
      patch.interval = this.data.interval
      patch.start = this.data.start
      patch.end = this.data.end
      patch.min_ratio = RATIO_OPTIONS[this.data.ratioIndex].value
    } else {
      patch.time = this.data.time
      patch.repeat = this.data.repeat
    }

    wx.showLoading({ title: '保存中', mask: true })
    try {
      await api.request('/api/v1/habits/' + this.data.id, 'PATCH', patch)
      wx.hideLoading()
      wx.showToast({ title: '已保存', icon: 'success' })
      setTimeout(() => wx.navigateBack(), 600)
    } catch (err) {
      wx.hideLoading()
      console.warn('保存习惯失败', err)
      wx.showToast({ title: '保存失败，请检查后端连接', icon: 'none' })
    }
  }
})
