#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SRC_NONE = 0,
    SRC_SNAPCAST,
    SRC_A2DP,
} audio_src_t;

typedef enum {
    PRIO_A2DP_FIRST = 0,
    PRIO_SNAPCAST_FIRST,
} audio_prio_t;

typedef void (*arbiter_snap_pause_cb_t)(bool pause);

esp_err_t arbiter_init(audio_prio_t prio);
void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb);
void arbiter_set_snapcast_active(bool active);
void arbiter_set_a2dp_connected(bool connected);
size_t arbiter_feed(audio_src_t from, const void *pcm, size_t bytes);
audio_src_t arbiter_current(void);

#ifdef __cplusplus
}
#endif
