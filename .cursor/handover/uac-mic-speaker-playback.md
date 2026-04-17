# Handover: UAC 麦克风实时扩音（Edgi_Talk_M33_USB_H）

> 交接对象：下一个接手该需求的 AI / 工程师
> 最后更新：2026-04-18
> 工程：`projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`
> 硬件：PSoC Edge E84 (Cortex-M33) + 板载 ES8388 codec + 外接 USB UAC 1.0 麦克风（VID/PID `3769:b01d`，FS，48 kHz / 16-bit / stereo，ISO IN `0x83`, mps=208, bInterval=1 ms）

---

## 1. 最终需求

**实时播放 USB 麦克风的音频（当扩音器用）**：

```
USB MIC (UAC 1.0 ISO IN, 48 kHz / 16-bit / stereo)
  -> CherryUSB Host (DWC2, ISO IN)                -- [DONE]
  -> mic_iso_complete IRQ -> rt_ringbuffer (32KB) -- [DONE]
  -> mic_uac worker thread (pull 64ms chunks)     -- [DONE]
  -> 3:1 decimation + stereo->mono mix (48k->16k) -- [DONE]
  -> rt_device_write(sound0)                      -- [DONE]
  -> drv_i2s (TDM0 TX) + ES8388 DAC               -- [DONE]
  -> 板载功放 -> 喇叭                              -- [DONE]
```

当前状态：**插入麦克风后，喇叭自动扩音；拔出即停。编译通过，固件已生成 `build/rtthread.hex`。**

---

## 2. 整体架构（收敛版）

### 2.1 单线程模型

本阶段从最初的"双线程（mic_worker + pump_thread）"方案合并成**单线程**：

```
attach: usbh_audio_run()
  -> 懒创建 s_pump_sem
  -> 创建 "mic_uac" 线程 (prio 15, stack 4KB)

mic_uac 线程:
  rt_ringbuffer_init(s_ring_pool, 32KB)
  ds_reset()
  speaker_open_sound()    -- 打开 sound0, 16kHz stereo, vol=60
  s_running = true
  submit all ISO URBs
  loop:
    rt_sem_take(s_pump_sem, 20ms)
    while (s_running && speaker_drain_one_chunk())
      { /* 每次 drain 64ms */ }
  speaker_close_sound()

DWC2 IRQ: mic_iso_complete()
  for each iso_packet:
    rt_ringbuffer_put_force(s_ring, pkt, actual_length)
  rt_sem_release(s_pump_sem)
  usbh_submit_urb(urb)    -- 立即复投

detach: usbh_audio_stop()
  s_running = false
  usbh_kill_urb(每个 URB)
  rt_sem_release(s_pump_sem)  -- 唤醒 worker 让它优雅退出
```

为什么不分两个线程：`speaker_pump` 和 `mic_worker` 共享 `s_ring`/`s_pump_sem`，分开后拔插瞬间的生命周期竞态非常难处理（pump 还没退出 worker 就重建了 s_ring，或者反过来）。合一之后"worker 在 = 流在 = sound0 开"是一对一关系，代码和状态都简单。

### 2.2 缓冲/速率几何

| 维度 | 值 | 依据 |
|---|---|---|
| USB 流速 | 192 kB/s | 48 kHz × 2 ch × 2 B |
| 单个 iso_packet | 192 B (偶发 196 B) | 48 或 49 samples |
| URB 周期 | 8 ms | 8 packets/urb, 1 URB in flight |
| 环形缓冲 | 32 KB | ≈170 ms headroom，溢出走 `put_force` 丢最旧 |
| 加工块 | 64 ms | 12288 B stereo in -> 2048 B mono out |
| 64 ms 对应 `sound0` | **正好一个 `PLAYBACK_DATA_FRAME_SIZE`** | 2048 `int16_t` @ 16 kHz stereo |
| ds 算法 | 每 3 个 stereo frame 求 (L+R)/6 -> 1 mono sample | 3:1 decimation + 声道混合 |

一次 drain 正好对齐 `drv_i2s` 内部的 ping-pong 一侧，下游 `i2s_playback_task` 拿到整块不会出现半旧半新数据。

### 2.3 关键文件

| 路径 | 作用 |
|---|---|
| `projects/.../applications/usbh_uac_mic.c` | **核心文件**：IRQ 收集 + ring buffer + worker + decimation + sound0 驱动（~540 行，有完整头注释） |
| `projects/.../board/board.c` | `en_gpio()` 把 `ES8388_CTRL (P16.2)` / `SPEAKER_OE_CTRL (P21.6)` 拉高 |
| `projects/.../rtconfig.h` + `.config` | 打开 `RT_USING_AUDIO` / `BSP_USING_AUDIO_PLAY` / `RT_USING_I2C` / `BSP_USING_HW_I2C0` / `RT_AUDIO_REPLAY_MP_*` |
| `libraries/HAL_Drivers/drv_i2s.c` | `sound0` 注册，16 kHz stereo，内部 `convert_mono_to_stereo` + TDM0 TX DMA |
| `libraries/Common/board/ports/audio/drv_es8388.c` | ES8388 I2C 控制（`es8388_init/start/volume_set/pa_power`） |
| `libraries/components/CherryUSB-1.6.0/port/dwc2/usb_hc_dwc2.c` | ISO IN Host 补丁 |
| `libraries/components/CherryUSB-1.6.0/class/audio/usbh_audio.c` | UAC class driver（无 IAD 兼容补丁） |
| `.cursor/docs/usb-host-uac-mic-notes.md` | 上一阶段 USB 侧开发笔记（建议回顾） |

---

## 3. 已完成的里程碑

### 3.1 阶段一：USB 采集（已在 master）

- `46973c9c feat: support uac macrophone`：DWC2 ISO IN 补齐 + UAC 无 IAD 兼容 + 初版 `usbh_uac_mic.c`
- `4f9102bf fix(uac-mic)`：单 URB × 8 packet 拓扑
- `950eaf20 docs(uac-mic)`：USB 笔记

实测：`[mic] urbs=3142 frames=25136 bytes_total=4825440 delta=192000 B errors=0`，速率准确且长期稳定。

### 3.2 阶段二：接播放通路（本阶段 = 本次交付）

本阶段改动：

1. **Kconfig**：`rtconfig.h` + `.config` 打开
   - `RT_USING_AUDIO` / `RT_AUDIO_REPLAY_MP_BLOCK_SIZE=4096` / `RT_AUDIO_REPLAY_MP_BLOCK_COUNT=2` / `RT_AUDIO_RECORD_PIPE_SIZE=2048`
   - `RT_USING_I2C` + `RT_USING_I2C_BITOPS`（被 `drv_i2c.c` 间接需要）
   - `BSP_USING_AUDIO` + `BSP_USING_AUDIO_PLAY` + `ENABLE_STEREO_INPUT_FEED`
   - `BSP_USING_I2C` + `BSP_USING_HW_I2C0`
2. **`board/board.c`**：从 `Edgi_Talk_M33_Audio` 引入 `en_gpio()`（`INIT_BOARD_EXPORT`），上电早期拉高功放+codec 使能。
3. **`applications/usbh_uac_mic.c`** 完整重写了运行逻辑：
   - 加 `rt_ringbuffer` + 32KB 池 (`.bss`)。
   - `mic_iso_complete` 改为按 `iso_packet[].actual_length` 逐包 `put_force`，释放 `s_pump_sem`。
   - 新增 `ds_convert`：3:1 decimation + (L+R)/6 混合，状态跨 chunk 持久化，容忍 49 sample 插入。
   - 新增 `speaker_open_sound/close_sound`：`AUDIO_MIXER_VOLUME` 设 60 + `AUDIO_CTL_CONFIGURE` 设 16 k stereo 16 bit。
   - 新增 `speaker_drain_one_chunk`：消费侧在 `rt_hw_interrupt_disable` 保护下从 ring 取 12288 B，转换后 `rt_device_write(sound0, 2048 B mono)`。
   - `mic_worker_entry`：单线程完整生命周期（ringbuf init → sound0 open → URB submit → 循环 drain → 退出时关 sound0）。
   - `usbh_audio_run/stop`：懒创建 sem；stop 时 kill URBs + release sem 让 worker 立即返回。
4. **MSH 命令**：
   - `mic_stat`：USB 采集统计（增量速率）
   - `mic_sample`：dump ISO packet 长度 + buffer 十六进制
   - `speaker_vol <0..100>`：运行时调音量
   - `speaker_stat`：ring/pump/sound0 状态 + 已写 chunk 数

### 3.3 构建/烧录验证

- `./run.sh build` 通过，`text=203128 data=17408 bss=242249`。
- `scons` 只剩两条无关 warning（`usbh_dfs.c` 的 `CONFIG_USB_DFS_MOUNT_POINT` 格式与 `drv_i2s.c` 的 `unused count`），不影响功能。
- `build/rtthread.hex` 已由 `edgeprotecttools` 重定位到外部 QSPI 区段，可直接 `./run.sh flash`。

---

## 4. 运行方式

```bash
cd projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H
./run.sh build      # 编译
./run.sh flash      # KitProg3 烧录
# 串口 (UART2) 115200 8N1
```

上电后：

1. `en_gpio` 先拉高 `ES8388_CTRL` / `SPEAKER_OE_CTRL`。
2. `main` 调用 `usbh_initialize(0, ...)`。
3. 插麦克风 -> 枚举 -> `usbh_audio_run` -> `mic_uac` 线程起来 -> `[mic] stream open: mps=208, urbs=1, frames/urb=8` -> `[spk] sound0 opened (16 kHz stereo, vol=60)`。
4. 正常情况：数据流直接从麦克风到喇叭，延迟 ≈ 64 ms（一个 chunk）+ `drv_i2s` ping-pong 抖动。
5. 拔麦克风 -> `usbh_audio_stop` -> worker 退出 -> sound0 关闭。

运行时排查命令：

| 命令 | 用途 |
|---|---|
| `mic_stat` | 看 USB 侧是否还在跑（`delta` 应 = 192000 B） |
| `mic_sample` | 看 iso_packet 长度 / 前 16 B hex，确认不是 0 |
| `speaker_stat` | 看 ring 水位、overruns/underruns、已写 chunk 数 |
| `speaker_vol 80` | 实时调音量 |
| `ps` / `list_thread` | 看 `mic_uac` 是否在跑 |

---

## 5. 已知的坑 / 设计取舍

1. **单 URB + 1 端点 1 channel**：DWC2 同一 ISO IN 端点只能一条活跃 URB；不要尝试双 URB 流水（上一阶段已踩过，速率会减半）。
2. **drop-oldest vs block**：ring 用 `rt_ringbuffer_put_force`，溢出时覆盖最旧数据。扩音器场景下宁可短 glitch 不要累积延迟；若将来要做录音存档，要改成 `rt_ringbuffer_put` + 计数丢包。
3. **`rt_hw_interrupt_disable` 保护消费侧**：因为生产侧在 IRQ 里跑 `put_force`，两者对 `read_index/write_index` 有并发写，这里简单用全局中断锁锁住一次 12KB 的 `memcpy`（≈几微秒），远小于 1 ms USB frame，不会丢包。
4. **mic0 / drv_pdm.c 会被一起编**：`HAL_Drivers/SConscript` 只看 `BSP_USING_AUDIO`，会把 `drv_pdm.c` 一起进来，但它只做 `INIT_DEVICE_EXPORT` 注册，不 `open` 就不会碰 PDM 硬件，不冲突，不必单独屏蔽。
5. **采样率：为什么是 16 k 不是 48 k**：`drv_i2s.h :: SAMPLING_RATE = 16 kHz` 是**编译时常量**，改成 48 k 会连带影响 AEC 参考链的代码路径。扩音器不需要保真度，在应用层 3:1 下采样 + 立体声混单声道最省事，且 64 ms chunk 正好对齐 `sound0` 的 ping-pong 帧（2048 int16）。
6. **ES8388 I2C 地址**：`0x18 @ 400 kHz`（`sound_init` 内硬编码），必须 `BSP_USING_HW_I2C0=y`。
7. **Cache / DMA**：`s_iso_buf` 在 `USB_NOCACHE_RAM_SECTION` (`.cy_socmem_data`)，是 DWC2 的 DMA 目的地。`s_pump_stereo/s_pump_mono` 是普通 SRAM，供 CPU 内存拷贝 + decimation 用，最后 `rt_device_write` 会把数据复制到 `drv_i2s` 自己管理的 DMA 缓冲。
8. **Kconfig 与 `.config` 手工同步**：这个 BSP 没开 `menuconfig` 的完整闭环，`rtconfig.h` 是手 维 护 的 ；每次改 Kconfig 都要**双写 `rtconfig.h` + `.config`**，否则下次 `menuconfig` 会把手工的改动覆盖。
9. **开机电源顺序**：`en_gpio` 在 `INIT_BOARD_EXPORT` 阶段运行，早于 `rt_device_open("sound0")`，所以 `es8388_init` 执行时电源已经稳定；若将来改动启动顺序要留意这条依赖。
10. **USB 热插拔**：`usbh_audio_stop` 里用 `rt_sem_release` 唤醒 worker 让它立刻退出；`s_running=false` 在 `mic_iso_complete` 里也会在 `-USB_ERR_NOTCONN/-SHUTDOWN` 时设为 false，双路径兜底。

---

## 6. 后续可能的优化方向（非本期需求）

- **AEC 反馈路径**：如果要把本地喇叭的回声从 MIC 去掉，可以把 `rt_device_write` 给 sound0 的 mono buffer 复制一份作为 AEC reference。
- **USB Feedback endpoint**：目前忽略了 UAC 里 implicit/explicit 反馈端点，48 kHz → 16 kHz 降采样用整数 3:1，偶发 49-sample 插入用 `ds_phase` 跨 chunk 吸收。若换 48 kHz 非整数倍的 codec 需要真正的 ASRC（可接 `IFX_asrc`）。
- **延迟下调**：把 chunk 从 64 ms 降到 16 ms 或 32 ms，需要同时修改 `drv_i2s` 的 `PLAYBACK_DATA_FRAME_SIZE`，并增加 ring 水位监控。
- **立体声直通**：若换上 48 k stereo 的 codec 链路，改 `drv_i2s` 让 `sound_transmit` 在 `channels==2` 时跳过 `convert_mono_to_stereo`，直接 `memcpy`。
- **音量控制 UI**：目前只有 MSH `speaker_vol`；可以接入旋钮/按键。

---

## 7. 交接对话索引

- [UAC 麦克风 Host 支持实现](944a3730-e7c5-4dcc-a5f7-34d99cb9163a)：阶段一（DWC2/UAC 补丁 + 192 kB/s 验证）。
- [UAC 麦克风实时扩音](f3a7796e-0070-465f-92d2-9e0dca869728)：阶段二（本次，把 USB 流送到 sound0）。

---

## 8. 一句话总结

**插入 UAC 麦克风即扩音。数据路径：DWC2 IRQ -> `rt_ringbuffer` (32 KB, drop-oldest) -> `mic_uac` 工作线程 (64 ms chunk) -> 3:1 decimate + L/R mono mix -> `rt_device_write(sound0)` -> ES8388 -> 功放 -> 喇叭。单线程模型，拔插自动 open/close sound0，MSH 可实时调音量/看水位。**
