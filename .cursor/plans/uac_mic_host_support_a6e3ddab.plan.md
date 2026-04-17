---
name: UAC Mic Host Support
overview: 在 Edgi_Talk_M33_USB_H demo 中实现 USB Host UAC 麦克风驱动，核心是给 CherryUSB 1.6.0 的 DWC2 Host 补齐 Isochronous IN 传输，并修复 UAC class 驱动对 IAD 的强依赖，最终通过 msh 命令周期性打印 PCM 数据流统计，证明音频数据真实流入。
todos:
  - id: patch_dwc2_iso
    content: 给 libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c 的 DWC2 Host 补齐 ISO IN 传输：启用 dwc2_iso_urb_init、在 usbh_submit_urb 的 ISO 分支调度首帧、在 inchan IRQ 里处理 XFRC/FRMOR/错误并调度下一帧。
    status: completed
  - id: fix_uac_no_iad
    content: 修改 libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c 的 usbh_audio_ctrl_connect，使其在没有 IAD 时以 bNumInterfaces-ctrl_intf 作为后备 cur_iface_count，把 cur_iface_count==0xff 错误分支改为触发后备。
    status: completed
  - id: enable_uac_host
    content: "在 projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/rtconfig.h 加 #define RT_CHERRYUSB_HOST_AUDIO，并同步 .config（若存在），让 SConscript 编入 class/audio/usbh_audio.c。"
    status: completed
  - id: impl_mic_demo
    content: 新建 projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c：实现 usbh_audio_run/stop、双缓冲 ISO IN URB 持续拉流、msh 命令 mic_stat 和 mic_sample 打印统计/采样。
    status: completed
  - id: build_verify
    content: scons 编译确认无 error/warning；插入麦克风观察 log 是否出现 Register Audio Class:/dev/audio0 以及稳态 ~190KB/s 的 stat 输出。
    status: completed
isProject: false
---

# UAC 麦克风 Host 支持（Edgi_Talk_M33_USB_H）

## 目标

Full-speed UAC 1.0 麦克风（VID/PID 3769:b01d，48kHz/16-bit/立体声 ISO IN 208B@1ms，无 IAD）成功枚举、识别、持续拉取音频数据，并通过 msh 命令查看数据流统计（log-only demo）。

## 现有阻塞点（必须解决）

1. **CherryUSB 1.6.0 DWC2 Host 无 ISO 实现**：[libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c](libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c) 中 `dwc2_iso_urb_init` 被 `#if 0` 禁用，`usbh_submit_urb()` ISO 分支空实现（第 1063-1064、1188、1351 行）。
2. **UAC class 驱动强依赖 IAD**：[libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c](libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c) 第 406/444/483 行依赖 IAD 去分辨 AC/AS 接口边界，该麦克风没有 IAD。
3. **未启用 UAC host class**：当前 rtconfig 只开了 MSC。

## 数据流

```mermaid
flowchart LR
    Mic[USB Mic ISO EP 0x83 208B/ms] --> DWC2[DWC2 Host Controller]
    DWC2 -->|"ISO IN URB complete"| IsoCb["dwc2 iso irq handler (新增)"]
    IsoCb -->|"usbh_iso_frame_packet"| AudioDrv[usbh_audio.c class driver]
    AudioDrv --> UacMic["usbh_audio_run (demo)"]
    UacMic -->|"ring buffer stats"| MshCmd["msh mic_stat 命令打印"]
```

## 任务分解

### 1. patch_dwc2_iso: 给 DWC2 Host 补 ISO IN 传输

在 [libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c](libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c) 做如下修改：

- **启用并完善 `dwc2_iso_urb_init()`**：去掉 `#if 0`。按单 iso_packet 初始化 channel（EPTYP=ISO, EPDIR=IN, MC=1, MPS=208, PID=DATA0）。额外处理 HCCHAR.ODDFRM：根据当前 `usbh_get_frame_number()` 的下一帧奇偶性设置。
- **`dwc2_chan_init()` 中追加 ISO 情况下的 HCINTMSK**：除了 XFRC/CHH，额外加 FRMOR（帧溢出）、DTERR（DataToggle 错误）、BBERR（babble）监听。
- **`usbh_submit_urb()` ISO 分支**（第 1063 行）：`chan->iso_frame_idx = 0; dwc2_iso_urb_init(bus, chidx, urb, &urb->iso_packet[0]);`。注意：对 ISO 端点，FIFO overflow 检查要用 `host_perio_tx_fifo_size` 路径，但 IN 方向不占 TX FIFO，这里的 check 可以保留不影响功能。
- **`dwc2_inchan_irq_handler()` ISO 分支**（第 1188 行）：
  - XFRC：从 HCTSIZ.XFRSIZ 计算本帧实际收到字节数，写入 `urb->iso_packet[iso_frame_idx]`，并 `iso_frame_idx++`。若还有下一帧，调用 `dwc2_iso_urb_init()` 启动下一帧；否则 `dwc2_urb_waitup(urb)` 完成回调。
  - FRMOR：标记本帧 `errorcode = -USB_ERR_NAK`、`actual_length = 0`，继续下一帧（ISO 不应当中止整个 urb）。
  - DTERR/BBERR/AHBERR：同上，标记错误但不中止。
- **`dwc2_outchan_irq_handler()` ISO 分支**（第 1351 行）：本次需求只要 IN，先留空但保留分支结构一致，便于后续扩展 OUT。
- **Buffer DMA 注意**：Infineon glue 使用 Buffer DMA（`host_dma_desc_enable = false`），`dwc2_chan_transfer()` 已会写 HCDMA，ISO 也走同一路径，无需特殊处理。
- **Cache**：CM33 NS 无 D-Cache（`CONFIG_USB_DCACHE_ENABLE` 未定义），默认 `usb_dcache_*` 无操作；ISO 接收 buffer 放进 `USB_NOCACHE_RAM_SECTION`（`.cy_socmem_data`）保守处理，和现有做法一致。

预估 ~200 行净增代码。

### 2. fix_uac_no_iad: 让 UAC class 兼容无 IAD 设备

在 [libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c](libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c) 的 `usbh_audio_ctrl_connect()` 中：

- 把 `cur_iface_count = 0` 初始化为一个「哨兵值」，当循环里始终没遇到 IAD 时，以 `hport->config.config_desc.bNumInterfaces - ctrl_intf` 作为后备计算所有 AS 接口都算进本 audio function。
- 把第 483 行 `if (cur_iface_count == 0xff)` 改为 `if (cur_iface_count == 0)` 触发时也走后备路径而不是直接报错。
- 保留原 IAD 路径完全不变，对有 IAD 的 composite device 零影响。

### 3. enable_uac_host: 项目配置启用 UAC host

- [projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/rtconfig.h](projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/rtconfig.h) 加 `#define RT_CHERRYUSB_HOST_AUDIO`，使 [SConscript](libraries/components/CherryUSB-1.6.0/SConscript) 编入 `class/audio/usbh_audio.c`。
- [libraries/Common/board/ports/usb/usb_config.h](libraries/Common/board/ports/usb/usb_config.h) 中 `CONFIG_USBHOST_MAX_AUDIO_CLASS = 1` 已足够，不改。
- **HID 不启用**（log 里那一行 HID `Do not support` 无害，不影响 Audio）。
- 同步 .config 里 `CONFIG_RT_CHERRYUSB_HOST_AUDIO=y`（用 Kconfig 扫描时保证一致）。

### 4. impl_mic_demo: UAC 麦克风拉流 demo

新建 [projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c](projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c)：

- 实现 CherryUSB 的 `__WEAK void usbh_audio_run(struct usbh_audio *audio_class)`：创建 `mic_worker` 线程（栈 4KB，prio 15）。
- 线程行为：
  - `usbh_audio_open(audio_class, "mic", 48000, 16)` 切 altsetting 1。
  - 分配 2 个 URB（双缓冲），每个 URB 含 4 个 `usbh_iso_frame_packet`（4ms 合 1 次 = 832B），buffer 放 `USB_NOCACHE_RAM_SECTION`。
  - 填 `urb->ep`（audio_class->isoin）、`num_of_iso_packets=4`、每帧 `transfer_buffer_length=208`、`complete` 回调、`timeout=0`（异步）。
  - 初始提交两个 URB。`complete` 回调里：累加统计（total_bytes、frame_count），立刻再次 `usbh_submit_urb()` 保持流不断。
- 实现 `usbh_audio_stop()`：kill urb → close stream → 线程退出。
- msh 命令：
  - `mic_stat` 打印最近 1 秒字节数、累计总字节、丢帧数。
  - `mic_sample` 打印当前 URB 头 16 字节（hexdump）看是否为真实 PCM。
- 期望稳态：48000 × 2 × 2 = 192 000 Byte/s ≈ 187.5 KB/s，每 URB 4 帧应收到 4 × 200~208 byte。

### 5. build_verify: 编译 + 简要验证

- 执行 `scons -j` 确认 0 error、0 warning（相关代码）。
- 接入麦克风看串口 log 应出现：
  - `[I/usbh_audio] Open audio stream :mic, altsetting: 1`
  - `[I/usbh_audio] Register Audio Class:/dev/audio0`
  - 持续的周期性 stat 打印（约 ~190KB/s）。

## 不做的事

- **HID 接口驱动**：按需求跳过；log 里 `Do not support Class:0x03` 可以忽略。
- **High-Speed split / HS hub 中转 FS 设备**：这个板子用根 hub 直接插入 FS 麦克风，不需要 split；split 相关代码保持现状。
- **UAC OUT（扬声器）、音量/静音控制**：不在需求内。
- **Descriptor DMA 模式**：Infineon glue 用的是 Buffer DMA，本次不切换。
- **I2S/Codec 回放链路**：按选择，仅 log 打印 demo。

## 主要风险

- **Frame overrun**：FS 每 1ms 一帧，回调里提交下一个 URB 必须足够快。用双缓冲 + 每 URB 含 4 帧 = 4ms 余量；若实测仍丢帧，可增到 8 帧/URB。
- **ODDFRM 奇偶错**：DWC2 host ISO 对 frame 奇偶敏感，每次 `dwc2_iso_urb_init()` 必须读当前 frame 号再设置 ODDFRM，否则会 FRMOR。
- **PSE84 DWC2 IP 版本差异**：若 `dwc2_hcd.hw_params.snpsid` 的 feature 集和 ST/HPM 不同，`HCCHAR_MC`/`HCINTMSK_FRMOR` 位可能需要细调；对照 [libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c](libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c) 中现有 bulk/intr 代码为模板，保守实现。
