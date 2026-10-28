const api = require('../../utils/request.js')
const mock = require('../../data/mock.js')

// 日期标签：今天 / 明天 / 后天 / 具体日期（含周几）
function labelFor(dateStr) {
  const d = new Date(dateStr.replace(/-/g, '/'))
  const today = new Date()
  today.setHours(0, 0, 0, 0)
  const diff = Math.round((d.getTime() - today.getTime()) / 86400000)
  const week = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'][d.getDay()]
  if (diff === 0) return '今天'
  if (diff === 1) return '明天'
  if (diff === 2) return '后天'
  return `${d.getMonth() + 1}月${d.getDate()}日 ${week}`
}

// 后端 event → 前端 item（status 映射为 done）
function toItem(e) {
  return { id: e.id, date: e.date, time: e.time, title: e.title, done: e.status === 'done' }
}

// 只保留「今天及以后」的日程 —— 和包子屏幕侧 build_sched_items 的规则保持一致。
// 后端返回的是全量历史，不过滤的话首页顶部会堆一堆过期日程（屏幕上却看不到它们，
// 两边条数不一致，用户会以为同步出了问题）。
// date 统一是 YYYY-MM-DD 零填充格式，可以直接字符串比较。
function upcomingOnly(items) {
  const t = new Date()
  const pad = n => (n < 10 ? '0' + n : '' + n)
  const today = t.getFullYear() + '-' + pad(t.getMonth() + 1) + '-' + pad(t.getDate())
  return items.filter(it => (it.date || '') >= today)
}

// 按日期分组 + 排序
function groupByDate(events) {
  const map = {}
  events.forEach(e => {
    if (!map[e.date]) map[e.date] = []
    map[e.date].push(e)
  })
  return Object.keys(map)
    .sort((a, b) => (a < b ? -1 : 1))
    .map(date => {
      const items = map[date].sort((a, b) => (a.time < b.time ? -1 : 1))
      return { date, dateLabel: labelFor(date), items }
    })
}

Page({
  data: {
    groups: [],
    statusBarHeight: 20,
    navBarHeight: 44,
    rightInset: 100,
    // 后端没连上：列表里是**示例数据**，必须在界面上说明（否则会被当成真实日程）
    offline: false,
    offlineMsg: '',
    apiBase: '',
    // 删除确认弹窗（长按日程触发）
    dlgShow: false,
    dlgDesc: '',
    pendingId: null
  },

  onLoad() {
    const sys = wx.getWindowInfo ? wx.getWindowInfo() : wx.getSystemInfoSync()
    const statusBarHeight = sys.statusBarHeight || 20
    let navBarHeight = 44
    let rightInset = 100
    try {
      const menu = wx.getMenuButtonBoundingClientRect()
      navBarHeight = (menu.top - statusBarHeight) * 2 + menu.height
      rightInset = sys.windowWidth - menu.left + 6
    } catch (e) {}
    this.setData({ statusBarHeight, navBarHeight, rightInset })
  },

  // ⚠️ 日程是在 schedule 页添加的，保存后 navigateBack 回本页。
  // 小程序导航返回**不会重新触发 onLoad**（本页一直留在页面栈里），
  // 所以拉取必须放在 onShow —— 否则刚添加的日程要退出重进才看得到。
  // 放在 onShow 而不是 onLoad，顺带解决了「从别的 tab 切回来数据过期」。
  onShow() {
    this.loadEvents()
  },

  async loadEvents() {
    try {
      const data = await api.request('/api/v1/events')
      this.setData({
        groups: groupByDate(upcomingOnly(data.events.map(toItem))),
        offline: false, offlineMsg: '', apiBase: api.baseUrl()
      })
    } catch (e) {
      // 后端没连上：用示例数据撑住页面，但**必须在界面上标明是示例**。
      // 否则用户会把示例日程当成自己的数据 —— 「刚添加的日程没出现」
      // 有一半是这么来的（另一半是本页只写了 onLoad，见上面的 onShow 注释）。
      console.warn('后端未连接，使用示例数据', e)
      this.setData({
        groups: groupByDate(mock.allEvents),
        offline: true,
        offlineMsg: api.errText(e),
        apiBase: api.baseUrl()
      })
    }
  },

  async toggleDone(e) {
    const id = e.currentTarget.dataset.id
    // 先乐观更新 UI，再同步后端
    let target = 'pending'
    const groups = this.data.groups.map(g => ({
      ...g,
      items: g.items.map(it => {
        if (it.id === id) {
          const done = !it.done
          if (done) target = 'done'
          return { ...it, done }
        }
        return it
      })
    }))
    this.setData({ groups })
    try {
      await api.request(`/api/v1/events/${id}/status`, 'PATCH', { status: target })
    } catch (err) {
      console.warn('同步状态失败', err)
    }
  },

  // ---------- 左滑：修改 / 删除 ----------
  // 收起某一行的动作区。动作区是"敞开"状态时如果不管它，
  // 从编辑页返回后它还敞着，视觉上很怪。
  closeSwipe(id) {
    const c = this.selectComponent('#sw-' + id)
    if (c) c.close()
  },

  onEditItem(e) {
    const id = e.currentTarget.dataset.id
    this.closeSwipe(id)
    // 编辑走"新建日程"同一张表单，带 id 进去就是改（后端 PUT 同一个接口）
    wx.navigateTo({ url: '/pages/schedule/schedule?id=' + id })
  },

  onAskDelete(e) {
    const id = e.currentTarget.dataset.id
    this.closeSwipe(id)
    let title = ''
    this.data.groups.forEach(g => g.items.forEach(it => {
      if (it.id === id) title = it.title
    }))
    this.setData({
      dlgShow: true,
      pendingId: id,
      dlgDesc: `「${title}」删除后无法恢复`
    })
  },

  // ---------- 长按删除 ----------
  onItemLongPress(e) {
    const { id, title } = e.currentTarget.dataset
    wx.vibrateShort && wx.vibrateShort({ type: 'light' })
    this.setData({
      dlgShow: true,
      pendingId: id,
      dlgDesc: `「${title}」删除后无法恢复`
    })
  },

  onCancelDelete() {
    this.setData({ dlgShow: false, pendingId: null, dlgDesc: '' })
  },

  async onConfirmDelete() {
    const id = this.data.pendingId
    this.setData({ dlgShow: false, pendingId: null, dlgDesc: '' })
    if (id === null || id === undefined) return

    // 乐观更新：先从列表移除（空分组一并去掉）
    const groups = this.data.groups
      .map(g => ({ ...g, items: g.items.filter(it => it.id !== id) }))
      .filter(g => g.items.length > 0)
    this.setData({ groups })

    try {
      await api.request(`/api/v1/events/${id}`, 'DELETE')
      wx.showToast({ title: '已删除', icon: 'none' })
    } catch (err) {
      console.warn('删除同步失败', err)
      wx.showToast({ title: '已从列表移除，云端同步失败', icon: 'none' })
    }
  },

  goSchedule() {
    wx.navigateTo({ url: '/pages/schedule/schedule' })
  }
})
