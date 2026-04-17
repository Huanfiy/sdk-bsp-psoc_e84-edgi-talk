# U 盘重复拔插 `wavplay` 只首次生效的死锁排障记录（2026-04）

## 背景

工程：`projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`

在 `wav_play_test.c` 里跑自动播放逻辑：

- `wav_auto` 线程每 500 ms 轮询 `/test.wav` 是否存在
- 检测到文件出现（上一轮还不存在）就调 `wavplay_start` → 新建 `wavplay` 线程去播
- `wavplay_start` 前置条件是 `s_wav_thread == RT_NULL`

实测表现：

**插入 U 盘第一次 → 自动播放正常；拔掉后再插入 → 只打印 `mount successfully`，不触发 `autoplay`，喇叭不响。**

代表性串口日志（用户提供）：

```text
udisk: /dev/sda mount successfully
[wav] autoplay /test.wav
[wav] play start: /test.wav
[wav] src: 48000 Hz, 1 ch, 16 bits, data=24680960 B
[I/i2s] Ready for I2S output

[E/usbh_msc] cbw transfer error: -4
usb mass_storage read failed
[wav] play end
[I/usbh_msc] Unregister MSC Class:/dev/sda
[I/usbh_core] Device on Bus 0, Hub 1, Port 1 disconnected
[wav] autostop: /test.wav removed
[I/usbh_hub] New high-speed device on Bus 0, Hub 1, Port 1 connected
...
udisk: /dev/sda mount successfully
<<< 没有 [wav] autoplay，后续无任何播放日志 >>>
```

## 现象拆解

日志里的关键观察点：

1. 第一次播放其实在结束前就被 USB 侧的 `cbw transfer error: -4` 打断（用户已经在往下拔 U 盘了）。
2. `[wav] play end` 打印了 → 说明 `wav_play_worker` 的播放循环已经跳出。
3. `[wav] autostop: /test.wav removed` 出现在 `disconnect` 之后 → 说明 `wav_auto` 在检测文件消失这一刻，`s_wav_thread != RT_NULL`。
4. 第二次 `mount successfully` 之后再没有 `autoplay` → 说明这之后 `wav_auto` 看到的依然是 `s_wav_thread != RT_NULL`。

换句话说：**`wav_play_worker` 打完 `[wav] play end` 后，再也没有把 `s_wav_thread` 回收成 `RT_NULL`，卡在了 `__exit` 里某个地方。**

## 根因

`wav_play_worker` 的收尾路径：

```c
__exit:
    if (fp != RT_NULL)  { fclose(fp); }
    if (dev != RT_NULL) { rt_device_close(dev); }    // <<< 卡在这里
    ...
    s_wav_thread = RT_NULL;                          // <<< 永远到不了
```

`rt_device_close(sound0)` 走 audio core 的 `_aduio_replay_stop`（`rt-thread/components/drivers/audio/audio.c`）：

```c
audio->replay->event |= REPLAY_EVT_STOP;
rt_completion_init(&audio->replay->cmp);
rt_completion_wait(&audio->replay->cmp, RT_WAITING_FOREVER);   // 等 ack
```

能 ack 这个 completion 的唯一路径是：

```
rt_audio_tx_complete()
  └─ _audio_send_replay_frame()
        └─ if (queue is empty && event & REPLAY_EVT_STOP)
               rt_completion_done(cmp);
```

而 `rt_audio_tx_complete()` 的唯一调用点在 `libraries/HAL_Drivers/drv_i2s.c` 的 `i2s_playback_task` 尾部：

```c
while (audio->replay->queue.is_empty == 1)
{
    rt_thread_mdelay(1);
#if defined(PKG_USING_WAVPLAYER) && !defined(BSP_USING_XiaoZhi)
    if (count >= 50) {
        rt_completion_done(&audio->replay->cmp);
        count = 0;
    }
    count++;
#endif
}
rt_audio_tx_complete(audio);
```

这就形成了一个**条件死锁**：

- 应用线程不再喂数据 → `audio->replay->queue` 空 → `i2s_playback_task` 在 `while (queue.is_empty == 1)` 里原地自旋 `rt_thread_mdelay(1)`，永远不走到 `rt_audio_tx_complete(audio)`。
- 所以 `_audio_send_replay_frame()` 永远不会被触发。
- `_aduio_replay_stop()` 的 `rt_completion_wait(RT_WAITING_FOREVER)` 永远等不到 done。
- `rt_device_close()` 不返回，worker 永远停在 `__exit`，`s_wav_thread` 永远不回 `RT_NULL`。
- `wav_auto` 下一次插入时 `s_wav_thread == RT_NULL` 不成立 → 不走 `wavplay_start`。

`i2s_playback_task` 内部**本来就有一条 50 ms 超时的兜底路径**（`count >= 50` 时强制 `rt_completion_done`），但它被包在：

```c
#if defined(PKG_USING_WAVPLAYER) && !defined(BSP_USING_XiaoZhi)
```

本工程 `.config` 里 `CONFIG_PKG_USING_WAVPLAYER is not set`，所以这条兜底**根本没编进来**。

## 最终修复

修改文件：

- `libraries/HAL_Drivers/drv_i2s.c`

把 `i2s_playback_task` 里那段兜底的 `#if` gate 从：

```c
#if defined(PKG_USING_WAVPLAYER) && !defined(BSP_USING_XiaoZhi)
```

放宽到：

```c
#if !defined(BSP_USING_XiaoZhi)
```

同时给 `static int count` 声明、和循环体里的 `if (count >= 50) rt_completion_done(...)` 两处都加上中文/英文注释，说明为什么不能删，以免后来者再把它 "顺手清理" 掉。

放宽后的覆盖面：

| 工程 | `BSP_USING_AUDIO` | `BSP_USING_XiaoZhi` | `PKG_USING_WAVPLAYER` | 修改前逃生路径 | 修改后逃生路径 |
|---|---|---|---|---|---|
| `Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H`（本次目标） | y | n | n | 关 | **开**（修好死锁） |
| `Edgi_Talk_M33_Audio` | y | n | n | 关 | **开**（顺带覆盖同类隐患） |
| `Edgi_Talk_M33_WavPlayer` | y | n | y | 开 | 开（行为不变） |
| `Edgi_Talk_M55_XiaoZhi` | y | y | y | 关 | 关（不碰 XiaoZhi） |

## 验证

编译：

```bash
cd projects/Edgi_Talk_CherryUSB/Edgi_Talk_M33_USB_H
./run.sh build
# text=207556 data=17408 bss=242249
```

功能：U 盘重复拔插测试应该看到——

1. 第 N 次插入，`udisk: /dev/sda mount successfully` 后紧接着 `[wav] autoplay /test.wav`
2. 任意时刻把 U 盘拔掉，最晚 50 ms 内 `rt_device_close(sound0)` 会返回，`s_wav_thread` 正确归零
3. `wavstop` 命令中断播放后，`s_wav_thread` 同样能归零，不会"锁死"播放器

## 经验结论

这次问题**最容易误判**成：

- DFS 重新挂载有残留 → 其实 `mount successfully` 已经打印，FS 可用
- `wav_file_exists()` 在拔插瞬间 fopen 卡住 → 其实 `[wav] autostop` 已经打印，说明 `wav_auto` 还活着
- `s_wav_thread = rt_thread_create()` 有竞态 → 其实 RT-Thread 的 `rt_thread_create` 是同步的

真正根因是：**底层 `drv_i2s.c` 在 queue 空时的 busy-wait 阻塞了 `rt_audio_tx_complete`，导致 audio core 的 stop ack 路径断掉，进而 `rt_device_close()` 永久阻塞。**

后续如果看到类似"某个线程打印了'结束'但没真的退出"，优先检查：

1. 这个线程的 `__exit` 里最后一个 syscall 是不是 `rt_device_close`（或等价的 sync-close）
2. 对应 device 的驱动是不是依赖某个回调来完成 "drain + done"
3. 如果这个回调本身由"上层继续喂数据"驱动，就要检查**喂数据 == close** 这条死锁链是不是被兜底了

相关现场：

- 修复代码：`libraries/HAL_Drivers/drv_i2s.c::i2s_playback_task` 内部 `#if !defined(BSP_USING_XiaoZhi)` 段
- 上层触发链：`wav_play_test.c::wav_play_worker` → `rt_device_close(sound0)` → `rt-thread/components/drivers/audio/audio.c::_aduio_replay_stop`
- 另一条同样会触发的路径：`usbh_uac_mic.c::speaker_close_sound`（拔 USB 麦克风时），现在一并受益于这条兜底
