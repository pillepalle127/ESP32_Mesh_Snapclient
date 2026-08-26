/**
 * @file snapclient_glue.c
 * @brief Snapcast client with PCM and Opus support.
 */
#include "snapclient_glue.h"
#include "source_arbiter.h"
#include "opus.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

typedef enum {
    SNAP_MSG_BASE = 0,
    SNAP_MSG_CODEC_HEADER = 1,
    SNAP_MSG_WIRE_CHUNK = 2,
    SNAP_MSG_SERVER_SETTINGS = 3,
    SNAP_MSG_TIME = 4,
    SNAP_MSG_HELLO = 5,
    SNAP_MSG_STREAM_TAGS = 6,
} snap_msg_type_t;

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

#define EVT_RESUME                 BIT0
#define SNAP_SAMPLE_RATE           48000
#define SNAP_CHANNELS              2
#define OPUS_MAX_FRAME_SAMPLES     5760
#define OPUS_PCM_SAMPLE_COUNT      (OPUS_MAX_FRAME_SAMPLES * SNAP_CHANNELS)
#define OPUS_PCM_BUFFER_BYTES      (OPUS_PCM_SAMPLE_COUNT * sizeof(opus_int16))
#define SNAP_MESSAGE_BUFFER_SIZE   8192U
#define SNAP_DISCARD_BUFFER_SIZE   2048U
#define SNAP_MAX_MESSAGE_SIZE      (64U * 1024U)
#define SNAP_TASK_STACK_SIZE       12288U
#define SNAP_TASK_PRIORITY         5U
#define SNAP_TASK_CORE             1
#define SNAP_CONNECT_RETRY_MS      2000U
#define SNAP_RECONNECT_DELAY_MS    1000U
#define SNAP_STATS_INTERVAL_US     5000000LL
#define SNAP_SOCKET_TIMEOUT_MS     500U
#define SNAP_YIELD_AFTER_MESSAGES  8U

static volatile bool s_paused = false;
static volatile bool s_run = true;
static bool s_task_started = false;
static EventGroupHandle_t s_evt = NULL;
static char s_host[64] = {0};
static uint16_t s_port = 1704;
static char s_codec[16] = {0};
static OpusDecoder *s_opus_decoder = NULL;
static opus_int16 *s_opus_pcm = NULL;

static uint32_t s_wire_chunks = 0;
static uint64_t s_wire_bytes = 0;
static uint32_t s_decoded_frames = 0;
static uint64_t s_decoded_bytes = 0;
static uint64_t s_arbiter_bytes = 0;
static uint64_t s_dropped_bytes = 0;
static uint32_t s_decode_errors = 0;
static int64_t s_last_stats_us = 0;

static int send_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *data = (const uint8_t *)buffer;
    size_t total = 0;
    while (total < length) {
        int rc = send(fd, data + total, length - total, 0);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        total += (size_t)rc;
    }
    return 0;
}

static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *data = (uint8_t *)buffer;
    size_t total = 0;
    while (total < length && s_run && !s_paused) {
        int rc = recv(fd, data + total, length - total, 0);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (rc <= 0) return -1;
        total += (size_t)rc;
    }
    return total == length ? 0 : -1;
}

static void set_socket_timeouts(int fd)
{
    struct timeval timeout = {
        .tv_sec = SNAP_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (SNAP_SOCKET_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int tcp_connect(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)s_port);

    int gai = getaddrinfo(s_host, port_text, &hints, &result);
    if (gai != 0 || result == NULL) {
        ESP_LOGW(TAG, "getaddrinfo fehlgeschlagen fuer %s", s_host);
        return -1;
    }

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return -1;
    }

    if (connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect() zu %s:%u fehlgeschlagen (errno=%d)",
                 s_host, (unsigned)s_port, errno);
        close(fd);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);
    set_socket_timeouts(fd);
    ESP_LOGI(TAG, "verbunden mit Snapserver %s:%u", s_host, (unsigned)s_port);
    return fd;
}

static int send_hello(int fd)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return -1;

    char id[18];
    snprintf(id, sizeof(id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char name[32];
    snprintf(name, sizeof(name), "ESP32-SnapMesh-%02X%02X", mac[4], mac[5]);

    char json[384];
    int n = snprintf(json, sizeof(json),
        "{\"MAC\":\"%s\",\"HostName\":\"%s\",\"Version\":\"0.27.0\","
        "\"ClientName\":\"%s\",\"OS\":\"esp-idf\",\"Arch\":\"xtensa\","
        "\"Instance\":1,\"SnapStreamProtocolVersion\":2}", id, name, name);
    if (n <= 0 || n >= (int)sizeof(json)) return -1;

    uint32_t payload_length = (uint32_t)n;
    snap_base_t header = {0};
    header.type = SNAP_MSG_HELLO;
    header.size = sizeof(payload_length) + payload_length;

    if (send_full(fd, &header, sizeof(header)) != 0 ||
        send_full(fd, &payload_length, sizeof(payload_length)) != 0 ||
        send_full(fd, json, payload_length) != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "Snapcast-Hello: Client=%s, ID=%s", name, id);
    return 0;
}

static esp_err_t opus_decoder_prepare(void)
{
    if (s_opus_pcm == NULL) {
        s_opus_pcm = heap_caps_malloc(OPUS_PCM_BUFFER_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_opus_pcm == NULL) {
            s_opus_pcm = heap_caps_malloc(OPUS_PCM_BUFFER_BYTES, MALLOC_CAP_8BIT);
        }
        if (s_opus_pcm == NULL) return ESP_ERR_NO_MEM;
    }

    if (s_opus_decoder == NULL) {
        int error = OPUS_OK;
        s_opus_decoder = opus_decoder_create(SNAP_SAMPLE_RATE, SNAP_CHANNELS, &error);
        if (s_opus_decoder == NULL || error != OPUS_OK) {
            if (s_opus_decoder != NULL) opus_decoder_destroy(s_opus_decoder);
            s_opus_decoder = NULL;
            return ESP_FAIL;
        }
    }

    opus_decoder_ctl(s_opus_decoder, OPUS_RESET_STATE);
    memset(s_opus_pcm, 0, OPUS_PCM_BUFFER_BYTES);
    ESP_LOGI(TAG, "Opus-Decoder bereit: %d Hz, %d Kanaele, PCM-Puffer=%u B",
             SNAP_SAMPLE_RATE, SNAP_CHANNELS, (unsigned)OPUS_PCM_BUFFER_BYTES);
    return ESP_OK;
}

static void opus_decoder_destroy_all(void)
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

static void reset_stream_stats(void)
{
    s_wire_chunks = 0;
    s_wire_bytes = 0;
    s_decoded_frames = 0;
    s_decoded_bytes = 0;
    s_arbiter_bytes = 0;
    s_dropped_bytes = 0;
    s_decode_errors = 0;
    s_last_stats_us = esp_timer_get_time();
}

static void log_stream_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_stats_us == 0) s_last_stats_us = now;
    if (now - s_last_stats_us < SNAP_STATS_INTERVAL_US) return;

    ESP_LOGI(TAG,
        "Stream stats/5s: codec=%s packets=%lu wire=%llu B decoded_frames=%lu "
        "decoded=%llu B arbiter=%llu B dropped=%llu B decode_errors=%lu src=%d",
        s_codec[0] ? s_codec : "unknown",
        (unsigned long)s_wire_chunks, (unsigned long long)s_wire_bytes,
        (unsigned long)s_decoded_frames, (unsigned long long)s_decoded_bytes,
        (unsigned long long)s_arbiter_bytes, (unsigned long long)s_dropped_bytes,
        (unsigned long)s_decode_errors, (int)arbiter_current());
    reset_stream_stats();
}

static void handle_codec_header(const uint8_t *payload, uint32_t size)
{
    if (payload == NULL || size < sizeof(uint32_t)) return;

    uint32_t codec_length = 0;
    memcpy(&codec_length, payload, sizeof(codec_length));
    if (codec_length == 0 || codec_length >= sizeof(s_codec) ||
        sizeof(uint32_t) + codec_length > size) {
        ESP_LOGE(TAG, "Ungueltiger CodecHeader: size=%lu codec_len=%lu",
                 (unsigned long)size, (unsigned long)codec_length);
        return;
    }

    memset(s_codec, 0, sizeof(s_codec));
    memcpy(s_codec, payload + sizeof(uint32_t), codec_length);
    ESP_LOGI(TAG, "CodecHeader: codec=%s", s_codec);
    reset_stream_stats();

    if (strcmp(s_codec, "opus") == 0) {
        if (opus_decoder_prepare() != ESP_OK) {
            s_decode_errors++;
            ESP_LOGE(TAG, "Opus-Decoder konnte nicht initialisiert werden");
        }
    }
}

static void handle_wire_chunk(const uint8_t *payload, uint32_t size)
{
    if (payload == NULL || size < 12) return;

    uint32_t audio_length = 0;
    memcpy(&audio_length, payload + 8, sizeof(audio_length));
    if (audio_length == 0 || audio_length > size - 12) {
        ESP_LOGE(TAG, "WireChunk ungueltig: msg=%lu B audio=%lu B",
                 (unsigned long)size, (unsigned long)audio_length);
        return;
    }

    const uint8_t *audio = payload + 12;
    s_wire_chunks++;
    s_wire_bytes += audio_length;

    if (s_paused || arbiter_current() != SRC_SNAPCAST) {
        log_stream_stats();
        return;
    }

    if (strcmp(s_codec, "pcm") == 0) {
        size_t accepted = arbiter_feed(SRC_SNAPCAST, audio, audio_length);
        s_decoded_frames++;
        s_decoded_bytes += audio_length;
        s_arbiter_bytes += accepted;
        s_dropped_bytes += audio_length - accepted;
        log_stream_stats();
        return;
    }

    if (strcmp(s_codec, "opus") == 0) {
        if ((s_opus_decoder == NULL || s_opus_pcm == NULL) &&
            opus_decoder_prepare() != ESP_OK) {
            s_decode_errors++;
            return;
        }

        int samples = opus_decode(s_opus_decoder, audio, (opus_int32)audio_length,
                                  s_opus_pcm, OPUS_MAX_FRAME_SAMPLES, 0);
        if (samples < 0) {
            s_decode_errors++;
            ESP_LOGW(TAG, "opus_decode fehlgeschlagen: %s (%d)",
                     opus_strerror(samples), samples);
            return;
        }

        size_t bytes = (size_t)samples * SNAP_CHANNELS * sizeof(opus_int16);
        size_t accepted = arbiter_feed(SRC_SNAPCAST, s_opus_pcm, bytes);
        s_decoded_frames++;
        s_decoded_bytes += bytes;
        s_arbiter_bytes += accepted;
        s_dropped_bytes += bytes - accepted;
        log_stream_stats();
        return;
    }

    static char last_codec[16] = {0};
    if (strncmp(last_codec, s_codec, sizeof(last_codec)) != 0) {
        ESP_LOGW(TAG, "Nicht unterstuetzter Snapcast-Codec: %s", s_codec);
        strlcpy(last_codec, s_codec, sizeof(last_codec));
    }
}

static void process_message(const snap_base_t *header, const uint8_t *payload)
{
    switch (header->type) {
    case SNAP_MSG_CODEC_HEADER:
        handle_codec_header(payload, header->size);
        break;
    case SNAP_MSG_WIRE_CHUNK:
        handle_wire_chunk(payload, header->size);
        break;
    default:
        break;
    }
}

static void connection_loop(int fd)
{
    static uint8_t discard_buffer[SNAP_DISCARD_BUFFER_SIZE];
    static uint8_t message_buffer[SNAP_MESSAGE_BUFFER_SIZE];
    unsigned messages_without_yield = 0;

    if (s_paused) return;
    arbiter_set_snapcast_active(true);

    while (s_run && !s_paused) {
        snap_base_t header;
        if (read_full(fd, &header, sizeof(header)) != 0) break;
        if (s_paused) break;
        if (header.size == 0) continue;

        if (header.size > SNAP_MAX_MESSAGE_SIZE) {
            ESP_LOGE(TAG, "Snapcast-Protokollfehler: Payload=%lu B",
                     (unsigned long)header.size);
            break;
        }

        if (header.size > sizeof(message_buffer)) {
            uint32_t remaining = header.size;
            while (remaining > 0 && s_run && !s_paused) {
                uint32_t part = remaining > sizeof(discard_buffer)
                              ? sizeof(discard_buffer) : remaining;
                if (read_full(fd, discard_buffer, part) != 0) {
                    remaining = 1;
                    break;
                }
                remaining -= part;
            }
            if (remaining != 0) break;
            continue;
        }

        if (read_full(fd, message_buffer, header.size) != 0) break;
        if (s_paused) break;
        process_message(&header, message_buffer);

        if (++messages_without_yield >= SNAP_YIELD_AFTER_MESSAGES) {
            messages_without_yield = 0;
            taskYIELD();
        }
    }

    arbiter_set_snapcast_active(false);
}

static void snap_task(void *argument)
{
    (void)argument;

    while (s_run) {
        if (s_paused) {
            xEventGroupWaitBits(s_evt, EVT_RESUME, pdTRUE, pdTRUE, portMAX_DELAY);
            if (!s_run) break;
            if (s_paused) continue;
        }

        int fd = tcp_connect();
        if (fd < 0) {
            if (!s_paused) vTaskDelay(pdMS_TO_TICKS(SNAP_CONNECT_RETRY_MS));
            continue;
        }

        if (s_paused) {
            close(fd);
            continue;
        }

        memset(s_codec, 0, sizeof(s_codec));
        reset_stream_stats();
        if (send_hello(fd) == 0) connection_loop(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);

        if (!s_paused && s_run) {
            ESP_LOGI(TAG, "Verbindung verloren, Reconnect in %u ms",
                     SNAP_RECONNECT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(SNAP_RECONNECT_DELAY_MS));
        }
    }

    arbiter_set_snapcast_active(false);
    opus_decoder_destroy_all();
    s_task_started = false;
    vTaskDelete(NULL);
}

void snapclient_pause(bool pause)
{
    if (pause) {
        if (s_paused) return;
        s_paused = true;
        ESP_LOGI(TAG, "PAUSE: A2DP aktiv");
        return;
    }

    if (!s_paused) return;
    s_paused = false;
    ESP_LOGI(TAG, "RESUME: Snapclient wird wieder verbunden");
    if (s_evt != NULL) xEventGroupSetBits(s_evt, EVT_RESUME);
}

esp_err_t snapclient_start(const char *host, uint16_t port)
{
    if (s_task_started) return ESP_OK;
    if (host == NULL || host[0] == '\0' || port == 0) return ESP_ERR_INVALID_ARG;

    strlcpy(s_host, host, sizeof(s_host));
    s_port = port;

    if (s_evt == NULL) {
        s_evt = xEventGroupCreate();
        if (s_evt == NULL) return ESP_ERR_NO_MEM;
    }

    s_run = true;
    s_paused = false;
    s_task_started = true;
    memset(s_codec, 0, sizeof(s_codec));
    reset_stream_stats();

    BaseType_t rc = xTaskCreatePinnedToCore(
        snap_task, "snap", SNAP_TASK_STACK_SIZE, NULL,
        SNAP_TASK_PRIORITY, NULL, SNAP_TASK_CORE);
    if (rc != pdPASS) {
        s_task_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Snapclient gestartet (Server %s:%u, Codec automatisch, CPU %d)",
             s_host, (unsigned)s_port, SNAP_TASK_CORE);
    return ESP_OK;
}
