#include "source_arbiter.h"
#include "audio_i2s.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "arbiter";

#define RB_SIZE_BYTES              (32U * 1024U)
#define WRITE_TMO_MS               50U
#define PCM_CHANNEL_COUNT          AUDIO_I2S_CHANNELS
#define PCM_BYTES_PER_SAMPLE       sizeof(int16_t)
#define PCM_BYTES_PER_FRAME        (PCM_CHANNEL_COUNT * PCM_BYTES_PER_SAMPLE)

/* Kanalbelegung und Linkwitz-Riley-Frequenzweiche vierter Ordnung. */
#define PCM_CHANNEL_LEFT           0U
#define PCM_CHANNEL_RIGHT          1U
#define SUBWOOFER_OUTPUT_CHANNEL   PCM_CHANNEL_LEFT
#define WIDEBAND_OUTPUT_CHANNEL    PCM_CHANNEL_RIGHT
#define CROSSOVER_PI               3.14159265358979323846f
#define CROSSOVER_SAMPLE_RATE_HZ   ((float)AUDIO_I2S_SAMPLE_RATE)
#define CROSSOVER_FREQUENCY_HZ     120.0f
#define CROSSOVER_Q                0.70710678118654752440f
#define SUBWOOFER_GAIN             1.0f
#define WIDEBAND_GAIN              1.0f

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} biquad_t;

static RingbufHandle_t s_rb = NULL;
static audio_prio_t s_prio = PRIO_A2DP_FIRST;
static volatile bool s_snap_act = false;
static volatile bool s_a2dp_conn = false;
static volatile audio_src_t s_active = SRC_NONE;
static arbiter_snap_pause_cb_t s_snap_pause_cb = NULL;
static uint64_t s_feed_accepted = 0U;
static uint64_t s_feed_dropped = 0U;
static biquad_t s_lowpass_1, s_lowpass_2;
static biquad_t s_highpass_1, s_highpass_2;

static int16_t clamp_int16(float sample) {
    if (sample >= (float)INT16_MAX) return INT16_MAX;
    if (sample <= (float)INT16_MIN) return INT16_MIN;
    return (int16_t)lrintf(sample);
}

static void biquad_reset(biquad_t *filter) {
    if (filter == NULL) return;
    filter->z1 = 0.0f;
    filter->z2 = 0.0f;
}

static float biquad_process(biquad_t *filter, float input) {
    const float output = filter->b0 * input + filter->z1;
    filter->z1 = filter->b1 * input - filter->a1 * output + filter->z2;
    filter->z2 = filter->b2 * input - filter->a2 * output;
    return output;
}

static void biquad_configure_lowpass(biquad_t *filter, float fs, float fc, float q) {
    const float omega = 2.0f * CROSSOVER_PI * fc / fs;
    const float sine = sinf(omega), cosine = cosf(omega);
    const float alpha = sine / (2.0f * q), a0 = 1.0f + alpha;
    filter->b0 = ((1.0f - cosine) * 0.5f) / a0;
    filter->b1 = (1.0f - cosine) / a0;
    filter->b2 = ((1.0f - cosine) * 0.5f) / a0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;
    biquad_reset(filter);
}

static void biquad_configure_highpass(biquad_t *filter, float fs, float fc, float q) {
    const float omega = 2.0f * CROSSOVER_PI * fc / fs;
    const float sine = sinf(omega), cosine = cosf(omega);
    const float alpha = sine / (2.0f * q), a0 = 1.0f + alpha;
    filter->b0 = ((1.0f + cosine) * 0.5f) / a0;
    filter->b1 = (-(1.0f + cosine)) / a0;
    filter->b2 = ((1.0f + cosine) * 0.5f) / a0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;
    biquad_reset(filter);
}

static void crossover_reset(void) {
    biquad_reset(&s_lowpass_1); biquad_reset(&s_lowpass_2);
    biquad_reset(&s_highpass_1); biquad_reset(&s_highpass_2);
}

static esp_err_t crossover_init(void) {
    if (CROSSOVER_FREQUENCY_HZ <= 0.0f ||
        CROSSOVER_FREQUENCY_HZ >= CROSSOVER_SAMPLE_RATE_HZ * 0.5f) {
        return ESP_ERR_INVALID_ARG;
    }
    biquad_configure_lowpass(&s_lowpass_1, CROSSOVER_SAMPLE_RATE_HZ, CROSSOVER_FREQUENCY_HZ, CROSSOVER_Q);
    biquad_configure_lowpass(&s_lowpass_2, CROSSOVER_SAMPLE_RATE_HZ, CROSSOVER_FREQUENCY_HZ, CROSSOVER_Q);
    biquad_configure_highpass(&s_highpass_1, CROSSOVER_SAMPLE_RATE_HZ, CROSSOVER_FREQUENCY_HZ, CROSSOVER_Q);
    biquad_configure_highpass(&s_highpass_2, CROSSOVER_SAMPLE_RATE_HZ, CROSSOVER_FREQUENCY_HZ, CROSSOVER_Q);
    ESP_LOGI(TAG, "Frequenzweiche aktiv: LR4 %.1f Hz, links=Tiefpass/Subwoofer, rechts=Hochpass/Breitband", CROSSOVER_FREQUENCY_HZ);
    return ESP_OK;
}

static void crossover_process_pcm(int16_t *pcm, size_t bytes) {
    const size_t frames = bytes / PCM_BYTES_PER_FRAME;
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t index = frame * PCM_CHANNEL_COUNT;
        const float mono = (float)((int32_t)pcm[index] + (int32_t)pcm[index + 1]) * 0.5f;
        float low = biquad_process(&s_lowpass_1, mono);
        low = biquad_process(&s_lowpass_2, low) * SUBWOOFER_GAIN;
        float high = biquad_process(&s_highpass_1, mono);
        high = biquad_process(&s_highpass_2, high) * WIDEBAND_GAIN;
        pcm[index + SUBWOOFER_OUTPUT_CHANNEL] = clamp_int16(low);
        pcm[index + WIDEBAND_OUTPUT_CHANNEL] = clamp_int16(high);
    }
}

static audio_src_t decide(void) {
    if (s_prio == PRIO_A2DP_FIRST) {
        if (s_a2dp_conn) return SRC_A2DP;
        if (s_snap_act) return SRC_SNAPCAST;
    } else {
        if (s_snap_act) return SRC_SNAPCAST;
        if (s_a2dp_conn) return SRC_A2DP;
    }
    return SRC_NONE;
}

static void clear_ringbuffer(void) {
    if (s_rb == NULL) return;
    size_t size = 0;
    void *item;
    while ((item = xRingbufferReceive(s_rb, &size, 0)) != NULL) vRingbufferReturnItem(s_rb, item);
}

static void switch_to(audio_src_t next) {
    const audio_src_t previous = s_active;
    if (next == previous) return;
    ESP_LOGI(TAG, "switch %d -> %d", (int)previous, (int)next);
    audio_i2s_mute(true);
    s_active = SRC_NONE;
    vTaskDelay(pdMS_TO_TICKS(5));
    clear_ringbuffer();
    crossover_reset();
    if (next == SRC_A2DP && s_snap_pause_cb) s_snap_pause_cb(true);
    if (previous == SRC_A2DP && next != SRC_A2DP && s_snap_pause_cb) s_snap_pause_cb(false);
    s_active = next;
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_i2s_mute(false);
}

static void player_task(void *arg) {
    (void)arg;
    static const uint8_t silence[512] = {0};
    for (;;) {
        const audio_src_t wanted = decide();
        if (wanted != s_active) switch_to(wanted);
        if (s_active == SRC_NONE) {
            size_t written = 0;
            audio_i2s_write(silence, sizeof(silence), &written, WRITE_TMO_MS);
            continue;
        }
        size_t size = 0;
        void *item = xRingbufferReceive(s_rb, &size, pdMS_TO_TICKS(WRITE_TMO_MS));
        if (item != NULL) {
            const size_t valid = size - (size % PCM_BYTES_PER_FRAME);
            if (valid > 0) {
                crossover_process_pcm((int16_t *)item, valid);
                size_t written = 0;
                audio_i2s_write(item, valid, &written, WRITE_TMO_MS);
            }
            vRingbufferReturnItem(s_rb, item);
        }
    }
}

esp_err_t arbiter_init(audio_prio_t prio) {
    s_prio = prio;
    esp_err_t err = crossover_init();
    if (err != ESP_OK) return err;
    s_rb = xRingbufferCreate(RB_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_rb == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(player_task, "player", 4096, NULL, 6, NULL, 1) != pdPASS) {
        vRingbufferDelete(s_rb); s_rb = NULL; return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "arbiter up, prio=%d", (int)prio);
    return ESP_OK;
}

void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb) { s_snap_pause_cb = cb; }
void arbiter_set_snapcast_active(bool active) { s_snap_act = active; }
void arbiter_set_a2dp_connected(bool connected) { s_a2dp_conn = connected; }
audio_src_t arbiter_current(void) { return s_active; }

size_t arbiter_feed(audio_src_t from, const void *pcm, size_t bytes) {
    if (pcm == NULL || bytes == 0 || s_rb == NULL || from != s_active) {
        s_feed_dropped += bytes;
        return 0;
    }
    const size_t valid = bytes - (bytes % PCM_BYTES_PER_FRAME);
    if (valid == 0) { s_feed_dropped += bytes; return 0; }
    if (xRingbufferSend(s_rb, pcm, valid, pdMS_TO_TICKS(WRITE_TMO_MS)) == pdTRUE) {
        s_feed_accepted += valid;
        s_feed_dropped += bytes - valid;
        return valid;
    }
    s_feed_dropped += bytes;
    return 0;
}
