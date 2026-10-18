#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SENSE_EMPTY = 0,
    SENSE_PRESENT,      // someone in the room, not moving
    SENSE_MOTION,       // active movement right now
    SENSE_CALIBRATING,  // learning the empty-room baseline
    SENSE_NO_WIFI,
} sense_state_t;

typedef struct {
    float jitter;          // smoothed motion metric (frame-to-frame change)
    float wander;          // smoothed presence metric (deviation from baseline)
    float jitter_thresh;
    float wander_thresh;
    sense_state_t state;
    uint32_t fps;          // CSI frames per second (last completed second)
    uint32_t frames_total;
    int8_t rssi;
    bool calibrated;
    float calib_progress;  // 0..1 while calibrating
} sense_snapshot_t;

#define SENSE_HISTORY_N 160

void csi_sense_init(void);              // queue + task + NVS; auto-calibrates on first boot
void csi_sense_enable(void);            // call once Wi-Fi is initialized
void csi_sense_set_wifi_ok(bool ok);
void csi_sense_start_calibration(void); // re-learn the empty-room baseline
bool csi_sense_get_snapshot(sense_snapshot_t *out);
// Copies the jitter history (oldest -> newest) into out[]. Returns sample count.
int csi_sense_get_history(float *out, int max_n);
