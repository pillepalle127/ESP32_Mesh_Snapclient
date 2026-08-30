/**
 * @file  source_arbiter.h
 * @brief Quellenauswahl, automatische Umschaltung und Frequenzweiche.
 *
 * Datenpfad:
 *
 *   Snapcast oder Bluetooth A2DP
 *       -> Source-Arbiter
 *       -> Stereo zu Mono
 *       -> Linkwitz-Riley-Frequenzweiche
 *       -> links: Subwoofer
 *       -> rechts: Breitbandlautsprecher
 *       -> I2S
 *
 * Es streamt immer nur eine Quelle.
 *
 * Beim Wechsel auf A2DP wird der Snapclient-Socket pausiert.
 * Beim Verlassen von A2DP wird der Snapclient wieder freigegeben.
 *
 * Der I2S-Takt läuft während der Quellenumschaltung weiter.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef enum
{
    SRC_NONE = 0,
    SRC_SNAPCAST,
    SRC_A2DP
} audio_src_t;


typedef enum
{
    PRIO_A2DP_FIRST = 0,
    PRIO_SNAPCAST_FIRST
} audio_prio_t;


/**
 * Callback zur Pause- und Resume-Steuerung des Snapclients.
 *
 * pause=true:
 *   Snapclient-Socket schließen.
 *
 * pause=false:
 *   Verbindung zum Snapserver erneut aufbauen.
 */
typedef void (*arbiter_snap_pause_cb_t)(bool pause);


/**
 * Source-Arbiter, Ringpuffer, Player-Task und Frequenzweiche starten.
 */
esp_err_t arbiter_init(audio_prio_t prio);


/**
 * Pause- und Resume-Callback des Snapclients registrieren.
 */
void arbiter_register_snap_pause_cb(
    arbiter_snap_pause_cb_t cb);


/**
 * Snapcast-Status setzen.
 */
void arbiter_set_snapcast_active(bool active);


/**
 * A2DP-Audiostream-Status setzen.
 *
 * Der Parameter bezeichnet den aktiven Audiostream und nicht nur
 * den Zustand der Bluetooth-Verbindung.
 */
void arbiter_set_a2dp_connected(bool connected);


/**
 * PCM-Daten einer Quelle einspeisen.
 *
 * Erwartetes Format:
 *
 *   48 kHz
 *   16 Bit signed PCM
 *   Stereo
 *   interleaved
 *
 * Nur Daten der aktiven Quelle werden übernommen.
 *
 * Rückgabewert:
 *
 *   Anzahl der in den Ringpuffer übernommenen Bytes.
 */
size_t arbiter_feed(
    audio_src_t from,
    const void *pcm,
    size_t bytes);


/**
 * Aktuell ausgewählte Audioquelle abfragen.
 */
audio_src_t arbiter_current(void);


#ifdef __cplusplus
}
#endif