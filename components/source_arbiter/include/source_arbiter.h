/**
 * @file  source_arbiter.h
 * @brief Quellenauswahl + automatische Umschaltung Snapcast <-> A2DP.
 *
 * Kernregel: Es streamt immer nur EINE Quelle (WLAN/BT teilen sich EIN
 * Funkmodul). Beim Wechsel auf A2DP wird der Snapclient-Socket pausiert,
 * damit die WLAN-Airtime frei wird. Umschaltung immer ueber Fade/Mute,
 * der I2S-Takt laeuft dabei durchgehend weiter (kein PLL-Re-Lock am ADAU).
 */
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

/** Prioritaetsstrategie fuer die Arbitrierung. */
typedef enum {
    PRIO_A2DP_FIRST = 0,   /* A2DP uebersteuert Snapcast, sobald verbunden */
    PRIO_SNAPCAST_FIRST,   /* Snapcast hat Vorrang, A2DP nur als Fallback  */
} audio_prio_t;

/**
 * Callback, mit dem der Arbiter die WLAN-Quelle (Snapclient) bei aktivem
 * A2DP pausiert und danach wieder freigibt. Vom App-Layer registriert.
 * pause=true -> Socket schliessen (Airtime frei); pause=false -> reconnect.
 */
typedef void (*arbiter_snap_pause_cb_t)(bool pause);

/** Arbiter + Ringpuffer + Mixer-Task starten. */
esp_err_t arbiter_init(audio_prio_t prio);

/** Pause/Resume-Callback fuer die Snapcast-Quelle registrieren. */
void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb);

/** Statusmeldungen von den Quellen (thread-safe). */
void arbiter_set_snapcast_active(bool active);
void arbiter_set_a2dp_connected(bool connected);

/**
 * PCM von einer Quelle einspeisen. Nur Daten der aktiven Quelle landen
 * im Ringpuffer; Daten der inaktiven Quelle werden verworfen.
 */
size_t arbiter_feed(audio_src_t from, const void *pcm, size_t bytes);

/** Aktuell aktive Quelle abfragen (Logging/Diagnose). */
audio_src_t arbiter_current(void);

#ifdef __cplusplus
}
#endif
