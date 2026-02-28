#pragma once

#include "driver/gpio.h"
#include "hal/i2c_types.h"
#include "driver/uart.h"

// -----------------------------------------------------------------------------
// MCU: ESP32-S3-WROOM-1U (N16R8)
// Board pin map (centralized)
// -----------------------------------------------------------------------------

// ---------------- Motor UART (M0603A) ----------------
#define BSP_MOTOR_UART_NUM       UART_NUM_1
#define BSP_MOTOR_UART_TX_GPIO   GPIO_NUM_13
#define BSP_MOTOR_UART_RX_GPIO   GPIO_NUM_14

// NOTE: Protocol spec says 9600 8N1; keep motor_driver.h authoritative for now.

// ---------------- RS485 Sensor UART ----------------
// Auto direction (no DE/RE). Uses ESP-IDF RS485 half duplex mode.
#define BSP_SENSOR_UART_NUM      UART_NUM_2
#define BSP_SENSOR_UART_TX_GPIO  GPIO_NUM_16
#define BSP_SENSOR_UART_RX_GPIO  GPIO_NUM_17

// ---------------- I2C Touch ----------------
#define BSP_TOUCH_I2C_PORT       I2C_NUM_0
#define BSP_TOUCH_I2C_SDA_GPIO   GPIO_NUM_1
#define BSP_TOUCH_I2C_SCL_GPIO   GPIO_NUM_2
#define BSP_TOUCH_INT_GPIO       GPIO_NUM_18

// ---------------- 8080 LCD (i80, 8-bit) ----------------
// Control pins
#define BSP_LCD_CS_GPIO          GPIO_NUM_4
#define BSP_LCD_DC_GPIO          GPIO_NUM_5   // RS / DC
#define BSP_LCD_WR_GPIO          GPIO_NUM_6
#define BSP_LCD_RD_GPIO          GPIO_NUM_7
#define BSP_LCD_BL_GPIO          GPIO_NUM_8

// Data bus pins DB0..DB7
#define BSP_LCD_D0_GPIO          GPIO_NUM_42
#define BSP_LCD_D1_GPIO          GPIO_NUM_41
#define BSP_LCD_D2_GPIO          GPIO_NUM_40
#define BSP_LCD_D3_GPIO          GPIO_NUM_39
#define BSP_LCD_D4_GPIO          GPIO_NUM_38
#define BSP_LCD_D5_GPIO          GPIO_NUM_48
#define BSP_LCD_D6_GPIO          GPIO_NUM_47
#define BSP_LCD_D7_GPIO          GPIO_NUM_21

// LCD has no dedicated reset pin; use software reset command sequence.
#define BSP_LCD_RST_GPIO         GPIO_NUM_NC

// ---------------- USB OTG ----------------
// ESP32-S3 native USB
#define BSP_USB_DM_GPIO          GPIO_NUM_19
#define BSP_USB_DP_GPIO          GPIO_NUM_20

// ---------------- TF (SDMMC 1-bit) ----------------
// User wiring: SD_CMD=IO9, SDCLK=IO10, SD_DAT0=IO11
#define BSP_SDMMC_CMD_GPIO        GPIO_NUM_9
#define BSP_SDMMC_CLK_GPIO        GPIO_NUM_10
#define BSP_SDMMC_D0_GPIO         GPIO_NUM_11
#define BSP_SDMMC_WIDTH           1

// ---------------- Physical keys ----------------
// KEY1: stop/home, KEY2: start/forward
#define BSP_KEY_STOP_GPIO        GPIO_NUM_15
#define BSP_KEY_START_GPIO       GPIO_NUM_12

