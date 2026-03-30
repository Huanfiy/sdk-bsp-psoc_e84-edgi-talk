# Edgi_Talk_M55_IPC Dual-Core Communication Example Project

[**中文**](./README_zh.md) | **English**

## Overview

This project implements **IPC (Inter-Processor Communication)** dual-core communication on the **Edgi-Talk M55 core**, demonstrating message passing between **Cortex-M55** and **Cortex-M33**.

M55 acts as the **receiver and responder**, receiving "Hello M33" messages from M33 and replying with "Hello M55".

## Default Configuration

* Response mode: Automatic reply
* Message format: ASCII encoded text messages
* Uses Infineon PSoC E84 on-chip IPC Pipe hardware

## Build and Flash

1. Build the project using RT-Thread Studio or SCons.
2. Flash firmware to M55 core via KitProg3 (DAP).
3. Both M33 and M55 projects must be built and flashed for dual-core communication.
4. Connect serial port to view communication logs as shown below:

![alt text](figures/1.png)

## Usage

### 1. Configuration and Build

Open in RT-Thread Studio:

```
RT-Thread Settings -> Hardware Drivers Config -> On-chip Peripheral Drivers
```

Check `Enable IPC` option.

### 2. Run Example

In M55 serial terminal, type:

```
msh> ipc_test_run
```

M55 will start a listener thread, waiting for messages from M33 and automatically replying.

### 3. Observe Output

**M55 Output Example**:

```
========================================
[M55] IPC Listener Running
----------------------------------------
Mode:     Receiver & Responder
Peer:     Cortex-M33
Response: Hello M55
========================================

[M55] RX <- [M33]: "Hello M33" | Seq:     1 | Time:     1250 ms
[M55] TX -> [M33]: "Hello M55" | Seq:     1 | Time:     1252 ms
[M55] RX <- [M33]: "Hello M33" | Seq:     2 | Time:     2250 ms
[M55] TX -> [M33]: "Hello M55" | Seq:     2 | Time:     2252 ms
----------------------------------------
[M55] Statistics: RX=10, TX=10
----------------------------------------
```

**Corresponding M33 Output**:

```
========================================
[M33] IPC Demo Started
----------------------------------------
Mode:     Sender
Target:   Cortex-M55
Message:  Hello M33
Interval: 1000 ms
========================================

[M33] TX -> [M55]: "Hello M33" | Seq:     1 | Time:     1234 ms
[M33] RX <- [M55]: "Hello M55" | Seq:     1 | Time:     1256 ms
[M33] TX -> [M55]: "Hello M33" | Seq:     2 | Time:     2234 ms
[M33] RX <- [M55]: "Hello M55" | Seq:     2 | Time:     2256 ms
----------------------------------------
[M33] Statistics: TX=10, RX=10
----------------------------------------
```

## Data Protocol

IPC communication uses the `edge_rc_frame_t` structure:

```c
typedef struct {
    uint8_t client_id;          // Client ID
    uint16_t intr_mask;         // Interrupt mask
    uint8_t role;               // Role identifier (M33/M55_ECHO)
    uint32_t magic;             // Magic word (0x5243444DU)
    uint32_t seq;               // Sequence number
    uint16_t channel[8];        // 8-channel data (stores ASCII messages)
    uint32_t checksum;          // Checksum
} edge_rc_frame_t;
```

Message "Hello M55" encoding:

```c
channel[0] = 0x4865  /* 'He' */
channel[1] = 0x6C6C  /* 'll' */
channel[2] = 0x6F20  /* 'o '  (space) */
channel[3] = 0x4D35  /* 'M5' */
channel[4] = 0x0035  /* '5' */
```

## Startup Sequence

M55 depends on M33 boot flow. Flash in this order:

```
+------------------+
|   Secure M33     |
|  (Secure Core)   |
+------------------+
		  |
		  v
+------------------+
|       M33        |
|   (IPC Sender)   |
+------------------+
		  |
		  v
+-------------------+
|       M55         |
|  (IPC Receiver)   |
+-------------------+
```

## Hardware Connection

This example uses Infineon PSoC E84 on-chip IPC hardware:

* Uses IPC Pipe hardware in CM0P
* CM33 uses EP1 (Endpoint 1)
* CM55 uses EP2 (Endpoint 2)
* Uses semaphores for mutual exclusion
* No external wiring required

## Working Principle

1. **Receive messages**: M55 continuously reads from IPC device in a separate thread
2. **Verify messages**: Check magic word, role identifier, and checksum
3. **Parse messages**: Verify if it's a "Hello M33" message
4. **Send reply**: Construct "Hello M55" message and reply to M33
5. **Statistics**: Print statistics every 10 messages processed

## Configuration Parameters

The following parameters can be modified in `libraries/HAL_Drivers/ipc_common.h`:

* `EDGE_IPC_FRAME_POOL_SIZE`: Transmit buffer pool size (default 64)
* `EDGE_IPC_RX_QUEUE_SIZE`: Receive queue size (default 128)
* `EDGE_IPC_SEMA_RETRY_MAX`: Semaphore retry count (default 2)
