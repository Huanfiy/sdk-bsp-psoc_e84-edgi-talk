# sdk-bsp-psoc_e84-edgi-talk

[**中文**](./README_zh.md) | **English**

## Introduction

`sdk-bsp-psoc_e84-edgi-talk` is the RT-Thread Board Support Package (BSP) for the **PSoC™ E84 Edgi-Talk development board**.
It can also be used as a software SDK for user development, enabling developers to build their own applications more easily and efficiently.

The Edgi-Talk board is based on the **PSoC™ E84 MCU** and provides a flexible and comprehensive development platform.
It integrates a variety of peripheral interfaces and sample modules, helping developers quickly implement applications involving multi-sensor integration, display, and communication.

![Edgi-Talk](docs/figures/board_large.jpg)

## Directory Structure

```
$ sdk-bsp-psoc_e84-edgi-talk
├── README.md
├── sdk-bsp-psoc_e84-edgi-talk.yaml
├── docs
│   ├── Edgi-Talk_Board_Schematic.pdf
│   └── Edgi-Talk_User_Manual.pdf
├── libraries
│   ├── HAL_Drivers
├── projects
│   ├── Edgi-Talk_ADC
│   ├── Edgi-Talk_AHT20
│   ├── Edgi-Talk_Audio
│   ├── Edgi-Talk_Blink_Led
│   ├── Edgi_Talk_CherryUSB
│   │   ├── Edgi_Talk_M33_USB_D
│   │   ├── Edgi_Talk_M33_USB_H
│   │   ├── Edgi_Talk_M55_USB_D
│   │   └── Edgi_Talk_M55_USB_H
│   ├── Edgi_Talk_IPC
│   │   ├── Edgi_Talk_M33_IPC
│   │   └── Edgi_Talk_M55_IPC
│   ├── Edgi-Talk_CoreMark
│   ├── Edgi-Talk_CDC_Echo
│   ├── Edgi-Talk_HyperRam
│   ├── Edgi-Talk_Key_Irq
│   ├── Edgi-Talk_LSM6DS3
│   ├── Edgi-Talk_LVGL
│   ├── Edgi_Talk_M33_Blink_LED
│   ├── Edgi-Talk_M33_Template
│   ├── Edgi-Talk_MIPI_LCD
│   ├── Edgi-Talk_RTC
│   ├── Edgi-Talk_SDCARD
│   ├── Edgi-Talk_WavPlayer
│   ├── Edgi-Talk_WIFI
│   ├── Edgi-Talk_XiaoZhi
└── rt-thread
```

* `sdk-bsp-psoc_e84-edgi-talk.yaml`: Hardware description file for the Edgi-Talk board
* `docs`: Board schematics, user manual, and datasheets
* `libraries`: Common peripheral drivers for Edgi-Talk
* `projects`: Example applications, including sensor, display, audio, USB, and network demos
* `rt-thread`: RT-Thread source code

## Usage

`sdk-bsp-psoc_e84-edgi-talk` supports development using **RT-Thread Studio**.

## **RT-Thread Studio Development Steps**

1. Open RT-Thread Studio and install the Edgi-Talk BSP (latest version recommended).
   ![Edgi-Talk](docs/figures/1.png)
2. Create a new Edgi-Talk project:
   File -> New -> RT-Thread Project -> Based on Board.
   Example and template projects are available for selection.
   ![project](docs/figures/2.png)
3. Build and download the project:
   Compilation, download, and debugging can be done directly inside RT-Thread Studio.
   ![Build](docs/figures/3.png)
## Notes

* To modify the **graphical configuration** of the project, use the following tool to open the configuration file:

  ```
  tools/device-configurator/device-configurator.exe
  libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/config/design.modus
  ```

* After modification, save the configuration and regenerate the code.

## Boot Sequence

The system boot sequence is as follows:

```
+------------------+
|   Secure M33     |
|   (Secure Core)  |
+------------------+
          |
          v
+------------------+
|       M33        |
| (Non-secure Core)|
+------------------+
          |
          v
+-------------------+
|       M55         |
| (Application Core)|
+-------------------+
```

⚠️ Please strictly follow the above sequence when flashing firmware; otherwise, the system may fail to run properly.

---

* If example projects cannot run correctly, compile and flash the **Edgi_Talk_M33_Blink_LED** project first to ensure core initialization and startup flow are functioning before running other demos.
* To enable the M55 core, configure it inside the **M33 project**:

  ```
  RT-Thread Settings --> Hardware --> select SOC Multi Core Mode --> Enable CM55 Core
  ```
![Enable M55](docs/figures/config.png)
