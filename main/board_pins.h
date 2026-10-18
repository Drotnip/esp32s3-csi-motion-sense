#pragma once
/*
 * Pin map for the generic "ESP32-S3 2.8in 240x320 capacitive touch" smart-display
 * board (SPEAKER / BAT / UART / I2C JST headers, micro-SD slot, USB-C).
 * Matches the on-device verified notes for this board family.
 */

// ---- Display: ILI9341, 240x320 portrait, SPI2_HOST ----
#define PIN_LCD_SCLK  12
#define PIN_LCD_MOSI  11
#define PIN_LCD_MISO  13   // wired, unused (we only write)
#define PIN_LCD_CS    10
#define PIN_LCD_DC    46   // strapping pin: driving it as output after boot is fine
#define PIN_LCD_RST   -1   // tied to board RESET -> we use software reset
#define PIN_LCD_BL    45   // strapping pin: fine after boot, active-HIGH via LEDC PWM
#define LCD_H_RES     240
#define LCD_V_RES     320
#define LCD_SPI_HZ    (40 * 1000 * 1000)

// ---- Touch: FT6336G capacitive, shared I2C bus ----
#define PIN_I2C_SDA   16
#define PIN_I2C_SCL   15
#define PIN_TOUCH_INT 17   // not used (we poll the status register)
#define PIN_TOUCH_RST 18
#define TOUCH_I2C_ADDR 0x38

// ---- Status LED: single WS2812B ----
#define PIN_LED_STRIP 42

// ---- Sense tuning ----
#define PING_INTERVAL_MS 20   // ping gateway at 50 Hz -> ~50 CSI frames/sec
