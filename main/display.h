#pragma once

// ILI9341 display init + on-screen UI (status banner, motion graph, presence
// bar, stats, recalibrate touch button). All drawing is done with esp_lcd
// primitives; no LVGL needed.
void display_init(void);
void display_splash(const char *line1, const char *line2);
void display_start_ui(void);  // 10 fps UI task, also polls touch + drives LED
