#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapclient-Task starten; der TCP-Aufbau wartet auf eine gueltige Mesh-IP. */
esp_err_t snapclient_start(const char *host, uint16_t port);

/**
 * Mesh-/IP-Netzwerkzustand melden.
 * false unterbricht den laufenden Socket sofort per shutdown().
 * true gibt einen unmittelbaren, zeitlich begrenzten Reconnect frei.
 */
void snapclient_set_network_available(bool available);

/** Snapcast fuer einen aktiven A2DP-Audiostream pausieren oder fortsetzen. */
void snapclient_pause(bool pause);

#ifdef __cplusplus
}
#endif
