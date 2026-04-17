/*
 * UAC microphone host demo
 *
 * Streams ISO IN audio packets from a UAC 1.0 microphone (e.g.
 * VID/PID 3769:b01d, 48 kHz / 16-bit / stereo, 208 B per 1 ms frame)
 * through the patched DWC2 host controller and exposes simple msh
 * commands (`mic_stat`, `mic_sample`) to verify the data really flows.
 *
 * Designed for the Edgi_Talk_M33_USB_H project. Implements the weak
 * symbols `usbh_audio_run` / `usbh_audio_stop` exported by CherryUSB's
 * usbh_audio class driver.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_audio.h"

#define MIC_TAG                "[mic] "

#define MIC_SAMPLE_RATE        48000U
#define MIC_BIT_RES            16U
#define MIC_PACKET_SIZE        208U /* matches descriptor wMaxPacketSize */
#define MIC_ISO_PACKETS_PER_URB 4U  /* 4 ms per urb */
#define MIC_ISO_URB_COUNT      2U   /* double-buffer to keep stream flowing */

#define MIC_URB_STORAGE_BYTES                                                   \
    (sizeof(struct usbh_urb) +                                                  \
     MIC_ISO_PACKETS_PER_URB * sizeof(struct usbh_iso_frame_packet))

/* Audio sample buffers must be DMA reachable and (when the platform has cache)
 * placed in non-cacheable memory; USB_NOCACHE_RAM_SECTION resolves to
 * .cy_socmem_data in this BSP, which is shared between cores and DMA safe. */
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
static uint8_t s_iso_buf[MIC_ISO_URB_COUNT][MIC_ISO_PACKETS_PER_URB * MIC_PACKET_SIZE];

/* The urb itself does not need to live in nocache memory because the
 * controller never DMAs into it; only iso_packet[].transfer_buffer is
 * accessed by hardware. We do need the trailing iso_packet[] flexible
 * array to have stable storage, which is what the byte arrays below
 * provide. */
static USB_MEM_ALIGNX uint8_t s_urb_storage[MIC_ISO_URB_COUNT][MIC_URB_STORAGE_BYTES];

#define MIC_URB(i) ((struct usbh_urb *)(void *)s_urb_storage[i])

static struct usbh_audio *s_audio_class;
static rt_thread_t s_worker;

/* These counters are touched from the IRQ-driven completion callback and
 * read by msh commands; volatile is enough since 32-bit reads are atomic
 * on Cortex-M33. */
static volatile uint32_t s_total_bytes;
static volatile uint32_t s_total_frames;
static volatile uint32_t s_total_errors;
static volatile uint32_t s_last_bytes;
static volatile bool s_running;

static void mic_iso_complete(void *arg, int nbytes)
{
    struct usbh_urb *urb = (struct usbh_urb *)arg;

    if (!s_running || urb == NULL) {
        return;
    }

    if (nbytes >= 0) {
        for (uint32_t i = 0; i < urb->num_of_iso_packets; i++) {
            if (urb->iso_packet[i].errorcode == 0) {
                s_total_bytes += urb->iso_packet[i].actual_length;
                s_total_frames++;
            } else {
                s_total_errors++;
            }
        }
    } else {
        /* whole urb aborted (e.g. NOTCONN on disconnect): stop the loop */
        s_total_errors++;
        if (nbytes == -USB_ERR_NOTCONN || nbytes == -USB_ERR_SHUTDOWN) {
            s_running = false;
            return;
        }
    }

    /* Re-arm immediately to keep the iso schedule continuous.
     * usbh_submit_urb is safe to call from the dwc2 IRQ context: it only
     * uses a critical section and register writes, no blocking. */
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
    urb->timeout = 0; /* async */
    urb->complete = mic_iso_complete;
    urb->arg = urb;

    for (uint32_t p = 0; p < MIC_ISO_PACKETS_PER_URB; p++) {
        urb->iso_packet[p].transfer_buffer = &s_iso_buf[idx][p * MIC_PACKET_SIZE];
        urb->iso_packet[p].transfer_buffer_length = mps;
        urb->iso_packet[p].actual_length = 0;
        urb->iso_packet[p].errorcode = 0;
    }
}

static void mic_worker_entry(void *arg)
{
    struct usbh_audio *audio_class = (struct usbh_audio *)arg;
    int ret;

    /* Give the host stack a moment to settle after enumeration. */
    rt_thread_mdelay(100);

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

    s_total_bytes = 0;
    s_total_frames = 0;
    s_total_errors = 0;
    s_last_bytes = 0;
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

    /* Streaming is now driven entirely from the IRQ completion callback;
     * this thread just waits for the stop signal so we can tear down. */
    while (s_running) {
        rt_thread_mdelay(200);
    }

    rt_kprintf(MIC_TAG "stream stopped, totals: frames=%u bytes=%u errors=%u\r\n",
               s_total_frames, s_total_bytes, s_total_errors);
}

void usbh_audio_run(struct usbh_audio *audio_class)
{
    if (audio_class == NULL) {
        return;
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
    s_running = false;
    /* Cancel any in-flight urbs so the controller stops accessing buffers
     * before the audio_class storage is freed by the class driver. */
    for (uint32_t u = 0; u < MIC_ISO_URB_COUNT; u++) {
        struct usbh_urb *urb = MIC_URB(u);
        if (urb->hport != NULL && urb->hcpriv != NULL) {
            usbh_kill_urb(urb);
        }
    }
    s_audio_class = NULL;
}

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
    rt_kprintf(MIC_TAG "frames=%u bytes_total=%u rate_since_last=%u B errors=%u\r\n",
               s_total_frames, cur, delta, s_total_errors);
    return 0;
}
MSH_CMD_EXPORT(mic_stat, print uac mic stream stats since last call);

static int mic_sample(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_audio_class == NULL || !s_running) {
        rt_kprintf(MIC_TAG "no active mic stream\r\n");
        return 0;
    }
    rt_kprintf(MIC_TAG "urb0 first 32 bytes:");
    for (int i = 0; i < 32; i++) {
        if ((i & 0xf) == 0) {
            rt_kprintf("\r\n  ");
        }
        rt_kprintf("%02x ", s_iso_buf[0][i]);
    }
    rt_kprintf("\r\n");
    return 0;
}
MSH_CMD_EXPORT(mic_sample, hexdump first 32 bytes of mic urb0 buffer);
