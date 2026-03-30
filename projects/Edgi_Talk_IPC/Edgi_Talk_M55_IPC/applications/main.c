#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>

#include "drv_ipc.h"

#define LED_PIN_G               GET_PIN(16, 6)

static rt_device_t g_ipc_dev = RT_NULL;
static rt_thread_t g_ipc_thread = RT_NULL;

static void ipc_demo_run(void)
{
    edge_rc_frame_t rx_frame;
    edge_rc_frame_t tx_frame;
    rt_uint32_t rx_count = 0;
    rt_uint32_t tx_count = 0;

    rt_kprintf("\r\n");
    rt_kprintf("========================================\r\n");
    rt_kprintf("[M55] IPC Listener Running\r\n");
    rt_kprintf("----------------------------------------\r\n");
    rt_kprintf("Mode:     Receiver & Responder\r\n");
    rt_kprintf("Peer:     Cortex-M33\r\n");
    rt_kprintf("Response: Hello M55\r\n");
    rt_kprintf("========================================\r\n");
    rt_kprintf("\r\n");

    while (1) {
        /* 接收消息 */
        if (rt_device_read(g_ipc_dev, 0, &rx_frame, 1) == 1) {
            /* 验证消息 */
            if (rx_frame.magic == RC_MAGIC_WORD &&
                rx_frame.role == RC_ROLE_M33 &&
                edge_rc_checksum(&rx_frame) == rx_frame.checksum) {

                /* 检查是否是 "Hello M33" 消息 */
                if (rx_frame.channel[0] == 0x4865UL &&  /* 'He' */
                    rx_frame.channel[1] == 0x6C6CUL &&  /* 'll' */
                    rx_frame.channel[2] == 0x6F20UL &&  /* 'o ' */
                    rx_frame.channel[3] == 0x4D33UL &&  /* 'M3' */
                    rx_frame.channel[4] == 0x0033UL) {  /* '3' */

                    rt_kprintf("[M55] RX <- [M33]: \"Hello M33\" | Seq: %5lu | Time: %8lu ms\r\n",
                               rx_frame.seq, rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
                    rx_count++;

                    /* 准备回复消息 - "Hello M55" */
                    memset(&tx_frame, 0, sizeof(tx_frame));
                    tx_frame.client_id = CM33_IPC_PIPE_CLIENT_ID;
                    tx_frame.role = RC_ROLE_M55_ECHO;
                    tx_frame.magic = RC_MAGIC_WORD;
                    tx_frame.seq = rx_frame.seq;
                    /* Encode "Hello M55" into channels (2 chars per channel) */
                    tx_frame.channel[0] = 0x4865UL;  /* 'He' in ASCII */
                    tx_frame.channel[1] = 0x6C6CUL;  /* 'll' in ASCII */
                    tx_frame.channel[2] = 0x6F20UL;  /* 'o ' in ASCII */
                    tx_frame.channel[3] = 0x4D35UL;  /* 'M5' in ASCII */
                    tx_frame.channel[4] = 0x0035UL;  /* '5'  in ASCII */
                    tx_frame.checksum = edge_rc_checksum(&tx_frame);

                    /* 发送回复 */
                    if (rt_device_write(g_ipc_dev, 0, &tx_frame, 1) == 1) {
                        rt_kprintf("[M55] TX -> [M33]: \"Hello M55\" | Seq: %5lu | Time: %8lu ms\r\n",
                                   tx_frame.seq, rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
                        tx_count++;

                        /* 每 10 次交互打印统计信息 */
                        if (tx_count % 10 == 0) {
                            rt_kprintf("----------------------------------------\r\n");
                            rt_kprintf("[M55] Statistics: RX=%lu, TX=%lu\r\n", rx_count, tx_count);
                            rt_kprintf("----------------------------------------\r\n");
                        }
                    } else {
                        rt_kprintf("[M55] Send reply failed\r\n");
                    }
                }
            } else {
                rt_kprintf("[M55] Invalid message received\r\n");
            }
        }

        rt_thread_mdelay(10);
    }
}

static void ipc_demo_entry(void* parameter)
{
    (void)parameter;
    ipc_demo_run();
}

static int ipc_test_run(void)
{
    /* 查找 IPC 设备 */
    g_ipc_dev = edge_ipc_device_find();
    if (g_ipc_dev == RT_NULL) {
        if (edge_ipc_device_register() != RT_EOK) {
            rt_kprintf("[M55] IPC: Device register failed\r\n");
            return -RT_ERROR;
        }
        g_ipc_dev = edge_ipc_device_find();
        if (g_ipc_dev == RT_NULL) {
            rt_kprintf("[M55] IPC: Device not found\r\n");
            return -RT_ERROR;
        }
    }

    /* 打开 IPC 设备 */
    if (rt_device_open(g_ipc_dev, RT_DEVICE_OFLAG_RDWR) != RT_EOK) {
        rt_kprintf("[M55] IPC: Open device failed\r\n");
        return -RT_ERROR;
    }

    /* 创建 IPC 处理线程 */
    if (g_ipc_thread == RT_NULL) {
        g_ipc_thread = rt_thread_create("ipc_demo",
                                        ipc_demo_entry,
                                        RT_NULL,
                                        2048,
                                        25,
                                        10);
        if (g_ipc_thread == RT_NULL) {
            rt_kprintf("[M55] IPC: Create thread failed\r\n");
            return -RT_ENOMEM;
        }

        rt_thread_startup(g_ipc_thread);
    }

    return RT_EOK;
}
MSH_CMD_EXPORT(ipc_test_run, Start M55 IPC test);

int main(void)
{
    rt_kprintf("\r\n");
    rt_kprintf("========================================\r\n");
    rt_kprintf("   RT-Thread on Cortex-M55 Core         \r\n");
    rt_kprintf("========================================\r\n");
    rt_kprintf("Command: ipc_test_run\r\n");
    rt_kprintf("Status:  Running\r\n");
    rt_kprintf("========================================\r\n");
    rt_kprintf("\r\n");

    rt_pin_mode(LED_PIN_G, PIN_MODE_OUTPUT);

    while (1)
    {
        rt_pin_write(LED_PIN_G, PIN_LOW);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN_G, PIN_HIGH);
        rt_thread_mdelay(500);
    }

    return 0;
}
