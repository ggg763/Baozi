"""目标检测（眼睛的"是什么、在哪里"）—— YOLOv8n，本地 CPU 推理。

为什么需要它（与 qwen3-vl 的分工）：
    qwen3-vl 描述画面是**生成式**的：慢（0.6~2s）、会幻觉、而且给不出位置坐标。
    YOLO 是**判别式**的：确定性、快（CPU 上 ~60~150ms）、能给出精确 bbox + 置信度。
    「你看到了什么」这类问题用 YOLO 又快又准；YOLO 什么都没检出时，
    再回落到 VL 做场景描述（见 vision.py 的 look()）。

为下一步「视觉跟踪」铺路：
    每个检测结果都算好了**归一化偏移** (dx, dy) ∈ [-0.5, 0.5]（相对画面中心）：
        dx > 0  = 目标在画面右侧  ->  TD1601 的 YAW 该向右转
        dy > 0  = 目标在画面下方  ->  PITCH 该低头
    现在只用来生成文字描述，不做舵机控制；做 B 档「跟随」时直接取 dx/dy 即可。

算力说明：本工程 venv 里是 torch CPU 版，所以 imgsz 默认 640（YOLOv8n 的官方面）；
    若嫌慢可调小到 320（速度约 4 倍，小目标精度下降）。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Sequence

# ---------------------------------------------------------------------
# COCO 80 类的中文名。只覆盖桌面机器人场景里真正会遇到的那些
# （人、宠物、常见物品、家具、食物）；没列到的退回英文原名，不影响使用。
_LABEL_CN = {
    "person": "人", "bicycle": "自行车", "car": "汽车", "motorcycle": "摩托车",
    "airplane": "飞机", "bus": "公交车", "train": "火车", "truck": "卡车",
    "boat": "船", "traffic light": "红绿灯", "fire hydrant": "消防栓",
    "stop sign": "停车标志", "bench": "长椅",
    "bird": "鸟", "cat": "猫", "dog": "狗", "horse": "马", "sheep": "羊",
    "cow": "牛", "elephant": "大象", "bear": "熊", "zebra": "斑马",
    "giraffe": "长颈鹿",
    "backpack": "背包", "umbrella": "雨伞", "handbag": "手提包", "tie": "领带",
    "suitcase": "行李箱", "frisbee": "飞盘", "skis": "滑雪板",
    "snowboard": "单板滑雪板", "sports ball": "球", "kite": "风筝",
    "baseball bat": "棒球棒", "baseball glove": "棒球手套", "skateboard": "滑板",
    "surfboard": "冲浪板", "tennis racket": "网球拍",
    "bottle": "瓶子", "wine glass": "酒杯", "cup": "杯子", "fork": "叉子",
    "knife": "刀", "spoon": "勺子", "bowl": "碗",
    "banana": "香蕉", "apple": "苹果", "sandwich": "三明治", "orange": "橙子",
    "broccoli": "西兰花", "carrot": "胡萝卜", "hot dog": "热狗", "pizza": "披萨",
    "donut": "甜甜圈", "cake": "蛋糕",
    "chair": "椅子", "couch": "沙发", "potted plant": "盆栽", "bed": "床",
    "dining table": "餐桌", "toilet": "马桶", "tv": "电视",
    "laptop": "笔记本电脑", "mouse": "鼠标", "remote": "遥控器",
    "keyboard": "键盘", "cell phone": "手机", "microwave": "微波炉",
    "oven": "烤箱", "toaster": "烤面包机", "sink": "水槽",
    "refrigerator": "冰箱", "book": "书", "clock": "时钟", "vase": "花瓶",
    "scissors": "剪刀", "teddy bear": "泰迪熊", "hair drier": "吹风机",
    "toothbrush": "牙刷",
}


def label_cn(name: str) -> str:
    return _LABEL_CN.get(name, name)


# ---------------------------------------------------------------------
@dataclass
class Detection:
    """一个检测框（含给"跟随"用的偏移量）。"""
    cls: str                     # 英文类名（COCO）
    label: str                   # 中文名
    conf: float                  # 置信度 0~1
    x1: float
    y1: float
    x2: float
    y2: float
    img_w: int
    img_h: int

    @property
    def cx(self) -> float:
        """中心点 x（像素）"""
        return (self.x1 + self.x2) / 2.0

    @property
    def cy(self) -> float:
        """中心点 y（像素）"""
        return (self.y1 + self.y2) / 2.0

    @property
    def dx(self) -> float:
        """归一化水平偏移 [-0.5, 0.5]。>0 = 偏右（YAW 该右转）。"""
        if not self.img_w:
            return 0.0
        return self.cx / self.img_w - 0.5

    @property
    def dy(self) -> float:
        """归一化垂直偏移 [-0.5, 0.5]。>0 = 偏下（PITCH 该低头）。"""
        if not self.img_h:
            return 0.0
        return self.cy / self.img_h - 0.5

    @property
    def area_ratio(self) -> float:
        """框面积占画面比例（粗略代表"离得多近"）。"""
        if not self.img_w or not self.img_h:
            return 0.0
        return ((self.x2 - self.x1) * (self.y2 - self.y1)) / (self.img_w * self.img_h)

    def position(self) -> str:
        """中文九宫格位置，如「左下角」「正中间」。"""
        if self.dx < -0.17:
            h = "左"
        elif self.dx > 0.17:
            h = "右"
        else:
            h = "中"
        if self.dy < -0.17:
            v = "上"
        elif self.dy > 0.17:
            v = "下"
        else:
            v = "中"

        if h == "中" and v == "中":
            return "正中间"
        if h == "中":
            return {"上": "中上方", "下": "中下方"}[v]
        if v == "中":
            return {"左": "左侧", "右": "右侧"}[h]
        return f"{h}{v}角"

    def distance_hint(self) -> str:
        """粗略远近（用框占画面的比例估）。"""
        a = self.area_ratio
        if a > 0.45:
            return "很近"
        if a > 0.15:
            return "较近"
        if a > 0.05:
            return "中等距离"
        return "较远"


# ---------------------------------------------------------------------
class YoloDetector:
    """YOLOv8n 目标检测（本地 CPU）。模型只在第一次 detect() 时加载。"""

    def __init__(
        self,
        model: str = "yolov8n.pt",
        conf: float = 0.25,
        imgsz: int = 640,
        max_dets: int = 12,
        log: Callable[[str], None] = print,
    ) -> None:
        self.model_name = model
        self.conf = float(conf)
        self.imgsz = int(imgsz)
        self.max_dets = int(max_dets)
        self.log = log
        self._model = None
        self.last_ms = 0.0

    def _ensure_model(self):
        if self._model is None:
            from ultralytics import YOLO  # 延迟导入：不用时不占启动时间
            t0 = time.time()
            self._model = YOLO(self.model_name)
            self.log(f"[det] YOLO 就绪（{self.model_name}，"
                     f"{time.time() - t0:.1f}s，imgsz={self.imgsz}）")
        return self._model

    # ------------------------------------------------------------------
    def detect(self, image_bytes: bytes) -> List[Detection]:
        """对 JPEG 字节做检测，返回按置信度排序的 Detection 列表。

        出错（模型坏了/图片坏了）返回空列表 —— 上层会回落到 VL 描述，
        不会因为检测失败就哑掉。
        """
        if not image_bytes:
            return []
        try:
            import numpy as np
            import cv2

            arr = np.frombuffer(image_bytes, dtype=np.uint8)
            bgr = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if bgr is None:
                self.log("[det] 图片解码失败")
                return []
            h, w = bgr.shape[:2]

            model = self._ensure_model()
            t0 = time.time()
            res = model(bgr, verbose=False, conf=self.conf, imgsz=self.imgsz)
            self.last_ms = (time.time() - t0) * 1000.0
        except Exception as exc:  # noqa: BLE001
            self.log(f"[det] YOLO 检测失败：{exc}")
            return []

        r = res[0] if res else None
        if r is None or r.boxes is None:
            return []

        dets: List[Detection] = []
        names = model.names
        for b in r.boxes:
            try:
                cls_name = str(names[int(b.cls)])
                x1, y1, x2, y2 = (float(v) for v in b.xyxy[0])
                dets.append(Detection(
                    cls=cls_name, label=label_cn(cls_name),
                    conf=float(b.conf),
                    x1=x1, y1=y1, x2=x2, y2=y2, img_w=w, img_h=h,
                ))
            except Exception:  # noqa: BLE001
                continue

        dets.sort(key=lambda d: d.conf, reverse=True)
        if len(dets) > self.max_dets:
            dets = dets[:self.max_dets]

        if dets:
            detail = ", ".join(f"{d.label}@{d.position()}/{d.conf:.2f}" for d in dets)
            self.log(f"[det] {len(dets)} 个目标（{self.last_ms:.0f}ms, "
                     f"{w}x{h}）: {detail}")
        else:
            self.log(f"[det] 没检出目标（{self.last_ms:.0f}ms, {w}x{h}）")
        return dets


# ---------------------------------------------------------------------
def describe_detections(dets: Sequence[Detection]) -> str:
    """把检测结果写成一句给 LLM 看的英文上下文（纯 ASCII，避免串口乱码）。

    ⚠️ 这里输出**英文**：与 vision.py 的 VISION_PROMPT 一致 —— 视觉链路
    全程 ASCII，避免编码问题。中文只出现在给用户看的日志里。

    例：
        "I can see 1 person (bottom-left, close) and 1 cup (center)."
    """
    if not dets:
        return ""

    # 按类别聚合数量
    counts: dict = {}
    for d in dets:
        counts[d.label] = counts.get(d.label, 0) + 1

    # 位置描述：每个类别取置信度最高的那个
    best: dict = {}
    for d in dets:
        if d.label not in best:
            best[d.label] = d

    parts = []
    for lbl, _n in sorted(counts.items(), key=lambda kv: -kv[1]):
        n = counts[lbl]
        d = best[lbl]
        pos_en = {
            "正中间": "center", "中上方": "center-top", "中下方": "center-bottom",
            "左侧": "left", "右侧": "right",
            "左上角": "top-left", "左下角": "bottom-left",
            "右上角": "top-right", "右下角": "bottom-right",
        }[d.position()]
        dist_en = {
            "很近": "very close", "较近": "close",
            "中等距离": "medium distance", "较远": "far",
        }[d.distance_hint()]
        name = lbl if lbl.isascii() else d.cls  # 英文上下文用英文类名
        parts.append(f"{n} {name} ({pos_en}, {dist_en})")

    if len(parts) == 1:
        return f"I can see {parts[0]}."
    return "I can see " + ", ".join(parts[:-1]) + " and " + parts[-1] + "."


def describe_detections_cn(dets: Sequence[Detection]) -> str:
    """中文版描述（只用于日志/调试，不进串口）。"""
    if not dets:
        return ""
    counts: dict = {}
    order: list = []
    for d in dets:
        if d.label not in counts:
            counts[d.label] = 0
            order.append(d.label)
        counts[d.label] += 1
    best: dict = {}
    for d in dets:
        if d.label not in best:
            best[d.label] = d
    items = []
    for lbl in order:
        n = counts[lbl]
        d = best[lbl]
        items.append(f"{n}个{lbl}（{d.position()}，{d.distance_hint()}）")
    return "画面里有" + "、".join(items)
