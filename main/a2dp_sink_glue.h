#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/** A2DP-Sink (BT Classic) starten; PCM-Callback -> Arbiter. */
esp_err_t a2dp_sink_start(void);
#ifdef __cplusplus
}
#endif
