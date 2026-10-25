/* render.c -- the mixer: Q12 phase, interpolate-then-gain, per-voice
 * saturating accumulate into int16, loop-end by one subtraction (live MMX
 * variant 0x1a360). */
#include "render.h"
#include "voice.h"

#ifndef GAIN_SEGMENT_FRAMES
/* [F] Segment = min(next envelope change, device->+0x18, buffer end); the gain
 * is sampled at the segment END and ramped linearly in amplitude across it
 * (0x19644, 0x19490, 0x18fba). The driver's cap is (rate+19)/20 = 1103 frames
 * (0x128f9); the binding length is the caller's KS buffer, which the driver
 * does not choose. Probes 45/46 measure ~512 frames, probe 44 implies
 * 24-28 ms; the corpus sweep favours 128 (128: -31.18, 256: -31.14,
 * 512: -30.90 dB mean). 256 ships: best envelope r and best on sine-gate/
 * sine2. 512 costs 08_reverb +9.5 dB and 03_velocity +7.1 dB, both
 * decay-dominated -- INVESTIGATE.adoc#i-segment-length. ponytail: no knob
 * for the 1103-frame cap, dead at every swept length. */
#define GAIN_SEGMENT_FRAMES SERVICE_BLOCK_FRAMES
#endif

static int16_t sat_add_i16(int32_t a, int32_t b)
{
    int32_t s = a + b;
    if (s > 32767)
        return 32767;
    if (s < -32768)
        return -32768;
    return (int16_t) s;
}

static void render_voice(Voice *v, int16_t *out, uint32_t frames,
                         uint32_t block_left)
{
    /* clip the voice into this call the way the driver clips it: a note-on
     * dispatched at the block top still starts on its own sample */
    if (v->start_delay)
    {
        if (v->start_delay >= frames)
        {
            v->start_delay -= frames;
            return;
        }
        out += (uint32_t) v->start_delay * 2u;
        frames -= v->start_delay;
        block_left -= v->start_delay;
        v->start_delay = 0;
    }
    uint32_t sample_end_q12 = (uint32_t) v->sample_end_s << 12;
    uint32_t loop_len_q12 = (uint32_t) v->loop_len_s << 12;
    int32_t sample_count = v->wave->sample_count;
    const int16_t *samples = v->wave->samples;

    for (uint32_t i = 0; i < frames && v->active; i++)
    {
        if (v->amp_left == 0)
        {
            uint32_t seg = GAIN_SEGMENT_FRAMES;
            uint32_t next = voice_env_frames_to_change(v);
            if (next < seg)
                seg = next;
            /* ...and never past this service block's end [A:0x196de],
             * DSP-advance region [A:0x196cd]-[A:0x196f0]: a segment that ran
             * past it would outrun undispatched events. */
            if (block_left < seg)
                seg = block_left;
            if (seg == 0)
                seg = 1;

            double env_end = v->env_level;
            for (uint32_t k = 0; k < seg; k++)
                env_end = voice_step_envelope(v);
            v->amp_retire = !v->active;
            v->active = 1;

            v->gain_l = v->gain_l_target;
            v->gain_r = v->gain_r_target;
            v->amp_step_l = (env_end * v->gain_l - v->amp_l) / (double) seg;
            v->amp_step_r = (env_end * v->gain_r - v->amp_r) / (double) seg;
            v->amp_left = seg;
        }

        /* phase step is a ramp accumulator held between refreshes (0x18fba),
         * advanced before use so sample 0 already reflects one step */
        if (v->phase_step_ramp_step)
        {
            /* 1/256 LSB precision: a +-2 semitone bend at key 72 moves ~420
             * LSBs, under 1 per sample, so a whole-LSB slope turned the ramp
             * into a rate limit (inert from 10 to 40 ms on probe 33) */
            v->phase_step_ramp_acc += v->phase_step_ramp_step;
            /* arithmetic, signed */
            int32_t step = v->phase_step_ramp_acc >> 8;
            v->phase_step_ramp_acc -= step << 8;
            int64_t next = (int64_t) v->phase_step + step;
            /* phase_step is unsigned; a big downward slope must not wrap it to
             * ~4 billion */
            if (next < 0)
                next = 0;
            v->phase_step = (uint32_t) next;
        }

        uint32_t pos = v->phase_pos;
        int32_t idx = (int32_t)(pos >> 12);
        int32_t frac = (int32_t)(pos & 0xFFFu);

        if (idx >= sample_count)
            idx = sample_count > 0 ? sample_count - 1 : 0;
        if (idx < 0)
            idx = 0;
        int32_t idx1 = (idx + 1 < sample_count) ? idx + 1 : idx;

        int32_t tap0 = samples ? samples[idx] : 0;
        int32_t tap1 = samples ? samples[idx1] : 0;
        int32_t interp = (tap0 * (4096 - frac) + tap1 * frac) >> 12;

        /* one ramp step of the segment's amplitude; envelope x gain is summed
         * to ONE attenuation and ramped as a single product [A:0x190dc], item
         * 21 */
        v->amp_l += v->amp_step_l;
        v->amp_r += v->amp_step_r;
        int32_t out_l = (int32_t)((double) interp * v->amp_l);
        int32_t out_r = (int32_t)((double) interp * v->amp_r);

        out[i * 2 + 0] = sat_add_i16(out[i * 2 + 0], out_l);
        out[i * 2 + 1] = sat_add_i16(out[i * 2 + 1], out_r);

        if (--v->amp_left == 0 && v->amp_retire)
        {
            v->active = 0;
            v->amp_retire = 0;
        }

        pos += v->phase_step;
        if (pos >= sample_end_q12)
        {
            if (loop_len_q12 == 0)
            {
                v->active = 0; /* one-shot end: stop producing (S6.4.8) */
            }
            else
            {
                pos -= loop_len_q12;
            }
        }
        v->phase_pos = pos;
    }
}

void render_frames(int16_t *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2; i++)
        out[i] = 0;
    uint32_t done = 0;
    while (done < frames)
    {
        uint32_t chunk = frames - done;
        /* sub-block modulation granularity: re-slice into LFO_UPDATE_FRAMES
         * sub-chunks so a long event-free held note's LFO still oscillates
         * instead of freezing at one offset */
        if (chunk > LFO_UPDATE_FRAMES)
            chunk = LFO_UPDATE_FRAMES;
        /* reserve top-up runs once per wall-clock service tick, NOT once per
         * render_frames() call: smf.c splits at every event (~800/s on
         * HueArme-Weekend), which cascaded Branch B into silence */
        voice_topup_tick(chunk);
        /* per-block modulation recompute (pitch S4.4/LFO, gain
         * S3.5/S3.6/S3.10): live-read every sub-chunk so a change reaches
         * already-sounding notes */
        voices_update_modulation();
        for (int vi = 0; vi < NUM_VOICES; vi++)
        {
            Voice *v = &g_voices[vi];
            if (v->active && v->wave)
            {
                render_voice(v, out + (uint32_t) done * 2, chunk,
                             frames - done);
            }
        }
        /* retire pitch ramps whose horizon elapsed THIS sub-chunk -- after
         * render_voice, so every frame the ramp owned was actually rendered
         * with it before it's cleared */
        voice_ramp_tick(chunk);
        voices_advance_lfo(chunk);
        done += chunk;
    }
}
