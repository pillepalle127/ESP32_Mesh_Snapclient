#include "resampler.h"
#include <stdbool.h>

#define FP_SHIFT 16
#define FP_ONE   (1u << FP_SHIFT)

void resampler_init(resampler_t *r, uint32_t in_rate, uint32_t out_rate)
{
    r->in_rate  = in_rate;
    r->out_rate = out_rate;
    r->phase    = 0;
    r->last_l   = 0;
    r->last_r   = 0;
    r->primed   = false;
}

/*
 * Lineare Interpolation mit 16.16-Fixpoint-Phase.
 * step = in_rate / out_rate  -> pro Ausgabesample ruecken wir 'step'
 * Eingangssamples vor. Bei 44100->48000 ist step < 1 (Upsampling).
 */
size_t resampler_process(resampler_t *r,
                         const int16_t *in, size_t in_frames,
                         int16_t *out, size_t out_cap_frames)
{
    if (r->in_rate == r->out_rate) {
        /* Bypass: 1:1 kopieren (bis Kapazitaet) */
        size_t n = in_frames < out_cap_frames ? in_frames : out_cap_frames;
        for (size_t i = 0; i < n; ++i) {
            out[2*i]   = in[2*i];
            out[2*i+1] = in[2*i+1];
        }
        return n;
    }

    const uint32_t step = (uint32_t)(((uint64_t)r->in_rate << FP_SHIFT) / r->out_rate);
    size_t out_n = 0;
    uint32_t phase = r->phase;

    int16_t prev_l = r->last_l;
    int16_t prev_r = r->last_r;
    if (!r->primed && in_frames > 0) {
        prev_l = in[0];
        prev_r = in[1];
        r->primed = true;
    }

    size_t idx = phase >> FP_SHIFT;      /* Index ins Eingangsfeld */

    while (out_n < out_cap_frames) {
        size_t i = phase >> FP_SHIFT;
        if (i >= in_frames) break;

        uint32_t frac = phase & (FP_ONE - 1);

        int16_t cur_l = in[2*i];
        int16_t cur_r = in[2*i+1];
        int16_t pl = (i == 0) ? prev_l : in[2*(i-1)];
        int16_t pr = (i == 0) ? prev_r : in[2*(i-1)+1];

        /* out = prev + (cur - prev) * frac */
        int32_t l = pl + (((int32_t)(cur_l - pl) * (int32_t)frac) >> FP_SHIFT);
        int32_t rr = pr + (((int32_t)(cur_r - pr) * (int32_t)frac) >> FP_SHIFT);

        out[2*out_n]   = (int16_t)l;
        out[2*out_n+1] = (int16_t)rr;
        out_n++;

        phase += step;
    }

    /* Phase relativ zum Pufferende neu ausrichten, letzte Samples merken */
    size_t consumed = phase >> FP_SHIFT;
    if (consumed > in_frames) consumed = in_frames;
    if (consumed > 0) {
        r->last_l = in[2*(consumed-1)];
        r->last_r = in[2*(consumed-1)+1];
    }
    r->phase = phase - ((uint32_t)consumed << FP_SHIFT);
    (void)idx;
    return out_n;
}
