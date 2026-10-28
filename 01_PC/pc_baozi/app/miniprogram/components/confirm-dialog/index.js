// 通用确认弹窗：按钮行【左=确定 / 右=取消】
// 说明：系统 wx.showModal 的按钮顺序固定为 左取消/右确定，无法调换，
//       因此这里自绘一个，保证「确定」在左下、「取消」在右下。
Component({
  properties: {
    show: { type: Boolean, value: false },
    title: { type: String, value: '确定要删除吗？' },
    desc: { type: String, value: '' },
    confirmText: { type: String, value: '确定' },
    cancelText: { type: String, value: '取消' }
  },

  methods: {
    // 吞掉冒泡（点弹窗内部不触发遮罩的取消）
    noop() {},

    onConfirm() {
      this.triggerEvent('confirm')
    },

    onCancel() {
      this.triggerEvent('cancel')
    }
  }
})
