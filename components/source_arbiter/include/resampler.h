/**
 * @file  resampler.h
 * @brief Leichter linearer Resampler fuer 16-bit-Stereo-PCM.
 *
 * Zweck: A2DP liefert meist 44.1 kHz SBC-PCM, die I2S-Kette laeuft fix auf
 * 48 kHz. Lineare Interpolation reicht fuer ein Uebungsprojekt; fuer hoehere
 * Qualitaet spaeter durch einen Polyphase-FIR ersetzen.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in_rate;
    uint32_t out_rate;
    /* Phasenakkumulator (Fixpoint 16.16) + letzte Samples fuer Kontinuitaet */
    uint32_t phase;         /* 16.16 */
    int16_t  last_l;
    int16_t  last_r;
    bool     primed;
} resampler_t;

void resampler_init(resampler_t *r, uint32_t in_rate, uint32_t out_rate);

/**
 * Resample interleaved 16-bit-Stereo.
 * @param in         Eingangs-Samples (L,R,L,R,...)
 * @param in_frames  Anzahl Stereo-Frames im Eingang
 * @param out        Ausgangspuffer
 * @param out_cap_frames  Kapazitaet des Ausgangs in Frames
 * @return  erzeugte Ausgangs-Frames
 */
size_t resampler_process(resampler_t *r,
                         const int16_t *in, size_t in_frames,
                         int16_t *out, size_t out_cap_frames);

/** Worst-case Ausgabe-Frames fuer gegebene Eingabemenge (fuer Puffergroesse). */
static inline size_t resampler_max_out(const resampler_t *r, size_t in_frames)
{
    return (size_t)((uint64_t)in_frames * r->out_rate / r->in_rate) + 2;
}

#ifdef __cplusplus
}
#endif
