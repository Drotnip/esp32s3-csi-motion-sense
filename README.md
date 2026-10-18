# CSI Motion Sense — on-device Wi-Fi motion detection + presence sensing

> 🚧 **Work in progress.** This project is shared as-is for anyone who wants
> to build on it or learn from it. Expect rough edges.

A self-contained ESP32-S3 project: the board sniffs Wi-Fi Channel State
Information (CSI) from its own network traffic, detects motion and presence
**entirely on-device**, and shows everything live on its 2.8" touchscreen.
No second board, no PC, no cloud.

## How it works

1. **Traffic source** — the board joins your 2.4 GHz Wi-Fi as a station and
   pings the gateway every 20 ms (50 Hz). Every ping reply that arrives is a
   fresh measurement of the radio channel.
2. **CSI capture** — Espressif's Wi-Fi CSI engine reports per-subcarrier
   amplitude/phase for each received frame. A lightweight callback copies each
   frame into a queue (never blocks the Wi-Fi task); a sense task does the math.
3. **Detection** — two metrics:
   - *jitter* (fast frame-to-frame change) → **MOTION**
   - *wander* (slow deviation from the empty-room baseline) → **PRESENCE**
   
   Thresholds come from a 20-second auto-calibration of the empty room
   (saved to NVS, survives reboots). Hysteresis + debouncing keep the state
   stable: EMPTY → PRESENT → MOTION.
4. **Display** — live status banner, scrolling motion-level graph with the
   detection threshold, presence bar, RSSI / FPS / uptime stats, and a
   touch button to recalibrate. The onboard WS2812B mirrors the state
   (green = motion, blue = presence, off = empty).

## Hardware

The generic **ESP32-S3 2.8" 240x320 capacitive-touch** smart-display board
(SPEAKER / BAT / UART / I2C headers, micro-SD, USB-C). The pin map in
`main/board_pins.h` matches this board family (ILI9341 over SPI, FT6336G
touch on I2C, WS2812B on GPIO42). Module should be the **N16R8** variant
(16 MB flash / 8 MB octal PSRAM).

## Build & flash

1. Install **ESP-IDF v5.3** (5.1–5.5 also fine):
   https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/get-started/
2. Set your Wi-Fi credentials in `main/net.c` (`WIFI_SSID` / `WIFI_PASSWORD`).
   Use a **2.4 GHz** network — the ESP32-S3 has no 5 GHz radio.
3. Build and flash over the native USB-C port:

   ```bash
   . $HOME/esp/esp-idf/export.sh
   idf.py set-target esp32s3
   idf.py build
   idf.py -p /dev/ttyACM0 flash monitor   # Linux; use the right port on macOS/Windows
   ```

   The managed component (`esp_lcd_ili9341`) downloads
   automatically on first build. The WS2812 LED is driven directly
   with the built-in RMT driver (no extra component needed).

4. First boot: keep the room empty for ~20 s while it auto-calibrates
   (yellow banner shows progress). After that, walk around and watch it react.

## Recalibrating

Tap **TAP TO RECALIBRATE** at the bottom of the screen (or press the board's
RESET button on a fresh install). Always recalibrate with the room empty and
the board in its final position — moving the board invalidates the baseline.

## Tuning

- `PING_INTERVAL_MS` in `main/board_pins.h`: lower = more CSI frames/sec
  (finer motion detail, more Wi-Fi chatter). 20 ms ≈ 50 fps is a good default.
- Threshold floors in `csi_sense.c` (`finish_calibration`): raise them if you
  get false positives in a noisy RF environment.
- Detection timing in `detect()`: motion needs 3 consecutive over-threshold
  frames, lingers 4 s; presence needs ~2 s; empty needs ~10 s of quiet.

## Troubleshooting

- **Black screen**: check the USB cable carries data (not charge-only).
  If the image is upside-down, the panel is mounted rotated — ask and I'll
  add the mirror flags.
- **"NO WIFI" forever**: wrong SSID/password in `main/net.c`, or the AP is
  5 GHz-only / too far away.
- **FPS reads 0 but Wi-Fi is up**: the gateway isn't answering pings
  (some APs ignore ICMP). The UI still works off beacons/other traffic, just
  slower — or point it at a pingable host.
- **Always shows MOTION/PRESENT in an empty room**: recalibrate; keep the
  board away from fans, curtains, pets. Raise the threshold floors.
- **Build error in `wifi_csi_config_t`**: your IDF version's struct differs —
  the code deliberately uses only the long-stable field names; check
  `esp_wifi_types.h` in your IDF for the exact spelling.
- **"Brownout" resets**: power from a decent 5 V USB source, not a weak hub.

## Files

- `main/app_main.c` — boot order
- `main/csi_sense.c` — CSI callback, metrics, calibration, state machine
- `main/net.c` — Wi-Fi station + 50 Hz gateway ping
- `main/display.c` — ILI9341 init (SPI) + full UI, no LVGL
- `main/touch.c` — FT6336G minimal driver
- `main/led.c` — WS2812B status LED
- `main/board_pins.h` — every pin in one place
- `sdkconfig.defaults` — ESP32-S3, 16 MB flash, octal PSRAM, CSI enabled
