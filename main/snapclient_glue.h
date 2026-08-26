#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t snapclient_start(const char *host, uint16_t port);
void snapclient_pause(bool pause);
#ifdef __cplusplus
}
#endif
