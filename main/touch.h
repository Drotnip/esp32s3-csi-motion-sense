#pragma once
#include <stdbool.h>

// Minimal FT6336G capacitive-touch driver (single touch point, polled).
void touch_init(void);
// Returns true and fills *x/*y (native 240x320 coords) when a finger is down.
bool touch_read(int *x, int *y);
