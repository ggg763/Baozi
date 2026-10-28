# 包子（BaoZi）小程序

微信原生小程序，作为包子桌面机器人的「配置入口」。米黄配色，与包子屏幕 UI 风格统一。

## 目录结构

```
baozi-miniprogram/
├── app.json                页面注册 + tabBar + 窗口样式
├── app.js                  全局逻辑
├── app.wxss                全局样式（米黄主题色）
├── project.config.json     项目配置（已关域名校验）
├── data/mock.js            ★ 假数据（后端好了换这里）
└── pages/
    ├── index/              首页：一句话摘要 + 今日日程
    ├── schedule/           日程编辑：标题/时间/多档提醒/类型/重复
    ├── settings/           设置：作息/喝水/锻炼/天气/学习
    └── mine/               我的：设备状态/开关/帮助
```

## 怎么跑

1. 安装 **微信开发者工具**（微信官方，免费）
2. 打开工具 → 「导入项目」→ 选择 `baozi-miniprogram` 这个文件夹
3. AppID 选「测试号」（工具会提示，或填 `touristappid`）
4. 编译运行，即可看到 4 个页面（当前用的是 `data/mock.js` 里的假数据）

## 配色规范

| 用途 | 色值 |
|---|---|
| 米黄底 | `#FDF6E3` |
| 卡片/衬底 | `#FAEEDA` |
| 主色/描边 | `#BA7517` |
| 浅线条 | `#E8DCC0` |
| 主文字 | `#412402` |
| 次文字 | `#854F0B` |

## 后续接后端（三步）

1. 在 `app.js` 的 `globalData.apiBase` 填入后端地址（局域网 PC 的 IP）
2. 把 `data/mock.js` 换成接口请求（`wx.request`），接口定义见《功能设计文档 v1.0》第 6 章
3. 真机调试时：手机和 PC 连同一个 WiFi，开发者工具勾选「不校验合法域名」

## 关键设计

- **一句话摘要**：`pages/index/index.js` 里的 `buildSummary()` 用模板拼接，不用大模型，快且稳
- **多档提醒**：日程编辑页支持最多 3 档提前量，存成 `remind_offsets` 数组（秒），对应后端 `events.remind_offsets`
