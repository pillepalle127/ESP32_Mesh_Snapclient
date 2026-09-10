/**
 * @file  net_mesh.c
 * @brief ESP-Mesh-Lite Snapclient im autonomen No-Router-Mesh.
 *
 * Topologie:
 *
 *   ESP32-S3 Snapserver
 *     - einziger Root, Mesh-Level 1
 *     - SoftAP 192.168.5.1
 *     - SSID-Praefix CONFIG_MESH_SOFTAP_SSID_PREFIX
 *
 *   ESP32 Snapclient
 *     - darf niemals Root beziehungsweise Level 1 werden
 *     - sucht selbststaendig einen passenden Mesh-Lite-Parent
 *     - benoetigt keinen externen WLAN-Router
 *     - startet den TCP-Snapclient erst nach IP_EVENT_STA_GOT_IP
 *     - meldet Linkverlust und neue IP an den Snapclient, damit blockierende
 *       TCP-Reconnects vermieden und laufende Sockets sofort beendet werden
 */

#include "net_mesh.h"
#include "snapclient_glue.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bridge.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "net_mesh";

static atomic_bool s_snapclient_started = false;
static atomic_bool s_sta_connected = false;
static atomic_bool s_sta_has_ip = false;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            atomic_store(&s_sta_connected, true);
            ESP_LOGI(TAG, "STA mit Mesh-Parent verbunden");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            atomic_store(&s_sta_connected, false);
            atomic_store(&s_sta_has_ip, false);
            snapclient_set_network_available(false);
            ESP_LOGW(TAG, "STA vom Mesh-Parent getrennt");
            break;

        default:
            break;
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_STA_GOT_IP || event_data == NULL) {
        return;
    }

    const ip_event_got_ip_t *event =
        (const ip_event_got_ip_t *)event_data;

    atomic_store(&s_sta_has_ip, true);
    snapclient_set_network_available(true);

    ESP_LOGI(TAG,
             "Mesh-IP erhalten: " IPSTR ", Gateway/Snapserver: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.gw));

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_snapclient_started,
                                        &expected,
                                        true)) {
        return;
    }

    ESP_LOGI(TAG,
             "Starte Snapclient fuer %s:%u",
             CONFIG_SNAPSERVER_HOST,
             (unsigned)CONFIG_SNAPSERVER_PORT);

    esp_err_t result = snapclient_start(CONFIG_SNAPSERVER_HOST,
                                        CONFIG_SNAPSERVER_PORT);
    if (result != ESP_OK) {
        atomic_store(&s_snapclient_started, false);
        ESP_LOGE(TAG,
                 "Snapclient-Start fehlgeschlagen: %s",
                 esp_err_to_name(result));
        return;
    }

    ESP_LOGI(TAG, "Snapclient nach erfolgreichem Mesh-IP-Bezug gestartet");
}

static void mesh_info_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint8_t level = esp_mesh_lite_get_level();
        const bool connected = atomic_load(&s_sta_connected);
        const bool has_ip = atomic_load(&s_sta_has_ip);

        if (level == 0) {
            ESP_LOGI(TAG,
                     "Mesh: Parent wird gesucht, level=0, STA=%s, IP=%s",
                     connected ? "verbunden" : "getrennt",
                     has_ip ? "vorhanden" : "fehlt");
        } else if (level == 1) {
            ESP_LOGE(TAG,
                     "Mesh-Konfigurationsfehler: Snapclient wurde Root/Level 1");
        } else {
            ESP_LOGI(TAG,
                     "Mesh: verbunden, level=%u, STA=%s, IP=%s",
                     level,
                     connected ? "verbunden" : "getrennt",
                     has_ip ? "vorhanden" : "fehlt");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static esp_err_t configure_client_softap(void)
{
    uint8_t mac[6] = {0};
    esp_err_t result = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "SoftAP-MAC konnte nicht gelesen werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    wifi_config_t ap_config = {0};

    const int ssid_length = snprintf(
        (char *)ap_config.ap.ssid,
        sizeof(ap_config.ap.ssid),
        "%s_%02X%02X%02X",
        CONFIG_MESH_SOFTAP_SSID_PREFIX,
        mac[3],
        mac[4],
        mac[5]);

    if (ssid_length <= 0 ||
        ssid_length >= (int)sizeof(ap_config.ap.ssid)) {
        ESP_LOGE(TAG, "Mesh-SoftAP-SSID ist ungueltig oder zu lang");
        return ESP_ERR_INVALID_SIZE;
    }

    ap_config.ap.ssid_len = (uint8_t)ssid_length;
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 10;
    ap_config.ap.beacon_interval = 100;

    const size_t password_length = strlen(CONFIG_MESH_SOFTAP_PASSWORD);
    if (password_length == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        if (password_length < 8 ||
            password_length >= sizeof(ap_config.ap.password)) {
            ESP_LOGE(TAG,
                     "Mesh-SoftAP-Passwort muss 8 bis 63 Zeichen lang sein");
            return ESP_ERR_INVALID_ARG;
        }

        strlcpy((char *)ap_config.ap.password,
                CONFIG_MESH_SOFTAP_PASSWORD,
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Mesh-SoftAP-Konfiguration fehlgeschlagen: %s",
                 esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG,
             "Lokaler Mesh-SoftAP vorbereitet: SSID=%s, Root-Verbindung=no-router",
             (const char *)ap_config.ap.ssid);

    return ESP_OK;
}

esp_err_t net_mesh_start(void)
{
    atomic_store(&s_snapclient_started, false);
    atomic_store(&s_sta_connected, false);
    atomic_store(&s_sta_has_ip, false);

    /*
     * ESP-IoT-Bridge erzeugt die von Mesh-Lite benoetigten AP- und
     * STA-Netifs. Event-Loop und esp_netif wurden bereits in app_main()
     * initialisiert.
     */
    esp_bridge_create_all_netif();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "WLAN-Initialisierung fehlgeschlagen: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "WLAN-RAM-Storage konnte nicht gesetzt werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "WLAN-Modus APSTA konnte nicht gesetzt werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    /*
     * Die vom IoT-Bridge-Kconfig gelesenen Router-Platzhalter duerfen nicht
     * als reale STA-Zugangsdaten aktiv bleiben. Im No-Router-Modus waehlt
     * Mesh-Lite den Parent anhand seiner Vendor-IE und Mesh-ID selbst aus.
     */
    wifi_config_t empty_sta_config = {0};
    result = esp_wifi_set_config(WIFI_IF_STA, &empty_sta_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Externe Router-Konfiguration konnte nicht geloescht werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = configure_client_softap();
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_instance_register(WIFI_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 wifi_event_handler,
                                                 NULL,
                                                 NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "WLAN-Eventhandler konnte nicht registriert werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = esp_event_handler_instance_register(IP_EVENT,
                                                 IP_EVENT_STA_GOT_IP,
                                                 ip_event_handler,
                                                 NULL,
                                                 NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "IP-Eventhandler konnte nicht registriert werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "WLAN konnte nicht gestartet werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    esp_mesh_lite_config_t mesh_config = ESP_MESH_LITE_DEFAULT_INIT();

    /*
     * Verbindlicher No-Router-Betrieb:
     * - kein Status eines externen Routers erforderlich
     * - Parent-Suche ohne konfigurierte Router-SSID zulassen
     */
    mesh_config.join_mesh_ignore_router_status = true;
    mesh_config.join_mesh_without_configured_wifi = true;

    esp_mesh_lite_init(&mesh_config);

    /*
     * Mesh-Lite-Verbindungsdaten fuer den lokalen SoftAP und die
     * WPA2-Anmeldung am ausgewaehlten Parent hinterlegen.
     */
    result = esp_mesh_lite_set_softap_info(
        CONFIG_MESH_SOFTAP_SSID_PREFIX,
        CONFIG_MESH_SOFTAP_PASSWORD);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Mesh-Lite-SoftAP-Information konnte nicht gesetzt werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    /* Nur der autonome ESP32-S3-Snapserver darf Level 1/Root sein. */
    result = esp_mesh_lite_set_disallowed_level(1);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Mesh-Level 1 konnte fuer den Client nicht gesperrt werden: %s",
                 esp_err_to_name(result));
        return result;
    }

    esp_mesh_lite_connect();
    esp_mesh_lite_start();

    BaseType_t task_result = xTaskCreate(mesh_info_task,
                                         "mesh_info",
                                         3072,
                                         NULL,
                                         3,
                                         NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "mesh_info-Task konnte nicht erstellt werden");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Mesh-Lite-Client gestartet: No-Router, Non-Root, Prefix=%s, "
             "Snapserver=%s:%u",
             CONFIG_MESH_SOFTAP_SSID_PREFIX,
             CONFIG_SNAPSERVER_HOST,
             (unsigned)CONFIG_SNAPSERVER_PORT);

    return ESP_OK;
}
