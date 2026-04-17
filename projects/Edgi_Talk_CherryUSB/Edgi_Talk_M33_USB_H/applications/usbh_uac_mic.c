/*
 * UAC microphone -> ES8388 speaker bridge (public-address / amplifier mode).
 *
 * Data path:
 *   USB UAC 1.0 mic (48 kHz / 16-bit / stereo, 192 B/ms)
 *       |  ISO IN URBs (8 packets/urb, 1 URB in flight)
 *       v  dwc2 IRQ -> mic_iso_complete()
 *   ring buffer (raw stereo PCM, lock-free single-producer/single-consumer)
 *       v  mic_uac worker thread (wakes on sem from IRQ)
 *   3:1 decimation + stereo->mono mix  (48 kHz stereo -> 16 kHz mono)
 *       v  rt_device_write(sound0)  every 64 ms
 *   ES8388 DAC (16 kHz stereo out, duplicated L/R) -> PA -> speaker
 *
 * Runtime control: `usbh_audio_run` / `usbh_audio_stop` are invoked by
 * the CherryUSB host stack on attach/detach. On attach a single worker
 * thread (`mic_uac`) is spawned that owns both the USB ISO submission
 * and the speaker drain; on detach the worker drains the remaining
 * chunks and closes sound0 before exiting.
 *
 * MSH helpers:
 *   mic_stat      - USB capture counters (bytes/urbs/errors delta)
 *   mic_sample    - dump ISO packet lengths + buffer hex windows
 *   speaker_vol   - live volume (0..100) via AUDIO_MIXER_VOLUME
 *   speaker_stat  - ring/pump/sound0 state + chunk counter
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <stdlib.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_audio.h"

#define MIC_TAG                "[mic] "
#define SPK_TAG                "[spk] "

/* ---- USB capture geometry -------------------------------------------- */
#define MIC_SAMPLE_RATE        48000U
#define MIC_BIT_RES            16U
#define MIC_PACKET_SIZE        208U  /* descriptor wMaxPacketSize */
#define MIC_ISO_PACKETS_PER_URB 8U   /* 8 ms of audio per URB */
#define MIC_ISO_URB_COUNT      1U    /* DWC2: one channel per IN endpoint */

#define MIC_URB_STORAGE_BYTES                                                   \
    (sizeof(struct usbh_urb) +                                                  \
     MIC_ISO_PACKETS_PER_URB * sizeof(struct usbh_iso_frame_packet))

/* ---- Ring buffer + pump geometry ------------------------------------- */
/* Input chunk we process at a time: 64 ms of 48 kHz stereo PCM.
 * 64 ms matches the full TDM ping-pong buffer period inside drv_i2s.c
 * (PLAYBACK_DATA_FRAME_SIZE = 2048 int16 @ 32 kB/s stereo), so each
 * rt_device_write() to sound0 fills exactly one playback frame and the
 * downstream i2s_playback_task never works on partially-stale data. */
#define SPK_STEREO_BYTES_PER_MS  (MIC_SAMPLE_RATE / 1000U * 2U * 2U) /* 192 */
#define SPK_CHUNK_MS             64U
#define SPK_CHUNK_STEREO_BYTES   (SPK_STEREO_BYTES_PER_MS * SPK_CHUNK_MS) /* 12288 */
/* 3:1 decimation + stereo→mono: 3 stereo frames (12 B) → 1 mono sample (2 B). */
#define SPK_CHUNK_MONO_SAMPLES   (SPK_CHUNK_STEREO_BYTES / 12U)           /* 1024 */
#define SPK_CHUNK_MONO_BYTES     (SPK_CHUNK_MONO_SAMPLES * 2U)            /* 2048 */

/* Ring buffer must absorb a handful of 8 ms URB bursts while the pump
 * thread is asleep waiting for tx_sem from drv_i2s. 32 KiB ≈ 170 ms
 * headroom at 192 kB/s; overflows fall back to drop-oldest via
 * rt_ringbuffer_put_force so the USB IRQ never stalls. */
#define SPK_RING_BYTES           (32U * 1024U)

#define SPK_SOUND_DEV_NAME       "sound0"
#define SPK_DEFAULT_VOLUME       60

/* ---- USB audio sample buffers (must be DMA reachable, non-cacheable) - */
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
static uint8_t s_iso_buf[MIC_ISO_URB_COUNT][MIC_ISO_PACKETS_PER_URB * MIC_PACKET_SIZE];

/* The urb struct itself is never DMAed into; only iso_packet[].transfer_buffer
 * is touched by hardware. Plain SRAM is fine. */
static USB_MEM_ALIGNX uint8_t s_urb_storage[MIC_ISO_URB_COUNT][MIC_URB_STORAGE_BYTES];

#define MIC_URB(i) ((struct usbh_urb *)(void *)s_urb_storage[i])

/* ---- Ring buffer storage (plain SRAM, CPU-accessed only) ------------- */
static rt_uint8_t s_ring_pool[SPK_RING_BYTES];
static struct rt_ringbuffer s_ring;

/* ---- Runtime state --------------------------------------------------- */
static struct usbh_audio *s_audio_class;
static rt_thread_t s_worker;      /* single thread: USB capture + speaker pump */
static rt_sem_t    s_pump_sem;    /* IRQ ↔ worker data-ready notify */
static rt_device_t s_sound_dev;
static volatile bool s_sound_opened;

/* Counters updated from IRQ, read from threads / shell. Plain 32-bit reads
 * are atomic on Cortex-M33 so no lock is needed for reporting. */
static volatile uint32_t s_total_bytes;    /* raw stereo bytes captured */
static volatile uint32_t s_total_frames;
static volatile uint32_t s_total_urbs;
static volatile uint32_t s_total_errors;
static volatile uint32_t s_ring_overruns;  /* put_force overwrote old data */
static volatile uint32_t s_pump_underruns; /* pump had no full chunk ready */
static volatile uint32_t s_pump_chunks;    /* chunks written to sound0 */
static volatile uint32_t s_last_bytes;
static volatile bool     s_running;

/* ---- 3:1 downsample + stereo→mono accumulator ------------------------
 * State persists across chunk boundaries so the occasional 196-byte
 * (49 stereo frames) ISO packet does not break 3-sample alignment. */
static int32_t s_ds_acc_l, s_ds_acc_r;
static uint32_t s_ds_phase;

static inline void ds_reset(void)
{
    s_ds_acc_l = 0;
    s_ds_acc_r = 0;
    s_ds_phase = 0;
}

/* Convert one 64 ms block of interleaved int16 stereo samples into mono
 * samples at 16 kHz by averaging each run of 3 stereo frames. Returns
 * the number of mono samples actually produced (~1024 in steady state,
 * may drift by ±1 when the USB device inserts a 49-sample ms). */
static uint32_t ds_convert(const int16_t *stereo_in, uint32_t stereo_frames,
                           int16_t *mono_out, uint32_t mono_cap)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < stereo_frames; i++) {
        s_ds_acc_l += stereo_in[2 * i];
        s_ds_acc_r += stereo_in[2 * i + 1];
        s_ds_phase++;
        if (s_ds_phase == 3) {
            if (out < mono_cap) {
                /* Average 3 L samples + 3 R samples → (/6) = mono mix.
                 * Max magnitude at int16 range stays inside int32. */
                mono_out[out++] = (int16_t)((s_ds_acc_l + s_ds_acc_r) / 6);
            }
            s_ds_acc_l = 0;
            s_ds_acc_r = 0;
            s_ds_phase = 0;
        }
    }
    return out;
}

/* ---- USB ISO completion (dwc2 IRQ context) --------------------------- */
static void mic_iso_complete(void *arg, int nbytes)
{
    struct usbh_urb *urb = (struct usbh_urb *)arg;

    if (!s_running || urb == NULL) {
        return;
    }

    if (nbytes > 0) {
        /* Walk the packet list so we copy only real payload bytes.
         * actual_length per packet is set by the dwc2 XFRC path and is
         * normally 192 (48 samples) with an occasional 196 (49 samples)
         * inserted by the device for 48 kHz ↔ 1 kHz frame drift. */
        for (uint32_t p = 0; p < urb->num_of_iso_packets; p++) {
            uint32_t len = urb->iso_packet[p].actual_length;
            if (len == 0) {
                continue;
            }
            const rt_uint8_t *src = (const rt_uint8_t *)
                urb->iso_packet[p].transfer_buffer;
            rt_size_t wrote = rt_ringbuffer_put_force(&s_ring, src, len);
            if (wrote < len || rt_ringbuffer_space_len(&s_ring) == 0) {
                s_ring_overruns++;
            }
        }

        s_total_bytes += (uint32_t)nbytes;
        s_total_frames += urb->num_of_iso_packets;
        s_total_urbs++;

        /* Kick the pump; it'll wake once per URB even though we only
         * process once every 8 URBs (64 ms). Extra releases are cheap
         * because the sem tops out at its initial count semantics. */
        if (s_pump_sem != RT_NULL) {
            rt_sem_release(s_pump_sem);
        }
    } else if (nbytes == 0) {
        /* Silent urb (unusual for ISO IN): still counts as a success. */
        s_total_urbs++;
    } else {
        s_total_errors++;
        if (nbytes == -USB_ERR_NOTCONN || nbytes == -USB_ERR_SHUTDOWN) {
            s_running = false;
            return;
        }
    }

    /* Re-arm immediately. usbh_submit_urb is safe from the DWC2 IRQ:
     * it only takes a critical section and writes registers. */
    int ret = usbh_submit_urb(urb);
    if (ret < 0 && ret != -USB_ERR_BUSY) {
        s_total_errors++;
    }
}

static void mic_urb_init(uint32_t idx, struct usbh_audio *audio_class, uint16_t mps)
{
    struct usbh_urb *urb = MIC_URB(idx);

    memset(urb, 0, MIC_URB_STORAGE_BYTES);
    urb->hport = audio_class->hport;
    urb->ep = audio_class->isoin;
    urb->num_of_iso_packets = MIC_ISO_PACKETS_PER_URB;
    urb->timeout = 0;
    urb->complete = mic_iso_complete;
    urb->arg = urb;

    for (uint32_t p = 0; p < MIC_ISO_PACKETS_PER_URB; p++) {
        urb->iso_packet[p].transfer_buffer = &s_iso_buf[idx][p * MIC_PACKET_SIZE];
        urb->iso_packet[p].transfer_buffer_length = mps;
        urb->iso_packet[p].actual_length = 0;
        urb->iso_packet[p].errorcode = 0;
    }
}

/* ---- Speaker pump: open sound0 and drive it from the ring buffer ----- */

/* Scratch buffers for one 64 ms block. Placed in .bss (plain SRAM) —
 * rt_device_write() will copy into sound0's own DMA-safe buffers. */
static int16_t s_pump_stereo[SPK_CHUNK_STEREO_BYTES / 2];
static int16_t s_pump_mono[SPK_CHUNK_MONO_SAMPLES];

static rt_err_t speaker_open_sound(void)
{
    if (s_sound_opened) {
        return RT_EOK;
    }

    s_sound_dev = rt_device_find(SPK_SOUND_DEV_NAME);
    if (s_sound_dev == RT_NULL) {
        rt_kprintf(SPK_TAG "device '%s' not found\r\n", SPK_SOUND_DEV_NAME);
        return -RT_ENOSYS;
    }

    /* Volume first — AUDIO_MIXER_VOLUME talks to es8388 via I2C and
     * survives subsequent open/close cycles. */
    struct rt_audio_caps caps;
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type  = AUDIO_MIXER_VOLUME;
    caps.udata.value = SPK_DEFAULT_VOLUME;
    rt_device_control(s_sound_dev, AUDIO_CTL_CONFIGURE, &caps);

    rt_err_t err = rt_device_open(s_sound_dev, RT_DEVICE_OFLAG_WRONLY);
    if (err != RT_EOK) {
        rt_kprintf(SPK_TAG "rt_device_open(%s) failed: %d\r\n",
                   SPK_SOUND_DEV_NAME, err);
        return err;
    }

    /* Sample rate is already 16 kHz / stereo by default inside drv_i2s;
     * push the same values explicitly so future changes to the default
     * won't silently break us. */
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = 16000;
    caps.udata.config.channels   = 2;
    caps.udata.config.samplebits = 16;
    rt_device_control(s_sound_dev, AUDIO_CTL_CONFIGURE, &caps);

    s_sound_opened = true;
    rt_kprintf(SPK_TAG "sound0 opened (16 kHz stereo, vol=%d)\r\n",
               SPK_DEFAULT_VOLUME);
    return RT_EOK;
}

static void speaker_close_sound(void)
{
    if (!s_sound_opened || s_sound_dev == RT_NULL) {
        return;
    }
    rt_device_close(s_sound_dev);
    s_sound_opened = false;
}

/* Drain one 64 ms chunk from the ring buffer into sound0. Returns true
 * when a chunk was actually written, false when the ring had less than
 * a full chunk ready. */
static bool speaker_drain_one_chunk(void)
{
    if (rt_ringbuffer_data_len(&s_ring) < SPK_CHUNK_STEREO_BYTES) {
        return false;
    }

    /* Consumer side: read guarded by interrupt disable to block a
     * racing put_force from overwriting read_index mid-copy. The disable
     * window is ~12 KB memcpy ≈ a few microseconds, well below the
     * 1 ms USB frame interval. */
    rt_base_t level = rt_hw_interrupt_disable();
    rt_size_t got = rt_ringbuffer_get(
        &s_ring,
        (rt_uint8_t *)s_pump_stereo,
        SPK_CHUNK_STEREO_BYTES);
    rt_hw_interrupt_enable(level);

    if (got != SPK_CHUNK_STEREO_BYTES) {
        s_pump_underruns++;
        return false;
    }

    uint32_t stereo_frames = got / 4U; /* 4 B per stereo frame */
    uint32_t mono_samples = ds_convert(
        s_pump_stereo, stereo_frames,
        s_pump_mono, SPK_CHUNK_MONO_SAMPLES);

    if (mono_samples == 0) {
        return false;
    }

    rt_device_write(s_sound_dev, 0, s_pump_mono, mono_samples * 2U);
    s_pump_chunks++;
    return true;
}

/* ---- USB capture + speaker pump worker (single thread per attach) ---- */
static void mic_worker_entry(void *arg)
{
    struct usbh_audio *audio_class = (struct usbh_audio *)arg;
    int ret;

    rt_thread_mdelay(100); /* let enumeration settle */

    ret = usbh_audio_open(audio_class, "mic", MIC_SAMPLE_RATE, MIC_BIT_RES);
    if (ret < 0) {
        rt_kprintf(MIC_TAG "open(mic, %u, %u) failed: %d\r\n",
                   MIC_SAMPLE_RATE, MIC_BIT_RES, ret);
        return;
    }

    if (audio_class->isoin == NULL) {
        rt_kprintf(MIC_TAG "no ISO IN endpoint after open\r\n");
        return;
    }

    rt_kprintf(MIC_TAG "stream open: mps=%u, urbs=%u, frames/urb=%u\r\n",
               audio_class->isoin_mps,
               (unsigned)MIC_ISO_URB_COUNT,
               (unsigned)MIC_ISO_PACKETS_PER_URB);

    /* Fresh ring buffer + counters for this attach session. */
    rt_ringbuffer_init(&s_ring, s_ring_pool, SPK_RING_BYTES);
    ds_reset();
    s_total_bytes = 0;
    s_total_frames = 0;
    s_total_urbs = 0;
    s_total_errors = 0;
    s_last_bytes = 0;
    s_ring_overruns = 0;
    s_pump_underruns = 0;
    s_pump_chunks = 0;

    /* Open the speaker first so the I2S/ES8388 path is live before the
     * IRQ starts dropping samples into the ring. */
    bool have_sink = (speaker_open_sound() == RT_EOK);
    if (!have_sink) {
        rt_kprintf(MIC_TAG "sound0 open failed, running in capture-only mode\r\n");
    }

    s_running = true;

    uint16_t mps = audio_class->isoin_mps;
    if (mps == 0 || mps > MIC_PACKET_SIZE) {
        rt_kprintf(MIC_TAG "unexpected mps=%u, clamp to %u\r\n",
                   mps, (unsigned)MIC_PACKET_SIZE);
        mps = MIC_PACKET_SIZE;
    }

    for (uint32_t u = 0; u < MIC_ISO_URB_COUNT; u++) {
        mic_urb_init(u, audio_class, mps);
        ret = usbh_submit_urb(MIC_URB(u));
        if (ret < 0) {
            rt_kprintf(MIC_TAG "submit urb %u failed: %d\r\n", u, ret);
        }
    }

    /* Main loop: wake on IRQ-posted sem (one post per URB ≈ every 8 ms)
     * or at worst every 20 ms, then drain as many 64 ms chunks as the
     * ring buffer holds. */
    while (s_running) {
        rt_sem_take(s_pump_sem, rt_tick_from_millisecond(20));
        if (have_sink) {
            while (s_running && speaker_drain_one_chunk()) {
                /* keep draining while data is available */
            }
        }
    }

    if (have_sink) {
        speaker_close_sound();
    }

    rt_kprintf(MIC_TAG "stream stopped, totals: frames=%u bytes=%u "
               "errors=%u chunks=%u overruns=%u underruns=%u\r\n",
               s_total_frames, s_total_bytes, s_total_errors,
               s_pump_chunks, s_ring_overruns, s_pump_underruns);
}

void usbh_audio_run(struct usbh_audio *audio_class)
{
    if (audio_class == NULL) {
        return;
    }

    /* Lazily create the shared IRQ -> worker semaphore. Keeping it alive
     * across attach/detach cycles avoids the double-create churn in the
     * CherryUSB PSC thread context. */
    if (s_pump_sem == RT_NULL) {
        s_pump_sem = rt_sem_create("mic_pump", 0, RT_IPC_FLAG_FIFO);
        if (s_pump_sem == RT_NULL) {
            rt_kprintf(MIC_TAG "failed to create pump sem\r\n");
            return;
        }
    }

    s_audio_class = audio_class;
    s_worker = rt_thread_create("mic_uac",
                                mic_worker_entry, audio_class,
                                4096, 15, 10);
    if (s_worker != RT_NULL) {
        rt_thread_startup(s_worker);
    } else {
        rt_kprintf(MIC_TAG "failed to create worker thread\r\n");
        s_audio_class = NULL;
    }
}

void usbh_audio_stop(struct usbh_audio *audio_class)
{
    (void)audio_class;
    s_running = false;
    for (uint32_t u = 0; u < MIC_ISO_URB_COUNT; u++) {
        struct usbh_urb *urb = MIC_URB(u);
        if (urb->hport != NULL && urb->hcpriv != NULL) {
            usbh_kill_urb(urb);
        }
    }
    /* Nudge the worker so it wakes up from rt_sem_take immediately
     * rather than waiting out its 20 ms tick. */
    if (s_pump_sem != RT_NULL) {
        rt_sem_release(s_pump_sem);
    }
    s_audio_class = NULL;
}

/* ---- Debug / manual MSH commands ------------------------------------- */
static int mic_stat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_audio_class == NULL || !s_running) {
        rt_kprintf(MIC_TAG "no active mic stream\r\n");
        return 0;
    }
    uint32_t cur = s_total_bytes;
    uint32_t delta = cur - s_last_bytes;
    s_last_bytes = cur;
    rt_kprintf(MIC_TAG "urbs=%u frames=%u bytes_total=%u delta=%u B errors=%u\r\n",
               s_total_urbs, s_total_frames, cur, delta, s_total_errors);
    return 0;
}
MSH_CMD_EXPORT(mic_stat, print uac mic stream stats since last call);

static void mic_dump_bytes(const char *tag, const uint8_t *p, uint32_t off, uint32_t n)
{
    rt_kprintf("  %s @ +%u:", tag, (unsigned)off);
    for (uint32_t i = 0; i < n; i++) {
        if ((i & 0xf) == 0) {
            rt_kprintf("\r\n   ");
        }
        rt_kprintf("%02x ", p[off + i]);
    }
    rt_kprintf("\r\n");
}

static int mic_sample(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_audio_class == NULL || !s_running) {
        rt_kprintf(MIC_TAG "no active mic stream\r\n");
        return 0;
    }

    for (uint32_t u = 0; u < MIC_ISO_URB_COUNT; u++) {
        struct usbh_urb *urb = MIC_URB(u);
        rt_kprintf(MIC_TAG "urb%u packet lengths:", u);
        for (uint32_t p = 0; p < urb->num_of_iso_packets; p++) {
            rt_kprintf(" [%u]=%u/err=%d",
                       (unsigned)p,
                       (unsigned)urb->iso_packet[p].actual_length,
                       urb->iso_packet[p].errorcode);
        }
        rt_kprintf("\r\n");
    }

    rt_kprintf(MIC_TAG "urb0 pkt0 buffer (first 16, around 96, last 16):\r\n");
    mic_dump_bytes("head", s_iso_buf[0], 0, 16);
    mic_dump_bytes("mid ", s_iso_buf[0], 88, 32);
    mic_dump_bytes("tail", s_iso_buf[0], MIC_PACKET_SIZE - 16, 16);
    return 0;
}
MSH_CMD_EXPORT(mic_sample, dump iso packet lengths and buffer windows);

static int speaker_vol(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf(SPK_TAG "usage: speaker_vol <0..100>\r\n");
        return 0;
    }
    if (!s_sound_opened || s_sound_dev == RT_NULL) {
        rt_kprintf(SPK_TAG "sound0 not open (is the mic plugged in?)\r\n");
        return 0;
    }
    int v = atoi(argv[1]);
    if (v < 0) v = 0;
    if (v > 100) v = 100;

    struct rt_audio_caps caps;
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type  = AUDIO_MIXER_VOLUME;
    caps.udata.value = (rt_uint8_t)v;
    rt_device_control(s_sound_dev, AUDIO_CTL_CONFIGURE, &caps);
    rt_kprintf(SPK_TAG "volume set to %d\r\n", v);
    return 0;
}
MSH_CMD_EXPORT(speaker_vol, set speaker volume 0..100 while mic is streaming);

static int speaker_stat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf(SPK_TAG "running=%d sound_opened=%d\r\n",
               (int)s_running, (int)s_sound_opened);
    rt_kprintf(SPK_TAG "ring: data=%u space=%u overruns=%u underruns=%u\r\n",
               (unsigned)rt_ringbuffer_data_len(&s_ring),
               (unsigned)rt_ringbuffer_space_len(&s_ring),
               s_ring_overruns, s_pump_underruns);
    rt_kprintf(SPK_TAG "chunks_written=%u (each %u B mono = %u ms)\r\n",
               s_pump_chunks,
               (unsigned)SPK_CHUNK_MONO_BYTES,
               (unsigned)SPK_CHUNK_MS);
    return 0;
}
MSH_CMD_EXPORT(speaker_stat, print speaker pump + ring buffer stats);
