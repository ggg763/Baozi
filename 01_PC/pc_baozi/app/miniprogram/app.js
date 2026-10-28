App({
  globalData: {
    // 后端地址：真机调试用电脑局域网 IP（手机和电脑需连同一 WiFi）
    // 注意：电脑 IP 变了这里也要改；模拟器调试可临时改回 http://127.0.0.1:8000
    apiBase: 'http://10.183.231.252:8000',
    deviceOnline: true
  },
  onLaunch() {
    // 后续在这里做登录 / 拉取设备状态
  }
})
