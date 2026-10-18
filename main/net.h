#pragma once
#include <stdbool.h>

// Connects as a Wi-Fi station and pings the gateway at 50 Hz so there is a
// steady stream of received frames for CSI to measure. Call once from app_main.
void net_init(void);
bool net_is_up(void);
