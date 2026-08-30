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

/* -------------------------------------------------------------------------
 * Allgemeine Audiokonfiguration
 * ------------------------------------------------------------------------- */

#define RB_SIZE_BYTES             (32U * 1024U)
#define WRITE_TMO_MS              50U

#define PCM_CHANNEL_COUNT         AUDIO_I2S_CHANNELS
#define PCM_BYTES_PER_SAMPLE      sizeof(int16_t)
#define PCM_BYTES_PER_FRAME       (PCM_CHANNEL_COUNT * PCM_BYTES_PER_SAMPLE)

/* -------------------------------------------------------------------------
 * Kanalbelegung und Frequenzweiche
 * ------------------------------------------------------------------------- */

#define PCM_CHANNEL_LEFT          0U
#define PCM_CHANNEL_RIGHT         1U

#define WIDEBAND_OUTPUT_CHANNEL   PCM_CHANNEL_LEFT
#define SUBWOOFER_OUTPUT_CHANNEL  PCM_CHANNEL_RIGHT

#define CROSSOVER_PI              3.14159265358979323846f
#define CROSSOVER_SAMPLE_RATE_HZ  ((float)AUDIO_I2S_SAMPLE_RATE)
#define CROSSOVER_FREQUENCY_HZ    120.0f
#define CROSSOVER_Q               0.70710678118654752440f

#define SUBWOOFER_GAIN            1.0f
#define WIDEBAND_GAIN             1.0f

/*
 * Diagnosemodus:
 *
 * 1 = Subwooferkanal fuehrt ungefiltertes Mono.
 *     Der Breitbandkanal bleibt hochpassgefiltert.
 *
 * 0 = Normalbetrieb mit Tiefpass auf dem Subwooferkanal.
 */
#define CROSSOVER_SUB_TEST_MONO   0

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
} biquad_t;

static RingbufHandle_t s_rb = NULL;
static audio_prio_t s_prio = PRIO_A2DP_FIRST;
static volatile bool s_snap_act = false;
static volatile bool s_a2dp_conn = false;
static volatile audio_src_t s_active = SRC_NONE;
static arbiter_snap_pause_cb_t s_snap_pause_cb = NULL;

static uint64_t s_feed_accepted = 0U;
static uint64_t s_feed_dropped = 0U;

static biquad_t s_lowpass_1;
static biquad_t s_lowpass_2;
static biquad_t s_highpass_1;
static biquad_t s_highpass_2;

static int16_t clamp_int16(float sample)
{
    if (sample >= (float)INT16_MAX) {
        return INT16_MAX;
    }

    if (sample <= (float)INT16_MIN) {
        return INT16_MIN;
    }

    return (int16_t)lrintf(sample);
}

static void biquad_reset(biquad_t *filter)
{
    if (filter == NULL) {
        return;
    }

    filter->z1 = 0.0f;
    filter->z2 = 0.0f;
}

static float biquad_process(biquad_t *filter, float input)
{
    const float output = filter->b0 * input + filter->z1;

    filter->z1 =
        filter->b1 * input -
        filter->a1 * output +
        filter->z2;

    filter->z2 =
        filter->b2 * input -
        filter->a2 * output;

    return output;
}

static void biquad_configure_lowpass(
    biquad_t *filter,
    float sample_rate_hz,
    float cutoff_hz,
    float q)
{
    const float omega =
        2.0f * CROSSOVER_PI * cutoff_hz / sample_rate_hz;
    const float sine = sinf(omega);
    const float cosine = cosf(omega);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    filter->b0 = ((1.0f - cosine) * 0.5f) / a0;
    filter->b1 = (1.0f - cosine) / a0;
    filter->b2 = ((1.0f - cosine) * 0.5f) / a0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;

    biquad_reset(filter);
}

static void biquad_configure_highpass(
    biquad_t *filter,
    float sample_rate_hz,
    float cutoff_hz,
    float q)
{
    const float omega =
        2.0f * CROSSOVER_PI * cutoff_hz / sample_rate_hz;
    const float sine = sinf(omega);
    const float cosine = cosf(omega);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    filter->b0 = ((1.0f + cosine) * 0.5f) / a0;
    filter->b1 = (-(1.0f + cosine)) / a0;
    filter->b2 = ((1.0f + cosine) * 0.5f) / a0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;

    biquad_reset(filter);
}

static void crossover_reset(void)
{
    biquad_reset(&s_lowpass_1);
    biquad_reset(&s_lowpass_2);
    biquad_reset(&s_highpass_1);
    biquad_reset(&s_highpass_2);
}

static esp_err_t crossover_init(void)
{
    if (CROSSOVER_SAMPLE_RATE_HZ <= 0.0f ||
        CROSSOVER_FREQUENCY_HZ <= 0.0f ||
        CROSSOVER_FREQUENCY_HZ >= CROSSOVER_SAMPLE_RATE_HZ * 0.5f) {
        ESP_LOGE(
            TAG,
            "Ungueltige Frequenzweichen-Konfiguration: fs=%.0f Hz, fc=%.1f Hz",
            CROSSOVER_SAMPLE_RATE_HZ,
            CROSSOVER_FREQUENCY_HZ);
        return ESP_ERR_INVALID_ARG;
    }

    biquad_configure_lowpass(
        &s_lowpass_1,
        CROSSOVER_SAMPLE_RATE_HZ,
        CROSSOVER_FREQUENCY_HZ,
        CROSSOVER_Q);
    biquad_configure_lowpass(
        &s_lowpass_2,
        CROSSOVER_SAMPLE_RATE_HZ,
        CROSSOVER_FREQUENCY_HZ,
        CROSSOVER_Q);
    biquad_configure_highpass(
        &s_highpass_1,
        CROSSOVER_SAMPLE_RATE_HZ,
        CROSSOVER_FREQUENCY_HZ,
        CROSSOVER_Q);
    biquad_configure_highpass(
        &s_highpass_2,
        CROSSOVER_SAMPLE_RATE_HZ,
        CROSSOVER_FREQUENCY_HZ,
        CROSSOVER_Q);

#if CROSSOVER_SUB_TEST_MONO
    ESP_LOGW(
        TAG,
        "Frequenzweichen-Test aktiv: links=Hochpass, rechts=ungefiltertes Mono");
#else
    ESP_LOGI(
        TAG,
        "Frequenzweiche aktiv: LR4 %.1f Hz, links=Hochpass, rechts=Tiefpass",
        CROSSOVER_FREQUENCY_HZ);
#endif

    return ESP_OK;
}

static void crossover_process_pcm(int16_t *pcm, size_t bytes)
{
    if (pcm == NULL || bytes < PCM_BYTES_PER_FRAME) {
        return;
    }

    const size_t frame_count = bytes / PCM_BYTES_PER_FRAME;

    for (size_t frame = 0U; frame < frame_count; ++frame) {
        const size_t index = frame * PCM_CHANNEL_COUNT;
        const int32_t left = pcm[index + PCM_CHANNEL_LEFT];
        const int32_t right = pcm[index + PCM_CHANNEL_RIGHT];
        const float mono = (float)(left + right) * 0.5f;

        float lowpass = biquad_process(&s_lowpass_1, mono);
        lowpass = biquad_process(&s_lowpass_2, lowpass);

        float highpass = biquad_process(&s_highpass_1, mono);
        highpass = biquad_process(&s_highpass_2, highpass);

        highpass *= WIDEBAND_GAIN;

#if CROSSOVER_SUB_TEST_MONO
        const float subwoofer = mono * SUBWOOFER_GAIN;
#else
        const float subwoofer = lowpass * SUBWOOFER_GAIN;
#endif

        pcm[index + WIDEBAND_OUTPUT_CHANNEL] = clamp_int16(highpass);
        pcm[index + SUBWOOFER_OUTPUT_CHANNEL] = clamp_int16(subwoofer);
    }
}

static esp_err_t write_all_pcm(const void *pcm, size_t bytes)
{
    if (pcm == NULL || bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)pcm;
    size_t offset = 0U;

    while (offset < bytes) {
        size_t bytes_written = 0U;
        const esp_err_t result = audio_i2s_write(
            data + offset,
            bytes - offset,
            &bytes_written,
            WRITE_TMO_MS);

        if (result != ESP_OK) {
            return result;
        }

        if (bytes_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }

        offset += bytes_written;
    }

    return ESP_OK;
}

static void clear_ringbuffer(void)
{
    if (s_rb == NULL) {
        return;
    }

    size_t item_size = 0U;
    void *item = NULL;

    while ((item = xRingbufferReceive(s_rb, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
}

static audio_src_t decide(void)
{
    if (s_prio == PRIO_A2DP_FIRST) {
        if (s_a2dp_conn) {
            return SRC_A2DP;
        }
        if (s_snap_act) {
            return SRC_SNAPCAST;
        }
    } else {
        if (s_snap_act) {
            return SRC_SNAPCAST;
        }
        if (s_a2dp_conn) {
            return SRC_A2DP;
        }
    }

    return SRC_NONE;
}

static void switch_to(audio_src_t next)
{
    const audio_src_t previous = s_active;

    if (next == previous) {
        return;
    }

    ESP_LOGI(TAG, "switch %d -> %d", (int)previous, (int)next);

    audio_i2s_mute(true);
    s_active = SRC_NONE;
    vTaskDelay(pdMS_TO_TICKS(5));

    clear_ringbuffer();
    crossover_reset();

    if (next == SRC_A2DP && s_snap_pause_cb != NULL) {
        ESP_LOGI(TAG, "A2DP aktiv -> Snapclient pausieren");
        s_snap_pause_cb(true);
    }

    if (previous == SRC_A2DP &&
        next != SRC_A2DP &&
        s_snap_pause_cb != NULL) {
        ESP_LOGI(TAG, "A2DP nicht mehr aktiv -> Snapclient fortsetzen");
        s_snap_pause_cb(false);
    }

    s_active = next;
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_i2s_mute(false);
}

static void player_task(void *arg)
{
    (void)arg;

    static const uint8_t silence[512] = {0};

    for (;;) {
        const audio_src_t wanted_source = decide();

        if (wanted_source != s_active) {
            switch_to(wanted_source);
        }

        if (s_active == SRC_NONE) {
            const esp_err_t result = write_all_pcm(silence, sizeof(silence));
            if (result != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "I2S-Stille konnte nicht geschrieben werden: %s",
                    esp_err_to_name(result));
            }
            continue;
        }

        size_t item_size = 0U;
        void *item = xRingbufferReceive(
            s_rb,
            &item_size,
            pdMS_TO_TICKS(WRITE_TMO_MS));

        if (item == NULL) {
            continue;
        }

        const size_t valid_bytes =
            item_size - (item_size % PCM_BYTES_PER_FRAME);

        if (valid_bytes > 0U) {
            crossover_process_pcm((int16_t *)item, valid_bytes);

            const esp_err_t result = write_all_pcm(item, valid_bytes);
            if (result != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "I2S-Ausgabe fehlgeschlagen: %s",
                    esp_err_to_name(result));
            }
        } else {
            ESP_LOGW(
                TAG,
                "Ungueltiger PCM-Block: %u Byte",
                (unsigned)item_size);
        }

        vRingbufferReturnItem(s_rb, item);
    }
}

esp_err_t arbiter_init(audio_prio_t prio)
{
    s_prio = prio;
    s_snap_act = false;
    s_a2dp_conn = false;
    s_active = SRC_NONE;
    s_feed_accepted = 0U;
    s_feed_dropped = 0U;

    const esp_err_t crossover_result = crossover_init();
    if (crossover_result != ESP_OK) {
        return crossover_result;
    }

    s_rb = xRingbufferCreate(RB_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_rb == NULL) {
        ESP_LOGE(TAG, "Ringpuffer konnte nicht angelegt werden");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        player_task,
        "player",
        4096,
        NULL,
        6,
        NULL,
        1);

    if (task_result != pdPASS) {
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        ESP_LOGE(TAG, "Player-Task konnte nicht angelegt werden");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "arbiter up, prio=%d", (int)prio);
    return ESP_OK;
}

void arbiter_register_snap_pause_cb(arbiter_snap_pause_cb_t cb)
{
    s_snap_pause_cb = cb;
}

void arbiter_set_snapcast_active(bool active)
{
    s_snap_act = active;
}

void arbiter_set_a2dp_connected(bool connected)
{
    s_a2dp_conn = connected;
}

audio_src_t arbiter_current(void)
{
    return s_active;
}

size_t arbiter_feed(
    audio_src_t from,
    const void *pcm,
    size_t bytes)
{
    if (pcm == NULL ||
        bytes == 0U ||
        s_rb == NULL ||
        from != s_active) {
        s_feed_dropped += bytes;
        return 0U;
    }

    const size_t valid_bytes =
        bytes - (bytes % PCM_BYTES_PER_FRAME);

    if (valid_bytes == 0U) {
        s_feed_dropped += bytes;
        return 0U;
    }

    const BaseType_t result = xRingbufferSend(
        s_rb,
        pcm,
        valid_bytes,
        pdMS_TO_TICKS(WRITE_TMO_MS));

    if (result == pdTRUE) {
        s_feed_accepted += valid_bytes;
        s_feed_dropped += bytes - valid_bytes;
        return valid_bytes;
    }

    s_feed_dropped += bytes;
    return 0U;
}
