#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include "usbh_core.h"

#define LED_PIN_B                 GET_PIN(16, 5)
#define USB_HOST_BUSID            0

static rt_bool_t s_usb_enabled = RT_FALSE;

static int usb_cmd(int argc, char **argv)
{
    int ret;

    if (argc != 2) {
        rt_kprintf("usage: usb <on|off>\r\n");
        rt_kprintf("usb is %s\r\n", s_usb_enabled ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        if (s_usb_enabled) {
            rt_kprintf("usb already on\r\n");
            return 0;
        }
        ret = usbh_initialize(USB_HOST_BUSID, USBHS_BASE, RT_NULL);
        if (ret == 0) {
            s_usb_enabled = RT_TRUE;
            rt_kprintf("usb on\r\n");
        } else {
            rt_kprintf("usb on failed: %d\r\n", ret);
        }
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        if (!s_usb_enabled) {
            rt_kprintf("usb already off\r\n");
            return 0;
        }
        ret = usbh_deinitialize(USB_HOST_BUSID);
        if (ret == 0) {
            s_usb_enabled = RT_FALSE;
            rt_kprintf("usb off\r\n");
        } else {
            rt_kprintf("usb off failed: %d\r\n", ret);
        }
        return 0;
    }

    rt_kprintf("usage: usb <on|off>\r\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(usb_cmd, usb, usb host control: usb on | usb off);

int main(void)
{
    rt_kprintf("Hello RT-Thread\r\n");
    rt_kprintf("This core is cortex-m33\n");
    rt_kprintf("USB host default: off, use 'usb on' to enable\r\n");

    rt_pin_mode(LED_PIN_B, PIN_MODE_OUTPUT);
    while (1)
    {
        rt_pin_write(LED_PIN_B, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN_B, PIN_LOW);
        rt_thread_mdelay(500);
    }
    return 0;
}
