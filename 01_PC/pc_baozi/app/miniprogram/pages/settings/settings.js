const api = require('../../utils/request.js')
const mock = require('../../data/mock.js')
const store = require('../../utils/habitStore.js')

// 属于 habit.config 的字段（扁平键 ↔ 后端 config JSON）
const CONFIG_KEYS = ['time', 'repeat', 'interval', 'start', 'end', 'sleep', 'wake']

// 两次播报的最小间隔（分钟）。下标和 cooldownOptions 一一对应
const COOLDOWN_VALUES = [0, 1, 2, 5, 10, 15, 30]

Page({
  data: {
    habits: [],
    other: {},
    intervalOptions: ['15', '30', '45', '60', '90', '120'],
    repeatOptions: ['每天', '每周', '工作日'],
    pomodoroOptions: ['15', '20', '25', '30', '45', '60'],
    restOptions: ['3', '5', '10', '15'],
    pomodoroIndex: 2,
    restIndex: 1,
    // 提醒设置（免打扰 / 冷却）：存在后端 settings 表，桥接每分钟回读一次
    remind: { enabled: true, start: '00:00', end: '07:00', exemptSleep: true, cooldownIndex: 3 },
    cooldownOptions: ['不限', '1 分钟', '2 分钟', '5 分钟', '10 分钟', '15 分钟', '30 分钟'],
    // 打卡相关
    todayStr: '',
    pendingItems: [],
    offline: false,          // 后端没连上：卡片区不显示（避免假数据），顶部给一条提示
    offlineMsg: '',          // 连不上的具体原因（真机排查用，直接显示出来）
    apiBase: '',             // 当前请求的后端地址（确认它是不是电脑的局域网 IP）
    // 删除确认弹窗（长按习惯卡片触发）
    dlgShow: false,
    dlgDesc: '',
    pendingId: null
  },

  onLoad() {
    this.viewDates = {}          // key -> 正在浏览的日期（补卡用）
    this._seq = 0                // 请求序号，防止慢请求覆盖新结果
    const other = JSON.parse(JSON.stringify(mock.otherSettings))
    this.setData({
      other,
      todayStr: store.todayStr(),
      pomodoroIndex: this.data.pomodoroOptions.indexOf(String(other.study.pomodoro)),
      restIndex: this.data.restOptions.indexOf(String(other.study.rest))
    })
    this.loadHabits()
    this.loadSettings()
  },

  // 从别的页面切回来时重算打卡状态（时间可能已经走过）。
  // 从"习惯编辑页"回来时连定义都可能变了（改名、改提前量），所以整份重拉，
  // 而不是只刷新卡片状态 —— 否则改完名字回来还是旧名字。
  onShow() {
    if (this.data.habits.length) this.loadHabits()
  },

  // ---------- 加载 ----------
  // 习惯定义 + 今日卡片，两段都来自后端
  async loadHabits() {
    let list
    try {
      const data = await api.request('/api/v1/habits')
      list = (data.habits || []).map(h => this.normalize(h))
    } catch (e) {
      // 后端未启动：习惯列表用本地兜底撑住页面，但**不给假天数**
      // （打卡判定只有后端一份，前端算不出来，所以卡片区直接隐藏）
      console.warn('后端未连接，习惯列表用本地兜底，打卡功能不可用', e)
      this.setData({
        habits: mock.habits.map(h => this.normalize(h)).map(h => Object.assign({}, h, { ck: null })),
        offline: true,
        offlineMsg: api.errText(e),
        apiBase: api.baseUrl(),
        todayStr: store.todayStr(),
        pendingItems: []
      })
      return
    }
    this.setData({ habits: list, offline: false, offlineMsg: '', apiBase: api.baseUrl() })
    await this.refreshCards()
  },

  // 补齐 picker 索引 + 打卡规格（key / 确认模式 / 窗口）
  normalize(h) {
    const o = store.normalizeHabit(h)
    if (o.type === 'interval') {
      o.intervalIndex = this.data.intervalOptions.indexOf(String(o.interval))
    }
    if (o.type === 'point') {
      o.repeatIndex = this.data.repeatOptions.indexOf(o.repeat)
    }
    return o
  },

  // 刷新所有卡片的打卡模型。
  // 判定全在后端（GET /habits/today），这里只做「按日期取卡片 + 合并到习惯上」。
  // 每个习惯可能正在浏览不同的日期（补卡），所以按日期去重后并发拉取，
  // 通常只有 1~2 个请求。
  async refreshCards() {
    const today = store.todayStr()
    const habits = this.data.habits
    if (!habits.length) {
      this.setData({ todayStr: today, pendingItems: [] })
      return
    }

    const wanted = {}
    wanted[today] = true
    habits.forEach(h => { wanted[this.viewDates[h.key] || today] = true })
    const dates = Object.keys(wanted)

    const seq = ++this._seq
    let packs
    try {
      packs = await Promise.all(dates.map(d => store.loadCards(d)))
    } catch (e) {
      console.warn('打卡数据拉取失败', e)
      if (seq === this._seq) {
        this.setData({
          habits: habits.map(h => Object.assign({}, h, { ck: null })),
          todayStr: today,
          pendingItems: [],
          offline: true,
          offlineMsg: api.errText(e),
          apiBase: api.baseUrl()
        })
      }
      return
    }
    if (seq !== this._seq) return         // 已经有更新的请求在跑，丢弃这次的结果

    const byDate = {}
    dates.forEach((d, i) => { byDate[d] = packs[i] })

    const merged = habits.map(h => {
      const pack = byDate[this.viewDates[h.key] || today]
      const ck = (pack && pack.cards) ? (pack.cards[h.key] || null) : null
      return Object.assign({}, h, { ck })
    })
    const todayPack = byDate[today]
    this.setData({
      habits: merged,
      todayStr: today,
      pendingItems: (todayPack && todayPack.pending) || [],
      offline: false,
      offlineMsg: '',
      apiBase: api.baseUrl()
    })
  },

  habitByKey(key) {
    const list = this.data.habits
    for (let i = 0; i < list.length; i++) {
      if (list[i].key === key) return list[i]
    }
    return null
  },

  // 从扁平习惯对象里挑出 config 字段
  configOf(h) {
    const out = {}
    CONFIG_KEYS.forEach(k => {
      if (h[k] !== undefined) out[k] = h[k]
    })
    return out
  },

  // 静默同步到后端（失败只告警，不打断操作）。返回 Promise，便于调用方 await。
  syncHabit(id, patch) {
    if (id === null || id === undefined) return Promise.resolve(null)
    if (typeof id === 'string' && id.indexOf('tmp_') === 0) return Promise.resolve(null) // 还没换成真实 id
    return api.request(`/api/v1/habits/${id}`, 'PATCH', patch).catch(err => {
      console.warn('习惯同步失败', patch, err)
      return null
    })
  },

  // ---------- 打卡 / 撤销 / 达标线 ----------
  // 点一下：待确认 → 完成；已错过 → 补卡；已完成 → 撤销；还没到 → 提示
  async onSlotTap(e) {
    const { hkey, slot, date, state } = e.currentTarget.dataset
    if (state === 'future') {
      wx.showToast({ title: '还没到时间', icon: 'none' })
      return
    }
    if (this.data.offline) {
      wx.showToast({ title: '未连接后端，无法打卡', icon: 'none' })
      return
    }

    // 已完成的再点一下 = 撤销。打勾是误触的重灾区，必须能一步退回来。
    // 撤销后圆圈回到原本的状态（窗口过了是「已错过」，没过是「窗口内」），
    // 再点一下又能打上 —— 这一步是可逆的，所以不必再加二次确认。
    if (state === 'done') {
      wx.vibrateShort && wx.vibrateShort({ type: 'light' })
      try {
        await store.uncheck(hkey, { date, slot })
        wx.showToast({ title: '已撤销', icon: 'none' })
        await this.refreshCards()
      } catch (err) {
        console.warn('撤销失败', err)
        wx.showToast({ title: '撤销失败，请检查后端连接', icon: 'none' })
      }
      return
    }

    wx.vibrateShort && wx.vibrateShort({ type: 'light' })
    try {
      await store.checkin(hkey, { date, slot, source: 'app' })
      wx.showToast({ title: state === 'missed' ? '已补卡' : '已完成', icon: 'none' })
      await this.refreshCards()
    } catch (err) {
      console.warn('打卡失败', err)
      wx.showToast({ title: '打卡失败，请检查后端连接', icon: 'none' })
    }
  },

  // 长按：只有已完成的才能撤销
  async onSlotUndo(e) {
    const { hkey, slot, date, state } = e.currentTarget.dataset
    if (state !== 'done') {
      wx.showToast({ title: '只有已完成的可撤销', icon: 'none' })
      return
    }
    if (this.data.offline) {
      wx.showToast({ title: '未连接后端，无法撤销', icon: 'none' })
      return
    }
    wx.vibrateShort && wx.vibrateShort({ type: 'light' })
    try {
      await store.uncheck(hkey, { date, slot })
      wx.showToast({ title: '已撤销', icon: 'none' })
      await this.refreshCards()
    } catch (err) {
      console.warn('撤销失败', err)
      wx.showToast({ title: '撤销失败，请检查后端连接', icon: 'none' })
    }
  },

  // 达标线：50% / 60% / 70% / 80% / 100%（改的是规则，存在后端 habits.min_ratio）
  onRatioPick(e) {
    const hkey = e.currentTarget.dataset.hkey
    const h = this.habitByKey(hkey)
    if (!h) return
    wx.showActionSheet({
      itemList: store.RATIO_LABELS.map(t => '达标线 ' + t),
      success: res => {
        this.saveMinRatio(h, store.RATIO_VALUES[res.tapIndex])
      }
    })
  },

  async saveMinRatio(h, ratio) {
    wx.showLoading({ title: '保存中', mask: true })
    try {
      await store.setMinRatio(h.id, ratio)
      wx.hideLoading()
      await this.refreshCards()
      wx.showToast({ title: '达标线已更新', icon: 'none' })
    } catch (err) {
      wx.hideLoading()
      console.warn('达标线保存失败', err)
      wx.showToast({ title: '保存失败，请检查后端连接', icon: 'none' })
    }
  },

  // 点日期 chip → 切到任意一天补卡（不能选未来）
  onPickDate(e) {
    const hkey = e.currentTarget.dataset.hkey
    this.viewDates[hkey] = e.detail.value
    this.refreshCards()
  },

  // ---------- 编辑 ----------
  async onToggle(e) {
    const id = e.currentTarget.dataset.id
    const enabled = e.detail.value
    this.setData({
      habits: this.data.habits.map(h => (h.id === id ? { ...h, enabled } : h))
    })
    await this.syncHabit(id, { enabled })
    // 停用的习惯不进待确认清单，要重算
    this.refreshCards()
  },

  // 统一处理习惯参数修改（按 type 区分 picker 类型）
  async onFieldChange(e) {
    const { id, field, kind } = e.currentTarget.dataset
    const patch = {}
    const habits = this.data.habits.map(h => {
      if (h.id !== id) return h
      const o = { ...h }
      if (kind === 'time') {
        o[field] = e.detail.value
        patch[field] = e.detail.value
      } else if (kind === 'interval') {
        const v = Number(this.data.intervalOptions[e.detail.value])
        o.intervalIndex = e.detail.value
        o.interval = v
        patch.interval = v
      } else if (kind === 'repeat') {
        const v = this.data.repeatOptions[e.detail.value]
        o.repeatIndex = e.detail.value
        o.repeat = v
        patch.repeat = v
      }
      return o
    })
    this.setData({ habits })
    // 先等后端把 config 改完，再刷新卡片 —— 否则会拿旧配置算出旧状态
    await this.syncHabit(id, patch)
    this.refreshCards()
  },

  // ---------- 新增 ----------
  // 先本地插入（拿到临时 id 立刻可见），后端返回真实 id 后替换
  createHabit(raw) {
    const tmpId = 'tmp_' + Date.now()
    const draft = this.normalize({ ...raw, id: tmpId })
    this.setData({ habits: this.data.habits.concat(draft) })

    api
      .request('/api/v1/habits', 'POST', {
        name: raw.name,
        type: raw.type,
        enabled: true,
        days: raw.days || 0,
        ...this.configOf(raw)
      })
      .then(res => {
        const realId = res && res.id
        if (realId === null || realId === undefined) return
        this.setData({
          habits: this.data.habits.map(h => (h.id === tmpId ? { ...h, id: realId } : h))
        })
        // 新建的习惯也要有卡片（天数从 0 起算）
        this.refreshCards()
      })
      .catch(err => {
        console.warn('新增习惯未同步到后端', err)
        wx.showToast({ title: '新增未同步到后端', icon: 'none' })
        // 同步失败就撤掉刚插进去的那张草稿，避免留着个点了没反应的卡片
        this.setData({ habits: this.data.habits.filter(h => h.id !== tmpId) })
      })
  },

  // 添加习惯：弹出预设列表 + 自定义
  addHabit() {
    const names = mock.presetHabits.map(h => h.name).concat(['自定义…'])
    wx.showActionSheet({
      itemList: names,
      success: res => {
        if (res.tapIndex === mock.presetHabits.length) {
          this.customHabit()
        } else {
          const preset = { ...mock.presetHabits[res.tapIndex] }
          this.createHabit({ ...preset, days: 0, enabled: true })
        }
      }
    })
  },

  // 自定义习惯：先输入名字，再选提醒类型
  customHabit() {
    wx.showModal({
      title: '自定义习惯',
      editable: true,
      placeholderText: '输入习惯名称，如：记日记',
      success: res => {
        if (!res.confirm || !res.content || !res.content.trim()) return
        const name = res.content.trim()
        wx.showActionSheet({
          itemList: ['每天固定时间提醒', '每隔一段时间提醒'],
          success: r => {
            const base = { name, days: 0, enabled: true }
            const habit =
              r.tapIndex === 1
                ? { ...base, type: 'interval', interval: 60, start: '09:00', end: '21:00' }
                : { ...base, type: 'point', time: '20:00', repeat: '每天' }
            this.createHabit(habit)
          }
        })
      }
    })
  },

  // ---------- 左滑：修改习惯 ----------
  onEditHabit(e) {
    const id = e.currentTarget.dataset.id
    const c = this.selectComponent('#sh-' + id)
    if (c) c.close()
    wx.navigateTo({ url: '/pages/habit-edit/habit-edit?id=' + id })
  },

  // ---------- 长按删除 ----------
  onHabitLongPress(e) {
    const { id, name } = e.currentTarget.dataset
    wx.vibrateShort && wx.vibrateShort({ type: 'light' })
    this.setData({
      dlgShow: true,
      pendingId: id,
      dlgDesc: `「${name}」和它的打卡记录都会被删除`
    })
  },

  onCancelDelete() {
    this.setData({ dlgShow: false, pendingId: null, dlgDesc: '' })
  },

  async onConfirmDelete() {
    const id = this.data.pendingId
    this.setData({ dlgShow: false, pendingId: null, dlgDesc: '' })
    if (id === null || id === undefined) return

    // 乐观更新：先从列表移除
    this.setData({ habits: this.data.habits.filter(h => h.id !== id) })

    // 本地临时 id（还没同步成功的）不用打后端
    if (typeof id === 'string' && id.indexOf('tmp_') === 0) {
      wx.showToast({ title: '已删除', icon: 'none' })
      return
    }
    try {
      await api.request(`/api/v1/habits/${id}`, 'DELETE')
      wx.showToast({ title: '已删除', icon: 'none' })
    } catch (err) {
      console.warn('删除同步失败', err)
      wx.showToast({ title: '已从列表移除，云端同步失败', icon: 'none' })
    }
    this.refreshCards()
  },

  // ---------- 其他功能 ----------
  // 这一区原来只改内存里的 mock，重启就回默认。现在一并落库到 settings 表。
  onWeatherToggle(e) {
    this.setData({ 'other.weather.enabled': e.detail.value })
    this.saveSettings({ weather_enabled: e.detail.value ? '1' : '0' })
  },
  onWeatherTime(e) {
    this.setData({ 'other.weather.time': e.detail.value })
    this.saveSettings({ weather_time: e.detail.value })
  },
  // 城市是输入框：边打字边存太吵（一个字一次请求），失焦时再存
  onCity(e) {
    this.setData({ 'other.weather.city': e.detail.value })
  },
  onCityBlur(e) {
    this.saveSettings({ weather_city: e.detail.value })
  },
  onPomodoroChange(e) {
    const v = Number(this.data.pomodoroOptions[e.detail.value])
    this.setData({ pomodoroIndex: e.detail.value, 'other.study.pomodoro': v })
    this.saveSettings({ pomodoro: String(v) })
  },
  onRestChange(e) {
    const v = Number(this.data.restOptions[e.detail.value])
    this.setData({ restIndex: e.detail.value, 'other.study.rest': v })
    this.saveSettings({ rest: String(v) })
  },

  // ---------- 提醒设置（免打扰 / 冷却）----------
  //
  // 【为什么这些值放后端而不是本地】
  //   真正出声的是 PC 端的桥接，不是小程序。设置只有存在后端，
  //   桥接才读得到 —— 存本地就成了"手机上关了免打扰，半夜照响"。
  //   桥接每分钟回读一次 settings，所以这里改完大约一分钟内生效。
  async loadSettings() {
    let s
    try {
      s = await api.request('/api/v1/settings')
    } catch (e) {
      console.warn('读取提醒设置失败，先用默认值', e)
      return
    }
    const idx = COOLDOWN_VALUES.indexOf(Number(s.remind_cooldown_min))
    const other = this.data.other
    // 其他功能：库里有值就用库里的，没有就保留 mock 的默认
    if (s.weather_enabled !== undefined) other.weather.enabled = String(s.weather_enabled) === '1'
    other.weather.time = s.weather_time || other.weather.time
    other.weather.city = s.weather_city || other.weather.city
    other.study.pomodoro = Number(s.pomodoro) || other.study.pomodoro
    other.study.rest = Number(s.rest) || other.study.rest

    this.setData({
      remind: {
        enabled: String(s.quiet_enabled) !== '0',
        start: s.quiet_start || '00:00',
        end: s.quiet_end || '07:00',
        exemptSleep: String(s.quiet_exempt_sleep) !== '0',
        cooldownIndex: idx >= 0 ? idx : 3
      },
      other,
      pomodoroIndex: Math.max(0, this.data.pomodoroOptions.indexOf(String(other.study.pomodoro))),
      restIndex: Math.max(0, this.data.restOptions.indexOf(String(other.study.rest)))
    })
  },

  // 静默同步到后端，失败只告警不打断（设置类操作不该弹一堆错误框）
  saveSettings(patch) {
    return api.request('/api/v1/settings', 'PUT', patch).catch(err => {
      console.warn('设置同步失败', patch, err)
      wx.showToast({ title: '设置没存上，请检查后端连接', icon: 'none' })
      return null
    })
  },

  onQuietToggle(e) {
    this.setData({ 'remind.enabled': e.detail.value })
    this.saveSettings({ quiet_enabled: e.detail.value ? '1' : '0' })
  },
  onQuietStart(e) {
    this.setData({ 'remind.start': e.detail.value })
    this.saveSettings({ quiet_start: e.detail.value })
  },
  onQuietEnd(e) {
    this.setData({ 'remind.end': e.detail.value })
    this.saveSettings({ quiet_end: e.detail.value })
  },
  onExemptToggle(e) {
    this.setData({ 'remind.exemptSleep': e.detail.value })
    this.saveSettings({ quiet_exempt_sleep: e.detail.value ? '1' : '0' })
  },
  onCooldownChange(e) {
    const i = Number(e.detail.value)
    this.setData({ 'remind.cooldownIndex': i })
    this.saveSettings({ remind_cooldown_min: String(COOLDOWN_VALUES[i]) })
  }
})
