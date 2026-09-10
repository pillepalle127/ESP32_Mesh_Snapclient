#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A2DP-Sink ueber Bluetooth Classic (BR/EDR) starten.
 * Dekodiertes SBC-PCM wird auf 48 kHz resampelt und dem Source-Arbiter
 * uebergeben. Die Quellenumschaltung erfolgt erst bei aktivem Audiostream.
 */
esp_err_t a2dp_sink_start(void);

#ifdef __cplusplus
}
#endif
