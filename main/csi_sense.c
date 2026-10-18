#include "csi_sense.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "csi_sense";

#define CSI_MAX_LEN   512   // big enough for any HT20/HT40 CSI buffer
#define QUEUE_LEN     32
#define MAX_PAIRS     192   // max I/Q pairs we track per frame
#define CALIB_SECONDS 20

typedef struct {
    uint16_t len;
    int8_t rssi;
    bool skip_first4;
    int8_t buf[CSI_MAX_LEN];
} csi_frame_t;

typedef struct {
    uint32_t magic;
    float jitter_thresh;
    float wander_thresh;
} calib_blob_t;
#define CALIB_MAGIC 0xC51CA1

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;

// --- metric state ---
static float s_prev_amp[MAX_PAIRS];
static float s_base_amp[MAX_PAIRS];   // slow baseline of "empty room"
static float s_jitter;               // fast: frame-to-frame change  -> motion
static float s_wander;               // slow: deviation from baseline -> presence
static float s_jitter_thresh = 4.0f;
static float s_wander_thresh = 8.0f;
static bool s_calibrated;
static bool s_wifi_ok;
static bool s_calibrating;
static float s_calib_progress;
static sense_state_t s_state = SENSE_NO_WIFI;
static uint32_t s_frames_total, s_frames_this_sec, s_fps;
static int64_t s_fps_window_start;
static int8_t s_rssi;
static float s_history[SENSE_HISTORY_N];
static int s_hist_idx;

// --- calibration accumulators ---
static double s_cal_jit_sum, s_cal_jit_sq, s_cal_wan_sum, s_cal_wan_sq;
static uint32_t s_cal_frames;
static int64_t s_cal_start_us;

// --- detection hysteresis ---
static uint32_t s_motion_streak;
static int64_t s_motion_latch_us;
static uint32_t s_present_frames, s_empty_frames;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void set_state(sense_state_t st)
{
    if (st != s_state) {
        s_state = st;
        ESP_LOGI(TAG, "state -> %d (jitter=%.2f thr=%.2f, wander=%.2f thr=%.2f)",
                 st, s_jitter, s_jitter_thresh, s_wander, s_wander_thresh);
    }
}

// ---------------- NVS calibration storage ----------------

static void save_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open("csi_sense", NVS_READWRITE, &h) != ESP_OK) return;
    calib_blob_t b = { .magic = CALIB_MAGIC,
                       .jitter_thresh = s_jitter_thresh,
                       .wander_thresh = s_wander_thresh };
    nvs_set_blob(h, "calib", &b, sizeof(b));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "calibration saved: jitter_thr=%.2f wander_thr=%.2f",
             s_jitter_thresh, s_wander_thresh);
}

static void load_calibration(void)
{
    nvs_handle_t h;
    calib_blob_t b = {0};
    size_t len = sizeof(b);
    if (nvs_open("csi_sense", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "calib", &b, &len) == ESP_OK && b.magic == CALIB_MAGIC) {
            s_jitter_thresh = b.jitter_thresh;
            s_wander_thresh = b.wander_thresh;
            s_calibrated = true;
            ESP_LOGI(TAG, "calibration loaded: jitter_thr=%.2f wander_thr=%.2f",
                     s_jitter_thresh, s_wander_thresh);
        }
        nvs_close(h);
    }
}

// ---------------- CSI plumbing ----------------

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len == 0) return;
    csi_frame_t f;
    uint16_t n = info->len > CSI_MAX_LEN ? CSI_MAX_LEN : info->len;
    memcpy(f.buf, info->buf, n);
    f.len = n;
    f.rssi = info->rx_ctrl.rssi;
    f.skip_first4 = info->first_word_invalid;
    // Never block the Wi-Fi task: drop the frame if the queue is full.
    xQueueSend(s_queue, &f, 0);
}

void csi_sense_enable(void)
{
    wifi_csi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lltf_en = true;            // legacy frames (beacons / management)
    cfg.htltf_en = true;           // HT frames (our ping replies, HT20/HT40)
    cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true;
    cfg.channel_filter_en = true;
    cfg.manu_scale = false;         // automatic scaling
    cfg.shift = 0;
    cfg.dump_ack_en = false;
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI enabled");
}

// ---------------- detection ----------------

static void finish_calibration(void)
{
    double jm = s_cal_jit_sum / s_cal_frames;
    double jv = s_cal_jit_sq / s_cal_frames - jm * jm;
    double wm = s_cal_wan_sum / s_cal_frames;
    double wv = s_cal_wan_sq / s_cal_frames - wm * wm;
    if (jv < 0) jv = 0;
    if (wv < 0) wv = 0;
    s_jitter_thresh = (float)(jm + 5.0 * sqrt(jv));
    s_wander_thresh = (float)(wm + 5.0 * sqrt(wv));
    if (s_jitter_thresh < 2.0f) s_jitter_thresh = 2.0f;  // sanity floors
    if (s_wander_thresh < 4.0f) s_wander_thresh = 4.0f;
    s_calibrated = true;
    s_calibrating = false;
    s_calib_progress = 1.0f;
    memcpy(s_base_amp, s_prev_amp, sizeof(s_base_amp));  // re-anchor baseline
    s_motion_streak = s_present_frames = s_empty_frames = 0;
    save_calibration();
    set_state(SENSE_EMPTY);
}

static void calib_feed(void)
{
    int64_t now = now_us();
    if (s_cal_start_us == 0) s_cal_start_us = now;
    s_cal_jit_sum += s_jitter;
    s_cal_jit_sq += (double)s_jitter * s_jitter;
    s_cal_wan_sum += s_wander;
    s_cal_wan_sq += (double)s_wander * s_wander;
    s_cal_frames++;
    s_calib_progress = (float)(now - s_cal_start_us) / (CALIB_SECONDS * 1000000.0f);
    if (s_calib_progress > 1.0f) s_calib_progress = 1.0f;
    if (now - s_cal_start_us >= (int64_t)CALIB_SECONDS * 1000000 && s_cal_frames > 50) {
        finish_calibration();
    }
}

static void detect(int64_t now)
{
    bool motion_now = s_calibrated && (s_jitter > s_jitter_thresh);
    bool present_now = s_calibrated && (s_wander > s_wander_thresh);

    if (motion_now) {
        s_motion_streak++;
        s_present_frames = s_empty_frames = 0;
        if (s_motion_streak >= 3) {
            set_state(SENSE_MOTION);
            s_motion_latch_us = now;
        }
        return;
    }
    s_motion_streak = 0;

    if (s_state == SENSE_MOTION) {
        if (now - s_motion_latch_us < 4000000) return;  // linger 4 s in MOTION
        // fall through to re-evaluate
    }
    if (present_now) {
        s_present_frames++;
        s_empty_frames = 0;
        if (s_present_frames > 100) set_state(SENSE_PRESENT);  // ~2 s
    } else {
        s_empty_frames++;
        s_present_frames = 0;
        if (s_empty_frames > 500) set_state(SENSE_EMPTY);      // ~10 s
    }
}

static void process_frame(const csi_frame_t *f)
{
    int64_t now = now_us();
    int offset = f->skip_first4 ? 4 : 0;
    int pairs = (f->len - offset) / 2;
    if (pairs <= 0) return;
    if (pairs > MAX_PAIRS) pairs = MAX_PAIRS;

    float jit_sum = 0.0f, wan_sum = 0.0f;
    bool first = (s_frames_total == 0);
    for (int i = 0; i < pairs; i++) {
        float amp = fabsf((float)f->buf[offset + 2 * i])
                  + fabsf((float)f->buf[offset + 2 * i + 1]);
        float dj = first ? 0.0f : fabsf(amp - s_prev_amp[i]);
        jit_sum += dj;
        s_prev_amp[i] = amp;
        // The baseline must keep representing the *empty* room: adapt it at a
        // normal rate while EMPTY, barely at all while someone is present
        // (otherwise a perfectly still person would fade into the baseline),
        // and freeze it entirely during strong motion.
        float adapt = (s_state == SENSE_EMPTY) ? 0.002f : 0.0001f;
        if (s_jitter < s_jitter_thresh * 3.0f) {
            s_base_amp[i] += adapt * (amp - s_base_amp[i]);
        }
        wan_sum += fabsf(amp - s_base_amp[i]);
    }
    float jit_inst = jit_sum / pairs;
    float wan_inst = wan_sum / pairs;
    s_jitter += 0.25f * (jit_inst - s_jitter);
    s_wander += 0.05f * (wan_inst - s_wander);

    s_rssi = f->rssi;
    s_frames_total++;
    s_frames_this_sec++;
    if (now - s_fps_window_start >= 1000000) {
        s_fps = s_frames_this_sec;
        s_frames_this_sec = 0;
        s_fps_window_start = now;
    }

    s_history[s_hist_idx] = s_jitter;
    s_hist_idx = (s_hist_idx + 1) % SENSE_HISTORY_N;

    if (s_calibrating) {
        calib_feed();
        return;
    }
    if (!s_wifi_ok) {
        set_state(SENSE_NO_WIFI);
        return;
    }
    if (!s_calibrated) {
        set_state(SENSE_CALIBRATING);
        return;
    }
    detect(now);
}

static void sense_task(void *arg)
{
    (void)arg;
    csi_frame_t f;
    s_fps_window_start = now_us();
    while (1) {
        if (xQueueReceive(s_queue, &f, pdMS_TO_TICKS(500)) == pdTRUE) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            process_frame(&f);
            xSemaphoreGive(s_mutex);
        } else {
            // No frames for a while: decay the fps readout.
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (now_us() - s_fps_window_start >= 1000000) {
                s_fps = s_frames_this_sec;
                s_frames_this_sec = 0;
                s_fps_window_start = now_us();
            }
            xSemaphoreGive(s_mutex);
        }
    }
}

// ---------------- public API ----------------

void csi_sense_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(csi_frame_t));
    s_mutex = xSemaphoreCreateMutex();
    load_calibration();
    xTaskCreatePinnedToCore(sense_task, "csi_sense", 4096, NULL, 5, NULL, 1);
    if (!s_calibrated) {
        ESP_LOGI(TAG, "no calibration found -> auto-calibrating on first boot");
        csi_sense_start_calibration();
    }
}

void csi_sense_set_wifi_ok(bool ok)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_wifi_ok = ok;
    if (!ok) set_state(SENSE_NO_WIFI);
    xSemaphoreGive(s_mutex);
}

void csi_sense_start_calibration(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cal_jit_sum = s_cal_jit_sq = 0;
    s_cal_wan_sum = s_cal_wan_sq = 0;
    s_cal_frames = 0;
    s_cal_start_us = 0;
    s_calib_progress = 0;
    s_calibrating = true;
    set_state(SENSE_CALIBRATING);
    ESP_LOGI(TAG, "calibration started: keep the room empty for %d s", CALIB_SECONDS);
    xSemaphoreGive(s_mutex);
}

bool csi_sense_get_snapshot(sense_snapshot_t *out)
{
    if (!out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->jitter = s_jitter;
    out->wander = s_wander;
    out->jitter_thresh = s_jitter_thresh;
    out->wander_thresh = s_wander_thresh;
    out->state = s_state;
    out->fps = s_fps;
    out->frames_total = s_frames_total;
    out->rssi = s_rssi;
    out->calibrated = s_calibrated;
    out->calib_progress = s_calib_progress;
    xSemaphoreGive(s_mutex);
    return true;
}

int csi_sense_get_history(float *out, int max_n)
{
    if (!out || max_n <= 0) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = max_n < SENSE_HISTORY_N ? max_n : SENSE_HISTORY_N;
    for (int i = 0; i < n; i++) {
        out[i] = s_history[(s_hist_idx + i) % SENSE_HISTORY_N];
    }
    xSemaphoreGive(s_mutex);
    return n;
}
