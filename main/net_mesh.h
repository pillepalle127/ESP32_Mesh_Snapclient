#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ESP-Mesh-Lite als Non-Root-Snapclient im No-Router-Mesh starten.
 *
 * Der ESP32-S3-Snapserver ist der einzige Root auf Level 1. Der TCP-Snapclient
 * startet automatisch nach IP_EVENT_STA_GOT_IP. Linkverlust und erneute
 * Netzwerkverfuegbarkeit werden an snapclient_glue weitergegeben.
 */
esp_err_t net_mesh_start(void);

#ifdef __cplusplus
}
#endif
