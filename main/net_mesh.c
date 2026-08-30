/**
 * @file  net_mesh.c
 * @brief ESP-Mesh-Lite bring-up for a non-root routing node.
 *
 * Mesh-Lite is self-organizing and self-healing. Every node has its own
 * LWIP stack and therefore its own IP address, so the Snapclient uses normal
 * sockets.
 *
 * This client:
 *   - must never become the root node (level 1),
 *   - does not use an external Wi-Fi router as uplink,
 *   - may connect through another Mesh-Lite parent,
 *   - keeps its SoftAP active and may serve additional child nodes,
 *   - remains capable of operating as a relay in a multi-hop chain.
 *
 * Mesh-Lite controls Wi-Fi connection and scanning. The application must not
 * call esp_wifi_connect() or start its own Wi-Fi scans.
 */

#include "net_mesh.h"
#include "snapclient_glue.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bridge.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#include "esp_wifi.h"

static const char *TAG = "net_mesh";
static atomic_bool s_snapclient_started = false;

/* Start the Snapclient once the station interface has received an IP address. */
static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP || data == NULL) {
        return;
    }

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "GOT IP: " IPSTR " (mesh uplink ready)",
             IP2STR(&event->ip_info.ip));

    /* Eine laufende Reconnect-Wartezeit des Snapclients sofort beenden. */
    snapclient_network_available();

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_snapclient_started,
                                        &expected, true)) {
        return;
    }

    esp_err_t err = snapclient_start(CONFIG_SNAPSERVER_HOST,
                                     CONFIG_SNAPSERVER_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Snapclient start failed: %s", esp_err_to_name(err));
        atomic_store(&s_snapclient_started, false);
        return;
    }

    ESP_LOGI(TAG, "Snapclient started after GOT IP");
}

/* Periodically report the current Mesh-Lite level. */
static void mesh_info_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint8_t level = esp_mesh_lite_get_level();

        if (level == 0) {
            ESP_LOGI(TAG, "mesh level=0 [not connected]");
        } else if (level == 1) {
            ESP_LOGE(TAG, "mesh level=1 [unexpected ROOT state]");
        } else {
            ESP_LOGI(TAG, "mesh level=%u [non-root routing node]", level);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t net_mesh_start(void)
{
    /* Create the Mesh-Lite SoftAP and station network interfaces. */
    esp_bridge_create_all_netif();

    /* Register the IP handler before Mesh-Lite starts connecting. */
    esp_err_t err = esp_event_handler_instance_register(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        &ip_event_handler,
        NULL,
        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event handler registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /*
     * Initialize Mesh-Lite for an autonomous mesh without an external router.
     * The node is not a leaf because it must keep its SoftAP active and relay
     * traffic for child nodes in a multi-hop chain.
     */
    esp_mesh_lite_config_t mesh_cfg = ESP_MESH_LITE_DEFAULT_INIT();
    mesh_cfg.join_mesh_ignore_router_status = true;
    mesh_cfg.join_mesh_without_configured_wifi = true;
    mesh_cfg.leaf_node = false;

    esp_mesh_lite_init(&mesh_cfg);

    /*
     * CLIENT role:
     * Level 1 is forbidden, so this device can never become root. All higher
     * levels remain available and its SoftAP remains active for child nodes.
     */
    err = esp_mesh_lite_set_disallowed_level(1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not block root level: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Configure this node's inter-node SoftAP. The MAC suffix makes the SSID
     * unique while all nodes continue using the common prefix and password.
     */
    uint8_t mac[6] = {0};
    err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read SoftAP MAC: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap = {0};
    const int written = snprintf((char *)ap.ap.ssid,
                                 sizeof(ap.ap.ssid),
                                 "%s_%02X%02X%02X",
                                 CONFIG_MESH_SOFTAP_SSID_PREFIX,
                                 mac[3], mac[4], mac[5]);
    if (written < 0 || written >= (int)sizeof(ap.ap.ssid)) {
        ESP_LOGE(TAG, "Mesh SoftAP SSID is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password,
            CONFIG_MESH_SOFTAP_PASSWORD,
            sizeof(ap.ap.password));
    ap.ap.authmode = strlen(CONFIG_MESH_SOFTAP_PASSWORD) > 0
                         ? WIFI_AUTH_WPA2_PSK
                         : WIFI_AUTH_OPEN;
    ap.ap.max_connection = 10;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Connect and start Mesh-Lite. */
    esp_mesh_lite_connect();
    esp_mesh_lite_start();

    if (xTaskCreate(mesh_info_task,
                    "mesh_info",
                    3072,
                    NULL,
                    3,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create mesh_info task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Mesh-Lite started as non-root multi-hop routing node "
             "(no router uplink, SoftAP=%s)",
             (char *)ap.ap.ssid);

    return ESP_OK;
}
