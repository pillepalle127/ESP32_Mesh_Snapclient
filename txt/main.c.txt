/**
 * @file  main.c
 * @brief Einstiegspunkt: Init-Reihenfolge fuer Mesh-Lite Snapclient + A2DP.
 *
 * Projekt: ESP32 WROVER, IDF v5.5.x
 *   [Snapclient] --\
 *                   >-- Source-Arbiter -- Ringpuffer -- I2S(Master) -- ADAU1701
 *   [A2DP-Sink]  --/
 *
 * Es streamt immer nur EINE Quelle (WLAN/BT teilen sich ein Funkmodul).
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

    /* 5) Mesh-Lite starten (WLAN self-organizing/-healing) */
    ESP_ERROR_CHECK(net_mesh_start());

    /* 6) Quellen anmelden.
     *    Hinweis Koexistenz: A2DP + aktives WLAN-Streaming stoeren sich.
     *    Der Arbiter pausiert daher den Snapclient, sobald A2DP verbindet. */
    //ESP_ERROR_CHECK(snapclient_start(CONFIG_SNAPSERVER_HOST, CONFIG_SNAPSERVER_PORT));   /* verbindet sich mit Snapserver  */
    ESP_ERROR_CHECK(a2dp_sink_start());    /* wartet auf BT-Quelle           */

    ESP_LOGI(TAG, "init complete");
}
