/**
 * @file a2dp_sink_glue.c
 * @brief A2DP sink with a non-blocking Bluetooth callback.
 */
#include "a2dp_sink_glue.h"
#include "source_arbiter.h"
#include "resampler.h"
#include "audio_i2s.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "a2dp";

#define A2DP_DEFAULT_RATE       44100U
#define A2DP_CHANNELS           2U
#define A2DP_BYTES_PER_FRAME    4U
#define A2DP_INPUT_RB_BYTES     (24U * 1024U)
#define A2DP_INPUT_CHUNK_FRAMES 1024U
#define A2DP_OUTPUT_FRAMES      2048U
#define A2DP_TASK_STACK         4096U
#define A2DP_TASK_PRIORITY      7U
#define A2DP_TASK_CORE          1

static RingbufHandle_t s_input_rb = NULL;
static TaskHandle_t s_audio_task = NULL;
static resampler_t s_resampler;
static volatile uint32_t s_input_rate = A2DP_DEFAULT_RATE;
static volatile bool s_audio_started = false;
static volatile bool s_flush_requested = false;
static int16_t s_output[A2DP_OUTPUT_FRAMES * A2DP_CHANNELS];
static char s_device_name[32];
static uint64_t s_callback_dropped = 0;

static uint32_t sbc_sample_rate(const uint8_t *cie)
{
    if (cie == NULL) return A2DP_DEFAULT_RATE;

    const uint8_t bits = cie[0] & 0xF0;
    if (bits & 0x10) return 48000;
    if (bits & 0x20) return 44100;
    if (bits & 0x40) return 32000;
    if (bits & 0x80) return 16000;

    ESP_LOGW(TAG, "Unbekannte SBC-Samplerate: cie[0]=0x%02X", cie[0]);
    return A2DP_DEFAULT_RATE;
}

static esp_err_t create_device_name(void)
{
    uint8_t mac[6] = {0};
    esp_err_t rc = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (rc != ESP_OK) return rc;

    int n = snprintf(s_device_name, sizeof(s_device_name),
                     "Snap-Blth-%02X%02X", mac[4], mac[5]);
    return (n > 0 && n < (int)sizeof(s_device_name)) ? ESP_OK : ESP_FAIL;
}

static void flush_input_buffer(void)
{
    if (s_input_rb == NULL) return;
    size_t size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_input_rb, &size, 0)) != NULL) {
        vRingbufferReturnItem(s_input_rb, item);
    }
}

static void a2dp_audio_task(void *arg)
{
    (void)arg;
    uint32_t configured_rate = 0;

    for (;;) {
        if (s_flush_requested) {
            flush_input_buffer();
            s_flush_requested = false;
        }

        size_t bytes = 0;
        void *item = xRingbufferReceive(s_input_rb, &bytes, pdMS_TO_TICKS(20));
        if (item == NULL) continue;

        uint32_t rate = s_input_rate;
        if (rate != configured_rate) {
            resampler_init(&s_resampler, rate, AUDIO_I2S_SAMPLE_RATE);
            configured_rate = rate;
        }

        if (s_audio_started && arbiter_current() == SRC_A2DP) {
            const int16_t *input = (const int16_t *)item;
            size_t frames = bytes / A2DP_BYTES_PER_FRAME;
            size_t done = 0;

            while (done < frames) {
                size_t chunk = frames - done;
                if (chunk > A2DP_INPUT_CHUNK_FRAMES) {
                    chunk = A2DP_INPUT_CHUNK_FRAMES;
                }

                size_t output_frames = resampler_process(
                    &s_resampler,
                    input + done * A2DP_CHANNELS,
                    chunk,
                    s_output,
                    A2DP_OUTPUT_FRAMES);

                if (output_frames > 0) {
                    arbiter_feed(SRC_A2DP, s_output,
                                 output_frames * A2DP_BYTES_PER_FRAME);
                }
                done += chunk;
            }
        }

        vRingbufferReturnItem(s_input_rb, item);
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (param == NULL) return;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            ESP_LOGI(TAG, "A2DP verbunden, noch kein aktiver Audiostream");
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "A2DP getrennt -> Snapclient wieder freigeben");
            s_audio_started = false;
            arbiter_set_a2dp_connected(false);
            s_flush_requested = true;
            break;
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            ESP_LOGI(TAG, "A2DP-Verbindung wird aufgebaut");
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            ESP_LOGI(TAG, "A2DP-Verbindung wird getrennt");
            break;
        default:
            break;
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            s_input_rate = sbc_sample_rate(param->audio_cfg.mcc.cie.sbc);
            ESP_LOGI(TAG, "A2DP audio cfg: SBC %lu Hz -> resample %d Hz",
                     (unsigned long)s_input_rate, AUDIO_I2S_SAMPLE_RATE);
        } else {
            ESP_LOGW(TAG, "Nicht unterstuetzter A2DP-Codec: %d",
                     (int)param->audio_cfg.mcc.type);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state=%d", (int)param->audio_stat.state);
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_audio_started = true;
            arbiter_set_a2dp_connected(true);
        } else {
            s_audio_started = false;
            arbiter_set_a2dp_connected(false);
            s_flush_requested = true;
        }
        break;

    default:
        break;
    }
}

static void a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (!s_audio_started || s_input_rb == NULL || data == NULL || len < 4) return;

    /* The Bluetooth callback never resamples and never waits. */
    if (xRingbufferSend(s_input_rb, data, len, 0) != pdTRUE) {
        s_callback_dropped += len;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    (void)event;
    (void)param;
}

esp_err_t a2dp_sink_start(void)
{
    esp_err_t rc;
    bool controller_initialized = false;
    bool controller_enabled = false;
    bool bluedroid_initialized = false;
    bool bluedroid_enabled = false;
    bool avrc_initialized = false;
    bool a2dp_initialized = false;

    if (s_input_rb != NULL) return ESP_ERR_INVALID_STATE;

    rc = create_device_name();
    if (rc != ESP_OK) return rc;

    s_input_rb = xRingbufferCreate(A2DP_INPUT_RB_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_input_rb == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreatePinnedToCore(a2dp_audio_task, "a2dp_audio",
                               A2DP_TASK_STACK, NULL, A2DP_TASK_PRIORITY,
                               &s_audio_task, A2DP_TASK_CORE) != pdPASS) {
        vRingbufferDelete(s_input_rb);
        s_input_rb = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    rc = esp_bt_controller_init(&config);
    if (rc != ESP_OK) goto fail;
    controller_initialized = true;

    rc = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (rc != ESP_OK) goto fail;
    controller_enabled = true;

    rc = esp_bluedroid_init();
    if (rc != ESP_OK) goto fail;
    bluedroid_initialized = true;

    rc = esp_bluedroid_enable();
    if (rc != ESP_OK) goto fail;
    bluedroid_enabled = true;

    rc = esp_bt_gap_set_device_name(s_device_name);
    if (rc != ESP_OK) goto fail;

    rc = esp_avrc_ct_init();
    if (rc != ESP_OK) goto fail;
    avrc_initialized = true;

    rc = esp_avrc_ct_register_callback(avrc_ct_cb);
    if (rc != ESP_OK) goto fail;

    rc = esp_a2d_register_callback(a2d_cb);
    if (rc != ESP_OK) goto fail;

    rc = esp_a2d_sink_register_data_callback(a2d_data_cb);
    if (rc != ESP_OK) goto fail;

    rc = esp_a2d_sink_init();
    if (rc != ESP_OK) goto fail;
    a2dp_initialized = true;

    rc = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                  ESP_BT_GENERAL_DISCOVERABLE);
    if (rc != ESP_OK) goto fail;

    ESP_LOGI(TAG, "A2DP-Sink aktiv als '%s' (BTDM)", s_device_name);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "A2DP-Start fehlgeschlagen: %s", esp_err_to_name(rc));
    if (a2dp_initialized) esp_a2d_sink_deinit();
    if (avrc_initialized) esp_avrc_ct_deinit();
    if (bluedroid_enabled) esp_bluedroid_disable();
    if (bluedroid_initialized) esp_bluedroid_deinit();
    if (controller_enabled) esp_bt_controller_disable();
    if (controller_initialized) esp_bt_controller_deinit();
    if (s_audio_task != NULL) {
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }
    if (s_input_rb != NULL) {
        vRingbufferDelete(s_input_rb);
        s_input_rb = NULL;
    }
    return rc;
}
