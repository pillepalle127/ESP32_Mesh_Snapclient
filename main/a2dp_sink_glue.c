/**
 * @file  a2dp_sink_glue.c
 * @brief A2DP-Sink (Bluetooth Classic) -> Resampler -> Source-Arbiter.
 *
 * Ablauf:
 *   BT-Controller (BTDM) -> Bluedroid -> A2DP-Sink + AVRC-Controller.
 *   - Audio-Config-Event  : SBC-Sample-Rate parsen, Resampler auf ->48k setzen.
 *   - Data-Callback       : SBC-dekodiertes PCM (16-bit stereo) resampeln,
 *                           an arbiter_feed(SRC_A2DP, ...) uebergeben.
 *   - Connection-Event    : arbiter_set_a2dp_connected(true/false).
 *                           Der Arbiter pausiert daraufhin den Snapclient
 *                           (Koexistenz: nur EINE Funkstrecke streamt).
 */
#include "a2dp_sink_glue.h"
#include "source_arbiter.h"
#include "resampler.h"
#include "audio_i2s.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

static const char *TAG = "a2dp";

static resampler_t s_resamp;
/* Ausgangspuffer: 44.1k->48k, worst case ~ in_frames*48/44.1. Grosszuegig. */
static int16_t s_out[2048 * 2];

/* --- SBC-Sample-Rate aus Codec-Info (Byte 0, Frequenz-Bits) ableiten ------ */
static uint32_t sbc_sample_rate(const uint8_t *cie)
{
    if (cie == NULL) {
        ESP_LOGW(TAG, "SBC codec info fehlt, verwende 44100 Hz");
        return 44100;
    }

    /*
     * SBC Codec Information Element, Byte 0:
     *
     * Bit 7 = 16 kHz
     * Bit 6 = 32 kHz
     * Bit 5 = 44,1 kHz
     * Bit 4 = 48 kHz
     *
     * Bei der ausgehandelten Konfiguration sollte genau eines
     * dieser Bits gesetzt sein.
     */
    const uint8_t frequency_bits = cie[0] & 0xF0;

    if (frequency_bits & 0x10) {
        return 48000;
    }

    if (frequency_bits & 0x20) {
        return 44100;
    }

    if (frequency_bits & 0x40) {
        return 32000;
    }

    if (frequency_bits & 0x80) {
        return 16000;
    }

    ESP_LOGW(
        TAG,
        "Unbekannte SBC-Samplerate: cie[0]=0x%02X, verwende 44100 Hz",
        cie[0]);

    return 44100;
}

/* --- A2DP Event-Callback (Connection / Audio-Config) ---------------------- */
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_connection_state_t st = param->conn_stat.state;
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected -> Snapclient wird pausiert");
            arbiter_set_a2dp_connected(true);
        } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected -> Snapclient wieder frei");
            arbiter_set_a2dp_connected(false);
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint32_t sr = sbc_sample_rate(param->audio_cfg.mcc.cie.sbc);
            resampler_init(&s_resamp, sr, AUDIO_I2S_SAMPLE_RATE);
            ESP_LOGI(TAG, "A2DP audio cfg: SBC %lu Hz -> resample %d Hz",
                     (unsigned long)sr, AUDIO_I2S_SAMPLE_RATE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state=%d", param->audio_stat.state);
        break;
    default:
        break;
    }
}

/* --- A2DP Data-Callback: PCM (16-bit stereo) ------------------------------ */
static void a2d_data_cb(const uint8_t *data, uint32_t len)
{
    /* Nur weiterreichen, wenn A2DP aktiv arbitriert ist (spart CPU). */
    if (arbiter_current() != SRC_A2DP) return;

    const int16_t *in = (const int16_t *)data;
    size_t in_frames  = len / 4;              /* 16-bit stereo -> 4 Byte/Frame */

    size_t cap = sizeof(s_out) / sizeof(int16_t) / 2;
    /* In Bloecken resampeln, falls Eingang groesser als Ausgangspuffer. */
    size_t done = 0;
    while (done < in_frames) {
        size_t chunk = in_frames - done;
        if (chunk > 1024) chunk = 1024;       /* Sicherheitsmarge zum Puffer */
        size_t out_frames = resampler_process(&s_resamp, in + done * 2, chunk,
                                              s_out, cap);
        arbiter_feed(SRC_A2DP, s_out, out_frames * 4);
        done += chunk;
    }
}

/* --- AVRC (optional): Metadaten/Transport, hier minimal ------------------- */
static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    /* Fuer das Uebungsprojekt nicht ausgewertet. Haken fuer Play/Pause/Titel. */
}

esp_err_t a2dp_sink_start(void)
{
    /* Resampler-Default, bis das erste AUDIO_CFG-Event kommt (meist 44.1k). */
    resampler_init(&s_resamp, 44100, AUDIO_I2S_SAMPLE_RATE);

    /* 1) BT-Controller im BTDM-Mode (Classic + BLE) */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    /* 2) Bluedroid-Host */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_set_device_name("SnapMesh-Speaker");

    /* 3) AVRC-Controller zuerst (Empfehlung der API), dann A2DP-Sink */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_ct_cb));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    /* 4) Auffindbar + verbindbar machen */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "A2DP-Sink aktiv als 'SnapMesh-Speaker' (BTDM)");
    return ESP_OK;
}
