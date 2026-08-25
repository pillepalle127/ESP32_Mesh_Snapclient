/**
 * @file  net_mesh.c
 * @brief ESP-Mesh-Lite Bring-up (voll implementiert).
 *
 * Mesh-Lite ist selbstorganisierend/-heilend; jeder Knoten hat einen eigenen
 * LWIP-Stack und damit eine eigene IP -> der Snapclient nutzt normale Sockets.
 *
 * Startsequenz (managed component espressif/mesh_lite):
 *   esp_bridge_create_all_netif();
 *   esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
 *   esp_mesh_lite_init(&cfg);
 *   esp_mesh_lite_set_router_config(&router);
 *   esp_mesh_lite_connect();
 *   esp_mesh_lite_start();
 *
 * WICHTIG: Im Self-Organized-Betrieb keine esp_wifi_connect()/scan()-Aufrufe
 * aus der App -> Mesh-Lite steuert das WLAN selbst.
 */
#include "net_mesh.h"
#include "snapclient_glue.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include <stdatomic.h>
#include "esp_bridge.h"
#include "esp_mesh_lite.h"

static const char *TAG = "net_mesh";
static atomic_bool s_snapclient_started = false;

/* --- Mesh-/IP-Events: Level- und Root-Status protokollieren --------------- */
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "GOT IP: " IPSTR " (Uplink steht)", IP2STR(&e->ip_info.ip));
		bool expected = false;

		if (atomic_compare_exchange_strong(
				&s_snapclient_started, &expected, true)) {

			esp_err_t err = snapclient_start(
				CONFIG_SNAPSERVER_HOST,
				CONFIG_SNAPSERVER_PORT);

			if (err != ESP_OK) {
				ESP_LOGE(TAG, "Snapclient-Start fehlgeschlagen: %s",
						 esp_err_to_name(err));

				atomic_store(&s_snapclient_started, false);
			} else {
				ESP_LOGI(TAG, "Snapclient nach GOT IP gestartet");
			}
		}
    }
}

/* Periodisch Topologie-Info ausgeben (Level, Knotenzahl). */
static void mesh_info_task(void *arg)
{
    for (;;) {
        uint8_t level = esp_mesh_lite_get_level();
        ESP_LOGI(TAG, "mesh level=%u  %s", level,
                 (level == 1) ? "[ROOT - Uplink zum Router/Snapserver]"
                              : "[Child - via Parent]");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t net_mesh_start(void)
{
    /* 1) Netif-Bridge (SoftAP+STA je Knoten) und WLAN-Basis */
    esp_bridge_create_all_netif();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    /* 2) Mesh-Lite initialisieren */
    esp_mesh_lite_config_t mesh_cfg = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_cfg);

    /* 3) Uplink-Router fuer den Root-Knoten (aus menuconfig) */
	mesh_lite_sta_config_t router = { 0 };

    strlcpy((char *)router.ssid,     CONFIG_ROUTER_SSID,     sizeof(router.ssid));
    strlcpy((char *)router.password, CONFIG_ROUTER_PASSWORD, sizeof(router.password));
    esp_mesh_lite_set_router_config(&router);

    /* 4) Inter-Node-SoftAP direkt im RAM setzen (kein NVS-Write pro Boot!).
     *    SSID = Prefix + MAC-Suffix -> je Knoten eindeutig. */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s_%02X%02X%02X",
             CONFIG_MESH_SOFTAP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, CONFIG_MESH_SOFTAP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode      = strlen(CONFIG_MESH_SOFTAP_PASSWORD) ? WIFI_AUTH_WPA2_PSK
                                                              : WIFI_AUTH_OPEN;
    ap.ap.max_connection = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* 5) Netzwerk aufbauen und starten */
    esp_mesh_lite_connect();
    esp_mesh_lite_start();

    xTaskCreate(mesh_info_task, "mesh_info", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "mesh-lite gestartet (Router=%s, SoftAP-Prefix=%s)",
             CONFIG_ROUTER_SSID, CONFIG_MESH_SOFTAP_SSID_PREFIX);
    return ESP_OK;
}
