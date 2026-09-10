/**
 * @file  a2dp_sink_glue.c
 * @brief Bluetooth-A2DP-Sink mit Resampling und Source-Arbiter.
 *
 * Datenpfad:
 *
 *   Bluetooth A2DP
 *       -> Bluedroid SBC Decoder
 *       -> PCM 16 Bit Stereo
 *       -> Resampler auf 48 kHz
 *       -> Source Arbiter
 *       -> I2S
 *
 * Umschaltlogik:
 *
 *   A2DP-Verbindung hergestellt
 *       -> noch keine Umschaltung
 *
 *   A2DP-Audiostream gestartet
 *       -> A2DP wird aktive Quelle
 *       -> Snapclient wird pausiert
 *
 *   A2DP-Audiostream gestoppt oder pausiert
 *       -> A2DP wird freigegeben
 *       -> Snapclient wird fortgesetzt
 *
 *   A2DP-Verbindung getrennt
 *       -> A2DP wird sicher freigegeben
 */

#include "a2dp_sink_glue.h"
#include "source_arbiter.h"
#include "resampler.h"
#include "audio_i2s.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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


/* -------------------------------------------------------------------------
 * Audiokonfiguration
 * ------------------------------------------------------------------------- */

#define A2DP_DEFAULT_SAMPLE_RATE    44100
#define A2DP_CHANNEL_COUNT          2
#define A2DP_BYTES_PER_SAMPLE       sizeof(int16_t)
#define A2DP_BYTES_PER_FRAME        \
    (A2DP_CHANNEL_COUNT * A2DP_BYTES_PER_SAMPLE)

#define A2DP_INPUT_CHUNK_FRAMES     1024
#define A2DP_OUTPUT_BUFFER_FRAMES   2048

#define A2DP_DEVICE_NAME_SIZE       40


/* -------------------------------------------------------------------------
 * Laufzeitzustand
 * ------------------------------------------------------------------------- */

static resampler_t s_resamp;

/*
 * Ausgangspuffer für das auf 48 kHz umgerechnete Stereo-PCM.
 *
 * Der Puffer enthält:
 *
 *   A2DP_OUTPUT_BUFFER_FRAMES
 *   x 2 Kanäle
 *   x 16 Bit
 */
static int16_t s_out[
    A2DP_OUTPUT_BUFFER_FRAMES *
    A2DP_CHANNEL_COUNT];

/*
 * Der Bluetooth-Gerätename muss dauerhaft gespeichert bleiben.
 * Deshalb kein lokaler Stack-Puffer in a2dp_sink_start().
 */
static char s_bt_device_name[A2DP_DEVICE_NAME_SIZE];


/* -------------------------------------------------------------------------
 * SBC-Samplerate
 * ------------------------------------------------------------------------- */

/**
 * @brief Aus dem SBC Codec Information Element die Samplerate bestimmen.
 *
 * SBC CIE Byte 0:
 *
 *   Bit 7 = 16 kHz
 *   Bit 6 = 32 kHz
 *   Bit 5 = 44,1 kHz
 *   Bit 4 = 48 kHz
 *
 * Bei einer ausgehandelten A2DP-Konfiguration sollte genau eines dieser
 * Bits gesetzt sein.
 */
static uint32_t sbc_sample_rate(const uint8_t *cie)
{
    if (cie == NULL) {
        ESP_LOGW(
            TAG,
            "SBC codec info fehlt, verwende %d Hz",
            A2DP_DEFAULT_SAMPLE_RATE);

        return A2DP_DEFAULT_SAMPLE_RATE;
    }

    const uint8_t frequency_bits =
        cie[0] & 0xF0;

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
        "Unbekannte SBC-Samplerate: cie[0]=0x%02X, "
        "verwende %d Hz",
        cie[0],
        A2DP_DEFAULT_SAMPLE_RATE);

    return A2DP_DEFAULT_SAMPLE_RATE;
}


/* -------------------------------------------------------------------------
 * Individueller Bluetooth-Gerätename
 * ------------------------------------------------------------------------- */

/**
 * @brief Eindeutigen Bluetooth-Namen aus der STA-MAC erzeugen.
 *
 * Beispiel:
 *
 *   STA-MAC:
 *     A8:42:E3:AE:99:04
 *
 *   Bluetooth-Name:
 *     Snap-Blth-9904
 *
 * Die STA-MAC wird bewusst auch für den lesbaren Bluetooth-Namen verwendet,
 * damit Snapcast- und Bluetooth-Gerät eindeutig zugeordnet werden können.
 */
static esp_err_t create_bt_device_name(void)
{
    uint8_t mac[6] = {0};

    esp_err_t result = esp_read_mac(
        mac,
        ESP_MAC_WIFI_STA);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "STA-MAC konnte nicht gelesen werden: %s",
            esp_err_to_name(result));

        return result;
    }

    int name_length = snprintf(
        s_bt_device_name,
        sizeof(s_bt_device_name),
        "Snap-Blth-%02X%02X",
        mac[4],
        mac[5]);

    if (name_length <= 0 ||
        name_length >= (int)sizeof(s_bt_device_name)) {

        ESP_LOGE(
            TAG,
            "Bluetooth-Gerätename konnte nicht erzeugt werden");

        s_bt_device_name[0] = '\0';

        return ESP_FAIL;
    }

    return ESP_OK;
}


/* -------------------------------------------------------------------------
 * A2DP Event-Callback
 * ------------------------------------------------------------------------- */

/**
 * @brief A2DP-Verbindungs-, Codec- und Audioereignisse verarbeiten.
 *
 * Wichtig:
 *
 * Die reine Bluetooth-Verbindung aktiviert A2DP noch nicht als Audioquelle.
 * Erst ESP_A2D_AUDIO_STATE_STARTED löst die Umschaltung aus.
 *
 * Dadurch kann ein Smartphone verbunden bleiben, während Snapcast spielt.
 */
static void a2d_cb(
    esp_a2d_cb_event_t event,
    esp_a2d_cb_param_t *param)
{
    if (param == NULL) {
        ESP_LOGW(
            TAG,
            "A2DP-Event ohne Parameter: event=%d",
            (int)event);

        return;
    }

    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state =
                param->conn_stat.state;

            switch (state) {
                case ESP_A2D_CONNECTION_STATE_CONNECTED:
                    /*
                     * Nur verbunden bedeutet noch nicht, dass Audio läuft.
                     * Snapcast bleibt deshalb zunächst aktive Quelle.
                     */
                    ESP_LOGI(
                        TAG,
                        "A2DP verbunden, noch kein aktiver Audiostream");
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
                    /*
                     * Bei einer Trennung A2DP sicher freigeben.
                     * Der Arbiter setzt anschließend Snapcast fort.
                     */
                    ESP_LOGI(
                        TAG,
                        "A2DP getrennt -> Snapclient wieder freigeben");

                    arbiter_set_a2dp_connected(false);
                    break;

                case ESP_A2D_CONNECTION_STATE_CONNECTING:
                    ESP_LOGI(
                        TAG,
                        "A2DP-Verbindung wird aufgebaut");
                    break;

                case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
                    ESP_LOGI(
                        TAG,
                        "A2DP-Verbindung wird getrennt");
                    break;

                default:
                    ESP_LOGW(
                        TAG,
                        "Unbekannter A2DP-Verbindungsstatus: %d",
                        (int)state);
                    break;
            }

            break;
        }

        case ESP_A2D_AUDIO_CFG_EVT: {
            if (param->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) {
                ESP_LOGW(
                    TAG,
                    "Nicht unterstützter A2DP-Codec: %d",
                    (int)param->audio_cfg.mcc.type);

                break;
            }

            const uint8_t *sbc_cie =
                param->audio_cfg.mcc.cie.sbc;

            uint32_t sample_rate =
                sbc_sample_rate(sbc_cie);

            resampler_init(
                &s_resamp,
                sample_rate,
                AUDIO_I2S_SAMPLE_RATE);

            ESP_LOGI(
                TAG,
                "A2DP audio cfg: SBC %lu Hz -> resample %d Hz",
                (unsigned long)sample_rate,
                AUDIO_I2S_SAMPLE_RATE);

            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT: {
            esp_a2d_audio_state_t state =
                param->audio_stat.state;

            ESP_LOGI(
                TAG,
                "A2DP audio state=%d",
                (int)state);

            if (state == ESP_A2D_AUDIO_STATE_STARTED) {
                /*
                 * Erst jetzt läuft tatsächlich ein Audiostream.
                 * A2DP erhält Vorrang, und der Snapclient wird pausiert.
                 */
                ESP_LOGI(
                    TAG,
                    "A2DP-Audiostream gestartet "
                    "-> Umschaltung auf Bluetooth");

                arbiter_set_a2dp_connected(true);
            } else {
                /*
                 * STOPPED und REMOTE_SUSPEND gemeinsam behandeln.
                 *
                 * In der verwendeten ESP-IDF-Version können beide
                 * Konstanten denselben Wert besitzen. Deshalb wird hier
                 * bewusst kein separater REMOTE_SUSPEND-Case verwendet.
                 */
                ESP_LOGI(
                    TAG,
                    "A2DP-Audiostream gestoppt oder pausiert "
                    "-> Umschaltung auf Snapcast");

                arbiter_set_a2dp_connected(false);
            }

            break;
        }

        default:
            ESP_LOGD(
                TAG,
                "Unbehandeltes A2DP-Event: %d",
                (int)event);
            break;
    }
}


/* -------------------------------------------------------------------------
 * A2DP PCM-Daten-Callback
 * ------------------------------------------------------------------------- */

/**
 * @brief Von Bluedroid dekodiertes SBC-PCM auf 48 kHz resampeln.
 *
 * Eingang:
 *
 *   16 Bit
 *   Stereo
 *   Samplerate gemäß AUDIO_CFG_EVT
 *
 * Ausgang:
 *
 *   16 Bit
 *   Stereo
 *   48 kHz
 */
static void a2d_data_cb(
    const uint8_t *data,
    uint32_t len)
{
    if (data == NULL ||
        len < A2DP_BYTES_PER_FRAME) {

        return;
    }

    /*
     * Nur arbeiten, wenn A2DP tatsächlich die aktive Quelle ist.
     * Dadurch werden unnötige CPU-Last und Ringpufferbelegung vermieden.
     */
    if (arbiter_current() != SRC_A2DP) {
        return;
    }

    const int16_t *input =
        (const int16_t *)data;

    size_t input_frames =
        len / A2DP_BYTES_PER_FRAME;

    size_t output_capacity_frames =
        sizeof(s_out) /
        sizeof(s_out[0]) /
        A2DP_CHANNEL_COUNT;

    size_t processed_frames = 0;

    while (processed_frames < input_frames) {
        size_t chunk_frames =
            input_frames - processed_frames;

        if (chunk_frames > A2DP_INPUT_CHUNK_FRAMES) {
            chunk_frames = A2DP_INPUT_CHUNK_FRAMES;
        }

        size_t output_frames = resampler_process(
            &s_resamp,
            input + processed_frames * A2DP_CHANNEL_COUNT,
            chunk_frames,
            s_out,
            output_capacity_frames);

        if (output_frames > 0) {
            size_t output_bytes =
                output_frames *
                A2DP_BYTES_PER_FRAME;

            arbiter_feed(
                SRC_A2DP,
                s_out,
                output_bytes);
        }

        processed_frames += chunk_frames;
    }
}


/* -------------------------------------------------------------------------
 * AVRCP Controller
 * ------------------------------------------------------------------------- */

static void avrc_ct_cb(
    esp_avrc_ct_cb_event_t event,
    esp_avrc_ct_cb_param_t *param)
{
    /*
     * Aktuell werden keine Metadaten oder Transportbefehle ausgewertet.
     * Callback bleibt als Erweiterungspunkt registriert.
     */
    (void)event;
    (void)param;
}


/* -------------------------------------------------------------------------
 * Öffentliche API
 * ------------------------------------------------------------------------- */

esp_err_t a2dp_sink_start(void)
{
    /*
     * Sicheren Standardwert verwenden, bis das erste AUDIO_CFG_EVT
     * die tatsächlich ausgehandelte SBC-Samplerate liefert.
     */
    resampler_init(
        &s_resamp,
        A2DP_DEFAULT_SAMPLE_RATE,
        AUDIO_I2S_SAMPLE_RATE);

    /*
     * Individuellen Gerätenamen erzeugen.
     */
    esp_err_t name_result =
        create_bt_device_name();

    if (name_result != ESP_OK) {
        return name_result;
    }

    /*
     * Bluetooth-Controller im Classic-Bluetooth-Modus (BR/EDR) initialisieren.
     *
     * A2DP und AVRCP verwenden Bluetooth Classic. BLE wird fuer diesen
     * Anwendungsfall nicht benoetigt.
     */
    esp_bt_controller_config_t bt_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    esp_err_t result =
        esp_bt_controller_init(&bt_config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluetooth-Controller-Initialisierung fehlgeschlagen: %s",
            esp_err_to_name(result));

        return result;
    }

    result = esp_bt_controller_enable(
        ESP_BT_MODE_CLASSIC_BT);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluetooth-Controller konnte nicht aktiviert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    /*
     * Bluedroid-Host initialisieren.
     */
    result = esp_bluedroid_init();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluedroid-Initialisierung fehlgeschlagen: %s",
            esp_err_to_name(result));

        return result;
    }

    result = esp_bluedroid_enable();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluedroid konnte nicht aktiviert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    /*
     * Eindeutigen Bluetooth-Namen setzen.
     */
    result = esp_bt_gap_set_device_name(
        s_bt_device_name);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluetooth-Gerätename konnte nicht gesetzt werden: %s",
            esp_err_to_name(result));

        return result;
    }

    /*
     * AVRCP Controller zuerst initialisieren.
     */
    result = esp_avrc_ct_init();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "AVRCP-Initialisierung fehlgeschlagen: %s",
            esp_err_to_name(result));

        return result;
    }

    result = esp_avrc_ct_register_callback(
        avrc_ct_cb);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "AVRCP-Callback konnte nicht registriert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    /*
     * A2DP Callbacks registrieren und Sink starten.
     */
    result = esp_a2d_register_callback(
        a2d_cb);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "A2DP-Callback konnte nicht registriert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    result = esp_a2d_sink_register_data_callback(
        a2d_data_cb);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "A2DP-Daten-Callback konnte nicht registriert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    result = esp_a2d_sink_init();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "A2DP-Sink konnte nicht initialisiert werden: %s",
            esp_err_to_name(result));

        return result;
    }

    /*
     * Gerät auffindbar und verbindbar machen.
     */
    result = esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE,
        ESP_BT_GENERAL_DISCOVERABLE);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Bluetooth Scan Mode konnte nicht gesetzt werden: %s",
            esp_err_to_name(result));

        return result;
    }

    ESP_LOGI(
        TAG,
        "A2DP-Sink aktiv als '%s' (BR/EDR)",
        s_bt_device_name);

    return ESP_OK;
}