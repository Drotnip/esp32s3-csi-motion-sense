#include "display.h"
#include "board_pins.h"
#include "font5x7.h"
#include "csi_sense.h"
#include "touch.h"
#include "led.h"
#include "net.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "display";

// RGB565 palette
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BLUE   0x001F
#define C_YELLOW 0xFFE0
#define C_CYAN   0x07FF
#define C_GRAY   0x8410
#define C_DARK   0x2104

#define GRAPH_X 10
#define GRAPH_Y 124
#define GRAPH_W 220
#define GRAPH_H 120

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lcd_mutex;
static uint16_t *s_graph_buf;

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// ---------- primitives ----------

#define CHUNK_ROWS 32
static uint16_t s_chunk[240 * CHUNK_ROWS];

// Hardware X offset: this panel's visible area starts a few pixels into
// the controller's memory. Shift all drawing right to compensate.
#define LCD_X_OFFSET 6

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    x += LCD_X_OFFSET;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    uint16_t sc = bswap16(color);
    for (int i = 0; i < w * CHUNK_ROWS; i++) s_chunk[i] = sc;
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    int done = 0;
    while (done < h) {
        int ch = (h - done > CHUNK_ROWS) ? CHUNK_ROWS : (h - done);
        esp_lcd_panel_draw_bitmap(s_panel, x, y + done, x + w, y + done + ch, s_chunk);
        done += ch;
    }
    xSemaphoreGive(s_lcd_mutex);
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static void draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    uint16_t sfg = bswap16(fg), sbg = bswap16(bg);
    static uint16_t buf[24 * 32];  // max char cell: 6*4 x 8*4
    int cw = 6 * scale, chh = 8 * scale;
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    while (*s) {
        const uint8_t *g = font_glyph(*s);
        for (int i = 0; i < cw * chh; i++) buf[i] = sbg;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            buf[(row * scale + sy) * cw + col * scale + sx] = sfg;
                }
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, x + LCD_X_OFFSET, y, x + LCD_X_OFFSET + cw, y + chh, buf);
        x += cw;
        s++;
    }
    xSemaphoreGive(s_lcd_mutex);
}

static void draw_text_centered(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    draw_text((LCD_H_RES - text_width(s, scale)) / 2, y, s, fg, bg, scale);
}

// ---------- motion graph ----------

static void draw_graph(const float *hist, int n, float thresh)
{
    float maxv = thresh * 1.6f;
    for (int i = 0; i < n; i++) if (hist[i] > maxv) maxv = hist[i];
    if (maxv < 1.0f) maxv = 1.0f;

    uint16_t bg = bswap16(C_DARK), bar = bswap16(C_CYAN), th = bswap16(C_RED);
    int total = GRAPH_W * GRAPH_H;
    for (int i = 0; i < total; i++) s_graph_buf[i] = bg;

    int bw = n > 0 ? GRAPH_W / n : GRAPH_W;
    if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        int h = (int)(hist[i] / maxv * GRAPH_H);
        if (h > GRAPH_H) h = GRAPH_H;
        int x0 = i * bw;
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < bw - 1 && x0 + xx < GRAPH_W; xx++)
                s_graph_buf[(GRAPH_H - 1 - yy) * GRAPH_W + x0 + xx] = bar;
    }
    int ty = GRAPH_H - 1 - (int)(thresh / maxv * GRAPH_H);
    if (ty < 0) ty = 0;
    if (ty >= GRAPH_H) ty = GRAPH_H - 1;
    for (int xx = 0; xx < GRAPH_W; xx++) s_graph_buf[ty * GRAPH_W + xx] = th;

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel, GRAPH_X + LCD_X_OFFSET, GRAPH_Y,
                              GRAPH_X + LCD_X_OFFSET + GRAPH_W, GRAPH_Y + GRAPH_H, s_graph_buf);
    xSemaphoreGive(s_lcd_mutex);
}

// ---------- full UI frame ----------

static void draw_ui(const sense_snapshot_t *s)
{
    char line[48];
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, C_BLACK);

    // header
    draw_text(6, 8, "CSI MOTION SENSE", C_WHITE, C_BLACK, 1);
    if (net_is_up()) {
        snprintf(line, sizeof(line), "RSSI %d", s->rssi);
        draw_text(LCD_H_RES - text_width(line, 1) - 6, 8, line, C_GREEN, C_BLACK, 1);
    } else {
        const char *t = "NO WIFI";
        draw_text(LCD_H_RES - text_width(t, 1) - 6, 8, t, C_RED, C_BLACK, 1);
    }

    // status banner
    const char *word;
    uint16_t bg, fg = C_BLACK;
    switch (s->state) {
        case SENSE_MOTION:      word = "MOTION";      bg = C_GREEN;  break;
        case SENSE_PRESENT:     word = "PRESENT";     bg = C_BLUE;   fg = C_WHITE; break;
        case SENSE_CALIBRATING: word = "CALIBRATING"; bg = C_YELLOW; break;
        case SENSE_NO_WIFI:     word = "NO WIFI";     bg = C_RED;    fg = C_WHITE; break;
        default:                word = "EMPTY";       bg = C_GRAY;   fg = C_WHITE; break;
    }
    fill_rect(0, 34, LCD_H_RES, 70, bg);
    if (s->state == SENSE_CALIBRATING) {
        draw_text_centered(44, word, fg, bg, 2);
        snprintf(line, sizeof(line), "KEEP ROOM EMPTY %d%%", (int)(s->calib_progress * 100));
        draw_text_centered(78, line, fg, bg, 1);
    } else {
        draw_text_centered(56, word, fg, bg, 3);
    }

    // motion graph
    draw_text(GRAPH_X, 108, "MOTION LEVEL", C_GRAY, C_BLACK, 1);
    snprintf(line, sizeof(line), "%lu FPS", (unsigned long)s->fps);
    draw_text(LCD_H_RES - text_width(line, 1) - GRAPH_X, 108, line, C_GRAY, C_BLACK, 1);
    float hist[SENSE_HISTORY_N];
    int hn = csi_sense_get_history(hist, SENSE_HISTORY_N);
    draw_graph(hist, hn, s->jitter_thresh);

    // presence bar
    draw_text(GRAPH_X, 250, "PRESENCE", C_GRAY, C_BLACK, 1);
    int barw = (int)(s->wander / (s->wander_thresh * 2.0f) * 130.0f);
    if (barw > 130) barw = 130;
    if (barw < 0) barw = 0;
    fill_rect(100, 250, 130, 12, C_DARK);
    if (barw) fill_rect(100, 250, barw, 12, C_BLUE);
    fill_rect(100 + 65, 248, 2, 16, C_RED);  // threshold tick

    // stats
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(line, sizeof(line), "UP %02d:%02d FRM %lu",
             (int)(up / 60), (int)(up % 60), (unsigned long)s->frames_total);
    draw_text(GRAPH_X, 278, line, C_GRAY, C_BLACK, 1);

    // recalibrate touch button
    fill_rect(0, 296, LCD_H_RES, 24, C_BLUE);
    draw_text_centered(302, "TAP TO RECALIBRATE", C_WHITE, C_BLUE, 1);
}

static void update_led(const sense_snapshot_t *s)
{
    static bool blink;
    blink = !blink;
    switch (s->state) {
        case SENSE_MOTION:      led_set(0, 40, 0); break;
        case SENSE_PRESENT:     led_set(0, 0, 40); break;
        case SENSE_CALIBRATING: led_set(blink ? 40 : 0, blink ? 28 : 0, 0); break;
        case SENSE_NO_WIFI:     led_set(blink ? 40 : 0, 0, 0); break;
        default:                led_off(); break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    sense_snapshot_t snap;
    int64_t last_tap_us = 0;
    while (1) {
        csi_sense_get_snapshot(&snap);
        draw_ui(&snap);
        int tx, ty;
        if (touch_read(&tx, &ty)) {
            int64_t now = esp_timer_get_time();
            if (ty >= 290 && now - last_tap_us > 2000000) {  // bottom button, debounced
                last_tap_us = now;
                csi_sense_start_calibration();
            }
        }
        update_led(&snap);
        vTaskDelay(pdMS_TO_TICKS(100));  // 10 fps
    }
}

// ---------- init ----------

void display_init(void)
{
    s_lcd_mutex = xSemaphoreCreateMutex();
    s_graph_buf = heap_caps_malloc(GRAPH_W * GRAPH_H * sizeof(uint16_t), MALLOC_CAP_8BIT);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,  // -1: no GPIO, software reset below
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,  // quirk: BGR + byte-swapped pixels
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_config, &s_panel));

    // LCD RST is tied to board reset -> software reset via command.
    esp_lcd_panel_io_tx_param(s_io, 0x01, NULL, 0);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));  // quirk: inversion ON
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));  // fix Y flip only; X was mirroring text backwards
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Backlight: GPIO45 via LEDC PWM (strapping pin, fine as output after boot).
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));

    ESP_LOGI(TAG, "ILI9341 240x320 ready");
}

void display_splash(const char *line1, const char *line2)
{
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, C_BLACK);
    draw_text_centered(140, line1, C_WHITE, C_BLACK, 2);
    draw_text_centered(170, line2, C_GRAY, C_BLACK, 1);
}

void display_start_ui(void)
{
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
}
