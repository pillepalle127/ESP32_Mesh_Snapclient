/**
 * @file  snapclient_glue.c
 * @brief Snapcast-Client mit PCM- und Opus-Unterstuetzung.
 *
 * Funktionen:
 *
 * - TCP-Verbindung zum Snapserver
 * - Snapcast Protocol Version 2
 * - Eindeutige Identifikation ueber die WLAN-STA-MAC
 * - PCM-Direktpfad
 * - Opus-Dekodierung nach 48 kHz, 16 Bit, Stereo
 * - Uebergabe an den Source-Arbiter
 * - Pause und Resume bei aktivem A2DP-Audiostream
 * - Schutz vor paralleler Opus-Dekodierung waehrend A2DP
 * - Scheduler-Entlastung nach jeder Snapcast-Nachricht
 */

#include "snapclient_glue.h"
#include "source_arbiter.h"
#include "audio_i2s.h"
#include "opus.h"

#include <errno.h>
#include <fcntl.h>
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


/* -------------------------------------------------------------------------
 * Snapcast-Nachrichtentypen
 * ------------------------------------------------------------------------- */

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
 * Snapcast Base-Header.
 *
 * Der Header besteht aus 26 Byte und wird vom Snapserver in
 * Little-Endian-Reihenfolge uebertragen.
 */
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t id;
    uint16_t refers_to;

    int32_t sent_sec;
    int32_t sent_usec;

    int32_t received_sec;
    int32_t received_usec;

    uint32_t size;
} snap_base_t;


/* -------------------------------------------------------------------------
 * Konstanten
 * ------------------------------------------------------------------------- */

#define EVT_RESUME                  BIT0
#define EVT_NETWORK_AVAILABLE       BIT1

#define SNAP_SAMPLE_RATE            48000
#define SNAP_CHANNELS               2

/*
 * Opus kann bei 48 kHz maximal 120 ms Audio pro Paket enthalten.
 *
 * 48000 Samples/s * 0,120 s = 5760 Samples pro Kanal.
 */
#define OPUS_MAX_FRAME_SAMPLES      5760

#define OPUS_PCM_SAMPLE_COUNT       \
    (OPUS_MAX_FRAME_SAMPLES * SNAP_CHANNELS)

#define OPUS_PCM_BUFFER_BYTES       \
    (OPUS_PCM_SAMPLE_COUNT * sizeof(opus_int16))

#define SNAP_MESSAGE_BUFFER_SIZE    8192
#define SNAP_DISCARD_BUFFER_SIZE    2048

#define SNAP_TASK_STACK_SIZE        12288
#define SNAP_TASK_PRIORITY          5
#define SNAP_TASK_CORE              1

#define SNAP_CONNECT_RETRY_MS       2000
#define SNAP_CONNECT_TIMEOUT_MS     3000
#define SNAP_RECONNECT_DELAY_MS     1000
#define SNAP_STATS_INTERVAL_US      5000000LL


/* -------------------------------------------------------------------------
 * Netzwerk- und Taskzustand
 * ------------------------------------------------------------------------- */

static int s_sock = -1;

static volatile bool s_paused = false;
static volatile bool s_run = true;

static bool s_task_started = false;

static EventGroupHandle_t s_evt = NULL;

static char s_host[64] = {0};
static uint16_t s_port = 1704;


/* -------------------------------------------------------------------------
 * Codec-Zustand
 * ------------------------------------------------------------------------- */

/*
 * Der Codec ist bis zum Empfang des CodecHeaders unbekannt.
 *
 * Dadurch wird kein komprimiertes Opus-Paket versehentlich als PCM
 * an den Source-Arbiter uebergeben.
 */
static char s_codec[16] = {0};


/* -------------------------------------------------------------------------
 * Opus-Decoder
 * ------------------------------------------------------------------------- */

static OpusDecoder *s_opus_decoder = NULL;
static opus_int16 *s_opus_pcm = NULL;


/* -------------------------------------------------------------------------
 * Streamstatistik
 * ------------------------------------------------------------------------- */

static uint32_t s_wire_chunks = 0;
static uint64_t s_wire_bytes = 0;

static uint32_t s_decoded_frames = 0;
static uint64_t s_decoded_bytes = 0;

static uint64_t s_arbiter_bytes = 0;
static uint64_t s_dropped_bytes = 0;

static uint32_t s_decode_errors = 0;

static int64_t s_last_stats_us = 0;


/* -------------------------------------------------------------------------
 * Socket-Hilfsfunktionen
 * ------------------------------------------------------------------------- */

/**
 * @brief Einen Puffer vollstaendig ueber einen Socket senden.
 */
static int send_full(
    int socket_fd,
    const void *buffer,
    size_t length)
{
    if (socket_fd < 0 ||
        buffer == NULL) {

        return -1;
    }

    const uint8_t *data =
        (const uint8_t *)buffer;

    size_t sent_total = 0;

    while (sent_total < length) {
        int sent = send(
            socket_fd,
            data + sent_total,
            length - sent_total,
            0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        sent_total += (size_t)sent;
    }

    return 0;
}


/**
 * @brief Eine feste Anzahl von Bytes vollstaendig empfangen.
 */
static int read_full(
    int socket_fd,
    void *buffer,
    size_t length)
{
    if (socket_fd < 0 ||
        buffer == NULL) {

        return -1;
    }

    uint8_t *data =
        (uint8_t *)buffer;

    size_t received_total = 0;

    while (received_total < length) {
        int received = recv(
            socket_fd,
            data + received_total,
            length - received_total,
            0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (received == 0) {
            return -1;
        }

        received_total += (size_t)received;
    }

    return 0;
}


/* -------------------------------------------------------------------------
 * TCP-Verbindung
 * ------------------------------------------------------------------------- */

/**
 * @brief TCP-Verbindung zum konfigurierten Snapserver herstellen.
 */
static int tcp_connect(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *result = NULL;
    char port_text[8] = {0};

    int port_length = snprintf(
        port_text,
        sizeof(port_text),
        "%u",
        (unsigned)s_port);

    if (port_length <= 0 ||
        port_length >= (int)sizeof(port_text)) {

        ESP_LOGE(TAG, "Snapserver-Port konnte nicht formatiert werden");
        return -1;
    }

    int gai_result = getaddrinfo(s_host, port_text, &hints, &result);
    if (gai_result != 0 || result == NULL) {
        ESP_LOGW(TAG, "getaddrinfo fehlgeschlagen fuer %s", s_host);
        return -1;
    }

    int socket_fd = socket(
        result->ai_family,
        result->ai_socktype,
        result->ai_protocol);

    if (socket_fd < 0) {
        ESP_LOGW(TAG, "socket() fehlgeschlagen, errno=%d", errno);
        freeaddrinfo(result);
        return -1;
    }

    int original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 ||
        fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {

        ESP_LOGW(TAG, "Socket konnte nicht auf nonblocking gesetzt werden, errno=%d",
                 errno);
        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    int connect_result = connect(
        socket_fd,
        result->ai_addr,
        result->ai_addrlen);

    if (connect_result != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect() zu %s:%u fehlgeschlagen (errno=%d)",
                 s_host, (unsigned)s_port, errno);
        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    if (connect_result != 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(socket_fd, &write_fds);

        struct timeval timeout = {
            .tv_sec = SNAP_CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (SNAP_CONNECT_TIMEOUT_MS % 1000) * 1000,
        };

        int select_result;
        do {
            select_result = select(
                socket_fd + 1,
                NULL,
                &write_fds,
                NULL,
                &timeout);
        } while (select_result < 0 && errno == EINTR);

        if (select_result <= 0) {
            if (select_result == 0) {
                ESP_LOGW(TAG, "connect() zu %s:%u Timeout nach %d ms",
                         s_host, (unsigned)s_port, SNAP_CONNECT_TIMEOUT_MS);
            } else {
                ESP_LOGW(TAG, "select() beim Verbindungsaufbau fehlgeschlagen, errno=%d",
                         errno);
            }

            close(socket_fd);
            freeaddrinfo(result);
            return -1;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &error_length) != 0 ||
            socket_error != 0) {

            ESP_LOGW(TAG, "connect() zu %s:%u fehlgeschlagen (errno=%d)",
                     s_host, (unsigned)s_port,
                     socket_error != 0 ? socket_error : errno);
            close(socket_fd);
            freeaddrinfo(result);
            return -1;
        }
    }

    if (fcntl(socket_fd, F_SETFL, original_flags) < 0) {
        ESP_LOGW(TAG, "Socket konnte nicht auf blocking zurueckgesetzt werden, errno=%d",
                 errno);
        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);

    ESP_LOGI(TAG, "verbunden mit Snapserver %s:%u",
             s_host, (unsigned)s_port);
    return socket_fd;
}


/* -------------------------------------------------------------------------
 * Snapcast-Hello
 * ------------------------------------------------------------------------- */

/**
 * @brief Snapcast-Hello mit der echten WLAN-STA-MAC senden.
 *
 * Beispiel:
 *
 *   STA-MAC:
 *     A8:42:E3:AE:99:04
 *
 *   Client:
 *     ESP32-SnapMesh-9904
 *
 *   Client-ID:
 *     A8:42:E3:AE:99:04
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
            "WLAN-STA-MAC konnte nicht gelesen werden: %s",
            esp_err_to_name(mac_result));

        return -1;
    }

    char mac_text[18] = {0};

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

        ESP_LOGE(
            TAG,
            "MAC-Adresse konnte nicht formatiert werden");

        return -1;
    }

    char client_name[32] = {0};

    int client_name_length = snprintf(
        client_name,
        sizeof(client_name),
        "ESP32-SnapMesh-%02X%02X",
        mac[4],
        mac[5]);

    if (client_name_length <= 0 ||
        client_name_length >= (int)sizeof(client_name)) {

        ESP_LOGE(
            TAG,
            "Snapcast-Clientname konnte nicht erzeugt werden");

        return -1;
    }

    char hello_json[384] = {0};

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
            "Snapcast-Hello JSON ist ungueltig oder zu lang");

        return -1;
    }

    uint32_t payload_length =
        (uint32_t)hello_length;

    snap_base_t header = {0};

    header.type = SNAP_MSG_HELLO;
    header.size =
        sizeof(payload_length) +
        payload_length;

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
            &payload_length,
            sizeof(payload_length)) != 0) {

        ESP_LOGE(
            TAG,
            "Snapcast-Hello-Laenge konnte nicht gesendet werden");

        return -1;
    }

    if (send_full(
            socket_fd,
            hello_json,
            payload_length) != 0) {

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


/* -------------------------------------------------------------------------
 * Opus-Decoder
 * ------------------------------------------------------------------------- */

/**
 * @brief Opus-Decoder und PCM-Puffer initialisieren.
 */
static esp_err_t opus_decoder_prepare(void)
{
    if (s_opus_decoder != NULL &&
        s_opus_pcm != NULL) {

        return ESP_OK;
    }

    if (s_opus_pcm == NULL) {
        /*
         * Den PCM-Puffer bevorzugt im externen PSRAM anlegen.
         * Falls das nicht moeglich ist, normalen 8-Bit-Heap verwenden.
         */
        s_opus_pcm = heap_caps_malloc(
            OPUS_PCM_BUFFER_BYTES,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT);

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
            opus_decoder_destroy(
                s_opus_decoder);

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


/**
 * @brief Opus-Decoder und PCM-Puffer freigeben.
 */
static void opus_decoder_reset(void)
{
    if (s_opus_decoder != NULL) {
        opus_decoder_destroy(
            s_opus_decoder);

        s_opus_decoder = NULL;
    }

    if (s_opus_pcm != NULL) {
        heap_caps_free(
            s_opus_pcm);

        s_opus_pcm = NULL;
    }
}


/* -------------------------------------------------------------------------
 * Statistik
 * ------------------------------------------------------------------------- */

static void reset_stream_stats(void)
{
    s_wire_chunks = 0;
    s_wire_bytes = 0;

    s_decoded_frames = 0;
    s_decoded_bytes = 0;

    s_arbiter_bytes = 0;
    s_dropped_bytes = 0;

    s_decode_errors = 0;

    s_last_stats_us =
        esp_timer_get_time();
}


/**
 * @brief Streamstatistik alle fuenf Sekunden ausgeben.
 */
static void log_stream_stats(void)
{
    int64_t now =
        esp_timer_get_time();

    if (s_last_stats_us == 0) {
        s_last_stats_us = now;
        return;
    }

    if ((now - s_last_stats_us) <
        SNAP_STATS_INTERVAL_US) {

        return;
    }

    ESP_LOGI(
        TAG,
        "Stream stats/5s: codec=%s packets=%lu wire=%llu B "
        "decoded_frames=%lu decoded=%llu B arbiter=%llu B "
        "dropped=%llu B decode_errors=%lu src=%d",
        s_codec[0] != '\0'
            ? s_codec
            : "unknown",
        (unsigned long)s_wire_chunks,
        (unsigned long long)s_wire_bytes,
        (unsigned long)s_decoded_frames,
        (unsigned long long)s_decoded_bytes,
        (unsigned long long)s_arbiter_bytes,
        (unsigned long long)s_dropped_bytes,
        (unsigned long)s_decode_errors,
        (int)arbiter_current());

    reset_stream_stats();
}


/* -------------------------------------------------------------------------
 * CodecHeader
 * ------------------------------------------------------------------------- */

/**
 * Snapcast CodecHeader:
 *
 *   uint32_t codec_name_length
 *   char     codec_name[]
 *   uint8_t  codec_specific_data[]
 */
static void handle_codec_header(
    const uint8_t *payload,
    uint32_t size)
{
    if (payload == NULL ||
        size < sizeof(uint32_t)) {

        ESP_LOGW(
            TAG,
            "CodecHeader ungueltig: payload=%p size=%lu",
            (const void *)payload,
            (unsigned long)size);

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

    reset_stream_stats();

    if (strcmp(
            s_codec,
            "opus") == 0) {

        opus_decoder_reset();

        esp_err_t result =
            opus_decoder_prepare();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Opus-Decoder konnte nicht initialisiert werden: %s",
                esp_err_to_name(result));

            s_decode_errors++;
        }

        return;
    }

    if (strcmp(
            s_codec,
            "pcm") == 0) {

        opus_decoder_reset();

        ESP_LOGI(
            TAG,
            "PCM-Direktpfad aktiv");

        return;
    }

    opus_decoder_reset();

    ESP_LOGW(
        TAG,
        "Codec wird nicht unterstuetzt: %s",
        s_codec);
}


/* -------------------------------------------------------------------------
 * WireChunk
 * ------------------------------------------------------------------------- */

/**
 * Snapcast WireChunk:
 *
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

    const uint8_t *audio =
        payload + 12;

    s_wire_chunks++;
    s_wire_bytes += audio_length;

    /*
     * Vor dem CodecHeader keine Daten verarbeiten.
     */
    if (s_codec[0] == '\0') {
        log_stream_stats();
        return;
    }

    /*
     * PCM-Direktpfad.
     */
    if (strcmp(
            s_codec,
            "pcm") == 0) {

        /*
         * Falls A2DP aktiviert wurde, keine Snapcast-Daten
         * mehr an den Arbiter uebergeben.
         */
        if (s_paused ||
            arbiter_current() != SRC_SNAPCAST) {

            log_stream_stats();
            return;
        }

        size_t accepted = arbiter_feed(
            SRC_SNAPCAST,
            audio,
            audio_length);

        s_decoded_frames++;
        s_decoded_bytes += audio_length;
        s_arbiter_bytes += accepted;

        if (accepted < audio_length) {
            s_dropped_bytes +=
                audio_length - accepted;
        }

        log_stream_stats();
        return;
    }

    /*
     * Opus-Dekodierung.
     */
    if (strcmp(
            s_codec,
            "opus") == 0) {

        /*
         * Bei aktivem A2DP keine Opus-Pakete dekodieren.
         *
         * Der Decoder ist CPU-intensiv. Die erzeugten PCM-Daten
         * wuerden vom Arbiter ohnehin verworfen werden.
         */
        if (s_paused ||
            arbiter_current() != SRC_SNAPCAST) {

            log_stream_stats();
            return;
        }

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
            s_dropped_bytes +=
                pcm_bytes - accepted;
        }

        log_stream_stats();
        return;
    }

    /*
     * Nicht unterstuetzten Codec nur einmal melden.
     */
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


/* -------------------------------------------------------------------------
 * Snapcast-Nachrichtenverarbeitung
 * ------------------------------------------------------------------------- */

static void process_message(
    const snap_base_t *header,
    const uint8_t *payload)
{
    if (header == NULL) {
        return;
    }

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


/* -------------------------------------------------------------------------
 * Verbindungsschleife
 * ------------------------------------------------------------------------- */

static void connection_loop(int socket_fd)
{
    static uint8_t discard_buffer[
        SNAP_DISCARD_BUFFER_SIZE];

    static uint8_t message_buffer[
        SNAP_MESSAGE_BUFFER_SIZE];

    /*
     * Falls A2DP waehrend des Verbindungsaufbaus aktiv wurde,
     * darf Snapcast nicht erneut aktiviert werden.
     */
    if (s_paused) {
        return;
    }

    arbiter_set_snapcast_active(
        true);

    while (s_run &&
           !s_paused) {

        snap_base_t header;

        if (read_full(
                socket_fd,
                &header,
                sizeof(header)) != 0) {

            break;
        }

        /*
         * A2DP kann waehrend des blockierenden Socket-Lesevorgangs
         * aktiviert worden sein.
         */
        if (s_paused) {
            break;
        }

        if (header.size == 0) {
            /*
             * Auch bei einer Folge leerer Nachrichten dem Scheduler
             * und dem Idle-Task Rechenzeit geben.
             */
            vTaskDelay(1);
            continue;
        }

        if (header.size >
            sizeof(message_buffer)) {

            ESP_LOGE(
                TAG,
                "Snapcast-Payload zu gross: %lu B, Puffer=%u B",
                (unsigned long)header.size,
                (unsigned)sizeof(message_buffer));

            uint32_t remaining =
                header.size;

            while (remaining > 0 &&
                   s_run &&
                   !s_paused) {

                uint32_t part =
                    remaining;

                if (part >
                    sizeof(discard_buffer)) {

                    part =
                        sizeof(discard_buffer);
                }

                if (read_full(
                        socket_fd,
                        discard_buffer,
                        part) != 0) {

                    arbiter_set_snapcast_active(
                        false);

                    return;
                }

                remaining -= part;

                vTaskDelay(1);
            }

            continue;
        }

        if (read_full(
                socket_fd,
                message_buffer,
                header.size) != 0) {

            break;
        }

        if (s_paused) {
            break;
        }

        process_message(
            &header,
            message_buffer);

        /*
         * Scheduler-Pause nach jeder Snapcast-Nachricht.
         *
         * Bei 20-ms-Opus-Paketen kommen etwa 50 Nachrichten pro
         * Sekunde. Ein Tick Pause pro Nachricht verhindert, dass
         * bei einem TCP-Rueckstau der Opus-Decoder den Idle-Task
         * ueber laengere Zeit verdraengt.
         */
        vTaskDelay(1);
    }

    arbiter_set_snapcast_active(
        false);
}


/* -------------------------------------------------------------------------
 * Snapclient-Task
 * ------------------------------------------------------------------------- */

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

            /*
             * Ein veraltetes Resume-Event darf keinen Verbindungsaufbau
             * starten, solange der Pausezustand weiterhin aktiv ist.
             */
            if (s_paused) {
                continue;
            }
        }

        int socket_fd =
            tcp_connect();

        if (socket_fd < 0) {
            if (!s_paused) {
                xEventGroupWaitBits(
                    s_evt,
                    EVT_NETWORK_AVAILABLE | EVT_RESUME,
                    pdTRUE,
                    pdFALSE,
                    pdMS_TO_TICKS(SNAP_CONNECT_RETRY_MS));
            }

            continue;
        }

        /*
         * A2DP kann waehrend des blockierenden connect()-Aufrufs
         * aktiv geworden sein.
         *
         * In diesem Fall den neuen Socket sofort schliessen und
         * kein Snapcast-Hello senden.
         */
        if (s_paused) {
            shutdown(
                socket_fd,
                SHUT_RDWR);

            close(socket_fd);
            continue;
        }

        s_sock = socket_fd;

        memset(
            s_codec,
            0,
            sizeof(s_codec));

        opus_decoder_reset();
        reset_stream_stats();

        if (send_hello(socket_fd) == 0) {
            connection_loop(
                socket_fd);
        }

        /*
         * Nur der Snapclient-Task schliesst den Socket endgueltig.
         * snapclient_pause() verwendet lediglich shutdown().
         */
        shutdown(
            socket_fd,
            SHUT_RDWR);

        close(
            socket_fd);

        if (s_sock == socket_fd) {
            s_sock = -1;
        }

        opus_decoder_reset();

        if (!s_paused &&
            s_run) {

            ESP_LOGI(
                TAG,
                "Verbindung verloren, Reconnect in %d ms",
                SNAP_RECONNECT_DELAY_MS);

            xEventGroupWaitBits(
                s_evt,
                EVT_NETWORK_AVAILABLE | EVT_RESUME,
                pdTRUE,
                pdFALSE,
                pdMS_TO_TICKS(SNAP_RECONNECT_DELAY_MS));
        }
    }

    arbiter_set_snapcast_active(
        false);

    opus_decoder_reset();

    s_sock = -1;
    s_task_started = false;

    vTaskDelete(NULL);
}


/* -------------------------------------------------------------------------
 * Oeffentliche API
 * ------------------------------------------------------------------------- */

/**
 * @brief Snapcast wegen eines aktiven A2DP-Audiostreams pausieren.
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

        /*
         * Nur shutdown() verwenden.
         *
         * close() erfolgt ausschliesslich im Snapclient-Task.
         * Dadurch wird ein Double-Close beziehungsweise ein Rennen
         * durch wiederverwendete File-Deskriptoren verhindert.
         */
        int socket_fd =
            s_sock;

        if (socket_fd >= 0) {
            shutdown(
                socket_fd,
                SHUT_RDWR);
        }

        return;
    }

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


/**
 * @brief Dem laufenden Snapclient mitteilen, dass das Netzwerk wieder bereit ist.
 *
 * Dadurch wird eine laufende Reconnect-Wartezeit sofort beendet. Ein bereits
 * gestarteter Snapclient-Task wird nicht dupliziert.
 */
void snapclient_network_available(void)
{
    if (s_evt != NULL) {
        xEventGroupSetBits(s_evt, EVT_NETWORK_AVAILABLE);
    }
}


/**
 * @brief Snapclient-Task starten.
 */
esp_err_t snapclient_start(
    const char *host,
    uint16_t port)
{
    if (s_task_started) {
        ESP_LOGW(
            TAG,
            "Snapclient wurde bereits gestartet");

        return ESP_OK;
    }

    if (host == NULL ||
        host[0] == '\0' ||
        port == 0) {

        ESP_LOGE(
            TAG,
            "Ungueltige Snapserver-Konfiguration");

        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(
        s_host,
        host,
        sizeof(s_host));

    s_port = port;

    if (s_evt == NULL) {
        s_evt =
            xEventGroupCreate();

        if (s_evt == NULL) {
            ESP_LOGE(
                TAG,
                "Event Group konnte nicht angelegt werden");

            return ESP_ERR_NO_MEM;
        }
    }

    s_run = true;
    s_paused = false;
    s_task_started = true;
    s_sock = -1;

    memset(
        s_codec,
        0,
        sizeof(s_codec));

    reset_stream_stats();

    /*
     * CPU-Aufteilung:
     *
     * CPU 0:
     *   Wi-Fi
     *   Mesh-Lite
     *   ESP-NOW
     *   Bluetooth
     *
     * CPU 1:
     *   Player-Task, Prioritaet 6
     *   Snapclient mit Opus-Decoder, Prioritaet 5
     *
     * Der Player-Task besitzt damit Vorrang vor der Opus-Dekodierung.
     */
    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            snap_task,
            "snap",
            SNAP_TASK_STACK_SIZE,
            NULL,
            SNAP_TASK_PRIORITY,
            NULL,
            SNAP_TASK_CORE);

    if (task_result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Snapclient-Task konnte nicht angelegt werden");

        s_task_started = false;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Snapclient gestartet "
        "(Server %s:%u, Codec automatisch, CPU %d)",
        s_host,
        (unsigned)s_port,
        SNAP_TASK_CORE);

    return ESP_OK;
}