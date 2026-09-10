#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Snapclient-Task starten (verbindet mit Snapserver, dekodiert -> Arbiter). */
esp_err_t snapclient_start(const char *host, uint16_t port);

/**
 * Pause/Resume fuer die Koexistenz. Wird vom Arbiter aufgerufen:
 *   pause=true  -> TCP-Socket schliessen (WLAN-Airtime frei fuer A2DP)
 *   pause=false -> Reconnect zum Snapserver
 */
void snapclient_pause(bool pause);

#ifdef __cplusplus
}
#endif
