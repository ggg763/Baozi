// 统一的 wx.request 封装：返回 Promise，自动拼 baseURL、统一错误处理
const app = getApp()

// 当前生效的后端地址。单独抽出来是因为真机排查时要把它显示出来 ——
// 「手机连不上」十有八九是地址不对，而不是代码不对。
function baseUrl() {
  return (app && app.globalData && app.globalData.apiBase) || ''
}

// 从异常里抽一句人能看懂的原因，用于直接显示在界面上。
// 真机最常撞见的两种：
//   request:fail url not in domain list -> 域名校验没过，点右上角「···」→ 打开调试
//   request:fail timeout                -> 根本连不上（不同网段 / 防火墙 / 后端没起）
function errText(err) {
  if (!err) return '未知错误'
  if (err.errMsg) return err.errMsg
  if (err.message) return err.message
  if (err.statusCode) return 'HTTP ' + err.statusCode + '（接口返回了错误状态）'
  if (err.code !== undefined) return '接口 code=' + err.code
  return String(err)
}

function request(path, method = 'GET', data = {}) {
  const base = baseUrl()
  return new Promise((resolve, reject) => {
    if (!base) {
      const e = new Error('apiBase 为空：app.js 里没配后端地址')
      e.errMsg = e.message
      reject(e)
      return
    }
    wx.request({
      url: base + path,
      method,
      data,
      timeout: 8000,
      header: { 'content-type': 'application/json' },
      success(res) {
        if (res.statusCode === 200 && res.data && res.data.code === 0) {
          resolve(res.data.data)
        } else {
          reject(res)
        }
      },
      fail(err) {
        reject(err)
      }
    })
  })
}

module.exports = { request, baseUrl, errText }
