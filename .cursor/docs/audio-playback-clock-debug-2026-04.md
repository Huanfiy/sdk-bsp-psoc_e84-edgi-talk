# 音频播放时钟异常排障记录（2026-04）

## 背景

工程：`projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`

目标原本是让 USB UAC 麦克风插入后，音频实时从板载 `ES8388 + sound0` 播放出来，作为扩音器使用。

在功能基本打通后，出现了一个很迷惑的问题：

- 喇叭有声音
- 但播放节奏明显异常
- 同一条播放链在不同试验阶段表现为“像开倍速”或“明显偏慢”

为了缩小范围，后续增加了一个本地 WAV 诊断命令 `wavplay`，用 U 盘中的 `test.wav` 绕开 USB 麦克风链，单独验证：

```text
U 盘 WAV -> FATFS -> sound0 -> drv_i2s -> ES8388 -> 喇叭
```

## 现象

### 1. UAC 麦克风实时扩音异常

初版 USB 麦克风桥接已经能稳定枚举、采集、写入 `sound0`，但听感异常，最开始表现为：

- 声音“很慢”
- 节奏不对

### 2. 本地 WAV 播放也异常

引入 `wavplay /test.wav` 后，确认：

- U 盘挂载正常
- `test.wav` 能成功打开
- `sound0` 确实开始播放

但听感仍然异常，且在不同配置下出现：

- 一版像“开倍速”
- 一版又明显偏慢

这说明问题不在 USB 主机栈，也不在文件系统或 WAV 解析，而在更下游的统一播放链。

## 做过的关键实验

### 实验 1：绕开 48k -> 16k 降采样

最早怀疑应用层 `3:1` 降采样有问题，因此在 `usbh_uac_mic.c` 中临时改成：

- 保持 `48 kHz`
- 只做 `stereo -> mono mix`
- 直接写 `sound0`

结论：

- USB 侧吞吐统计正常
- `sound0` 写入节拍也正常
- 但听感仍可能异常

因此可以基本排除：

- UAC ISO IN 采集速率错误
- 环形缓冲消费节拍错误
- 应用层 `rt_device_write()` 调用频率错误

### 实验 2：本地 WAV 播放链验证

新增文件：

- `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/wav_play_test.c`

新增命令：

- `wavplay [path]`
- `wavstop`

实测串口确认：

- `udisk: /dev/sda mount successfully`
- `wavplay /test.wav`
- `[wav] src: 48000 Hz, 1 ch, 16 bits`

说明：

- `test.wav` 本身有效
- 本地文件播放路径也能复现异常
- 问题已经收敛到 `sound0 / drv_i2s / TDM clock`

### 实验 3：修改 `channels` 参数

一度怀疑 `sound0` 配置时把单声道源流误配成双声道，影响了时钟分频选择。

这个判断**方向是对的，但不完整**：

- `audio_config.channels` 的确不应该决定最终物理 I2S 时钟
- 但仅改应用层传入的 `channels=1` 还不够
- 因为底层 `drv_i2s.c::ifx_set_samplerate()` 本身就把“源流声道数”错误地耦合进了分频表

## 根因

根因位于：

- `libraries/HAL_Drivers/drv_i2s.c`
- 函数：`ifx_set_samplerate()`

旧逻辑的问题有两个：

### 1. 错把“源流声道数”当成 I2S 物理接口分频条件

`sound0` 的实际输出硬件配置来自生成文件：

- `CYBSP_TDM_CONTROLLER_0_tx_config.channelNum = 2`
- `CYBSP_TDM_CONTROLLER_0_tx_config.clkDiv = 4`

也就是说：

- 物理 TDM / I2S 输出接口始终是固定的双声道帧
- `drv_i2s` 会把上层单声道数据复制成双声道输出

因此，运行时 `audio_config.channels` 只是“上层源流格式”，**不应该**决定 TDM 接口时钟分频。

### 2. 旧分频表本身不对

生成代码默认 `16 kHz` 对应的外围分频是：

- `frac_div = 23`

而旧运行时表却使用了：

- mono 16 kHz -> `24`
- mono 48 kHz -> `8`
- stereo 48 kHz -> `3`

这会直接导致：

- 有时偏快
- 有时偏慢
- 看起来像应用层问题，实际是底层时钟表错误

## 最终修复

### 修复 1：`drv_i2s.c` 只按目标采样率选 divider

修改文件：

- `libraries/HAL_Drivers/drv_i2s.c`

把 `ifx_set_samplerate()` 改成：

- 不再依据 `audio_config.channels` 分支
- 统一按目标采样率选择固定 divider

最终采用的表：

- `16000 -> frac_div = 23`
- `24000 -> frac_div = 15`
- `48000 -> frac_div = 7`
- `96000 -> frac_div = 3`

并新增日志：

```text
[I/i2s] set tdm samplerate=48000 src_ch=1 frac_div=7
```

用于后续快速确认 runtime 走到的真实时钟配置。

### 修复 2：上层统一把 `sound0` 当作“mono source + stereo output sink”

修改文件：

- `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/usbh_uac_mic.c`
- `projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H/applications/wav_play_test.c`

约定：

- 上层写入 `sound0` 的始终是单声道数据
- `drv_i2s` 负责在输出侧复制到 L/R
- `channels=1` 只表示源流格式，不再参与错误的物理时钟推导

### 修复 3：UAC 最终路径定为 48 kHz stereo -> mono mix

`usbh_uac_mic.c` 最终收口为：

```text
USB mic 48k stereo
  -> IRQ copy to ringbuffer
  -> worker thread drains 4096 B stereo blocks
  -> stereo -> mono mix at 48 kHz
  -> rt_device_write(sound0, mono source)
  -> drv_i2s duplicates mono to stereo
  -> ES8388 / speaker
```

此前临时保留的 `48k -> 16k` 降采样实验分支已移除，避免后续阅读和维护混乱。

## 验证结果

### 本地 WAV 验证通过

实测串口：

```text
[I/i2s] set tdm samplerate=48000 src_ch=1 frac_div=7
[wav] play start: /test.wav
[wav] src: 48000 Hz, 1 ch, 16 bits, data=24680960 B
[I/i2s] Ready for I2S output
```

用户听感结论：

- `test.wav` 播放速度正常

### USB 麦克风扩音验证通过

在同一版底层修复固件上，用户复测：

- 插入 USB 麦克风后扩音正常
- 播放节奏恢复正常

因此可以确认：

- `UAC host`
- `ringbuffer + worker`
- `sound0`
- `drv_i2s`
- `ES8388`

整条链路已经闭环通过。

## 当前保留的诊断入口

### 1. 本地 WAV 回放

```text
wavplay /test.wav
wavstop
```

用途：

- 排除 USB 侧因素
- 单独验证 `sound0 / drv_i2s / ES8388`

### 2. UAC 麦克风扩音状态

```text
mic_stat
mic_sample
speaker_stat
speaker_vol 80
```

用途：

- 看 USB 采集速率
- 看 ISO 包长度
- 看 ringbuffer 水位和已写 chunk
- 在线调音量

## 经验结论

这次问题最容易误判成：

- USB 麦克风采样率不对
- UAC 降采样算法有问题
- 应用层写 `sound0` 的块大小不对

但真正根因是：

**底层 `drv_i2s` 把“源流格式”错误地参与进了“物理 I2S 输出时钟”配置，同时分频表本身也不正确。**

后续如果再遇到“音频快慢不对”，优先检查：

1. `ifx_set_samplerate()` 最终打出的 `frac_div`
2. 生成配置里的 `TDM channelNum / clkDiv`
3. 上层写入 `sound0` 的数据究竟是 mono 还是 stereo
4. `sound0` 的 `channels` 参数语义是不是又被误用
