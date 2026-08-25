/**
 * @file  snapclient_glue.c
 * @brief Snapcast-Client mit PCM- und Opus-Unterstuetzung.
 */

#include "snapclient_glue.h"
#include "source_arbiter.h"
#include "audio_i2s.h"
#include "opus.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

static const char *TAG = "snap";

/*
 * Snapcast-Nachrichtentypen.
 */
typedef enum {
    SNAP_MSG_BASE            = 0,
    SNAP_MSG_CODEC_HEADER    = 1,
    SNAP_MSG_WIRE_CHUNK      = 2,
    SNAP_MSG_SERVER_SETTINGS = 3,
    SNAP_MSG_TIME            = 4,
    SNAP_MSG_HELLO           = 5,
    SNAP_MSG_STREAM_TAGS     = 6,
} snap_msg_type_t;

/*
 * Snapcast Base-Header, 26 Byte, Little Endian.
 */
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t id;
    uint16_t refers_to;
    int32_t  sent_sec;
    int32_t  sent_usec;
    int32_t  received_sec;
    int32_t  received_usec;
    uint32_t size;
} snap_base_t;

#define EVT_RESUME             BIT0

#define SNAP_SAMPLE_RATE       48000
#define SNAP_CHANNELS          2

/*
 * Opus kann maximal 120 ms Audio pro Paket enthalten.
 * Bei 48 kHz sind das 5760 Samples pro Kanal.
 */
#define OPUS_MAX_FRAME_SAMPLES 5760
#define OPUS_PCM_SAMPLE_COUNT  (OPUS_MAX_FRAME_SAMPLES * SNAP_CHANNELS)
#define OPUS_PCM_BUFFER_BYTES  (OPUS_PCM_SAMPLE_COUNT * sizeof(opus_int16))

/*
 * Netzwerk- und Taskzustand.
 */
static int                s_sock         = -1;
static volatile bool      s_paused       = false;
static volatile bool      s_run          = true;
static bool               s_task_started = false;
static EventGroupHandle_t s_evt          = NULL;

static char               s_host[64]     = {0};
static uint16_t           s_port         = 1704;

/*
 * Codec ist bis zum CodecHeader unbekannt.
 * Dadurch wird kein Opus-Paket versehentlich als PCM behandelt.
 */
static char s_codec[16] = {0};

/*
 * Opus-Decoder und PCM-Ausgabepuffer.
 */
static OpusDecoder *s_opus_decoder = NULL;
static opus_int16   *s_opus_pcm     = NULL;

/*
 * Streamstatistik.
 */
static uint32_t s_wire_chunks    = 0;
static uint64_t s_wire_bytes     = 0;
static uint32_t s_decoded_frames = 0;
static uint64_t s_decoded_bytes  = 0;
static uint64_t s_arbiter_bytes  = 0;
static uint64_t s_dropped_bytes  = 0;
static uint32_t s_decode_errors  = 0;
static int64_t  s_last_stats_us  = 0;

/*
 * Vollstaendig senden, auch wenn send() nur einen Teil verarbeitet.
 */
static int send_full(int socket_fd, const void *buffer, size_t length)
{
    const uint8_t *data = (const uint8_t *)buffer;
    size_t sent_total = 0;

    while (sent_total < length) {
        int sent = send(
            socket_fd,
            data + sent_total,
            length - sent_total,
            0);

        if (sent <= 0) {
            return -1;
        }

        sent_total += (size_t)sent;
    }

    return 0;
}

/*
 * Vollstaendig empfangen.
 */
static int read_full(int socket_fd, void *buffer, size_t length)
{
    uint8_t *data = (uint8_t *)buffer;
    size_t received_total = 0;

    while (received_total < length) {
        int received = recv(
            socket_fd,
            data + received_total,
            length - received_total,
            0);

        if (received <= 0) {
            return -1;
        }

        received_total += (size_t)received;
    }

    return 0;
}

/*
 * TCP-Verbindung zum Snapserver herstellen.
 */
static int tcp_connect(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *result = NULL;

    char port_text[8];
    snprintf(
        port_text,
        sizeof(port_text),
        "%u",
        (unsigned)s_port);

    int gai_result = getaddrinfo(
        s_host,
        port_text,
        &hints,
        &result);

    if (gai_result != 0 || result == NULL) {
        ESP_LOGW(
            TAG,
            "getaddrinfo fehlgeschlagen fuer %s",
            s_host);

        return -1;
    }

    int socket_fd = socket(
        result->ai_family,
        result->ai_socktype,
        result->ai_protocol);

    if (socket_fd < 0) {
        freeaddrinfo(result);
        return -1;
    }

    if (connect(
            socket_fd,
            result->ai_addr,
            result->ai_addrlen) != 0) {

        ESP_LOGW(
            TAG,
            "connect() zu %s:%u fehlgeschlagen (errno=%d)",
            s_host,
            (unsigned)s_port,
            errno);

        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);

    ESP_LOGI(
        TAG,
        "verbunden mit Snapserver %s:%u",
        s_host,
        (unsigned)s_port);

    return socket_fd;
}

/*
 * Snapcast-Hello mit echter Wi-Fi-STA-MAC senden.
 */
static int send_hello(int socket_fd)
{
    uint8_t mac[6] = {0};

    esp_err_t mac_result = esp_read_mac(
        mac,
        ESP_MAC_WIFI_STA);

    if (mac_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "WLAN-MAC konnte nicht gelesen werden: %s",
            esp_err_to_name(mac_result));

        return -1;
    }

    /*
     * Vollstaendige MAC-Adresse als stabile Snapcast-Client-ID.
     *
     * Beispiel:
     *   A8:42:E3:AE:88:44
     */
    char mac_text[18];

    int mac_length = snprintf(
        mac_text,
        sizeof(mac_text),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);

    if (mac_length <= 0 ||
        mac_length >= (int)sizeof(mac_text)) {

        ESP_LOGE(TAG, "MAC-Adresse konnte nicht formatiert werden");
        return -1;
    }

    /*
     * Eindeutiger und im Snapweb gut lesbarer Geraetename.
     *
     * Beispiel:
     *   A8:42:E3:AE:88:44
     *   -> ESP32-SnapMesh-8844
     *
     * Jeder ESP32 kann dadurch mit derselben Firmware betrieben werden.
     */
    char client_name[32];

    int client_name_length = snprintf(
        client_name,
        sizeof(client_name),
        "ESP32-SnapMesh-%02X%02X",
        mac[4],
        mac[5]);

    if (client_name_length <= 0 ||
        client_name_length >= (int)sizeof(client_name)) {

        ESP_LOGE(TAG, "Clientname konnte nicht formatiert werden");
        return -1;
    }

    /*
     * Snapcast-Hello als laengenpraefixierter JSON-String.
     *
     * MAC:
     *   stabile technische Client-ID
     *
     * HostName und ClientName:
     *   gut lesbare Anzeige im Snapserver/Snapweb
     */
    char hello_json[384];

    int hello_length = snprintf(
        hello_json,
        sizeof(hello_json),
        "{"
        "\"MAC\":\"%s\","
        "\"HostName\":\"%s\","
        "\"Version\":\"0.27.0\","
        "\"ClientName\":\"%s\","
        "\"OS\":\"esp-idf\","
        "\"Arch\":\"xtensa\","
        "\"Instance\":1,"
        "\"SnapStreamProtocolVersion\":2"
        "}",
        mac_text,
        client_name,
        client_name);

    if (hello_length <= 0 ||
        hello_length >= (int)sizeof(hello_json)) {

        ESP_LOGE(
            TAG,
            "Snapcast-Hello ist ungueltig oder zu lang");

        return -1;
    }

    uint32_t string_length =
        (uint32_t)hello_length;

    snap_base_t header = {0};

    header.type = SNAP_MSG_HELLO;
    header.size =
        sizeof(string_length) +
        string_length;

    if (send_full(
            socket_fd,
            &header,
            sizeof(header)) != 0) {

        ESP_LOGE(
            TAG,
            "Snapcast-Hello-Header konnte nicht gesendet werden");

        return -1;
    }

    if (send_full(
            socket_fd,
            &string_length,
            sizeof(string_length)) != 0) {

        ESP_LOGE(
            TAG,
            "Snapcast-Hello-Laenge konnte nicht gesendet werden");

        return -1;
    }

    if (send_full(
            socket_fd,
            hello_json,
            string_length) != 0) {

        ESP_LOGE(
            TAG,
            "Snapcast-Hello-Payload konnte nicht gesendet werden");

        return -1;
    }

    ESP_LOGI(
        TAG,
        "Snapcast-Hello: Client=%s, ID=%s",
        client_name,
        mac_text);

    return 0;
}

/*
 * Opus-Decoder initialisieren.
 */
static esp_err_t opus_decoder_prepare(void)
{
    if (s_opus_decoder != NULL &&
        s_opus_pcm != NULL) {
        return ESP_OK;
    }

    if (s_opus_pcm == NULL) {
        /*
         * Ausgabepuffer bevorzugt im PSRAM anlegen.
         * Fallback auf normalen 8-Bit-faehigen Heap.
         */
        s_opus_pcm = heap_caps_malloc(
            OPUS_PCM_BUFFER_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (s_opus_pcm == NULL) {
            s_opus_pcm = heap_caps_malloc(
                OPUS_PCM_BUFFER_BYTES,
                MALLOC_CAP_8BIT);
        }

        if (s_opus_pcm == NULL) {
            ESP_LOGE(
                TAG,
                "Kein Speicher fuer Opus-PCM-Puffer (%u B)",
                (unsigned)OPUS_PCM_BUFFER_BYTES);

            return ESP_ERR_NO_MEM;
        }

        memset(
            s_opus_pcm,
            0,
            OPUS_PCM_BUFFER_BYTES);
    }

    int opus_error = OPUS_OK;

    s_opus_decoder = opus_decoder_create(
        SNAP_SAMPLE_RATE,
        SNAP_CHANNELS,
        &opus_error);

    if (s_opus_decoder == NULL ||
        opus_error != OPUS_OK) {

        ESP_LOGE(
            TAG,
            "opus_decoder_create fehlgeschlagen: %s (%d)",
            opus_strerror(opus_error),
            opus_error);

        if (s_opus_decoder != NULL) {
            opus_decoder_destroy(s_opus_decoder);
            s_opus_decoder = NULL;
        }

        heap_caps_free(s_opus_pcm);
        s_opus_pcm = NULL;

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Opus-Decoder bereit: %d Hz, %d Kanaele, PCM-Puffer=%u B",
        SNAP_SAMPLE_RATE,
        SNAP_CHANNELS,
        (unsigned)OPUS_PCM_BUFFER_BYTES);

    return ESP_OK;
}

/*
 * Opus-Decoder zuruecksetzen.
 */
static void opus_decoder_reset(void)
{
    if (s_opus_decoder != NULL) {
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
    }

    if (s_opus_pcm != NULL) {
        heap_caps_free(s_opus_pcm);
        s_opus_pcm = NULL;
    }
}

/*
 * Statistik alle 5 Sekunden ausgeben.
 */
static void log_stream_stats(void)
{
    int64_t now = esp_timer_get_time();

    if (s_last_stats_us == 0) {
        s_last_stats_us = now;
        return;
    }

    if ((now - s_last_stats_us) < 5000000) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Stream stats/5s: codec=%s packets=%lu wire=%llu B "
        "decoded_frames=%lu decoded=%llu B arbiter=%llu B "
        "dropped=%llu B decode_errors=%lu src=%d",
        s_codec[0] != '\0' ? s_codec : "unknown",
        (unsigned long)s_wire_chunks,
        (unsigned long long)s_wire_bytes,
        (unsigned long)s_decoded_frames,
        (unsigned long long)s_decoded_bytes,
        (unsigned long long)s_arbiter_bytes,
        (unsigned long long)s_dropped_bytes,
        (unsigned long)s_decode_errors,
        (int)arbiter_current());

    s_wire_chunks    = 0;
    s_wire_bytes     = 0;
    s_decoded_frames = 0;
    s_decoded_bytes  = 0;
    s_arbiter_bytes  = 0;
    s_dropped_bytes  = 0;
    s_decode_errors  = 0;
    s_last_stats_us  = now;
}

/*
 * CodecHeader:
 *   uint32_t codec_name_length
 *   char     codec_name[]
 *   weitere codecspezifische Daten
 */
static void handle_codec_header(
    const uint8_t *payload,
    uint32_t size)
{
    if (payload == NULL ||
        size < sizeof(uint32_t)) {
        return;
    }

    uint32_t codec_length = 0;

    memcpy(
        &codec_length,
        payload,
        sizeof(codec_length));

    if (codec_length == 0 ||
        codec_length >= sizeof(s_codec) ||
        sizeof(uint32_t) + codec_length > size) {

        ESP_LOGE(
            TAG,
            "Ungueltiger CodecHeader: size=%lu codec_len=%lu",
            (unsigned long)size,
            (unsigned long)codec_length);

        return;
    }

    memset(
        s_codec,
        0,
        sizeof(s_codec));

    memcpy(
        s_codec,
        payload + sizeof(uint32_t),
        codec_length);

    s_codec[codec_length] = '\0';

    ESP_LOGI(
        TAG,
        "CodecHeader: codec=%s",
        s_codec);

    /*
     * Bei Codecwechsel Decoderzustand sauber neu anlegen.
     */
    if (strcmp(s_codec, "opus") == 0) {
        opus_decoder_reset();

        esp_err_t result = opus_decoder_prepare();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Opus-Decoder konnte nicht initialisiert werden");
        }
    } else {
        opus_decoder_reset();
    }
}

/*
 * WireChunk:
 *   int32_t  timestamp_sec
 *   int32_t  timestamp_usec
 *   uint32_t audio_payload_length
 *   uint8_t  audio_payload[]
 */
static void handle_wire_chunk(
    const uint8_t *payload,
    uint32_t size)
{
    if (payload == NULL ||
        size < 12) {

        ESP_LOGW(
            TAG,
            "WireChunk zu klein: %lu B",
            (unsigned long)size);

        return;
    }

    uint32_t audio_length = 0;

    memcpy(
        &audio_length,
        payload + 8,
        sizeof(audio_length));

    if (audio_length == 0 ||
        audio_length > size - 12) {

        ESP_LOGE(
            TAG,
            "WireChunk ungueltig: msg=%lu B audio=%lu B",
            (unsigned long)size,
            (unsigned long)audio_length);

        return;
    }

    const uint8_t *audio = payload + 12;

    s_wire_chunks++;
    s_wire_bytes += audio_length;

    /*
     * Vor dem CodecHeader keine Daten weitergeben.
     */
    if (s_codec[0] == '\0') {
        log_stream_stats();
        return;
    }

    if (strcmp(s_codec, "pcm") == 0) {
        size_t accepted = arbiter_feed(
            SRC_SNAPCAST,
            audio,
            audio_length);

        s_decoded_frames++;
        s_decoded_bytes += audio_length;
        s_arbiter_bytes += accepted;

        if (accepted < audio_length) {
            s_dropped_bytes += audio_length - accepted;
        }

        log_stream_stats();
        return;
    }

    if (strcmp(s_codec, "opus") == 0) {
        if (s_opus_decoder == NULL ||
            s_opus_pcm == NULL) {

            if (opus_decoder_prepare() != ESP_OK) {
                s_decode_errors++;
                log_stream_stats();
                return;
            }
        }

        /*
         * opus_decode() liefert die Anzahl Samples pro Kanal.
         * frame_size ist die maximale Kapazitaet pro Kanal.
         */
        int samples_per_channel = opus_decode(
            s_opus_decoder,
            audio,
            (opus_int32)audio_length,
            s_opus_pcm,
            OPUS_MAX_FRAME_SAMPLES,
            0);

        if (samples_per_channel < 0) {
            s_decode_errors++;

            ESP_LOGW(
                TAG,
                "opus_decode fehlgeschlagen: %s (%d)",
                opus_strerror(samples_per_channel),
                samples_per_channel);

            log_stream_stats();
            return;
        }

        size_t pcm_bytes =
            (size_t)samples_per_channel *
            SNAP_CHANNELS *
            sizeof(opus_int16);

        size_t accepted = arbiter_feed(
            SRC_SNAPCAST,
            s_opus_pcm,
            pcm_bytes);

        s_decoded_frames++;
        s_decoded_bytes += pcm_bytes;
        s_arbiter_bytes += accepted;

        if (accepted < pcm_bytes) {
            s_dropped_bytes += pcm_bytes - accepted;
        }

        log_stream_stats();
        return;
    }

    static char last_unsupported_codec[16] = {0};

    if (strncmp(
            last_unsupported_codec,
            s_codec,
            sizeof(last_unsupported_codec)) != 0) {

        ESP_LOGW(
            TAG,
            "Nicht unterstuetzter Snapcast-Codec: %s",
            s_codec);

        strlcpy(
            last_unsupported_codec,
            s_codec,
            sizeof(last_unsupported_codec));
    }

    log_stream_stats();
}

/*
 * Snapcast-Nachricht verarbeiten.
 */
static void process_message(
    const snap_base_t *header,
    const uint8_t *payload)
{
    switch (header->type) {
        case SNAP_MSG_CODEC_HEADER:
            handle_codec_header(
                payload,
                header->size);
            break;

        case SNAP_MSG_WIRE_CHUNK:
            handle_wire_chunk(
                payload,
                header->size);
            break;

        case SNAP_MSG_SERVER_SETTINGS:
        case SNAP_MSG_TIME:
        case SNAP_MSG_STREAM_TAGS:
        case SNAP_MSG_BASE:
        case SNAP_MSG_HELLO:
        default:
            break;
    }
}

/*
 * Empfangsschleife einer bestehenden Snapserver-Verbindung.
 */
static void connection_loop(int socket_fd)
{
    static uint8_t discard_buffer[2048];
    static uint8_t message_buffer[8192];

    arbiter_set_snapcast_active(true);

    while (s_run && !s_paused) {
        snap_base_t header;

        if (read_full(
                socket_fd,
                &header,
                sizeof(header)) != 0) {
            break;
        }

        if (header.size == 0) {
            continue;
        }

        if (header.size > sizeof(message_buffer)) {
            ESP_LOGE(
                TAG,
                "Snapcast-Payload zu gross: %lu B, Puffer=%u B",
                (unsigned long)header.size,
                (unsigned)sizeof(message_buffer));

            uint32_t remaining = header.size;

            while (remaining > 0) {
                uint32_t part = remaining;

                if (part > sizeof(discard_buffer)) {
                    part = sizeof(discard_buffer);
                }

                if (read_full(
                        socket_fd,
                        discard_buffer,
                        part) != 0) {

                    arbiter_set_snapcast_active(false);
                    return;
                }

                remaining -= part;
            }

            continue;
        }

        if (read_full(
                socket_fd,
                message_buffer,
                header.size) != 0) {
            break;
        }

        process_message(
            &header,
            message_buffer);
    }

    arbiter_set_snapcast_active(false);
}

/*
 * Snapclient-Task.
 */
static void snap_task(void *argument)
{
    (void)argument;

    while (s_run) {
        if (s_paused) {
            xEventGroupWaitBits(
                s_evt,
                EVT_RESUME,
                pdTRUE,
                pdTRUE,
                portMAX_DELAY);

            if (!s_run) {
                break;
            }
        }

        s_sock = tcp_connect();

        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        memset(
            s_codec,
            0,
            sizeof(s_codec));

        if (send_hello(s_sock) == 0) {
            connection_loop(s_sock);
        }

        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }

        if (!s_paused) {
            ESP_LOGI(
                TAG,
                "Verbindung verloren, Reconnect in 1 s");

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    opus_decoder_reset();
    s_task_started = false;

    vTaskDelete(NULL);
}

/*
 * Snapcast wegen A2DP pausieren oder wieder freigeben.
 */
void snapclient_pause(bool pause)
{
    if (pause) {
        if (s_paused) {
            return;
        }

        s_paused = true;

        ESP_LOGI(
            TAG,
            "PAUSE: A2DP aktiv, Snapserver-Socket wird geschlossen");

        if (s_sock >= 0) {
            shutdown(s_sock, SHUT_RDWR);
        }

        return;
    }

    /*
     * Kein unnoetiges RESUME beim ersten Arbiter-Switch.
     */
    if (!s_paused) {
        return;
    }

    s_paused = false;

    ESP_LOGI(
        TAG,
        "RESUME: Snapclient wird wieder verbunden");

    if (s_evt != NULL) {
        xEventGroupSetBits(
            s_evt,
            EVT_RESUME);
    }
}

/*
 * Snapclient starten.
 */
esp_err_t snapclient_start(
    const char *host,
    uint16_t port)
{
    if (s_task_started) {
        return ESP_OK;
    }

    if (host == NULL ||
        host[0] == '\0' ||
        port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(
        s_host,
        host,
        sizeof(s_host));

    s_port = port;

    if (s_evt == NULL) {
        s_evt = xEventGroupCreate();

        if (s_evt == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_run          = true;
    s_paused       = false;
    s_task_started = true;

    BaseType_t task_result = xTaskCreatePinnedToCore(
        snap_task,
        "snap",
        12288,
        NULL,
        5,
        NULL,
        0);

    if (task_result != pdPASS) {
        s_task_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Snapclient gestartet (Server %s:%u, Codec wird automatisch erkannt)",
        s_host,
        (unsigned)s_port);

    return ESP_OK;
}