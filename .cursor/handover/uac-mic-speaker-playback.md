# Handover: UAC 麦克风实时扩音（Edgi_Talk_M33_USB_H）

> 交接对象：下一个接手该需求的 AI / 工程师
> 最后更新：2026-04-18
> 工程：`projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`
> 硬件：PSoC Edge E84 (Cortex-M33) + 板载 ES8388 codec + 外接 USB UAC 1.0 麦克风（VID/PID `3769:b01d`，FS，48 kHz / 16-bit / stereo，ISO IN `0x83`, mps=208, bInterval=1 ms）

---

## 1. 最终需求

**实时播放 USB 麦克风的音频（当扩音器用）**：
USB MIC (UAC 1.0 ISO IN, 48 kHz / 16-bit / stereo)
→ CherryUSB Host (DWC2, ISO IN) ← **已打通**
→ 应用层缓冲 ← **待实现**
→ RT-Thread audio framework (`sound0`) ← **待启用**
→ I2S TDM0 + ES8388 DAC ← **驱动已存在，需引入**
→ 板载功放 → 喇叭

参考实现：`projects/Edgi_Talk_M33_Audio/applications/main.c`（它跑的是板载 PDM `mic0` → `sound0` 回环，不是 USB 麦克风）。

---

## 2. 当前已完成的里程碑（上一阶段）

**USB 采集链路已稳定跑通**，`mic_stat` 输出已经显示期望速率：

```
[mic] urbs=3142 frames=25136 bytes_total=4825440 delta=192000 B errors=0
```

`delta = 192000 B/s = 48000 samples × 2 ch × 2 B` 完全符合 UAC 1.0 标称值，`errors=0`。

### 2.1 关键代码改动（已在 master 上）

| 提交 | 文件 | 作用 |
|---|---|---|
| `46973c9c feat: support uac macrophone` | `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c` | 给 DWC2 Host 补齐 ISO IN：实现 `dwc2_iso_urb_init` + `dwc2_iso_advance_or_finish` + 在 IRQ handler 里按 ISO 规则推进/错误吞咽 |
| `46973c9c` | `libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c` | 兼容无 IAD 设备：`cur_iface_count=0xff` 时按 `bNumInterfaces - ctrl_intf` 回退 |
| `46973c9c` | `projects/.../rtconfig.h` + `.config` | 开启 `RT_CHERRYUSB_HOST_AUDIO` |
| `46973c9c` | `projects/.../applications/usbh_uac_mic.c` | 新建；实现 `usbh_audio_run/stop` 弱符号 + `mic_stat/mic_sample` msh 命令 |
| `4f9102bf fix(uac-mic): ...` | `usbh_uac_mic.c` | 单 URB（8 packet × 208 B）+ 回调直接用 `urb->actual_length` 累加 |
| `950eaf20 docs(uac-mic): ...` | `.cursor/docs/usb-host-uac-mic-notes.md` | 详细开发笔记（**务必先读！**） |

### 2.2 当前 `usbh_uac_mic.c` 关键状态

- 数据缓冲：`s_iso_buf[1][8 * 208]`，放在 `USB_NOCACHE_RAM_SECTION`（`.cy_socmem_data`），DMA 安全。
- URB 回调 `mic_iso_complete()` 在 **DWC2 IRQ 上下文**执行，目前只做统计 + 立即 `usbh_submit_urb()` 重投递，**没有把 PCM 向外传递**。
- 每个 iso_packet 的 `actual_length` 实测稳定 =192 B（注意：`wMaxPacketSize=208` 是协议预留，实际音频载荷是 `48 samples × 2 ch × 2 B = 192 B`；尾部 16 B 是 0 填充/无效）。
- 配套命令：`mic_stat`（统计）、`mic_sample`（dump iso_packet 长度 + buffer 头/中/尾字节），用于诊断。

---

## 3. 下一阶段要做的事：接播放通路

### 3.1 参考 demo 的播放链路（`Edgi_Talk_M33_Audio`）

```
rt_device_write(sound0, buf, n)
  → drv_i2s.c :: sound_transmit()              -- writeBuf 被包装成 i2s_playback_q_data 发 mq
  → i2s_playback_task()                        -- 线程从 mq 取出
  → convert_mono_to_stereo() [mono → L=R]      -- 注意: demo 默认输入是 mono
  → TDM0 TX + ES8388 DAC
```

注册入口：`libraries/HAL_Drivers/drv_i2s.c` → `rt_hw_sound_init()` → `rt_audio_register(&snd_dev.audio, "sound0", RT_DEVICE_FLAG_WRONLY, ...)`（`INIT_DEVICE_EXPORT`）。

ES8388 上电 + I2S init 发生在 `sound_init()`（`rt_device_open(sound0, WRONLY)` 触发），调用 `es8388_init("i2c0", ...)` + `es8388_start(ES_MODE_DAC)` + `es8388_volume_set(80)` + `ifx_set_samplerate(...)`。

### 3.2 引入播放的步骤（建议实现顺序）

#### Step A — 编译并启用 `sound0`

1. 在 `projects/.../Kconfig` 或 menuconfig 里打开：
   - `CONFIG_RT_USING_AUDIO=y`（RT-Thread audio framework）
   - `CONFIG_BSP_USING_AUDIO=y`
   - `CONFIG_BSP_USING_AUDIO_PLAY=y`（`BSP_USING_AUDIO_RECORD` 不需要，咱们不复用板载 PDM）
   - 对应 `RT_USING_I2C` + `BSP_USING_HW_I2C0`（ES8388 通过 i2c0 控制）
2. 确认 `libraries/HAL_Drivers/SConscript` 会把 `drv_i2s.c` / `drv_pdm.c` 编入（`drv_pdm.c` 无法剥离，只编 drv_i2s 需改 SConscript；先可带着 pdm）。
3. 确认 `libraries/Common/board/SConscript` 在 `BSP_USING_AUDIO_PLAY` 下把 `ports/audio/drv_es8388.c` 编入。
4. **硬件电源**：从 `projects/Edgi_Talk_M33_Audio/board/board.c` 复制 `en_gpio()`（`INIT_BOARD_EXPORT`）到 `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/board/board.c`，打开 `ES8388_CTRL (P16.2)` / `SPEAKER_OE_CTRL (P21.6)`。不做这一步声音链路是不通电的。

#### Step B — 解决采样率 / 声道不匹配

`libraries/HAL_Drivers/drv_i2s.h` 里 `SAMPLING_RATE` 是**编译时宏**，默认 16 kHz：

```c
#define SAMPLING_RATE  (SAMPLING_RATE_16kHz)   // 16000
```

UAC MIC 送来的是 48 kHz stereo，有两条路：

- **方案 A（推荐，改动最小）**：把 `SAMPLING_RATE` 改成 `SAMPLING_RATE_48kHz`，让 I2S/ES8388 直接跑 48 k。`FRAME_SIZE` 自动变成 480，`PLAYBACK_DATA_FRAME_SIZE=960 int16_t`。
  - 风险：AEC 参考链（`AEC_REF_SAMPLING_RATE=16 k`）会走 down-sampling 分支，里面有 `init_IFX_asrc(&asrc_mem_down_sampling, 48000, 16000)`，不会出错但占 CPU；本需求不用 AEC，可以在 `i2s_playback_task` 里用宏关掉 AEC 分支，或者先不管。
- **方案 B（更通用）**：保留 16 k，在应用层对 USB 进来的 48 k stereo PCM 做 down-sample。可以直接复用 demo 里的 `IFX_asrc`（下采样 48k→16k + stereo→mono 混）。麻烦点：需要在 USB_H 工程里把 `IFX_asrc`（位于 `libraries/.../IFX_asrc.*`）编进来。

**声道**：`drv_i2s.c :: i2s_playback_task` 调用 `convert_mono_to_stereo((int16_t*)q.data, q.data_len, ...)`，也就是说**当前 sound0 接口是 mono**。麦克风过来是 stereo (L R L R ...)，有两条路：
- 下采样时顺便做 `L = (L+R)/2` 得到 mono，然后走 demo 现成的 mono→stereo 路径；
- 或 patch `drv_i2s.c` 让 `sound_transmit` 支持 stereo 直通（看 `audio_config.channels`，=2 时跳过 `convert_mono_to_stereo` 直接 `memcpy`）。

#### Step C — 把 USB 数据送到 `sound0`

**不要**在 `mic_iso_complete` 里直接 `rt_device_write(sound0, ...)`，那是 IRQ 上下文，`rt_device_write` 最终会走 `rt_mq_send` 通常可以但整条链里的 `rt_data_queue_push` 可能阻塞。推荐结构：

```
USB IRQ (dwc2) -> mic_iso_complete:
    memcpy(ringbuf, iso_packet.transfer_buffer, 192)
    rt_sem_release(pcm_ready_sem)
    usbh_submit_urb(urb)          // 重投递保持 ISO 不断流

pcm_pump_thread (新增, 优先级比 worker 略高):
    while (s_running) {
        rt_sem_take(pcm_ready_sem, TIMEOUT);
        while (ringbuf 至少有 sound0 一帧 (FRAME_SIZE*2*2 字节)):
            pull_one_frame(buf)
            [可选: 下采样 / stereo→mono]
            rt_device_write(sound0, 0, buf, len);
    }
```

选型提示：
- Ring buffer 用 `struct rt_ringbuffer` 即可（`rt_ringbuffer_create(N*KB, ...)`），容量建议 ≥ 40 ms PCM（= 48000 * 2 * 2 * 0.04 ≈ 7.7 KB）。
- 每 1 ms 一个 iso_packet 共 192 B；sound0 一帧在 48k 配置下需要 `FRAME_SIZE=480 sample × 2 ch × 2 B = 1920 B`，**正好 10 个 iso_packet**。可以积 10 个 packet 再下发，也可以直接每个 packet 都 push ringbuf。
- `sound_transmit` 返回 `size`，说明它立刻接收；但实际消耗速度受 I2S 中断节拍决定。如果消费慢于生产，ring buffer 会满 → 要么丢最旧数据、要么阻塞上游；首版建议**丢最旧**（扩音器场景宁可短时 glitch 不要累积延迟）。

#### Step D — 连通性验证

- 开机 → 插麦克风 → `ls device` 看到 `mic_uac` 线程；`rt_device_find("sound0")` 成功。
- 新增一个 msh 命令 `speaker_start` / `speaker_stop` 控制 pump 线程启停，便于和 `mic_stat` 一起 debug。
- 正确性指标：
  - 无杂音 / 无断流（`mic_stat` 仍然稳定 `delta=192000 B`、`errors=0`）；
  - ring buffer 使用率在中位数（打印 high-water mark）；
  - 延迟主观 < 200 ms（麦克风离嘴 20 cm 比对喇叭输出）。

---

## 4. 已知的坑 / 注意事项

1. **无 IAD 的 UAC 设备**：已在 `usbh_audio.c` 打过兼容补丁；换不同麦克风时留意是否引入新的 descriptor 变体。参见 `.cursor/docs/usb-host-uac-mic-notes.md` §3.2。
2. **DWC2 一端点一 Channel**：ISO IN 同一 endpoint 只能有一条活跃 URB。**不要在应用层同时 submit 多个 URB 给同一个 endpoint**，DWC2 会只跑第一个，其余统计值会是 0（曾经踩过：原本双 URB 结果 delta 只有一半）。同一个 URB 内的多 packet 没问题。
3. **IRQ 回调里 `usbh_submit_urb` 是安全的**（只用 critical section + 寄存器写），但别在回调里 `rt_mq_send` 到没有等待线程的 mq；要么用 `rt_sem_release` + 专门的消费线程，要么用 `rt_ringbuffer_put`。
4. **Cache / DMA 一致性**：所有给 DWC2 用的 buffer 必须放在 `.cy_socmem_data`（`USB_NOCACHE_RAM_SECTION` 宏）。给 ES8388/TDM 用的 `i2s_stereo_playback_buffer*` 也是同样的 section，复制过去没问题。ring buffer 可以在 SRAM（CPU 只读写），只在发送前把数据 `memcpy` 进 `.cy_socmem_data` 的 I2S buffer。
5. **ES8388 I2C**：绑定在 `i2c0`（见 `sound_init`）；USB_H 项目里目前没开启 I2C0，需要同步打开。`I2C_ADDRESS=0x18`，`I2C_FREQUENCY=400 kHz`。
6. **电源/PA 时序**：`es8388_pa_power(true)` 在 `es8388_start(ES_MODE_DAC)` 里会调用。开机顺序建议：先 `en_gpio()` 拉高 3V3 + SPEAKER_OE + ES8388_CTRL → USB 初始化 → 枚举 → `rt_device_open(sound0)` 触发 I2S+codec 初始化 → pump 线程启动。
7. **采样率切换时机**：不要在 `sound0` 已经在跑的时候改 `audio_config.samplerate`；demo 里是在 `main` 一次性 `AUDIO_CTL_CONFIGURE` 后 `rt_device_open`。
8. **Build**：必须确保 `.config` 和 `rtconfig.h` 一致。`.config` 由 `scons --menuconfig` 生成 `rtconfig.h`；可以手工同步但容易漏改（上一阶段 `RT_CHERRYUSB_HOST_AUDIO` 就是两边都手动加的）。
9. **烧录脚本**：`projects/.../Edgi_Talk_M33_USB_H/run.sh`（git untracked）是用于 Linux 下 build + flash 的辅助脚本，可以直接用。

---

## 5. 关键文件速查

| 路径 | 说明 |
|---|---|
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/main.c` | USB Host 入口，`usbh_initialize(0, ...)` |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c` | **本阶段要扩展的核心文件**（目前只做统计，下一步接 ring buffer + pump） |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/board/board.c` | 需要追加 `en_gpio()` + ES8388/SPEAKER 上电 |
| `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/rtconfig.h` / `.config` | 打开 `RT_USING_AUDIO` / `BSP_USING_AUDIO_PLAY` / `RT_USING_I2C` |
| `projects/Edgi_Talk_M33_Audio/applications/main.c` | 参考：`sound0`/`mic0` 回环 |
| `projects/Edgi_Talk_M33_Audio/board/board.c` | 参考：`en_gpio()` 电源控制 |
| `libraries/HAL_Drivers/drv_i2s.c` | `sound0` 注册 + I2S TDM0 TX + `i2s_playback_task`（若要 stereo 直通/改采样率需在此 patch） |
| `libraries/HAL_Drivers/drv_i2s.h` | `SAMPLING_RATE` 宏、`FRAME_SIZE`、`PLAYBACK_DATA_FRAME_SIZE` |
| `libraries/HAL_Drivers/drv_pdm.c` | 板载 PDM `mic0`（本需求不用，但 SConscript 会一起编） |
| `libraries/Common/board/ports/audio/drv_es8388.{c,h}` | ES8388 I2C 驱动，`es8388_init/start/volume_set/pa_power` |
| `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c` | DWC2 Host（ISO 补丁在这） |
| `libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c` | UAC class driver（无 IAD 补丁在这） |
| `libraries/components/CherryUSB-1.6.0/common/usb_hc.h` | `usbh_urb` / `usbh_iso_frame_packet` 结构 |
| `.cursor/docs/usb-host-uac-mic-notes.md` | **上一阶段详细笔记，必读** |
| `.cursor/docs/psoc-e84-edgi-talk-guide.md` | SDK/板子整体概览 |
| `tmp/audio-descriptor` | 目标麦克风 `lsusb -v` 原始输出 |

---

## 6. 交接对话索引

- [UAC 麦克风 Host 支持实现](944a3730-e7c5-4dcc-a5f7-34d99cb9163a)：上一阶段全部讨论与实现（从需求澄清 → DWC2/UAC 补丁 → 192 kB/s 验证 → 文档 + commit）。

---

## 7. 一句话总结

**USB 端 PCM 已稳定流入 `s_iso_buf`，下一步在 `mic_iso_complete` 与 `sound0` 之间搭一根 ring-buffer + pump thread，并把 `drv_i2s` 切到 48 kHz stereo，就能实时扩音。**
