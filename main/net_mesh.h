/**
 * @file net_mesh.h
 * @brief Routerless ESP-Mesh-Lite child-network interface.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the IoT bridge and start this device as Mesh-Lite child.
 *
 * The Snapclient is started automatically after the STA interface receives
 * an address from its parent.
 */
esp_err_t net_mesh_start(void);

#ifdef __cplusplus
}
#endif
