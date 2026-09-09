/**
 * @file source_arbiter.c
 * @brief Audio source selection, PCM jitter buffering and I2S playback.
 *
 * Target: ESP-IDF 5.4.3.
 *
 * Snapcast is prebuffered before playback. After an underrun, playback waits
 * for the prebuffer level again. This prevents rapid audio/silence toggling.
 * I2S is continuously clocked with zero samples while no playable PCM exists.
 */
#include "source_arbiter.h"
#include "audio_i2s.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "arbiter";

#define RB_SIZE_BYTES          (40U * 1024U)
#define SNAP_PREBUFFER_BYTES   (8U * 1024U) /* about 107 ms at 48k/16-bit/stereo */
#define FEED_TMO_MS            10U
#define WRITE_TMO_MS           50U
#define PCM_BLOCK_BYTES        3840U         /* 20 ms at 48k/16-bit/stereo */
#define SILENCE_BYTES          PCM_BLOCK_BYTES
#define STATS_INTERVAL_US      5000000LL

static RingbufHandle_t s_rb = NULL;
static SemaphoreHandle_t s_lock = NULL;
static audio_prio_t s_prio = PRIO_A2DP_FIRST;
static bool s_snap_act = false;
static bool s_a2dp_conn = false;
static audio_src_t s_active = SRC_NONE;
static arbiter_snap_pause_cb_t s_snap_pause_cb = NULL;
static size_t s_buffered_bytes = 0;

static uint64_t s_feed_accepted = 0;
static uint64_t s_feed_wrong_source = 0;
static uint64_t s_feed_full = 0;
static uint64_t s_feed_lock_busy = 0;
static uint32_t s_player_empty = 0;
static uint32_t s_rebuffers = 0;
static int64_t s_last_stats_us = 0;

static audio_src_t decide_locked(void)
{
    if (s_prio == PRIO_A2DP_FIRST) {
        if (s_a2dp_conn) return SRC_A2DP;
        if (s_snap_act) return SRC_SNAPCAST;
    } else {
        if (s_snap_act) return SRC_SNAPCAST;
        if (s_a2dp_conn) return SRC_A2DP;
    }
    return SRC_NONE;
}

static audio_src_t get_wanted_source(void)
{
    audio_src_t result = SRC_NONE;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        result = decide_locked();
        xSemaphoreGive(s_lock);
    }
    return result;
}

static size_t get_buffered_bytes(void)
{
    size_t result = 0;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        result = s_buffered_bytes;
        xSemaphoreGive(s_lock);
    }
    return result;
}

static void flush_ringbuffer_locked(void)
{
    size_t size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_rb, &size, 0)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
    s_buffered_bytes = 0;
}

static void switch_to(audio_src_t next)
{
    audio_src_t previous;
    arbiter_snap_pause_cb_t callback;

    if (s_lock == NULL) return;

    audio_i2s_mute(true);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        audio_i2s_mute(false);
        return;
    }

    previous = s_active;
    if (next == previous) {
        xSemaphoreGive(s_lock);
        audio_i2s_mute(false);
        return;
    }

    /* Drop all data belonging to the previous source atomically with the
     * source change. Producers cannot enqueue while this lock is held. */
    flush_ringbuffer_locked();
    s_active = next;
    callback = s_snap_pause_cb;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "switch %d -> %d", (int)previous, (int)next);

    /* Never invoke application callbacks while holding the arbiter lock. */
    if (callback != NULL) {
        if (next == SRC_A2DP) {
            ESP_LOGI(TAG, "A2DP aktiv -> Snapclient pausieren");
            callback(true);
        } else if (previous == SRC_A2DP) {
            ESP_LOGI(TAG, "A2DP nicht mehr aktiv -> Snapclient fortsetzen");
            callback(false);
        }
    }

    audio_i2s_mute(false);
}

static void write_silence(void)
{
    static const uint8_t zeros[SILENCE_BYTES] = {0};
    size_t written = 0;
    (void)audio_i2s_write(zeros, sizeof(zeros), &written, WRITE_TMO_MS);
}

static void log_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_stats_us == 0) s_last_stats_us = now;
    if (now - s_last_stats_us < STATS_INTERVAL_US) return;

    audio_src_t active = SRC_NONE;
    size_t buffered = 0;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        active = s_active;
        buffered = s_buffered_bytes;
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG,
             "stats/5s: active=%d buffered=%u B accepted=%llu B wrong_src=%llu B "
             "lock_busy=%llu B ring_full=%llu B player_empty=%lu rebuffer=%lu",
             (int)active, (unsigned)buffered,
             (unsigned long long)s_feed_accepted,
             (unsigned long long)s_feed_wrong_source,
             (unsigned long long)s_feed_lock_busy,
             (unsigned long long)s_feed_full,
             (unsigned long)s_player_empty,
             (unsigned long)s_rebuffers);

    s_feed_accepted = 0;
    s_feed_wrong_source = 0;
    s_feed_lock_busy = 0;
    s_feed_full = 0;
    s_player_empty = 0;
    s_rebuffers = 0;
    s_last_stats_us = now;
}

static void player_task(void *arg)
{
    (void)arg;
    audio_src_t last_source = SRC_NONE;
    bool snap_buffer_ready = false;

    for (;;) {
        audio_src_t wanted = get_wanted_source();
        audio_src_t current = arbiter_current();

        if (wanted != current) {
            switch_to(wanted);
            current = arbiter_current();
        }

        if (current != last_source) {
            snap_buffer_ready = (current != SRC_SNAPCAST);
            last_source = current;
        }

        if (current == SRC_NONE) {
            write_silence();
            log_stats();
            continue;
        }

        if (current == SRC_SNAPCAST && !snap_buffer_ready) {
            if (get_buffered_bytes() < SNAP_PREBUFFER_BYTES) {
                write_silence();
                log_stats();
                continue;
            }
            snap_buffer_ready = true;
            ESP_LOGI(TAG, "Snapcast prebuffer ready: %u B",
                     (unsigned)get_buffered_bytes());
        }

        size_t size = 0;
        void *item = xRingbufferReceiveUpTo(
            s_rb,
            &size,
            pdMS_TO_TICKS(WRITE_TMO_MS),
            PCM_BLOCK_BYTES);
        if (item == NULL) {
            s_player_empty++;
            if (current == SRC_SNAPCAST) {
                snap_buffer_ready = false;
                s_rebuffers++;
                ESP_LOGW(TAG, "Snapcast underrun -> rebuffer");
            }
            write_silence();
            log_stats();
            continue;
        }

        size_t written = 0;
        (void)audio_i2s_write(item, size, &written, WRITE_TMO_MS);
        vRingbufferReturnItem(s_rb, item);

        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_buffered_bytes = size <= s_buffered_bytes
                             ? s_buffered_bytes - size : 0;
            xSemaphoreGive(s_lock);
        }
        log_stats();
    }
}

esp_err_t arbiter_init(audio_prio_t prio)
{
    if (s_rb != NULL) return ESP_ERR_INVALID_STATE;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    s_rb = xRingbufferCreate(RB_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_rb == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_prio = prio;
    s_active = SRC_NONE;
    s_snap_act = false;
    s_a2dp_conn = false;
    s_buffered_bytes = 0;
    s_last_stats_us = esp_timer_get_time();

    BaseType_t rc = xTaskCreatePinnedToCore(
        player_task, "player", 4096, NULL, 6, NULL, 1);
    if (rc != pdPASS) {
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "arbiter up, prio=%d, rb=%u B, prebuffer=%u B",
             (int)prio, (unsigned)RB_SIZE_BYTES,
             (unsigned)SNAP_PREBUFFER_BYTES);
    return ESP_OK;
}

void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_snap_pause_cb = cb;
        xSemaphoreGive(s_lock);
    }
}

void arbiter_set_snapcast_active(bool active)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_snap_act = active;
        xSemaphoreGive(s_lock);
    }
}

void arbiter_set_a2dp_connected(bool connected)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_a2dp_conn = connected;
        xSemaphoreGive(s_lock);
    }
}

audio_src_t arbiter_current(void)
{
    audio_src_t result = SRC_NONE;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        result = s_active;
        xSemaphoreGive(s_lock);
    }
    return result;
}

size_t arbiter_feed(audio_src_t from, const void *pcm, size_t bytes)
{
    if (pcm == NULL || bytes == 0 || s_rb == NULL || s_lock == NULL) return 0;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(FEED_TMO_MS)) != pdTRUE) {
        s_feed_lock_busy += bytes;
        return 0;
    }

    if (from != s_active) {
        s_feed_wrong_source += bytes;
        xSemaphoreGive(s_lock);
        return 0;
    }

    /* Keep source validation, queue insertion and accounting atomic.
     * The wait is bounded by FEED_TMO_MS. */
    BaseType_t ok = xRingbufferSend(s_rb, pcm, bytes,
                                    pdMS_TO_TICKS(FEED_TMO_MS));

    if (ok == pdTRUE) {
        s_buffered_bytes += bytes;
        s_feed_accepted += bytes;
        xSemaphoreGive(s_lock);
        return bytes;
    }

    s_feed_full += bytes;
    xSemaphoreGive(s_lock);
    return 0;
}
