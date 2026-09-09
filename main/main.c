/**
 * @file main.c
 * @brief Application entry point for the routerless ESP-Mesh-Lite Snapclient.
 *
 * Target: ESP-IDF 5.4.3 on ESP32.
 *
 * Initialization order:
 * 1. NVS
 * 2. TCP/IP stack and default event loop
 * 3. I2S output
 * 4. source arbiter and audio player task
 * 5. routerless Mesh-Lite child networking
 *
 * net_mesh.c starts the Snapcast client only after the STA interface has
 * received an IP address from its selected parent. This avoids an early TCP
 * connection attempt without a valid route to the Snapserver.
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

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32 routerless Mesh-Lite Snapclient");

    /* NVS stores Wi-Fi calibration and Mesh-Lite runtime data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Start the I2S master before the player task. */
    ESP_ERROR_CHECK(audio_i2s_init());

    /*
     * The minimal build has one active network audio source. The arbiter is
     * retained as the boundary between stream decoding and I2S playback.
     */
    ESP_ERROR_CHECK(arbiter_init(PRIO_A2DP_FIRST));

    /* Starts Mesh-Lite; Snapcast starts later from IP_EVENT_STA_GOT_IP. */
    ESP_ERROR_CHECK(net_mesh_start());

    ESP_LOGI(TAG, "init complete");
}
