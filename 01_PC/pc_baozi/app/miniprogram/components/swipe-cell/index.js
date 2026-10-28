Component({
  options: {
    multipleSlots: true
  },

  properties: {
    // 动作区总宽度（px）：两个按钮就填 152，一个按钮填 76
    actionWidth: { type: Number, value: 76 },
    disabled: { type: Boolean, value: false }
  },

  data: {
    // 动作区在右侧，内容层往**左**平移 offset px 把它露出来。
    // 这里只存"露出了多少"（恒为 0..actionWidth 的正数），
    // 正负号统一交给 wxml 的 translateX(-offset) —— 方向只在一个地方定义，不会两边对不上。
    offset: 0,
    animating: true   // 拖动中要关掉过渡，否则跟手感很差
  },

  methods: {
    onTouchStart(e) {
      if (this.data.disabled) return
      this._startX = e.touches[0].clientX
      this._startOffset = this.data.offset
      this._moved = false
      this.setData({ animating: false })
    },

    onTouchMove(e) {
      if (this.data.disabled || this._startX === undefined) return
      // 左滑 = 手指往左走 = clientX 变小 ⇒ 取反得到正位移
      const dx = this._startX - e.touches[0].clientX
      // 位移超过一点就认为"这是一次滑动"，页面据此忽略随后的误触
      if (Math.abs(dx) > 6) this._moved = true
      let offset = this._startOffset + dx
      if (offset < 0) offset = 0        // 右滑到底就停住，不要出现反向露出
      if (offset > this.data.actionWidth) offset = this.data.actionWidth
      this.setData({ offset })
    },

    onTouchEnd() {
      if (this._startX === undefined) return
      this._startX = undefined
      // 过半就吸附到展开，否则收回
      const open = this.data.offset > this.data.actionWidth / 2
      this.setData({ offset: open ? this.data.actionWidth : 0, animating: true })
    },

    // 页面在按钮点击后调它收起（不然点完"修改"回来还是敞着的）
    close() {
      this.setData({ offset: 0, animating: true })
    },

    // 页面判断"这次 tap 是不是滑动之后的误触"
    wasMoved() {
      return !!this._moved
    }
  }
})
