/**
 * @file snapclient_glue.h
 * @brief Public interface of the Snapcast stream client.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deferred network-recovery notification.
 *
 * The callback runs in the Snapclient task and must return immediately. A
 * separate network task must perform Wi-Fi or Mesh-Lite recovery work.
 */
typedef void (*snapclient_recovery_cb_t)(void);

/**
 * @brief Start the Snapcast client task.
 *
 * The client connects asynchronously and reconnects automatically. The call
 * is idempotent while the task is already running.
 */
esp_err_t snapclient_start(const char *host, uint16_t port);

/**
 * @brief Pause or resume Snapcast reception.
 *
 * Pause is retained for integration with an optional external source arbiter.
 * The current minimal build does not initialize Bluetooth/A2DP in main.c.
 */
void snapclient_pause(bool pause);

/** Register the non-blocking network-recovery callback. */
void snapclient_set_recovery_callback(snapclient_recovery_cb_t callback);

#ifdef __cplusplus
}
#endif
