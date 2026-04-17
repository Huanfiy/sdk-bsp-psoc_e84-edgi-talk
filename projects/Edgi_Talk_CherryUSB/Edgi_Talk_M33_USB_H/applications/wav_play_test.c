/*
 * Diagnostic WAV playback helper for sound0 / ES8388 validation.
 *
 * This stays in the project as a bring-up command so we can isolate
 * "local file playback" from "USB mic bridge" whenever audio timing
 * regresses again.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>

#define WAV_TAG               "[wav] "
#define WAV_SOUND_DEV_NAME    "sound0"
#define WAV_THREAD_STACK      4096
#define WAV_THREAD_PRIO       18
#define WAV_IO_BYTES          4096
#define WAV_PATH_MAX          256

struct wav_info
{
    rt_uint16_t format;
    rt_uint16_t channels;
    rt_uint32_t sample_rate;
    rt_uint16_t bits_per_sample;
    rt_uint32_t data_size;
};

static rt_thread_t s_wav_thread;
static volatile rt_bool_t s_wav_stop_req;
static char s_wav_path[WAV_PATH_MAX];

static rt_uint16_t rd_le16(const rt_uint8_t *p)
{
    return (rt_uint16_t)(p[0] | (p[1] << 8));
}

static rt_uint32_t rd_le32(const rt_uint8_t *p)
{
    return (rt_uint32_t)(p[0] |
                         (p[1] << 8) |
                         (p[2] << 16) |
                         (p[3] << 24));
}

static int wav_skip(FILE *fp, rt_uint32_t bytes)
{
    while (bytes > 0) {
        rt_uint8_t scratch[32];
        rt_size_t chunk = (bytes > sizeof(scratch)) ? sizeof(scratch) : bytes;
        if (fread(scratch, 1, chunk, fp) != chunk) {
            return -1;
        }
        bytes -= chunk;
    }
    return 0;
}

static int wav_parse_header(FILE *fp, struct wav_info *info)
{
    rt_uint8_t hdr[12];
    rt_bool_t fmt_found = RT_FALSE;

    if (fp == RT_NULL || info == RT_NULL) {
        return -1;
    }

    memset(info, 0, sizeof(*info));
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return -1;
    }

    if (memcmp(&hdr[0], "RIFF", 4) != 0 || memcmp(&hdr[8], "WAVE", 4) != 0) {
        return -1;
    }

    while (1) {
        rt_uint8_t chunk_hdr[8];
        rt_uint32_t chunk_size;

        if (fread(chunk_hdr, 1, sizeof(chunk_hdr), fp) != sizeof(chunk_hdr)) {
            return -1;
        }

        chunk_size = rd_le32(&chunk_hdr[4]);
        if (memcmp(&chunk_hdr[0], "fmt ", 4) == 0) {
            rt_uint8_t fmt[16];
            if (chunk_size < sizeof(fmt)) {
                return -1;
            }
            if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt)) {
                return -1;
            }
            info->format = rd_le16(&fmt[0]);
            info->channels = rd_le16(&fmt[2]);
            info->sample_rate = rd_le32(&fmt[4]);
            info->bits_per_sample = rd_le16(&fmt[14]);
            fmt_found = RT_TRUE;
            if (chunk_size > sizeof(fmt) && wav_skip(fp, chunk_size - sizeof(fmt)) != 0) {
                return -1;
            }
        } else if (memcmp(&chunk_hdr[0], "data", 4) == 0) {
            if (!fmt_found) {
                return -1;
            }
            info->data_size = chunk_size;
            return 0;
        } else {
            if (wav_skip(fp, chunk_size) != 0) {
                return -1;
            }
        }

        if (chunk_size & 1U) {
            if (wav_skip(fp, 1) != 0) {
                return -1;
            }
        }
    }
}

static rt_uint32_t mix_stereo_to_mono(const rt_int16_t *stereo,
                                      rt_uint32_t frames,
                                      rt_int16_t *mono)
{
    for (rt_uint32_t i = 0; i < frames; i++) {
        mono[i] = (rt_int16_t)(((rt_int32_t)stereo[2 * i] +
                                (rt_int32_t)stereo[2 * i + 1]) / 2);
    }
    return frames * 2U;
}

static rt_err_t wav_open_output(rt_device_t *out_dev, const struct wav_info *info)
{
    struct rt_audio_caps caps;
    rt_device_t dev;
    rt_err_t err;

    dev = rt_device_find(WAV_SOUND_DEV_NAME);
    if (dev == RT_NULL) {
        rt_kprintf(WAV_TAG "device '%s' not found\r\n", WAV_SOUND_DEV_NAME);
        return -RT_ENOSYS;
    }

    err = rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY);
    if (err != RT_EOK) {
        rt_kprintf(WAV_TAG "open %s failed: %d\r\n", WAV_SOUND_DEV_NAME, err);
        return err;
    }

    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = info->sample_rate;
    /* sound0 expects the source stream channel count here; drv_i2s will
     * duplicate mono samples to L/R itself for speaker playback. */
    caps.udata.config.channels   = 1;
    caps.udata.config.samplebits = info->bits_per_sample;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);

    *out_dev = dev;
    return RT_EOK;
}

static void wav_play_worker(void *parameter)
{
    FILE *fp = RT_NULL;
    rt_device_t dev = RT_NULL;
    rt_uint8_t *in_buf = RT_NULL;
    rt_int16_t *mono_buf = RT_NULL;
    struct wav_info info;
    rt_uint32_t left;

    (void)parameter;

    fp = fopen(s_wav_path, "rb");
    if (fp == RT_NULL) {
        rt_kprintf(WAV_TAG "open file failed: %s\r\n", s_wav_path);
        goto __exit;
    }

    if (wav_parse_header(fp, &info) != 0) {
        rt_kprintf(WAV_TAG "invalid/unsupported wav header: %s\r\n", s_wav_path);
        goto __exit;
    }

    if (info.format != 1 || info.bits_per_sample != 16 ||
        (info.channels != 1 && info.channels != 2)) {
        rt_kprintf(WAV_TAG "only PCM16 mono/stereo supported "
                   "(fmt=%u ch=%u bits=%u)\r\n",
                   info.format, info.channels, info.bits_per_sample);
        goto __exit;
    }

    if (info.sample_rate != 16000 && info.sample_rate != 24000 &&
        info.sample_rate != 48000) {
        rt_kprintf(WAV_TAG "unsupported sample rate: %u\r\n",
                   (unsigned)info.sample_rate);
        goto __exit;
    }

    if (wav_open_output(&dev, &info) != RT_EOK) {
        goto __exit;
    }

    in_buf = rt_malloc(WAV_IO_BYTES);
    mono_buf = rt_malloc(WAV_IO_BYTES);
    if (in_buf == RT_NULL || mono_buf == RT_NULL) {
        rt_kprintf(WAV_TAG "buffer alloc failed\r\n");
        goto __exit;
    }

    rt_kprintf(WAV_TAG "play start: %s\r\n", s_wav_path);
    rt_kprintf(WAV_TAG "src: %u Hz, %u ch, %u bits, data=%u B\r\n",
               (unsigned)info.sample_rate,
               (unsigned)info.channels,
               (unsigned)info.bits_per_sample,
               (unsigned)info.data_size);
    if (info.channels == 2) {
        rt_kprintf(WAV_TAG "stereo source will be mixed to mono, then duplicated to L/R\r\n");
    }

    left = info.data_size;
    while (!s_wav_stop_req && left > 0) {
        rt_size_t to_read = (left > WAV_IO_BYTES) ? WAV_IO_BYTES : left;
        rt_size_t got = fread(in_buf, 1, to_read, fp);
        rt_size_t out_bytes = 0;

        if (got == 0) {
            break;
        }

        if (info.channels == 1) {
            out_bytes = got;
            rt_memcpy(mono_buf, in_buf, out_bytes);
        } else {
            rt_uint32_t frames = got / 4U;
            out_bytes = mix_stereo_to_mono((const rt_int16_t *)in_buf, frames, mono_buf);
        }

        if (out_bytes > 0) {
            rt_device_write(dev, 0, mono_buf, out_bytes);
        }

        left -= got;
    }

    rt_kprintf(WAV_TAG "%s\r\n", s_wav_stop_req ? "play stopped" : "play end");

__exit:
    if (fp != RT_NULL) {
        fclose(fp);
    }
    if (dev != RT_NULL) {
        rt_device_close(dev);
    }
    if (in_buf != RT_NULL) {
        rt_free(in_buf);
    }
    if (mono_buf != RT_NULL) {
        rt_free(mono_buf);
    }

    s_wav_stop_req = RT_FALSE;
    s_wav_thread = RT_NULL;
}

static int wavplay_cmd(int argc, char **argv)
{
    const char *path = "/test.wav";

    if (argc >= 2) {
        path = argv[1];
    }

    if (s_wav_thread != RT_NULL) {
        rt_kprintf(WAV_TAG "player busy, use wavstop first\r\n");
        return 0;
    }

    if (rt_strlen(path) >= sizeof(s_wav_path)) {
        rt_kprintf(WAV_TAG "path too long\r\n");
        return 0;
    }

    rt_strncpy(s_wav_path, path, sizeof(s_wav_path) - 1);
    s_wav_path[sizeof(s_wav_path) - 1] = '\0';
    s_wav_stop_req = RT_FALSE;

    s_wav_thread = rt_thread_create("wavplay",
                                    wav_play_worker, RT_NULL,
                                    WAV_THREAD_STACK,
                                    WAV_THREAD_PRIO, 10);
    if (s_wav_thread == RT_NULL) {
        rt_kprintf(WAV_TAG "create player thread failed\r\n");
        return 0;
    }

    rt_thread_startup(s_wav_thread);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(wavplay_cmd, wavplay, play wav file from udisk default /test.wav);

static int wavstop_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_wav_thread == RT_NULL) {
        rt_kprintf(WAV_TAG "player idle\r\n");
        return 0;
    }

    s_wav_stop_req = RT_TRUE;
    rt_kprintf(WAV_TAG "stop requested\r\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(wavstop_cmd, wavstop, stop wav playback);
