Page({
  data: {
    online: true,
    remindOn: true
  },

  onRemindToggle(e) {
    this.setData({ remindOn: e.detail.value })
  },

  showHelp() {
    wx.showModal({
      title: '语音指令',
      content: '「包子，明天下午3点提醒我交报告，提前2小时」\n「包子，看看今天日程」\n「包子，开始一个番茄钟」\n「包子，该喝水了记得提醒我」',
      showCancel: false
    })
  },

  bindDevice() {
    wx.showToast({ title: '绑定待接入', icon: 'none' })
  }
})
