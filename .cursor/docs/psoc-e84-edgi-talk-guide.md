# PSoC Edge E84 Edgi-Talk SDK 项目指南

> 面向项目新人的全面参考，涵盖芯片架构、内存映射、构建烧录、多核启动等核心知识。

---

## 目录

1. [项目概览](#1-项目概览)
2. [芯片架构与多核系统](#2-芯片架构与多核系统)
3. [内存映射详解](#3-内存映射详解)
4. [地址别名与总线视图](#4-地址别名与总线视图)
5. [为什么需要 hex-relocate](#5-为什么需要-hex-relocate)
6. [安全侧与非安全侧](#6-安全侧与非安全侧)
7. [启动流程](#7-启动流程)
8. [SDK 目录结构](#8-sdk-目录结构)
9. [构建系统](#9-构建系统)
10. [固件后处理与烧录](#10-固件后处理与烧录)
11. [多核通信 (IPC)](#11-多核通信-ipc)
12. [外设驱动分层](#12-外设驱动分层)
13. [常见问题](#13-常见问题)

---

## 1. 项目概览

本项目是 **PSoC Edge E84 (Edgi-Talk)** 开发板的 **RT-Thread BSP/SDK**。

- **芯片**: PSE846GPS2DBZC4A（PSoC Edge E84 EPC2 安全等级）
- **操作系统**: RT-Thread v5.x
- **开发板**: KIT_PSE84_EVAL_EPC2（SODIMM SOM + 底板）
- **板载资源**: 128 Mb QSPI Flash, 1 Gb Octal Flash, 128 Mb Octal RAM, CYW55513 Wi-Fi/BT Combo, KitProg3 调试器, MIPI-DSI 显示接口, USB Host/Device, Ethernet, 音频 Codec, IMU 等

---

## 2. 芯片架构与多核系统

PSoC Edge E84 是一颗 **三核异构 MCU**：

```
┌─────────────────────────────────────────────┐
│              PSoC Edge E84 SoC              │
│                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │  M0+ SEC │  │  CM33    │  │  CM55    │  │
│  │ (安全启动│  │ (主控核) │  │ (应用核) │  │
│  │  ROM 固件│  │ TrustZone│  │ 无 TZ    │  │
│  │  不可编程│  │ S + NS   │  │ 仅 NS    │  │
│  └──────────┘  └──────────┘  └──────────┘  │
│       ↓              ↓              ↓       │
│    安全启动      S 侧初始化     由 CM33      │
│    验签引导      NS 侧应用     释放/启动     │
└─────────────────────────────────────────────┘
```

| 核心 | 架构 | TrustZone | 角色 |
|------|------|-----------|------|
| M0+ Secure | Cortex-M0+ | — | ROM 安全引导，不可编程 |
| CM33 | Cortex-M33 r1p0 | 有 (S + NS) | 主控核：安全侧初始化 + 非安全侧运行 RT-Thread |
| CM55 | Cortex-M55 r1p0 | 无 (仅 NS) | 应用/算力核：AI/DSP/图形等，由 CM33 释放 |

CM55 有 **ITCM** (256KB) 和 **DTCM** (256KB) 紧耦合存储器，适合高性能代码与数据存放。

---

## 3. 内存映射详解

### 3.1 片内存储器

| 区域 | NS S-BUS 基址 | S S-BUS 基址 | NS C-BUS 基址 | S C-BUS 基址 | 大小 |
|------|--------------|-------------|--------------|-------------|------|
| **RRAM (NVM)** | 0x22000000 | 0x32000000 | 0x02000000 | 0x12000000 | 512 KB |
| **SRAM** (SRAM0+SRAM1) | 0x24000000 | 0x34000000 | 0x04000000 | 0x14000000 | 1 MB |
| **SoCMem** | 0x26000000 | 0x36000000 | 0x06000000 | 0x16000000 | 5 MB |
| **CM55 ITCM** (外部视图) | 0x48000000 | 0x58000000 | 0x68000000 | 0x78000000 | 256 KB |
| **CM55 DTCM** (外部视图) | 0x48040000 | — | — | — | 256 KB |

> CM55 内部看到的 ITCM 起始于 `0x00000000`，DTCM 起始于 `0x20000000`。外部视图是其他核或编程工具访问时用的地址。

### 3.2 外部 Flash（SMIF/QSPI）

同一块外部 Flash 有 **4 种地址别名**（详见第 4 节）：

| 访问方式 | 基址范围 | Bank 0 | Bank 1 | Bank 2 | Bank 3 |
|---------|---------|--------|--------|--------|--------|
| NS + CBUS | 0x08... | 0x08000000 | 0x0C000000 | 0x18000000 | 0x1C000000 |
| NS + SBUS | 0x60... | 0x60000000 | 0x64000000 | 0x70000000 | 0x74000000 |
| S + CBUS  | 0x18... | — | — | — | — |
| S + SBUS  | 0x70... | — | — | — | — |

### 3.3 RRAM 分区布局（EPC2 安全等级）

```
0x22000000 ┌─────────────────────────┐
           │  Boot 固件保留区         │  ← 不可触碰
0x22011000 ├─────────────────────────┤
           │  用户可编程区 (MAIN)     │  356 KB (EPC2)
           │  Extended Boot + App    │
0x22069FFF ├─────────────────────────┤
           │  PROTECTED 可回收区      │  168 KB (EPC2 可回收)
0x2206FFFF └─────────────────────────┘
```

### 3.4 SRAM 分区（链接脚本视图，M33 NS 工程）

| 区域 | 地址 | 大小 | 用途 |
|------|------|------|------|
| m33_code | 0x24058000 (C-BUS 0x04058000) | 404 KB | CM33 NS 运行时代码（从外部 Flash 拷入） |
| m33_data | 0x240BD000 (C-BUS 0x040BD000) | 256 KB | CM33 NS 数据（.data/.bss/heap/stack） |
| m33_allocatable_shared | 0x240FE000 | 4 KB | CM33 可分配的全核共享区 |

### 3.5 SoCMem 分区（M55 链接脚本视图）

| 区域 | 地址 | 大小 | 用途 |
|------|------|------|------|
| m55_code_secondary | 0x26000000 | 384 KB | CM55 辅助代码区（性能次于 ITCM） |
| m55_data_secondary | 0x26060000 | 1408 KB | CM55 辅助数据区 + Heap（性能次于 DTCM） |
| m33_m55_shared | 0x261C0000 | 256 KB | CM33 NS 与 CM55 共享数据区 |
| gfx_mem | 0x26200000 | 3 MB | GPU/图形缓冲区（CM33 NS 与 CM55 共享） |

### 3.6 外部 Flash 分区（各核）

| 区域 | CBUS 地址 | SBUS 地址 | 大小 | 用途 |
|------|----------|----------|------|------|
| m33s_nvm | — | — | — | CM33 安全侧代码+数据+NSC（预置签名镜像） |
| **m33_nvm** | 0x08340000 | 0x60340000 | 2 MB | CM33 NS 主代码（XIP 或 copy to SRAM） |
| m33_trailer | 0x08540000 | 0x60540000 | 256 KB | CM33 NS 签名 trailer |
| **m55_nvm** | — | 0x60580000 | 8 MB | CM55 主代码 |
| m55_trailer | — | 0x60D80000 | 256 KB | CM55 签名 trailer |

---

## 4. 地址别名与总线视图

这是理解本芯片内存的**最重要概念之一**。同一块物理存储有最多 **4 种地址别名**：

```
              ┌─────────────────┐
              │  物理存储器       │
              │ (RRAM/SRAM/Flash)│
              └────────┬────────┘
         ┌─────────────┼─────────────┐
         │             │             │
    ┌────┴────┐  ┌────┴────┐  ┌────┴────┐
    │  CBUS   │  │  SBUS   │  │  安全   │
    │ (代码总线)│  │ (系统总线)│  │ 属性   │
    └────┬────┘  └────┬────┘  └────┬────┘
         │             │             │
    只读、快速      读写、可编程    S / NS
    适合 XIP        适合烧录写入
```

以外部 Flash 为例：

| 别名 | 地址前缀 | 读 | 写 | 性能 | 典型场景 |
|------|---------|---|---|------|---------|
| **NS CBUS** | 0x08... | 可 | **不可** | **高** | 运行时代码执行 (XIP) |
| **NS SBUS** | 0x60... | 可 | **可** | 较低 | 编程/烧录写入 |
| **S CBUS** | 0x18... | 可 | **不可** | **高** | 安全侧代码执行 |
| **S SBUS** | 0x70... | 可 | **可** | 较低 | 安全侧编程/烧录写入 |

**链接器选择 CBUS 地址**（执行快），**烧录工具需要 SBUS 地址**（才能写入）——这就是 hex-relocate 存在的原因。

---

## 5. 为什么需要 hex-relocate

### 问题

编译器/链接器按 **CBUS 地址** 生成代码（运行时高速路径），例如 M33 NS 代码链接到 `0x08340000`。但 OpenOCD 通过 **SMIF Flash Bank** 写外部 Flash 时，使用的是 **SBUS 地址** `0x60340000`。CBUS 是只读的，无法写入。

### hex-relocate 做了什么

`edgeprotecttools run-config` 按 `boot_with_extended_boot_scons.json` 中的映射表，把 hex 文件里每个地址段从 CBUS 地址**平移**到对应的 SBUS 地址：

| 源 (CBUS) | 目标 (SBUS) | 对应存储器 |
|-----------|------------|-----------|
| 0x02000000 → | 0x22000000 | RRAM NS |
| 0x04000000 → | 0x24000000 | SRAM NS |
| 0x06000000 → | 0x26000000 | SoCMem NS |
| **0x08000000** → | **0x60000000** | **外部 Flash Bank 0 NS** |
| 0x0C000000 → | 0x64000000 | 外部 Flash Bank 1 NS |
| 0x12000000 → | 0x32000000 | RRAM S |
| 0x14000000 → | 0x34000000 | SRAM S |
| 0x16000000 → | 0x36000000 | SoCMem S |
| 0x18000000 → | 0x70000000 | 外部 Flash Bank 2 S |
| 0x1C000000 → | 0x74000000 | 外部 Flash Bank 3 S |

### merge 做了什么

relocate 之后，还要把**安全侧预编译签名镜像** `proj_cm33_s_signed.hex`（位于 `tools/edgeprotecttools/cm33_s_signed_fw/`）与用户的 NS 应用 hex 合并，生成一个完整的 `build/rtthread.hex`，烧录时一次写入全部内容。

### 流程图

```
rtthread.hex (CBUS 地址)
       │
       ▼  hex-relocate
build/rtthread.hex (SBUS 地址)
       │
       ▼  merge
build/rtthread.hex (NS app + S-side 签名镜像)
       │
       ▼  OpenOCD flash write_image
     芯片 Flash
```

---

## 6. 安全侧与非安全侧

### CM33 TrustZone 架构

CM33 具有 ARMv8-M **TrustZone** 安全扩展，将执行环境分为：

- **Secure (S)**：安全侧，负责安全启动验签、密钥管理、安全服务（SE RT Services）
- **Non-Secure (NS)**：非安全侧，运行用户应用（如 RT-Thread）

地址空间中，**bit[28]** 通常区分 S/NS：
- `0x2xxxxxxx` = NS S-BUS, `0x3xxxxxxx` = S S-BUS
- `0x0xxxxxxx` = NS C-BUS, `0x1xxxxxxx` = S C-BUS

### CM55 无 TrustZone

CM55 **没有**安全扩展，仅工作在 NS 模式，行为类似 CM33 的 NS 部分。

### 预置的安全侧镜像

SDK 自带 `tools/edgeprotecttools/cm33_s_signed_fw/proj_cm33_s_signed.hex`，这是已签名的 CM33 安全侧固件。日常开发中**不需要重新编译此部分**，只需在后处理时与 NS 应用合并即可。

---

## 7. 启动流程

```
┌─────────────────────┐
│  上电 / 复位         │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│  M0+ Secure ROM     │  ① ROM Boot：验签、初始化 NVM、
│  (安全引导)          │     加载 SE RT Services & Extended Boot
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│  Extended Boot      │  ② 验签 CM33 S 侧镜像并加载
│  (RRAM 中)          │
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│  CM33 Secure (S)    │  ③ s_start_pse84.c: 建立安全向量表、
│                     │     TrustZone 分区、SystemInit
└──────────┬──────────┘
           ▼
┌─────────────────────┐
│  CM33 Non-Secure    │  ④ ns_start_pse84.c: NS 向量表 → SRAM、
│  (NS)               │     SystemInit → RT-Thread main()
└──────────┬──────────┘
           ▼ (可选，取决于 SOC_Enable_CM55 配置)
┌─────────────────────┐
│  CM55               │  ⑤ CM33 调用 Cy_SysEnableCM55()
│  (应用核)           │     释放 CM55 电源域并跳转
└─────────────────────┘
```

### 关键点

- **CM55 不会自动启动**：必须由 CM33 的 `board.c` 中 `Cy_SysEnableCM55()` 显式释放
- **Kconfig 控制**：`RT-Thread Settings → 硬件 → SOC Multi Core Mode → Enable CM55 Core`
- **烧录顺序**：必须先烧 M33 工程（含 S 侧），再烧 M55 工程
- **M55 无法独立运行**：如果 M33 侧没有正确初始化和释放 CM55，M55 工程不会有任何现象

---

## 8. SDK 目录结构

```
sdk-bsp-psoc_e84-edgi-talk/
├── docs/                           # 原理图、手册等板级文档
├── libraries/
│   ├── HAL_Drivers/                # RT-Thread 设备驱动适配层 (drv_*.c)
│   │   ├── drv_gpio.c             #   GPIO
│   │   ├── drv_uart.c             #   UART
│   │   ├── drv_spi.c              #   SPI
│   │   ├── drv_i2c.c              #   I2C (硬件)
│   │   ├── drv_soft_i2c.c         #   I2C (软件模拟)
│   │   ├── drv_adc.c              #   ADC
│   │   ├── drv_pwm.c              #   PWM
│   │   ├── drv_rtc.c              #   RTC
│   │   ├── drv_sdio.c             #   SDIO
│   │   ├── drv_canfd.c            #   CAN-FD
│   │   ├── drv_hwtimer.c          #   硬件定时器
│   │   ├── drv_i2s.c              #   I2S 音频
│   │   ├── drv_pdm.c              #   PDM 麦克风
│   │   ├── drv_lcd.c              #   LCD/显示
│   │   ├── drv_ipc.c              #   核间通信
│   │   └── drv_wdt.c              #   看门狗
│   ├── Common/board/ports/         # 板级外设移植
│   │   ├── audio/                  #   ES8388 Codec
│   │   ├── display_panels/         #   显示屏 + 触摸
│   │   ├── drv_hyperam.c           #   HyperRAM
│   │   ├── fal/                    #   Flash 抽象层
│   │   ├── lvgl/                   #   LVGL 移植
│   │   └── usb/                    #   USB 配置
│   ├── components/                 # Infineon + 第三方组件
│   │   ├── mtb-device-support-pse8xxgp/  # PSE84 PDL + HAL
│   │   ├── CherryUSB-1.6.0/       #   USB 协议栈
│   │   ├── wifi-host-driver/       #   Wi-Fi 驱动
│   │   ├── mtb-ipc/               #   核间通信库
│   │   ├── mtb-srf/               #   安全请求框架 (NS→S)
│   │   ├── littlefs/              #   LittleFS 文件系统
│   │   ├── lvgl_9.2.0/            #   LVGL 图形库
│   │   └── ...
│   ├── M33_Config/                 # M33 Kconfig
│   └── M55_Config/                 # M55 Kconfig
├── projects/                       # 示例工程
│   ├── Edgi_Talk_M33_Blink_LED/    # ★ M33 最小系统 LED 闪烁
│   ├── Edgi_Talk_M55_Blink_LED/    #   M55 LED 闪烁
│   ├── Edgi_Talk_CherryUSB/        #   USB Host/Device 示例
│   ├── Edgi_Talk_IPC/              #   核间通信示例
│   ├── Edgi_Talk_M55_XiaoZhi/      #   AI 语音助手
│   ├── Edgi_Talk_M55_LVGL/         #   图形界面
│   ├── libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/  # BSP 板级支持包
│   └── ...
├── rt-thread/                      # RT-Thread 内核源码
├── tools/
│   ├── edgeprotecttools/           # 安全工具 + 签名镜像
│   │   ├── cm33_s_signed_fw/       #   预置 CM33 S 侧签名固件
│   │   └── src/                    #   Python 源码（可 pip install）
│   ├── device-configurator/        # 图形化设备配置器
│   └── merge-hex/                  # HEX 合并工具
├── resources/                      # USB 图形工具、板级描述等
└── sdk-bsp-psoc_e84-edgi-talk.yaml # 硬件描述文件
```

---

## 9. 构建系统

### 工具链

- **编译器**: ARM GCC (`arm-none-eabi-gcc`)，推荐 13.x+
- **构建工具**: SCons（Python 构建系统，RT-Thread 标准）
- **环境变量**: `RTT_EXEC_PATH` 指向工具链 `bin/` 目录

### 编译命令

```bash
cd projects/Edgi_Talk_M33_Blink_LED
export RTT_EXEC_PATH=/path/to/arm-none-eabi/bin
scons -j$(nproc)
```

### 产出文件

| 文件 | 说明 |
|------|------|
| `rt-thread.elf` | 带调试信息的 ELF（链接视图 CBUS 地址） |
| `rtthread.hex` | 根目录，CBUS 地址，不可直接烧录 |
| `build/rtthread.hex` | 后处理后，SBUS 地址 + S 侧合并，**可烧录** |

### 配置修改

- **RT-Thread 组件**: 修改 `.config` / `rtconfig.h`（推荐通过 `menuconfig` 或 RT-Thread Studio）
- **硬件引脚/时钟**: 使用 `tools/device-configurator/` 打开 BSP 的 `design.modus`

---

## 10. 固件后处理与烧录

### 为什么 Linux 下需要手动后处理

`SConstruct` 中的 `edgeprotecttools` 后处理**仅在 Windows 上自动执行**。Linux 下需手动运行。

### 安装 edgeprotecttools

```bash
cd /path/to/sdk-bsp-psoc_e84-edgi-talk
python3 -m pip install --user tools/edgeprotecttools/src
```

### 后处理命令

```bash
cd projects/Edgi_Talk_M33_Blink_LED
edgeprotecttools run-config --input config/boot_with_extended_boot_scons.json
```

这会执行：
1. **hex-relocate**: CBUS → SBUS 地址转换
2. **merge**: 与 `proj_cm33_s_signed.hex` 合并

### 烧录（Infineon OpenOCD + KitProg3）

关键：必须在加载 target 配置**之前**定义 `SMIF_BANKS`，否则 OpenOCD 不会创建外部 Flash bank，导致写 0 字节。

```bash
OPENOCD_ROOT=/path/to/openocd

$OPENOCD_ROOT/bin/openocd \
    -s "$OPENOCD_ROOT/scripts" \
    -f interface/kitprog3.cfg \
    -c "array set SMIF_BANKS {
        0 {addr 0x60000000 size 0x4000000}
        1 {addr 0x64000000 size 0x4000000}
        2 {addr 0x70000000 size 0x4000000}
        3 {addr 0x74000000 size 0x4000000}
    }" \
    -f target/infineon/pse84xgxs2.cfg \
    -c "transport select swd" \
    -c "cat1d.cm33 configure -rtos auto -rtos-wipe-on-reset-halt 1" \
    -c "gdb_breakpoint_override hard" \
    -c "init; reset init; adapter speed 12000" \
    -c "flash write_image erase /path/to/build/rtthread.hex" \
    -c "reset run" \
    -c "shutdown"
```

### Flash Bank 说明

OpenOCD 默认只创建 **RRAM** bank（0x22000000-0x22069FFF）。外部 QSPI/SMIF Flash bank 需通过 `SMIF_BANKS` 显式声明。OpenOCD 的 `func_cat1d.cfg` 会查找此变量，找不到则跳过 SMIF bank 创建。

---

## 11. 多核通信 (IPC)

### 硬件基础

PSE84 有两个 IPC 实例：
- **IPC0** (通道 0-15): 系统保留
- **IPC1** (通道 16-31): 用户应用，**CM33 ↔ CM55 通信专用**

### 软件接口 (mtb-ipc 库)

| API | 调用方 | 说明 |
|-----|-------|------|
| `mtb_ipc_init()` | **先启动的核** (通常 CM33) | 初始化 IPC 通道与信号量 |
| `mtb_ipc_get_handle()` | **后启动的核** (通常 CM55) | 轮询等待对端就绪，获取句柄 |
| `mtb_ipc_send()` / `mtb_ipc_recv()` | 两端 | 收发数据 |

### 注意事项

- 两核使用不同的 IPC 中断号（冲突检测内置于库中）
- 共享内存须位于两核都能访问的区域（如 `m33_m55_shared` 或 `allocatable_shared`）
- CM55 有 D-Cache，需注意缓存一致性

---

## 12. 外设驱动分层

```
┌───────────────────────────────┐
│  应用代码 (main.c)            │
├───────────────────────────────┤
│  RT-Thread 设备框架            │  rt_device_find / rt_pin_write / ...
├───────────────────────────────┤
│  HAL_Drivers (drv_*.c)        │  BSP 适配层，对接 RT-Thread 框架
├───────────────────────────────┤
│  Infineon HAL (mtb_hal_*.c)   │  硬件抽象层
├───────────────────────────────┤
│  Infineon PDL (cy_*.c)        │  外设驱动库，直接操作寄存器
├───────────────────────────────┤
│  硬件 (PSoC Edge E84)         │
└───────────────────────────────┘
```

| 层级 | 路径 | 职责 |
|------|------|------|
| RT-Thread 驱动框架 | `rt-thread/components/drivers/` | 统一设备模型 |
| HAL_Drivers | `libraries/HAL_Drivers/drv_*.c` | 把 Infineon HAL/PDL 接到 RT-Thread |
| Infineon HAL | `libraries/components/.../hal/` | 跨平台硬件抽象 |
| Infineon PDL | `libraries/components/.../pdl/` | 寄存器级外设驱动 |
| 板级端口 | `libraries/Common/board/ports/` | Edgi-Talk 特有外设 |

---

## 13. 常见问题

### Q: M55 工程烧录后没有任何现象？

必须先烧录一个 **M33 工程**（如 `Edgi_Talk_M33_Blink_LED`），并确保其中开启了 `SOC_Enable_CM55`。M55 电源域由 CM33 控制释放。

### Q: 烧录时 `wrote 0 bytes` 怎么办？

OpenOCD 未创建 SMIF Flash bank。确保在 `-f target/...` **之前**添加 `SMIF_BANKS` 定义。

### Q: `boot_with_extended_boot_scons.json` 和 `boot_with_extended_boot.json` 有什么区别？

两者内容几乎相同，路径约定不同：
- `*_scons.json`: 适配 **SCons 命令行** 构建路径（`rtthread.hex` → `build/rtthread.hex`）
- 不带 `_scons` 的: 适配 **RT-Thread Studio / Eclipse** 路径（`../Debug/rtthread.hex`）

### Q: BOOT 切换 INT/EXT 是什么意思？

开发板有 BOOT 模式开关。切 **INT** (内部) 可跳过外部 Flash 验证进入恢复/下载状态；正常运行时切 **EXT** (外部) 从外部 Flash 启动。

### Q: 可以不做后处理直接烧录吗？

不建议。链接器生成的 hex 使用 CBUS 地址（只读），OpenOCD 需要 SBUS 地址（可写）才能写入 Flash。不做 relocate 会导致地址不匹配。同时缺少 S 侧镜像的 merge 可能导致启动链断裂。

### Q: KitProg3 "failed to acquire the device" 是否致命？

通常不致命。这表示 **Test Mode** 获取失败，但 OpenOCD 仍能通过正常 SWD 路径连接和编程。只要后续 `init; reset init` 成功并写入了实际字节数即可。

---

## 参考资料

- Infineon AN239774: *Selecting and configuring memories for power and performance in PSoC Edge MCU*
- Infineon AN237849: *Getting started with PSoC Edge security*
- Infineon 002-37778: *PSoC Edge MCU Programming Specification*
- [Infineon OpenOCD Releases](https://github.com/Infineon/openocd/releases)
- [Edge Protect Signing Service](https://osts.infineon.com/epss/home)
- [RT-Thread 文档中心](https://www.rt-thread.org/document/site/)
