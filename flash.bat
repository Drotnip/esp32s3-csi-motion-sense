@echo off
REM ============================================================
REM  CSI motion-sense: one-click build + flash + watch the log
REM
REM  HOW TO USE:
REM    1. Copy this file into your project folder, next to the
REM       "main" folder (C:\Users\eli09\Downloads\csi-motion-sense\)
REM    2. Make sure your board is plugged in (it shows up as COM35).
REM    3. Double-click this file. That's it - no typing needed.
REM
REM  To leave the log view afterwards, press Ctrl and ] together.
REM ============================================================

cd /d "%~dp0"

REM ---- Sanity check: WiFi login must be filled in first ----
findstr /C:"YOUR_WIFI_SSID" main\net.c >nul 2>&1
if not errorlevel 1 (
    echo.
    echo  *** One thing needed first: your WiFi login ***
    echo.
    echo  1. Open main\net.c in Notepad.
    echo  2. Replace YOUR_WIFI_SSID with your network name (keep the quotes).
    echo  3. Replace YOUR_WIFI_PASSWORD with your WiFi password (keep the quotes).
    echo  4. Save the file, then double-click this file again.
    echo.
    pause
    exit /b 1
)

REM ---- Load the ESP-IDF tools ----
if not exist "C:\Espressif\frameworks\esp-idf-v5.3.1\export.bat" (
    echo.
    echo  *** Could not find ESP-IDF at C:\Espressif\frameworks\esp-idf-v5.3.1 ***
    echo  If you installed it somewhere else, open this file in Notepad and
    echo  change that path to where your export.bat is, then try again.
    echo.
    pause
    exit /b 1
)
call "C:\Espressif\frameworks\esp-idf-v5.3.1\export.bat"

REM ---- Build, flash to COM35, and show the live log ----
idf.py -D CMAKE_TOOLCHAIN_FILE=C:/Espressif/frameworks/esp-idf-v5.3.1/tools/cmake/toolchain-esp32s3.cmake -p COM35 flash monitor

echo.
echo  All done. To do it again later, just double-click this file.
pause
