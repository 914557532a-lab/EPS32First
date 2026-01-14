/**
 * @file Pin_Config.h
 * @brief ESP32-S3 硬件引脚定义
 * @details 
 * Board: ESP32-S3R8
 * Display: ST7735S (128x128)
 * Audio: ES8311 + NS4150B
 * Wireless: LE271-GL (4G), CMT2300A (433MHz), IR
 * Framework: Arduino IDE / LVGL 8.3.11
 */

#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include <Arduino.h>

/* =================================================================
 * 1. 屏幕接口 (Display - ST7735S)
 * 分辨率: 128*128
 * 通信: 4-wire SPI
 * ================================================================= */
#define PIN_TFT_SCLK        4   // SPI_CLK -> TFT_SCL
#define PIN_TFT_MOSI        2   // SPI_MOSI -> TFT_SDA
#define PIN_TFT_MISO        -1  // 未使用标准MISO，物理引脚3被用作DC
#define PIN_TFT_CS          11  // SPI_CS0 -> TFT_CS
#define PIN_TFT_DC          3   // SPI_MISO -> TFT_D/C (注意：MISO引脚被复用为Data/Command)
#define PIN_TFT_RST         17  // RST_TFT -> TFT_RESET

// 背光控制 (注意：描述中提到此引脚与 LE271_NETSTATE 连接)
// 如果 4G 模块网络状态变化，背光可能会闪烁，建议软件上做处理或硬件确认
#define PIN_TFT_BL          14  // INT_LE271_NETSTATE -> EN_TFT_LED


/* =================================================================
 * 2. 音频接口 (Audio - ES8311 & NS4150B)
 * 通信: I2S + I2C
 * ================================================================= */
// I2C 控制接口 (ES8311)
#define PIN_I2C_SDA         47  // I2C_SDA
#define PIN_I2C_SCL         48  // I2C_SCL

// I2S 数据接口
#define PIN_I2S_MCLK        42  // I2S_MSCK -> ES8311_MCLK
#define PIN_I2S_BCLK        41  // I2S_BCK -> ES8311_BCLK
#define PIN_I2S_LRCK        39  // I2S_WS -> ES8311_WS (Word Select/LR Clock)
#define PIN_I2S_DOUT        38  // I2S_SDO -> ES8311_DOUT (ESP输出 -> 喇叭)
#define PIN_I2S_DIN         40  // I2S_SDI -> ES8311_DIN (Mic输入 -> ESP)

// 功放使能 (NS4150B)
#define PIN_PA_EN           18  // EN_NS4150B -> PA_EN_NS4150B (高电平有效)


/* =================================================================
 * 3. 4G 模块 (LTE Cat.1 - LE270-EU)
 * 通信: UART2
 * ================================================================= */
#define PIN_4G_PWR      45  // 主电源控制 (高电平开启)
#define PIN_4G_PWRKEY   46  // 开机键 (硬件下拉，自动开机)
#define PIN_4G_RX       12  // 连接模块的 TXD
#define PIN_4G_TX       13  // 连接模块的 RXD
#define PIN_4G_NET      14  // 网络状态灯 (复用屏幕背光)

/* =================================================================
 * 4. 433MHz 射频模块 (CMT2300A)
 * 接口: SPI-like / GPIO
 * ================================================================= */
#define PIN_RF_SDIO         8   // CMT2300T_SDIO (双向数据)
#define PIN_RF_SCLK         9   // CMT2300T_SCLK (时钟)
#define PIN_RF_CSB          7   // SPI_CS3 -> CMT2300T_CSB (片选)
#define PIN_RF_FCSB         6   // SPI_CS2 -> CMT2300T_FCSB (FIFO 片选 / 这里的配置较特殊)
#define PIN_RF_GPIO3        10  // INT_RF433 -> CMT2300T_GPIO3 (中断/状态)


/* =================================================================
 * 5. 输入与传感器 (Input & Sensors)
 * ================================================================= */
// 按键
// KEY1 (CHIP_PU) 是硬件复位，通常不作为软件 GPIO 使用
#define PIN_KEY2            21  // INT_KEY2 -> 自定义功能按键 (需配置 INPUT_PULLUP)

// 温度检测
#define PIN_ADC_TEMP        1   // GPIO1 -> ADC_TEMP (NTC热敏电阻分压)

// 红外线 (IR)
#define PIN_IR_TX           16  // IR_TX
#define PIN_IR_RX           15  // IR_RX


/* =================================================================
 * 6. 系统与调试 (System & Debug)
 * ================================================================= */
 // 6. 系统与调试 (System & Debug)
// PC 通讯 / 调试串口 (UART0/1)
// ESP32-S3 默认 UART0 是 GPIO 43(TX) / 44(RX)，与这里一致
// PC 通讯 / 调试串口 (UART1)
#define PIN_DBG_TX          43  // UART1_TX -> 连接电脑/USB转串口
#define PIN_DBG_RX          44  // UART1_RX

// USB (原生 USB 接口)
#define PIN_USB_DN          19  // USB_D-
#define PIN_USB_DP          20  // USB_D+

// Flash SPI (通常由系统自动管理，不建议在应用层重定义，仅作记录)
// CS1=5, MISO=SPIQ, CLK=SPICLK, MOSI=SPID

#endif // PIN_CONFIG_H