# UAC 麦克风首次拔出卡死排障记录（2026-04）

## 现象

工程：`projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`

异常只在下面这个路径稳定复现：

1. 冷启动
2. 首次插入 USB UAC 麦克风
3. 拔出麦克风

表现为：

- 板上 LED 停止闪烁
- 串口终端无响应
- 没有正常打印 `Device on Bus ... disconnected`

但如果先插拔一次 U 盘，再插拔麦克风，系统又能正常走完 disconnect。

## 排查结论

通过逐层加日志，最终确认：

1. `DWC2` 端口中断和断开中断都能正常进入。
2. root hub 事件能成功投递到 `hub` 线程。
3. `roothub GET_STATUS` 在异常现场已经返回稳定断开状态：
   - `wPortStatus` 只有 `POWER`
   - `wPortChange` 已经清零
4. 系统卡死发生在 `usbh_hub.c` 的 **root hub disconnect debounce** 窗口内，而不是发生在：
   - `DWC2` 端口中断识别前
   - `hub` 线程唤醒前
   - `usbh_hubport_release()`
   - `usbh_audio_stop()`

也就是说，问题不是“拔出没识别到”，而是：

**root hub 在已经确认“端口稳定断开、无后续 change”的情况下，仍继续执行 25 ms 一轮的 debounce 延时；对活跃的 UAC ISO IN 流来说，这个延迟窗口会把系统拖进异常。**

## 最终保留的修复

### 1. root hub 稳定断开 fast-path

文件：

- `libraries/components/CherryUSB-1.6.0/class/hub/usbh_hub.c`

修复点：

- 在 `C_CONNECTION` 路径里，如果当前是 `roothub`
- 并且清掉 `C_CONNECTION / C_ENABLE` 后重新读取端口状态，发现：
  - `CONNECTION == 0`
  - `C_CONNECTION == 0`
- 则直接执行 `usbh_hubport_release(child)`，跳过后面的 debounce sleep

这个修改只影响 **root hub 已经稳定断开** 的情况，不影响：

- 外部 hub 的常规 debounce
- 连接时的 debounce
- reset/enumeration 流程

### 2. USB IRQ 包装接入 RT-Thread 中断上下文

文件：

- `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_glue_infineon.c`

修复点：

- 在 `USBHS_HOST_IRQHandler()` / `USBHS_DEVICE_IRQHandler()` 外层补：
  - `rt_interrupt_enter()`
  - `rt_interrupt_leave()`

这是 RT-Thread 集成上的正确性修复。因为 USB IRQ 中会触发：

- `rt_mq_send()`
- `rt_sem_release()`

如果不先进入 RT-Thread 的中断上下文，内核会把这些唤醒/调度动作当作普通线程上下文处理，存在调度语义风险。

### 3. UAC worker 停止握手补强

文件：

- `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c`

保留的加固点：

- `s_stop_req`
- `s_worker_exit`
- `worker_wait_abortable(100 ms)`
- `usbh_audio_stop()` 等待 worker 真正退出

这不是本次 root cause 的主因，但能避免 detach 时：

- worker 还没退出
- `audio_class` 已被上层释放

这种生命周期竞态。

### 4. DWC2 channel IRQ 空指针保护

文件：

- `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c`

保留的加固点：

- `dwc2_inchan_irq_handler()` / `dwc2_outchan_irq_handler()` 在 `chan->urb == NULL` 时直接清中断并返回

这是为了避免拔插竞态下 host channel IRQ 访问空 `urb` 指针。

## 为什么“先插一次 U 盘再测麦克风”容易正常

从日志看，U 盘路径没有触发这个 root hub disconnect 窗口里的异常；而麦克风路径因为存在活跃的 UAC ISO IN 流，更容易在“设备已断开但 hub 线程还在做 debounce”这个区间里踩中问题。

所以真正的差异不是“U 盘修好了系统”，而是：

- U 盘路径没把系统带进这个异常窗口
- 麦克风路径会

当 root hub disconnect fast-path 落地后，两条路径都恢复正常。

## 当前状态

当前版本实测：

- 冷启动后首次插入麦克风，再拔出：正常
- 先插拔 U 盘，再插拔麦克风：正常

因此可以认为该问题已经收敛并完成修复。
