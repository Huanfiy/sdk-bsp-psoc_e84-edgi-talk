# USB Host UAC 麦克风开发笔记（Edgi_Talk_M33_USB_H）

> 记录在 `Edgi_Talk_M33_USB_H` 工程里给 CherryUSB 1.6.0 补齐 DWC2 Host ISO IN 支持、
> 驱动 Generic Newmine UAC 1.0 麦克风（VID/PID 3769:b01d）过程中的关键问题与决策。
> 适用范围：PSoC Edge E84 CM33 NS 侧 + CherryUSB 1.6.0 + DWC2 host glue infineon。

---

## 1. 背景与目标

- 目标设备：Full-speed UAC 1.0 麦克风，48 kHz / 16-bit / 立体声，ISO IN @ EP 0x83，
  `wMaxPacketSize = 208 B`，`bInterval = 1 ms`。
- 描述符特征：**没有 IAD**，3 个 interface 直接罗列（AC、AS、HID）。
- demo 目标：log-only，msh 命令打印速率统计、PCM hexdump 佐证数据真实流入。

最终实测稳态：`urbs +125/s`、`frames +1000/s`、`delta 192000 B/s`、`errors 0` —
完全吻合 `48000 × 2ch × 2B = 192000 B/s` 的理论值。

---

## 2. 两大隐蔽阻塞点（官方代码既有问题）

### 2.1 CherryUSB DWC2 Host 完全不实现 Isochronous

`libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c` 在上游 **v1.6.0 和 master**
中都有以下"占位"：

- `dwc2_iso_urb_init()` 整个函数被 `#if 0 … #endif` 禁用。
- `usbh_submit_urb()` 里 `case USB_ENDPOINT_TYPE_ISOCHRONOUS: break;` 空实现。
- `dwc2_inchan_irq_handler()` / `dwc2_outchan_irq_handler()` 的 ISO 分支同样空。

所以**仅启用 `RT_CHERRYUSB_HOST_AUDIO` 是不够的**——即便 class 驱动接管了接口，
端点层面根本没有任何 transfer 发生，URB 永远等不到完成。

### 2.2 `usbh_audio.c` 强依赖 IAD

`class/audio/usbh_audio.c::usbh_audio_ctrl_connect()` 用 `cur_iface_count` 判定
哪些接口属于本 audio function，仅在 `USB_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION`
分支里赋值。而 **UAC 1.0 规范并不要求 IAD**（IAD 是 USB 2.0 ECN 针对 composite
多 function 才引入的）；本麦克风就没有 IAD。原代码路径：

```c
uint8_t cur_iface_count = 0;  // 初始 0
…
if (cur_iface_count == 0xff) {  // 永远不会成立（bug）
    USB_LOG_ERR("Audio descriptor must have iad descriptor");
    return -USB_ERR_INVAL;
}
```

在 `cur_iface_count == 0` 时，循环里 `cur_iface < ctrl_intf + 0` 让所有 stream
interface 都被拒绝，静默失败，表面看 class 注册成功但后续开流时找不到 "mic"。

---

## 3. 解决思路要点

### 3.1 DWC2 Host ISO IN 实现关键点

参考 `dwc2_bulk_intr_urb_init` 的模板，每次「arm 一个 iso_frame_packet」：

- `dwc2_chan_init()` 的默认 `HCINTMSK = CHHM` 足够，不必专门打开 FRMOR/DTERR，
  因为 Channel Halt 触发后再读 `HCINT` 能拿到所有 error 位（和 bulk/intr 一致）。
- `dwc2_chan_transfer()` 里已经自动根据 `HFNUM` 写 `HCCHAR.ODDFRM`，ISO 也直接复用。
- ISO IN 不分 short packet / full packet：硬件用 `HCTSIZ.XFRSIZ` 减剩余字节，
  `count = chan->xferlen - HCTSIZ.XFRSIZ` 得到真实接收字节（PSE84 行为与 ST/HPM 一致）。
- **错误分支（FRMOR / DTERR / BBERR / AHBERR / TXERR）**：对非 ISO 端点保持原逻辑直接
  `dwc2_urb_waitup`；对 ISO 改为"本帧标 errorcode，推进到下一帧"——引入
  `dwc2_iso_advance_or_finish()` 把"继续下一帧 OR 完成 URB"两条路径收敛到一处。
- **`dwc2_urb_waitup` 是 `static inline` 且定义在我们 patch 的下方**，直接引用会
  implicit declaration；加一行前向声明解决。

对应提交见 `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c`
里 `dwc2_iso_urb_init` + `dwc2_iso_advance_or_finish` 两个函数和 4 处 IRQ 分支。

### 3.2 UAC class 无 IAD 兼容

`cur_iface_count` 初值改 `0xff`（真哨兵），循环末尾如仍是 `0xff`
则用 `hport->config.config_desc.bNumInterfaces - ctrl_intf` 回填：

```c
uint8_t cur_iface_count = 0xff;
…
if (cur_iface_count == 0xff) {
    uint8_t total_intf = hport->config.config_desc.bNumInterfaces;
    cur_iface_count = (total_intf > intf) ? (total_intf - intf) : 1;
    USB_LOG_INFO("Audio device has no IAD, fallback to %u interfaces…\r\n", …);
}
```

这样有 IAD 的复合设备零影响；无 IAD 的单 audio function 设备（带 HID 控制盘
也算）能正确把后续所有 AS 接口都吸收进来。

---

## 4. 踩坑清单（按发现顺序）

### 坑 1：`#if 0` 关掉的不只是函数，还有 `break` 分支

`#if 0` 只圈住了 `dwc2_iso_urb_init` 本体，`usbh_submit_urb` 的 ISO `case`
是正常编译的**空 break**，运行时 silently drop URB。排查时容易误以为
"有 `dwc2_iso_urb_init` 就能跑"。实际 4 处都要改。

### 坑 2：同一 IN endpoint 不能并发多 URB

最初 demo 设计了 2 个 URB 双缓冲，想减小 callback → resubmit 之间的 gap。
实测 `mic_sample` 显示：

```
[mic] urb0 packet lengths: [0]=192 [1]=192 [2]=192 [3]=192
[mic] urb1 packet lengths: [0]=0   [1]=0   [2]=0   [3]=0    ← 从没被调度
```

**DWC2 host 对每个 device endpoint 只绑定一个 host channel**；两个 URB 同时指向
`audio_class->isoin`（0x83）时，第二个 URB 占了 channel 但永远抢不到端点，
URB0 一个人来回跑，带宽正好是 1/N（N = URB 数）。

修复：`MIC_ISO_URB_COUNT = 1`，`MIC_ISO_PACKETS_PER_URB = 8`。
单个 URB 跨 8 个 SOF（8 ms）完成，callback 里立刻 resubmit，硬件几乎无 gap。

### 坑 3：`iso_packet[]` 是柔性数组，URB 存储必须"包一层"

`struct usbh_urb` 末尾是 `struct usbh_iso_frame_packet iso_packet[0]`（GCC 柔性数组）。
静态分配 URB 时要预留 `N * sizeof(struct usbh_iso_frame_packet)` 空间：

```c
#define MIC_URB_STORAGE_BYTES (sizeof(struct usbh_urb) + N * sizeof(struct usbh_iso_frame_packet))
static USB_MEM_ALIGNX uint8_t s_urb_storage[1][MIC_URB_STORAGE_BYTES];
#define MIC_URB(i) ((struct usbh_urb *)(void *)s_urb_storage[i])
```

**注意对齐**：`USB_MEM_ALIGNX`（= `CONFIG_USB_ALIGN_SIZE`）确保满足 DMA 对齐；
URB 自身不需要进 nocache section（控制器不 DMA 写 URB 结构），只有
`iso_packet[].transfer_buffer` 指向的 PCM buffer 必须放 `USB_NOCACHE_RAM_SECTION`
（在本 BSP 上是 `.cy_socmem_data`）。

另外避免写出 `urb->iso_packet == NULL`——柔性数组取地址恒为非 NULL，GCC
会给 `-Waddress` 死代码警告，只保留 `urb->num_of_iso_packets == 0` 判空就行。

### 坑 4：callback 用 `urb->actual_length` 比遍历 `iso_packet[]` 更稳

最初 demo 在 completion callback 里：

```c
for (i = 0; i < urb->num_of_iso_packets; i++)
    s_total_bytes += urb->iso_packet[i].actual_length;
```

而驱动 IRQ 在每次 XFRC 都做 `urb->iso_packet[idx].actual_length = count`，
这两者存在细微读时机问题（callback 是在 IRQ 上下文被调起的，但紧接着的
resubmit 会继续产生 XFRC）。实测字节数计到一半，但 `mic_sample` 显示
`iso_packet[].actual_length` 是 192 — 明显读的不是同一瞬间。

修复：callback 直接用 `nbytes`（= `urb->actual_length`），这个值已经在
通用 XFRC 路径 `urb->actual_length += count` 累加完毕，做 URB 级统计最可靠。
`iso_packet[].actual_length` 继续保留给需要 per-frame 信息的用户（例如做
jitter 分析或按 frame 写 I2S）。

### 坑 5：错误分支如果无脑走 `dwc2_urb_waitup`，ISO 会被单点故障中止

ISO 的语义是"允许偶发丢包，不该让整个 URB 报错"。原 IRQ 里
`FRMOR / DTERR / BBERR / AHBERR / TXERR` 都直接 `dwc2_urb_waitup`，对 control/bulk/intr
合理，对 ISO 会导致一次偶发 DTERR 就让上层重启整个 URB，抖动极大。
统一到 `dwc2_iso_advance_or_finish` 后：当前帧标 errorcode、其他帧继续跑；
URB 完成时 `urb->errorcode = 0`（per-packet errorcode 被上层消费）。

### 坑 6：前向声明与函数顺序

`dwc2_iso_advance_or_finish()` 放在了 `dwc2_urb_waitup()` 之前。
C 默认向前声明只到同一 TU 前面的定义，所以必须：

```c
static inline void dwc2_urb_waitup(struct usbh_urb *urb);  // 前向声明
```

否则会 `error: static declaration of 'dwc2_urb_waitup' follows non-static declaration`。

---

## 5. 架构与参数速查

### 5.1 数据流

```
USB Mic (ISO EP 0x83, 208B/1ms)
    │  DWC2 host channel (periodic, IN, MC=1)
    ▼
HCDMA → PCM buffer (.cy_socmem_data)
    │  XFRC IRQ per frame
    ▼
dwc2_inchan_irq_handler ─┬─> iso_packet[idx].actual_length = count
                         └─> iso_frame_idx++, 推下一帧或 urb_waitup
    │  complete(arg, urb->actual_length) at URB 尾
    ▼
mic_iso_complete (demo)
    │  s_total_bytes += nbytes
    └─> usbh_submit_urb(urb)  ← 立即在 IRQ 里 resubmit
```

### 5.2 关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| `MIC_PACKET_SIZE` | 208 | `wMaxPacketSize`，预留上限（实际每帧 192） |
| `MIC_ISO_PACKETS_PER_URB` | 8 | 8 ms/URB，足够掩盖 IRQ→resubmit 延迟 |
| `MIC_ISO_URB_COUNT` | **1** | DWC2 单 channel，不要双缓冲 |
| worker 栈 | 4 KB | prio 15；worker 只负责 open + 启动，后续全 IRQ 驱动 |
| PCM buffer 位置 | `.cy_socmem_data` | DMA 可达、跨 Core 共享、CM55 无 cache 问题 |

### 5.3 稳态验收标准

- `urbs` 每秒 `+125`（`1000 fps / 8 packets/urb`）
- `frames` 每秒 `+1000`（1 ms 一帧）
- `delta` 每秒 `+192000`（`48k×2×2`）
- `errors == 0`

---

## 6. 未在本次处理的范围（后续扩展提示）

- **HS 模式 ISO**：本次仅在 FS 下验证。HS 的 ISO 可能需要处理 MultiCount(MC) > 1
  和 8 microframe/ms 的情况；`USB_GET_MULT(ep->wMaxPacketSize)` 已透传，但
  `dwc2_iso_urb_init` 的 `num_packets = 1` 逻辑要在 HS 下重新推导。
- **ISO OUT（扬声器播放）**：`dwc2_outchan_irq_handler` 的 ISO 分支本次已补上
  "推进下一帧"骨架，但没跑过数据；启用前要核对 periodic TX FIFO 大小。
- **Descriptor DMA 模式**：Infineon glue 现在用 `host_dma_desc_enable = false`
  (Buffer DMA)。切换到 Descriptor DMA 能让硬件一次调度多个 iso_frame，软件
  开销更低，但 `dwc2_chan_transfer` 和 IRQ 都要重写一遍。
- **音频数据落地**：当前 demo 仅做 log，不做 I2S/codec 回放、IPC 上 CM55、
  或写文件系统。真实产品链路里至少要把 `complete` 回调里的 PCM 推到 ring
  buffer 并唤醒消费者线程。
- **音量 / 静音**：`usbh_audio_set_volume / set_mute` 已随 class 一起编入，
  未做对接。本麦克风描述符有 Feature Unit 支持 `Mute Control` 和 `Volume Control`，
  想做时调 API 即可，不涉及底层补丁。

---

## 7. 文件映射

| 文件 | 作用 |
|---|---|
| `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c` | DWC2 Host ISO IN 补丁 |
| `libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c` | 无 IAD UAC 兼容 |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/rtconfig.h` | `RT_CHERRYUSB_HOST_AUDIO` |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/.config` | 同步 Kconfig 状态 |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c` | `usbh_audio_run/stop` 实现 + `mic_stat/mic_sample` msh 命令 |
| `.cursor/plans/uac_mic_host_support_a6e3ddab.plan.md` | 实施计划 |
