/**
 * @file  main.c
 * @brief Einstiegspunkt: Init-Reihenfolge fuer Mesh-Lite Snapclient + A2DP.
 *
 * Projekt: ESP32-WROVER, ESP-IDF v5.4.3
 *   [Snapclient] --\
 *                   >-- Source-Arbiter -- Ringpuffer -- I2S(Master) -- ADAU1701
 *   [A2DP-Sink]  --/
 *
 * Es wird immer nur eine Audioquelle ausgegeben. Bei aktivem A2DP-
 * Audiostream pausiert der Arbiter den Snapclient; die Mesh-Verbindung bleibt
 * aktiv. Nach A2DP-Ende wird Snapcast automatisch wieder verbunden.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "audio_i2s.h"
#include "source_arbiter.h"
#include "net_mesh.h"
#include "snapclient_glue.h"
#include "a2dp_sink_glue.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32 mesh-lite snapclient + a2dp");

    /* 1) NVS (WLAN-Kalibrierung, Mesh-/BT-Config) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2) Netif + Event-Loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3) I2S-Master zuerst hoch -> Takt zum ADAU steht, PLL kann locken */
    ESP_ERROR_CHECK(audio_i2s_init());

    /* 4) Arbiter/Player-Task. A2DP hat Vorrang, sobald ein Geraet verbindet.
     *    Pause-Callback verdrahten: bei A2DP wird der Snapclient-Socket
     *    geschlossen (Koexistenz), bei Rueckkehr reconnectet er. */
    ESP_ERROR_CHECK(arbiter_init(PRIO_A2DP_FIRST));
    arbiter_register_snap_pause_cb(snapclient_pause);

    /* 5) Mesh-Lite im autonomen No-Router-Modus starten.
     *    Der ESP32-S3-Snapserver ist Root/Level 1. Dieser Client darf nur
     *    Level 2 oder hoeher annehmen. Der Snapclient startet erst nach GOT_IP. */
    ESP_ERROR_CHECK(net_mesh_start());

    /* 6) A2DP-Sink starten.
     *    Der Snapclient wird nicht hier gestartet, sondern von net_mesh.c nach
     *    IP_EVENT_STA_GOT_IP. A2DP erhaelt erst bei AUDIO_STATE_STARTED Vorrang. */
    ESP_ERROR_CHECK(a2dp_sink_start());

    ESP_LOGI(TAG, "init complete");
}
