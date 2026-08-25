/**
 * @file  snapclient_glue.c
 * @brief Snapcast-Client: TCP zum Snapserver, Basisprotokoll, PCM -> Arbiter.
 */
#include "snapclient_glue.h"
#include "source_arbiter.h"
#include "audio_i2s.h"
#include "esp_mac.h"
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "snap";

/* --- Snapcast Base-Message ------------------------------------------------ */
typedef enum {
    SNAP_MSG_BASE            = 0,
    SNAP_MSG_CODEC_HEADER    = 1,
    SNAP_MSG_WIRE_CHUNK      = 2,
    SNAP_MSG_SERVER_SETTINGS = 3,
    SNAP_MSG_TIME            = 4,
    SNAP_MSG_HELLO           = 5,
    SNAP_MSG_STREAM_TAGS     = 6,
} snap_msg_type_t;

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

/* --- Zustand -------------------------------------------------------------- */
static int                s_sock   = -1;
static volatile bool      s_paused = false;
static volatile bool      s_run    = true;
static EventGroupHandle_t s_evt    = NULL;
#define EVT_RESUME  BIT0

static char s_host[64] = {0};
static uint16_t s_port = 1704;
static char s_codec[16] = "pcm";

/* --- Statistik --- */
static uint32_t s_wire_chunks   = 0;
static uint64_t s_wire_bytes    = 0;
static uint32_t s_arbiter_bytes = 0;
static uint32_t s_dropped_bytes = 0;
static int64_t  s_last_stats_us = 0;

/* ------------------------------------------------------------------------- */
static int tcp_connect(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)s_port);

    if (getaddrinfo(s_host, port, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "getaddrinfo fehlgeschlagen fuer %s", s_host);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect() zu %s:%u fehlgeschlagen (errno=%d)",
                 s_host, (unsigned)s_port, errno);
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "verbunden mit Snapserver %s:%u", s_host, (unsigned)s_port);
    return s;
}

static int read_full(int s, void *buf, size_t n)
{
    uint8_t *p = buf; size_t got = 0;
    while (got < n) {
        int r = recv(s, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int send_hello(int s)
{
    uint8_t mac[6];

    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA-MAC konnte nicht gelesen werden: %s",
                 esp_err_to_name(err));
        return -1;
    }

    char client_id[18];
    snprintf(client_id, sizeof(client_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    char json[320];
    int json_len = snprintf(
        json, sizeof(json),
        "{"
        "\"MAC\":\"%s\","
        "\"HostName\":\"snapmesh-%02x%02x%02x\","
        "\"Version\":\"0.27.0\","
        "\"ClientName\":\"ESP32-SnapMesh\","
        "\"OS\":\"esp-idf\","
        "\"Arch\":\"xtensa\","
        "\"Instance\":1,"
        "\"SnapStreamProtocolVersion\":2"
        "}",
        client_id,
        mac[3], mac[4], mac[5]);

    if (json_len < 0 || json_len >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "Snapcast-Hello JSON zu lang");
        return -1;
    }

    uint32_t payload_len = (uint32_t)json_len;

    snap_base_t h = {0};
    h.type = SNAP_MSG_HELLO;
    h.size = sizeof(payload_len) + payload_len;

    if (send(s, &h, sizeof(h), 0) != (int)sizeof(h)) {
        return -1;
    }

    if (send(s, &payload_len, sizeof(payload_len), 0)
            != (int)sizeof(payload_len)) {
        return -1;
    }

    if (send(s, json, payload_len, 0) != (int)payload_len) {
        return -1;
    }

    ESP_LOGI(TAG, "Snapcast-Hello mit Client-ID %s", client_id);
    return 0;
}

static void log_stream_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_stats_us == 0) { s_last_stats_us = now; return; }

    if ((now - s_last_stats_us) >= 5000000) {
        ESP_LOGI(TAG,
                 "PCM stats/5s: chunks=%lu wire=%llu B arbiter=%lu B dropped=%lu B src=%d",
                 (unsigned long)s_wire_chunks,
                 (unsigned long long)s_wire_bytes,
                 (unsigned long)s_arbiter_bytes,
                 (unsigned long)s_dropped_bytes,
                 (int)arbiter_current());
        s_wire_chunks = 0;
        s_wire_bytes = 0;
        s_arbiter_bytes = 0;
        s_dropped_bytes = 0;
        s_last_stats_us = now;
    }
}

/* CodecHeader: [codecStrLen][codecStr][payloadLen][payload] */
static void handle_codec_header(const uint8_t *p, uint32_t size)
{
    if (size < 4) return;
    uint32_t clen; memcpy(&clen, p, 4);
    if (clen < sizeof(s_codec) && 4 + clen <= size) {
        memcpy(s_codec, p + 4, clen);
        s_codec[clen] = 0;
        ESP_LOGI(TAG, "CodecHeader: codec=%s", s_codec);
    }
}

/* WireChunk: [ts_sec][ts_usec][payloadLen][payload(audio)] */
static void handle_wire_chunk(const uint8_t *p, uint32_t size)
{
    if (size < 12) {
        ESP_LOGW(TAG, "WireChunk zu klein: %lu B", (unsigned long)size);
        return;
    }
    uint32_t plen; memcpy(&plen, p + 8, 4);
    if (12 + plen > size) {
        ESP_LOGE(TAG, "WireChunk ungueltig: msg=%lu payload=%lu",
                 (unsigned long)size, (unsigned long)plen);
        return;
    }
    const uint8_t *audio = p + 12;

    s_wire_chunks++;
    s_wire_bytes += plen;

    if (strcmp(s_codec, "pcm") == 0) {
        size_t accepted = arbiter_feed(SRC_SNAPCAST, audio, plen);
        s_arbiter_bytes += accepted;
        if (accepted < plen) s_dropped_bytes += plen - accepted;
    } else {
        /* TODO: FLAC/OPUS dekodieren */
    }

    log_stream_stats();
}

static void process_message(const snap_base_t *h, const uint8_t *payload)
{
    switch (h->type) {
    case SNAP_MSG_CODEC_HEADER:    handle_codec_header(payload, h->size); break;
    case SNAP_MSG_WIRE_CHUNK:      handle_wire_chunk(payload, h->size);   break;
    case SNAP_MSG_SERVER_SETTINGS: break;
    case SNAP_MSG_TIME:            break;
    default:                       break;
    }
}

static void connection_loop(int s)
{
    static uint8_t buf[2048];
    static uint8_t big[8192];
    arbiter_set_snapcast_active(true);

    while (s_run && !s_paused) {
        snap_base_t h;
        if (read_full(s, &h, sizeof(h)) != 0) break;

        if (h.size > 0) {
            uint32_t sz = h.size;
            if (sz > sizeof(big)) {
                ESP_LOGE(TAG, "Payload zu gross: %lu B, Puffer=%u B",
                         (unsigned long)sz, (unsigned)sizeof(big));
                uint32_t off = 0;
                while (off < sz) {
                    uint32_t c = sz - off;
                    if (c > sizeof(buf)) c = sizeof(buf);
                    if (read_full(s, buf, c) != 0) {
                        arbiter_set_snapcast_active(false);
                        return;
                    }
                    off += c;
                }
                continue;
            }
            if (read_full(s, big, sz) != 0) break;
            process_message(&h, big);
        }
    }
    arbiter_set_snapcast_active(false);
}

static void snap_task(void *arg)
{
    while (s_run) {
        if (s_paused) {
            xEventGroupWaitBits(s_evt, EVT_RESUME, pdTRUE, pdTRUE, portMAX_DELAY);
            if (!s_run) break;
        }

        s_sock = tcp_connect();
        if (s_sock < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        if (send_hello(s_sock) == 0) {
            connection_loop(s_sock);
        }

        close(s_sock);
        s_sock = -1;
        if (!s_paused) {
            ESP_LOGI(TAG, "Verbindung verloren -> reconnect in 1s");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    vTaskDelete(NULL);
}

void snapclient_pause(bool pause)
{
    s_paused = pause;
    if (pause) {
        ESP_LOGI(TAG, "PAUSE (A2DP aktiv) -> Socket schliessen");
        if (s_sock >= 0) { shutdown(s_sock, SHUT_RDWR); close(s_sock); s_sock = -1; }
    } else {
        ESP_LOGI(TAG, "RESUME -> reconnect zum Snapserver");
        xEventGroupSetBits(s_evt, EVT_RESUME);
    }
}

esp_err_t snapclient_start(const char *host, uint16_t port)
{
    if (host && host[0]) strncpy(s_host, host, sizeof(s_host) - 1);
    if (port) s_port = port;

    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;
    s_run = true;

    xTaskCreatePinnedToCore(snap_task, "snap", 8192, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Snapclient gestartet (Server %s:%u, PCM-Pfad aktiv)",
             s_host, (unsigned)s_port);
    return ESP_OK;
}
