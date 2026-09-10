#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapclient-Task starten. Der TCP-Aufbau wartet auf Netzwerkfreigabe. */
esp_err_t snapclient_start(const char *host, uint16_t port);

/**
 * Aktuellen Mesh-/IP-Netzwerkzustand an den Snapclient melden.
 * false bricht einen laufenden Socket sofort per shutdown() ab.
 * true gibt einen unmittelbaren, zeitlich begrenzten Reconnect frei.
 */
void snapclient_set_network_available(bool available);

/** Pause/Resume fuer die A2DP-Koexistenz. */
void snapclient_pause(bool pause);

#ifdef __cplusplus
}
#endif
