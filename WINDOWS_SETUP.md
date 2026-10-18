# Getting this on your board — Windows 11 walkthrough

Companion to `README.md`, written for a first-time ESP-IDF install on Windows 11.

## 1. Install ESP-IDF (30–60 min, mostly unattended)

1. Open the official guide:
   https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/get-started/
   and follow the **Windows** setup page from there.
2. Download and run the **ESP-IDF Tools Installer**. Choose **ESP-IDF v5.3**.
   It downloads the compiler toolchain and everything else (a few GB) —
   start it now, it takes a while.
3. When it finishes you'll have a new Start-menu shortcut: **ESP-IDF PowerShell**.
   Always run the build commands from that shortcut (not a normal terminal) —
   it sets up all the compiler paths for you.

## 2. Put the project somewhere safe

Copy the `csi-motion-sense` folder to a path with **no spaces**, for example:

```
C:\esp\csi-motion-sense
```

Do NOT put it in `Desktop` or `Documents` — the ESP-IDF build system breaks
on paths with spaces.

## 3. Tell it your Wi-Fi

Open `main\net.c` in Notepad or VS Code and find these two lines:

```c
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Type your home Wi-Fi name and password between the quotes and save.
Use your **2.4 GHz** network — the ESP32-S3 has no 5 GHz radio.

## 4. Plug in the board and find its COM port

1. Connect the board with a USB-C cable that carries **data**. Charge-only
   cables are the #1 beginner trap: if nothing shows up below, swap cables.
2. Open **Device Manager** → **Ports (COM & LPT)** and look for
   **USB Serial Device (COMx)**. Remember the number — that's your port.

## 5. Build and flash

In **ESP-IDF PowerShell**:

```powershell
cd C:\esp\csi-motion-sense
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

(Replace `COM3` with the port you found in step 4.)

- The first build downloads the display library automatically —
  it needs internet and takes several minutes. Later builds are fast.
- `monitor` shows the board's log output live. Exit it with `Ctrl+]`.
- If it says the COM port is busy/access denied, close Arduino IDE or any
  other serial monitor and try again.

## 6. What success looks like

1. The screen shows **CSI MOTION SENSE / BOOTING...**, then **WIFI CONNECTING...**
2. A yellow **CALIBRATING** banner appears with a percentage and
   "KEEP ROOM EMPTY" — leave the room empty for ~20 seconds.
3. The banner turns gray: **EMPTY**. Walk into the room → **PRESENT**
   (blue LED). Wave your arms → **MOTION** (green LED) with a live graph
   of the motion level and a red threshold line.
4. The bottom of the screen has a **TAP TO RECALIBRATE** button. Tap it
   anytime to redo the 20-second calibration — always with the room empty
   and the board sitting in its final spot.

## If something's wrong

- **Black screen, nothing at all**: almost always the cable — it must be a
  data cable. Backlight glowing but no image means the code is running and
  the display init didn't take; tell me and we'll sort it.
- **"NO WIFI" forever**: wrong SSID/password in `main\net.c`, or the
  network is 5 GHz-only / too far away.
- **FPS reads 0 but Wi-Fi is connected**: some routers ignore pings. The
  sensor still works off other Wi-Fi traffic, just slower.
- **Always shows MOTION or PRESENT in an empty room**: recalibrate (tap the
  button), and keep the board away from fans, curtains, and pets.
- **Random reboots ("brownout")**: power it from a decent USB port or phone
  charger, not a weak hub.
