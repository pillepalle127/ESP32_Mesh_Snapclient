#include "source_arbiter.h"
#include "audio_i2s.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "arbiter";

#define RB_SIZE_BYTES  (32 * 1024)
#define WRITE_TMO_MS   50

static RingbufHandle_t s_rb = NULL;
static SemaphoreHandle_t s_lock = NULL;
static audio_prio_t s_prio = PRIO_A2DP_FIRST;
static bool s_snap_act = false;
static bool s_a2dp_conn = false;
static audio_src_t s_active = SRC_NONE;
static arbiter_snap_pause_cb_t s_snap_pause_cb = NULL;

static uint64_t s_feed_accepted = 0;
static uint64_t s_feed_wrong_source = 0;
static uint64_t s_feed_full = 0;

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

static void flush_ringbuffer_locked(void)
{
    size_t size = 0;
    void *item = NULL;

    while ((item = xRingbufferReceive(s_rb, &size, 0)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
}

static void switch_to(audio_src_t next)
{
    audio_src_t previous;
    arbiter_snap_pause_cb_t callback;

    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    previous = s_active;
    if (next == previous) {
        xSemaphoreGive(s_lock);
        return;
    }

    ESP_LOGI(TAG, "switch %d -> %d", (int)previous, (int)next);
    audio_i2s_mute(true);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Feed and flush are serialized by s_lock. A block from the old source
     * can therefore not be inserted after this flush. */
    flush_ringbuffer_locked();
    s_active = next;
    callback = s_snap_pause_cb;
    xSemaphoreGive(s_lock);

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

    vTaskDelay(pdMS_TO_TICKS(5));
    audio_i2s_mute(false);
}

static void player_task(void *arg)
{
    (void)arg;
    static const uint8_t zeros[512] = {0};

    for (;;) {
        audio_src_t wanted = get_wanted_source();
        if (wanted != arbiter_current()) {
            switch_to(wanted);
        }

        if (arbiter_current() == SRC_NONE) {
            size_t written = 0;
            audio_i2s_write(zeros, sizeof(zeros), &written, WRITE_TMO_MS);
            continue;
        }

        size_t size = 0;
        void *item = xRingbufferReceive(s_rb, &size, pdMS_TO_TICKS(WRITE_TMO_MS));
        if (item != NULL) {
            size_t written = 0;
            audio_i2s_write(item, size, &written, WRITE_TMO_MS);
            vRingbufferReturnItem(s_rb, item);
        }
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

    BaseType_t rc = xTaskCreatePinnedToCore(
        player_task, "player", 4096, NULL, 6, NULL, 1);
    if (rc != pdPASS) {
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "arbiter up, prio=%d", (int)prio);
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

    /* Non-blocking by design: audio producers must never stall for 50 ms. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        s_feed_full += bytes;
        return 0;
    }

    if (from != s_active) {
        s_feed_wrong_source += bytes;
        xSemaphoreGive(s_lock);
        return 0;
    }

    BaseType_t ok = xRingbufferSend(s_rb, pcm, bytes, 0);
    if (ok == pdTRUE) {
        s_feed_accepted += bytes;
        xSemaphoreGive(s_lock);
        return bytes;
    }

    s_feed_full += bytes;
    xSemaphoreGive(s_lock);
    return 0;
}
