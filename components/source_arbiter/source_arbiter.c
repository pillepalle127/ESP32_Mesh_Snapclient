#include "source_arbiter.h"
#include "audio_i2s.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

static const char *TAG = "arbiter";

#define RB_SIZE_BYTES   (32 * 1024)   /* ~170 ms @ 48k/16bit/stereo */
#define FADE_STEPS      16
#define WRITE_TMO_MS    50

static RingbufHandle_t s_rb        = NULL;
static audio_prio_t    s_prio      = PRIO_A2DP_FIRST;
static volatile bool   s_snap_act  = false;
static volatile bool   s_a2dp_conn = false;
static volatile audio_src_t s_active = SRC_NONE;
static arbiter_snap_pause_cb_t s_snap_pause_cb = NULL;

static uint64_t s_feed_accepted = 0;
static uint64_t s_feed_dropped = 0;

/* --- Prioritaetsregel ---------------------------------------------------- */
static audio_src_t decide(void)
{
    if (s_prio == PRIO_A2DP_FIRST) {
        if (s_a2dp_conn) return SRC_A2DP;
        if (s_snap_act)  return SRC_SNAPCAST;
    } else { /* PRIO_SNAPCAST_FIRST */
        if (s_snap_act)  return SRC_SNAPCAST;
        if (s_a2dp_conn) return SRC_A2DP;
    }
    return SRC_NONE;
}

/* Umschaltung mit Fade-out/Fade-in ueber Mute; Ringpuffer leeren. */
static void switch_to(audio_src_t next)
{
    if (next == s_active) return;

    ESP_LOGI(TAG, "switch %d -> %d", s_active, next);
    audio_i2s_mute(true);                 /* Fade-out (hier: hartes Mute) */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Ringpuffer verwerfen, damit keine alten Frames der Altquelle durchlaufen */
    size_t sz; void *p;
    while ((p = xRingbufferReceive(s_rb, &sz, 0)) != NULL) {
        vRingbufferReturnItem(s_rb, p);
    }

    /* Koexistenz: bei A2DP den Snapclient-Socket pausieren (WLAN-Airtime
     * frei), bei Rueckkehr zu Snapcast wieder reconnecten. */
    if (s_snap_pause_cb) {
        if (next == SRC_A2DP)          s_snap_pause_cb(true);
        else if (next == SRC_SNAPCAST) s_snap_pause_cb(false);
    }

    s_active = next;
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_i2s_mute(false);                /* Fade-in */
}

/* --- Mixer/Player-Task: Ringpuffer -> I2S -------------------------------- */
static void player_task(void *arg)
{
    for (;;) {
        audio_src_t want = decide();
        if (want != s_active) switch_to(want);

        if (s_active == SRC_NONE) {           /* Stille ausgeben, Takt haelt */
            static uint8_t zeros[512];
            size_t w;
            audio_i2s_write(zeros, sizeof(zeros), &w, WRITE_TMO_MS);
            continue;
        }

        size_t sz;
        void *item = xRingbufferReceive(s_rb, &sz, pdMS_TO_TICKS(WRITE_TMO_MS));
        if (item) {
            size_t w;
            audio_i2s_write(item, sz, &w, WRITE_TMO_MS);
            vRingbufferReturnItem(s_rb, item);
        } else {
            /* Underrun: auto_clear im I2S sorgt fuer Stille statt Rauschen. */
        }
    }
}

esp_err_t arbiter_init(audio_prio_t prio)
{
    s_prio = prio;
    s_rb = xRingbufferCreate(RB_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_rb) return ESP_ERR_NO_MEM;

    /* Player auf Core 1 (Audio), Netzwerk/BT laufen auf Core 0. */
    xTaskCreatePinnedToCore(player_task, "player", 4096, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "arbiter up, prio=%d", prio);
    return ESP_OK;
}

void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb) { s_snap_pause_cb = cb; }
void arbiter_set_snapcast_active(bool active)   { s_snap_act  = active; }
void arbiter_set_a2dp_connected(bool connected) { s_a2dp_conn = connected; }
audio_src_t arbiter_current(void)               { return s_active; }

size_t arbiter_feed(audio_src_t from, const void *pcm, size_t bytes)
{
    //if (from != s_active || s_rb == NULL) return 0;   /* Fremdquelle -> drop */
if (from != s_active || s_rb == NULL) {
    s_feed_dropped += bytes;
    return 0;
}
    BaseType_t ok = xRingbufferSend(s_rb, pcm, bytes, pdMS_TO_TICKS(WRITE_TMO_MS));
if (ok == pdTRUE) {
    s_feed_accepted += bytes;
    return bytes;
}

s_feed_dropped += bytes;
return 0;

    return (ok == pdTRUE) ? bytes : 0;
}
