/**
 * @file  audio_i2s.h
 * @brief I2S-Master-Ausgang zum ADAU1701 (48 kHz, MCLK aktiv, APLL-Takt).
 *
 * ESP32 = I2S-Master, ADAU1701 = Input-Port-Slave (PLL-Lock auf MCLK).
 *
 * Verdrahtung ESP32-WROVER -> ADAU1701:
 *   GPIO0   MCLK  12.288 MHz (256xfs) -> MCLKI        (PLL-Referenz)
 *   GPIO23  BCLK  3.072 MHz           -> MP5  INPUT_BCLK
 *   GPIO22  LRCLK 48 kHz              -> MP4  INPUT_LRCLK
 *   GPIO21  SDATA                     -> MP0  SDATA_IN0
 *   GND                               -> DGND
 *
 * ADAU1701: PLLMODE0=GND, PLLMODE1=VDD (256xfs). ADAU-Quarz entfaellt.
 *           MP10/MP11 (Output-Clocks) unbenutzt/aufgetrennt.
 *
 * Taktkette @ 48 kHz:
 *   MCLK  = 256 x fs = 12.288 MHz
 *   BCLK  =  64 x fs =  3.072 MHz   (32-bit-Slots)
 *   LRCLK =           48.000 kHz
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"


#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_I2S_SAMPLE_RATE   48000
#define AUDIO_I2S_BITS          16      /* Nutzdaten; Slot-Breite = 32 bit */
#define AUDIO_I2S_CHANNELS      2

/** I2S-Master initialisieren und Kanal aktivieren. Takt laeuft danach durch. */
esp_err_t audio_i2s_init(void);

/**
 * PCM-Frames (16 bit, stereo, interleaved) in den I2S-DMA schreiben.
 * Blockiert bis Platz frei ist oder timeout_ms abgelaufen ist.
 */
esp_err_t audio_i2s_write(const void *pcm, size_t bytes,
                          size_t *bytes_written, uint32_t timeout_ms);

/** Weiche Stummschaltung (Nullframes) ohne den Takt anzuhalten. */
esp_err_t audio_i2s_mute(bool mute);

#ifdef __cplusplus
}
#endif
