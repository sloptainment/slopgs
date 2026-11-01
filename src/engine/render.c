/* render.c -- the mixer: Q12 phase, interpolate-then-gain, per-voice
 * saturating accumulate into int16, loop-end by one subtraction (live MMX
 * variant 0x1a360). */
#include "render.h"
#include "synth.h"
#include "voice.h"

static int32_t abs_i32(int32_t x) { return x < 0 ? -x : x; }

/* pmulhw keeps the high word of the product, which rounds down (0x1a4b3) */
static int32_t floor_i32(double x)
{
    int32_t t = (int32_t) x;
    return t - (x < (double) t);
}

static int16_t sat_add_i16(int32_t a, int32_t b)
{
    int32_t s = a + b;
    if (s > 32767)
        return 32767;
    if (s < -32768)
        return -32768;
    return (int16_t) s;
}

/* t: the g_now frame out[0] lands on */
static void render_voice(Voice *v, int16_t *out, uint32_t frames,
                         uint32_t block_left, uint64_t t)
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
        t += v->start_delay;
        v->start_delay = 0;
    }
    if (v->starting)
    {
        /* pitch and gain as due at the note's time, same-time events
           included. An EG's first bound is its attack (0x198b6), so a zero
           attack makes a zero-length first segment that primes the mixer
           with EG2 at full depth; with an attack, EG2 is 0 here anyway */
        v->starting = 0;
        v->eg2_cents = voice_eg2_cents(v, 0);
        voice_update_pitch_at(v, t);
        v->phase_step = v->phase_step_target;
        voice_update_gain_at(v, t);
        int32_t env_hdb = voice_env_hdb(v->env_level);
        v->amp_l = voice_amp(v->atten_l_hdb, env_hdb);
        v->amp_r = voice_amp(v->atten_r_hdb, env_hdb);
    }
    uint32_t sample_end_q12 = (uint32_t) v->sample_end_s * PHASE_ONE;
    uint32_t loop_len_q12 = (uint32_t) v->loop_len_s * PHASE_ONE;
    int32_t sample_count = v->wave->sample_count;
    const int16_t *samples = v->wave->samples;

    for (uint32_t i = 0; i < frames && v->active; i++)
    {
        if (v->amp_left == 0)
        {
            /* Segment = the voice's next change, never past this Mix
             * call's end [A:0x196cd]-[A:0x196f0] */
            uint32_t seg = voice_segment_frames(v);
            if (block_left - i < seg)
                seg = block_left - i;
            if (seg == 0)
                seg = 1;
            uint64_t end = t + i + seg;

            /* pitch [A:0x1970c] and volume [A:0x19720] are read at the
               segment end; the mixer starts from the last segment's end
               value, so what stairs left over lands now (0x19444) */
            v->phase_step = (uint32_t) ((int32_t) v->phase_step + v->step_left);
            voice_eg2_segment(v, seg);
            v->lfo_elapsed = v->eg2_t + seg;
            voice_update_pitch_at(v, end);
            voice_update_gain_at(v, end);
            int32_t share =
                (int32_t) v->phase_step_target - (int32_t) v->phase_step;

            double env_end = v->env_level;
            for (uint32_t k = 0; k < seg; k++)
            {
                // a note-off releases at its own frame [A:0x19a6f]
                if (t + i + k == v->release_at)
                    voice_release_due(v, v->eg2_t + k);
                env_end = voice_step_envelope(v);
            }
            v->amp_retire = !v->active;
            v->active = 1;

            v->gain_l = v->gain_l_target;
            v->gain_r = v->gain_r_target;
            /* steps this segment takes on the BASE_RATE grid */
            uint32_t steps = (seg + RESAMPLE_FACTOR - 1) / RESAMPLE_FACTOR;
            int32_t env_hdb = voice_env_hdb(env_end);
            v->amp_step_l = (voice_amp(v->atten_l_hdb, env_hdb) - v->amp_l) /
                            (double) steps;
            v->amp_step_r = (voice_amp(v->atten_r_hdb, env_hdb) - v->amp_r) /
                            (double) steps;
            v->amp_sub = 0;
            v->amp_left = seg;

            /* the stairs the phase step takes through this segment */
            int32_t att_l = v->atten_l_hdb + env_hdb;
            int32_t att_r = v->atten_r_hdb + env_hdb;
            int32_t d_att = abs_i32(att_l - v->prev_att_l);
            if (abs_i32(att_r - v->prev_att_r) > d_att)
                d_att = abs_i32(att_r - v->prev_att_r);
            v->stair_k = voice_stair_frames(
                steps, d_att, abs_i32(v->mod_cents - v->prev_mod_cents));
            v->stair_left = v->stair_k;
            v->stair_sub = 0;
            v->step_left = share;
            v->step_inc = (int32_t) ((int64_t) share * v->stair_k * 256 /
                                     (int64_t) steps);
            v->step_acc = 0;
            v->prev_att_l = att_l;
            v->prev_att_r = att_r;
            v->prev_mod_cents = v->mod_cents;
        }

        /* the step moves (S1-S0)*K/N every K frames, the first K-1
         * frames in (0x1a360) */
        if (v->stair_sub++ % RESAMPLE_FACTOR == 0 && --v->stair_left == 0)
        {
            v->stair_left = v->stair_k;
            v->step_acc += v->step_inc;
            int32_t d = v->step_acc >> 8;
            v->step_acc -= d * 256;
            v->phase_step = (uint32_t) ((int32_t) v->phase_step + d);
            v->step_left -= d;
        }

        uint32_t pos = v->phase_pos;
        int32_t idx = (int32_t) (pos / PHASE_ONE);
        int32_t frac = (int32_t) (pos % PHASE_ONE / RESAMPLE_FACTOR);

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
        if (v->amp_sub++ % RESAMPLE_FACTOR == 0)
        {
            v->amp_l += v->amp_step_l;
            v->amp_r += v->amp_step_r;
        }
        int32_t out_l = floor_i32((double) interp * v->amp_l);
        int32_t out_r = floor_i32((double) interp * v->amp_r);

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
        v->eg2_t++;
    }
}

void render_frames(int16_t *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames * 2; i++)
        out[i] = 0;
    /* reserve top-up runs once per wall-clock service tick, NOT once per
     * render_frames() call: smf.c splits at every event (~800/s on
     * HueArme-Weekend), which cascaded Branch B into silence */
    voice_topup_tick(frames);
    /* the driver's active list takes new voices at the head
     * (0x12ffe-0x1300c) and Mix walks it from there (0x130bc), so
     * saturation sees the newest voice first: age descending, ties by
     * index */
    int order[NUM_VOICES];
    int n = 0;
    for (int vi = 0; vi < NUM_VOICES; vi++)
    {
        int j = n++;
        while (j > 0 && g_voices[order[j - 1]].age < g_voices[vi].age)
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = vi;
    }
    for (int oi = 0; oi < NUM_VOICES; oi++)
    {
        Voice *v = &g_voices[order[oi]];
        if (v->active && v->wave)
            render_voice(v, out, frames, frames, g_now);
    }
    g_now += frames;
    // what is older than this call's end is folded (0x1310a-0x13180)
    synth_ctl_fold(g_now);
    synth_service_tick();
}
