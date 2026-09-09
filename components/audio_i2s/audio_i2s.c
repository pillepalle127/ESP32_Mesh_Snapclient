/**
 * @file audio_i2s.c
 * @brief 48 kHz, stereo, 16-bit Philips-I2S output for ESP-IDF 5.4.3.
 */
#include "audio_i2s.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio_i2s";

#define PIN_MCLK   GPIO_NUM_0
#define PIN_BCLK   GPIO_NUM_23
#define PIN_WS     GPIO_NUM_22
#define PIN_DOUT   GPIO_NUM_21

static i2s_chan_handle_t s_tx_chan = NULL;
static volatile bool s_muted = false;
static uint64_t s_i2s_written = 0;
static uint32_t s_i2s_errors = 0;
static uint32_t s_i2s_partial = 0;
static int64_t s_i2s_last_log = 0;

esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_I2S_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "init_std_mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan),
                        TAG, "channel_enable failed");

    s_i2s_last_log = esp_timer_get_time();
    ESP_LOGI(TAG, "I2S master up: 48kHz, MCLK=12.288MHz on GPIO%d", PIN_MCLK);
    return ESP_OK;
}

esp_err_t audio_i2s_write(const void *pcm, size_t bytes,
                          size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_tx_chan == NULL) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || bytes == 0) return ESP_ERR_INVALID_ARG;

    static const uint8_t zeros[512] = {0};
    const uint8_t *input = (const uint8_t *)pcm;
    size_t total = 0;
    esp_err_t result = ESP_OK;

    while (total < bytes) {
        const void *data;
        size_t request;
        if (s_muted) {
            data = zeros;
            request = bytes - total;
            if (request > sizeof(zeros)) request = sizeof(zeros);
        } else {
            data = input + total;
            request = bytes - total;
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, data, request, &written,
                                          pdMS_TO_TICKS(timeout_ms));
        total += written;

        if (err != ESP_OK) {
            result = err;
            break;
        }
        if (written == 0) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (written < request) s_i2s_partial++;
    }

    if (bytes_written != NULL) *bytes_written = total;
    s_i2s_written += total;
    if (result != ESP_OK) s_i2s_errors++;

    int64_t now = esp_timer_get_time();
    if (now - s_i2s_last_log >= 5000000LL) {
        ESP_LOGI(TAG,
                 "I2S stats/5s: written=%llu B, errors=%lu, partial=%lu, muted=%d",
                 (unsigned long long)s_i2s_written,
                 (unsigned long)s_i2s_errors,
                 (unsigned long)s_i2s_partial,
                 (int)s_muted);
        s_i2s_written = 0;
        s_i2s_errors = 0;
        s_i2s_partial = 0;
        s_i2s_last_log = now;
    }
    return result;
}

esp_err_t audio_i2s_mute(bool mute)
{
    s_muted = mute;
    return ESP_OK;
}
