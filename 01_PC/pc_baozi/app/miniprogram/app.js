App({
  globalData: {
    // 后端地址：默认回环（开发者工具模拟器用）。
    //
    // 真机调试要填电脑的局域网 IP，但**不要写在这个文件里**——仓库是公开的。
    // 在开发者工具的 Console 里执行一次即可（存在本地缓存，不进源码）：
    //     wx.setStorageSync('apiBase', 'http://192.168.1.x:8000')
    // 想改回来：
    //     wx.removeStorageSync('apiBase')
    // 读取优先级见 utils/request.js 的 baseUrl()。
    apiBase: 'http://127.0.0.1:8000',
    deviceOnline: true
  },
  onLaunch() {
    // 后续在这里做登录 / 拉取设备状态
  }
})
