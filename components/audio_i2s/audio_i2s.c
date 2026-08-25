// =====================================================================
// audio_i2s.c  -  I2S-Master @ 48 kHz mit MCLK-Ausgabe (APLL)
//                 + Datenfluss-Statistik (alle 5 s)
// =====================================================================
#include "audio_i2s.h"

#include <string.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio_i2s";

/* -------------------------------------------------------------------------
 *  Pinout ADAU1701  (originaler ESP32: MCLK-Ausgabe nur auf GPIO0/1/3!)
 * ------------------------------------------------------------------------- */
#define PIN_MCLK   GPIO_NUM_0    /* -> ADAU MCLKI  (12.288 MHz)        */
#define PIN_BCLK   GPIO_NUM_23   /* -> ADAU MP5  INPUT_BCLK            */
#define PIN_WS     GPIO_NUM_22   /* -> ADAU MP4  INPUT_LRCLK          */
#define PIN_DOUT   GPIO_NUM_21   /* -> ADAU MP0  SDATA_IN0            */

static i2s_chan_handle_t s_tx_chan = NULL;
static volatile bool     s_muted   = false;

/* --- Statistik --- */
static uint64_t s_i2s_written   = 0;
static uint32_t s_i2s_errors    = 0;
static int64_t  s_i2s_last_log  = 0;

esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;     /* Puffer-Reserve gegen Netz-Jitter */
    chan_cfg.dma_frame_num = 240;   /* ~5 ms je Puffer @ 48 kHz         */
    chan_cfg.auto_clear    = true;  /* Underrun -> Stille               */

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_I2S_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_APLL,       /* praezise Audio-Freq. */
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,  /* 256*48k = 12.288 MHz */
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false,
                              .bclk_inv = false,
                              .ws_inv   = false },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "init_std_mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan),
                        TAG, "channel_enable failed");

    ESP_LOGI(TAG, "I2S master up: 48kHz, MCLK=12.288MHz on GPIO%d", PIN_MCLK);
    return ESP_OK;
}

esp_err_t audio_i2s_write(const void *pcm, size_t bytes,
                          size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    size_t written = 0;

    if (s_muted) {
        /* Bei Mute Nullframes gleicher Laenge schreiben -> Takt laeuft weiter. */
        static const uint8_t zeros[512] = {0};
        size_t left = bytes;
        while (left) {
            size_t chunk = left > sizeof(zeros) ? sizeof(zeros) : left;
            size_t w = 0;
            i2s_channel_write(s_tx_chan, zeros, chunk, &w,
                              pdMS_TO_TICKS(timeout_ms));
            written += w;
            left    -= chunk;
        }
        err = ESP_OK;
    } else {
        err = i2s_channel_write(s_tx_chan, pcm, bytes, &written,
                                pdMS_TO_TICKS(timeout_ms));
    }

    if (bytes_written) {
        *bytes_written = written;
    }

    /* --- Statistik --- */
    if (err == ESP_OK) {
        s_i2s_written += written;
    } else {
        s_i2s_errors++;
    }

    int64_t now = esp_timer_get_time();
    if (s_i2s_last_log == 0) {
        s_i2s_last_log = now;
    }
    if ((now - s_i2s_last_log) >= 5000000) {
        ESP_LOGI(TAG, "I2S stats/5s: written=%llu B, errors=%lu, muted=%d",
                 (unsigned long long)s_i2s_written,
                 (unsigned long)s_i2s_errors,
                 (int)s_muted);
        s_i2s_written  = 0;
        s_i2s_errors   = 0;
        s_i2s_last_log = now;
    }

    return err;
}

esp_err_t audio_i2s_mute(bool mute)
{
    s_muted = mute;
    return ESP_OK;
}