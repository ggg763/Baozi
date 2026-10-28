"""包子 App 服务（组员的「习惯 / 日程 / 提醒」那一套）—— 整合层。

放在这里的都是【组员 app 工程】的资产，整合后由 pc_baozi/main.py 统一拉起：

    app/backend/       FastAPI + SQLite 后端（手机小程序读写数据）
    app/miniprogram/   微信小程序源码（用微信开发者工具打开）
    app/link.py        屏幕桥接：后端数据 -> A5 帧 -> TD1601 UART0；
                       屏幕打卡回写；提醒到点弹卡 + 语音播报
    app/board.py       A5 帧组帧 / 中文点阵（原 baozi-uart-test.py）
    app/phrases.py     提醒话术池（原 remind_phrases.py）

⚠️ 原来那一套是两个独立进程 + 自己的 TCP 8765 语音服务端（baozi_voice.py）。
   整合后语音端口归 pc_baozi/voice_server.py 独占，提醒语音改走
   `pipeline.say()`（同一条 TTS -> ESP32 -> 喇叭 链路），不再起第二个服务端。
"""

__all__ = ["AppService"]


def __getattr__(name):
    """延迟导入 AppService，避免 import app 时就把 pyserial 等依赖拉进来。"""
    if name == "AppService":
        from .service import AppService

        return AppService
    raise AttributeError(name)
