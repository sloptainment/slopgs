/* voice.c -- voice pool (48 primary + 6 reserve), stealing, key-group choke,
 * per-voice pitch/envelope/gain computation. The envelope generator is a
 * per-sample model, not the driver's unrecovered block-cadenced one
 * (INVESTIGATE.adoc#i-envelope-cadence). */
#include "voice.h"
#include "rt.h"
#include "synth.h"
#include "tables.h"

Voice g_voices[NUM_VOICES];
uint32_t g_voice_age_counter;

/* TOPUP_RESERVE_COUNT: the driver's reserve target, device+0xf52 = 6
 * (0x12b05); -D0 collapses to a flat pool */
#ifndef TOPUP_RESERVE_COUNT
#define TOPUP_RESERVE_COUNT 6
#endif
#define NUM_RESERVE TOPUP_RESERVE_COUNT
#define NUM_PRIMARY (NUM_VOICES - NUM_RESERVE)

/* top-up tick clock, see voice_topup_tick */
static uint32_t g_topup_accum = 0;

void voice_pool_reset(void)
{
    g_topup_accum = 0;
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        v->active = 0;
        v->channel = -1;
        v->note = -1;
        v->locale = 0;
        v->key_group = 0;
        v->wave = 0;
        v->artic = 0;
        v->phase_pos = 0;
        v->phase_step = 0;
        v->phase_step_target = 0;
        v->phase_step_ramp_step = 0;
        v->phase_step_ramp_acc = 0;
        v->ramp_left = 0;
        v->base_cents = 0;
        v->base_ratio_q12 = 4096; /* unity */
        v->loop_start_s = v->loop_len_s = v->sample_end_s = 0;
        v->gain_l = v->gain_r = 0.0;
        v->gain_l_target = v->gain_r_target = 0.0;
        v->amp_left = 0;
        v->amp_l = v->amp_r = 0.0;
        v->amp_step_l = v->amp_step_r = 0.0;
        v->amp_retire = 0;
        v->start_delay = 0;
        v->atten_const_hdb = 0;
        v->env_stage = ENV_IDLE;
        v->env_level = 0.0;
        v->env_decay_samples_left = 0;
        v->eg2_stage = ENV_IDLE;
        v->eg2_level = 0.0;
        v->eg2_depth_cents = 0.0;
        v->held = 0;
        v->sustain_deferred = 0;
        v->age = 0;
        v->lfo_elapsed = 0;
        /* which indices start as reserve is cosmetic: nothing distinguishes
         * tier once active, and Branch A retags nodes over time */
        v->in_reserve = (i >= NUM_PRIMARY) ? 1 : 0;
        v->fast_release_committed = 0;
    }
    g_voice_age_counter = 0;
}

/* CentsToRatio's +-4800 clamp (0x18e2c), also applied to EG2's pitch
 * contribution: same bound, same table domain */
#define CENTS_CLAMP 4800

static int32_t cents_to_ratio_q12(int32_t cents)
{
    if (cents > CENTS_CLAMP)
        cents = CENTS_CLAMP;
    if (cents < -CENTS_CLAMP)
        cents = -CENTS_CLAMP;
    if (cents >= -100 && cents <= 100)
    {
        return g_table_cents[cents + 100];
    }
    int32_t whole = cents / 100;
    int32_t rem_cents = cents % 100;
    int32_t octaves = whole / 12;
    int32_t rem_semi = whole % 12;
    int64_t t2 = g_table_cents[rem_cents + 100];
    int64_t t3 = g_table_semi[rem_semi + 48];
    int64_t scaled_t2 = (octaves >= 0) ? (t2 << octaves) : (t2 >> (-octaves));
    return (int32_t)((t3 * scaled_t2) >> 12);
}

/* 0x15364: tc = lScale/65536.0; duration = 2^(tc/1200), trunc toward
 * zero (kept as a plain double here, feeding a continuous-time coefficient, not
 * an integer table). */
/* Where inside the current render call the event being dispatched actually
 * falls; smf.c sets it per event, direct msgs_midi injection leaves it at 0
 * ("now"). Only note-on reads it -- see Voice::start_delay. */
static uint32_t g_event_offset = 0;

void voice_set_event_offset(uint32_t frames) { g_event_offset = frames; }

static double timecents_to_seconds(int32_t tc)
{
    if (tc == (int32_t) 0x80000000)
        return 0.0;
    double t = (double) tc / 65536.0;
    return rt_pow(2.0, t / 1200.0);
}

/* [A:0x198e1],[A:0x1991a]: idiv by 0x7f, so /127 not /128; the term alone is
 * clamped to +-4800 before scaling the duration */
static int32_t scale_tc_by_source(int32_t tc, int16_t depth, int src)
{
    if (tc == (int32_t) 0x80000000 || depth == 0)
        return tc;
    int32_t cents = (int32_t) depth * (int32_t) src / 127;
    if (cents > 4800)
        cents = 4800;
    if (cents < -4800)
        cents = -4800;
    return tc + cents * 65536;
}

/* exp_coef: release traverses the 96 dB scale in the authored time.
 * ponytail: per-sample geometric, not the driver's piecewise-linear segments;
 * upgrade path = the EG consumption code (+0x13c/0x194da/0x19644). */
#ifndef RELEASE_RATE_MULT
#define RELEASE_RATE_MULT 1.0
#endif
/* [F] RELEASE_FLOOR_S: the driver has no floor (0x197dc scales the authored
 * duration by level/1000 and nothing else). Removing it moves the corpus mean
 * -32.48 -> -32.28 dB (37_rac_volume_order -14 dB better; 25_pan_law,
 * 27_gain_curves +5 dB worse), so it stands in for an unrecovered release
 * shape -- INVESTIGATE.adoc#i-release-floor. */
#ifndef RELEASE_FLOOR_S
#define RELEASE_FLOOR_S 0.060
#endif

/* FAST_RELEASE_S = 1/70: `0x19834` divides the device rate (22050, set once
 * at 0x12b40) by 70 and timestamps are sample positions, so the bound is 315
 * samples = 14.29 ms */
#ifndef FAST_RELEASE_S
#define FAST_RELEASE_S (1.0 / 70.0)
#endif

/* AUDIBLE_FLOOR=1e-4, -74 dB below a full-scale voice: currently
 * unreferenced (finish detection reads the bare EG, see ENV_RELEASE) */
#ifndef AUDIBLE_FLOOR
#define AUDIBLE_FLOOR 0.0001
#endif

/* ENV_SPAN_DB=96.0 [A:0x1685e]: the EG level is a position on a 96 dB scale
 * (1000 = 0 dB, 0 = -96 dB); governs decay and release alike */
#define ENV_SPAN_DB 96.0

/* ENV_ATTACK_TABLE_C: the driver's 1/200-quantized Table C attack. Off: no
 * item moved >0.3 dB, mean -32.47 vs -32.48 dB --
 * INVESTIGATE.adoc#i-attack-quantize */
#ifndef ENV_ATTACK_TABLE_C
#define ENV_ATTACK_TABLE_C 0
#endif

/* mult is a rate multiplier, 1.0 shipped; a 2.85 decay fit against probe 04
 * lost on the corpus mean, probe 04 and GENERAL_SERUM. Overridable for
 * sweeps. */
static double exp_coef_scaled(double seconds, double mult)
{
    if (seconds <= 0.0)
        return 0.0;
    double samples = seconds * (double) RENDER_RATE;
    if (samples < 1.0)
        samples = 1.0;
    return rt_pow(10.0, (-(ENV_SPAN_DB / 20.0) * mult) / samples);
}

#define EG2_BLOCK_FRAMES LFO_UPDATE_FRAMES
#define EG2_BLOCK_RATE ((double) RENDER_RATE / (double) EG2_BLOCK_FRAMES)

/* EG2_LINEAR_SEGMENTS: the pitch EG moves linearly in cents, not
 * exponentially (probe 34's spectrogram; -2.73 dB on 26_other_gains) */
#ifndef EG2_LINEAR_SEGMENTS
#define EG2_LINEAR_SEGMENTS 1
#endif

/* "Close enough" threshold for snapping a decaying/releasing envelope level to
 * its target (sustain level or zero) instead of asymptoting forever. */
#define ENV_SNAP_EPSILON 0.0005

static double eg2_coef(double seconds)
{
    if (seconds <= 0.0)
        return 0.0;
    double blocks = seconds * EG2_BLOCK_RATE;
    if (blocks < 1.0)
        blocks = 1.0;
#if EG2_LINEAR_SEGMENTS
    /* linear: fraction of full span per block */
    return 1.0 / blocks;
#else
    return rt_pow(10.0, -5.0 / blocks); /* EG1's exponential calibration */
#endif
}

static int32_t voice_step_eg2(Voice *v)
{
    if (v->eg2_depth_cents == 0.0)
        return 0;
    switch (v->eg2_stage)
    {
        case ENV_ATTACK:
            v->eg2_level += v->eg2_attack_step;
            if (v->eg2_level >= 1.0)
            {
                v->eg2_level = 1.0;
                v->eg2_stage =
                    (v->eg2_decay_coef > 0.0 && v->eg2_decay_coef < 1.0)
                        ? ENV_DECAY
                        : ENV_SUSTAIN;
            }
            break;
        case ENV_DECAY:
#if EG2_LINEAR_SEGMENTS
            v->eg2_level -= v->eg2_decay_coef * (1.0 - v->eg2_sustain_level);
#else
            v->eg2_level =
                v->eg2_sustain_level +
                (v->eg2_level - v->eg2_sustain_level) * v->eg2_decay_coef;
#endif
            if (v->eg2_level - v->eg2_sustain_level < ENV_SNAP_EPSILON)
            {
                v->eg2_level = v->eg2_sustain_level;
                v->eg2_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            v->eg2_level = v->eg2_sustain_level;
            break;
        case ENV_RELEASE:
#if EG2_LINEAR_SEGMENTS
            v->eg2_level -= v->eg2_release_coef;
#else
            v->eg2_level *= v->eg2_release_coef;
#endif
            if (v->eg2_level < ENV_SNAP_EPSILON)
            {
                v->eg2_level = 0.0;
                v->eg2_stage = ENV_IDLE;
            }
            break;
        case ENV_IDLE:
        default:
            return 0;
    }
    double c = v->eg2_depth_cents * v->eg2_level;
    if (c > (double) CENTS_CLAMP)
        c = (double) CENTS_CLAMP;
    if (c < -(double) CENTS_CLAMP)
        c = -(double) CENTS_CLAMP;
    return (int32_t) c;
}

/* LFO output [A:0x18992],[A:0x189e4]: (CC1*cc1_depth/127 + inherent) *
 * sine/100, integer; cents for pitch, hdB for attenuation. 0 until the delay
 * has elapsed, then Table D index ((elapsed - delay) * step >> 12) & 255
 * [A:0x1891e]. step and delay are in BASE_RATE frames, elapsed in
 * RENDER_RATE frames. */
static int32_t voice_lfo(Voice *v, int32_t cc1_depth, int32_t inherent)
{
    uint64_t delay = (uint64_t) v->artic->lfo_delay_frames * RESAMPLE_FACTOR;
    if (v->lfo_elapsed < delay)
        return 0;
    int32_t depth =
        g_channels[v->channel].modulation * cc1_depth / 127 + inherent;
    uint64_t t = v->lfo_elapsed - delay;
    int idx = (int) ((t * v->artic->lfo_step / RESAMPLE_FACTOR) >> 12) & 255;
    return depth * g_table_sine[idx] / 100;
}

static int32_t voice_lfo_cents(Voice *v)
{
    if (!v->artic)
        return 0;
    return voice_lfo(v, v->artic->lfo_pitch_cc1_cents,
                     v->artic->lfo_pitch_inherent_cents);
}

/* [F] RAMP_HORIZON_FRAMES=320 (14.5 ms): a bend glides linearly over a fixed
 * duration, independent of step size (probe 33: 10-90% of 20-24 ms from 200c
 * to 4000c). 320 is the optimum on 33_ramp (-34.04 dB; 256: -31.45, 448:
 * -32.29) but per-rep edge timing reads 448-512 frames, a ~40% disagreement;
 * town.mid's rises and falls glide at different rates --
 * INVESTIGATE.adoc#i-bend-ramp. ponytail: one constant, no ceiling,
 * no magnitude keying, no per-source exemption -- the measurement says duration
 * does not depend on step size, so nothing here should either. */
#ifndef RAMP_HORIZON_FRAMES
#define RAMP_HORIZON_FRAMES (320 * RESAMPLE_FACTOR)
#endif

/* Sizes the held slope so phase_step reaches phase_step_target in exactly
 * `frames` samples; truncation residue is bounded, not corrected, since a zero
 * slope snaps immediately instead of crawling. */
static void ramp_reaim(Voice *v, uint32_t frames)
{
    int64_t d = (int64_t) v->phase_step_target - (int64_t) v->phase_step;
    int32_t slope = (int32_t)((d << 8) / (int64_t) frames); /* Q8 per sample */
    if (slope == 0)
    {
        v->phase_step = v->phase_step_target;
        v->phase_step_ramp_acc = 0;
        v->ramp_left = 0;
        v->phase_step_ramp_step = 0;
        return;
    }
    v->phase_step_ramp_step = slope;
    v->ramp_left = frames;
}

/* render.c's accumulator has no arrival test; this counts
 * every active voice's ramp down and snaps it exactly on expiry, called AFTER
 * render_voice so the expiring chunk is still rendered with it first. */
void voice_ramp_tick(uint32_t frames)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        if (!v->active || !v->ramp_left)
            continue;
        if (v->ramp_left > frames)
        {
            v->ramp_left -= frames;
            continue;
        }
        v->ramp_left = 0;
        v->phase_step = v->phase_step_target;
        v->phase_step_ramp_step = 0;
        v->phase_step_ramp_acc = 0;
    }
}

void voice_update_pitch(Voice *v)
{
    if (!v->active || !v->wave)
        return;
    /* Two-factor pitch: base (note-on, clamped, latched RPN1/RPN2) times live
     * bend/LFO/EG2 (clamped separately), the live delta from 0x195a2 */
    int32_t bend_cents = synth_pitch_bend_cents(v->channel);
    int32_t mod_cents = bend_cents
                        /* `[A:0x189e4]` */
                        + voice_lfo_cents(v)
                        /* `[A:0x15838]` */
                        + voice_step_eg2(v);
    uint64_t raw =
        (uint64_t) v->wave->sample_rate * (uint64_t) v->base_ratio_q12;
    raw = (raw * (uint64_t)(uint32_t) cents_to_ratio_q12(mod_cents)) >> 12;
    uint32_t new_target = (uint32_t)(raw / RENDER_RATE);

    /* Only bend is ramped: probes 32/33 carry no LFO or pitch EG, and
     * routing them through the ramp cost 06_modwheel 3.2 dB and
     * 26_other_gains 1.3 dB */
    int bend_moved = (bend_cents != v->bend_cents_applied);
    if (!bend_moved)
    {
        int64_t moved = (int64_t) v->phase_step +
                        ((int64_t) new_target - (int64_t) v->phase_step_target);
        v->phase_step = (uint32_t)(moved < 0 ? 0 : moved);
    }
    v->bend_cents_applied = bend_cents;
    v->phase_step_target = new_target;

    /* Bend ramp re-aimed instantly on target move, over a fixed horizon, not a
     * shared clock's remainder: a shared clock made identical gestures glide at
     * up to 2x different speeds (town.mid) */
    if (bend_moved)
        ramp_reaim(v, RAMP_HORIZON_FRAMES);
}

/* Per-voice LFO clock; sub-chunked so a held note with no MIDI traffic still
 * oscillates */
void voices_advance_lfo(uint32_t frames)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (g_voices[i].active)
            g_voices[i].lfo_elapsed += frames;
    }
}

/* GAIN_CEILING = 4095/8192: TABLE[0] of the hdB->gain table (0x1bfc0) is the
 * per-voice ceiling, ~0.5 of computed unity (pmulhw on a signed lane) */
#define GAIN_CEILING (4095.0 / 8192.0)

/* Gain law: re-read live every block (mirrors
 * voice_update_pitch) so CC7/CC10/CC11/master-volume changes reach a held note;
 * atten_const_hdb (velocity+region attenuation) is fixed at note-on. */
/* Pan law [A:0x19c12-0x19c2a]: one g_table_lin read forward/reversed, summed
 * into the attenuation before the clamp; the measured "unity at centre" is
 * that clamp, not the law */
static double atten_to_gain(int32_t atten_hdb)
{
    /* 0x18e0b: signed index, 0.1dB steps */
    int32_t i = atten_hdb / 10;
    if (i > 0)
        i = 0; /* 0x18dea: clamp(.., -1000, 0) */
    if (i < -1000)
        i = -1000;
    return (double) g_table_dbamp[i + 1000] / 8192.0;
}

void voice_update_gain(Voice *v)
{
    if (!v->active)
        return;
    int32_t chan_vol = g_table_vel[g_channels[v->channel].volume];
    int32_t expr = g_table_vel[g_channels[v->channel].expression];
    int32_t atten_hdb = g_master_vol_hdb + chan_vol + expr +
                        v->atten_const_hdb +
                        voice_lfo(v, v->artic->lfo_atten_cc1_hdb,
                                  v->artic->lfo_atten_inherent_hdb);

    int pan = (int) g_channels[v->channel].pan + v->artic->pan_cb;
    if (pan < 0)
        pan = 0;
    if (pan > 127)
        pan = 127;
    v->gain_l_target = atten_to_gain(atten_hdb + g_table_lin[127 - pan]);
    v->gain_r_target = atten_to_gain(atten_hdb + g_table_lin[pan]);
}

void voices_update_modulation(void)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (g_voices[i].active)
        {
            voice_update_pitch(&g_voices[i]);
            voice_update_gain(&g_voices[i]);
        }
    }
}

static Voice *find_free_primary(void)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (!g_voices[i].active && !g_voices[i].in_reserve)
            return &g_voices[i];
    }
    return 0;
}

static Voice *find_free_reserve(void)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (!g_voices[i].active && g_voices[i].in_reserve)
            return &g_voices[i];
    }
    return 0;
}

/* 0x12426 analogue -- top-up Branch B's own comparator only: skips marked
 * voices; released beats held from both sides; then lower level; then
 * oldest */
static Voice *find_steal_candidate_symmetric(void)
{
    Voice *best = 0;
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *cand = &g_voices[i];
        if (!cand->active || cand->fast_release_committed)
            continue;
        if (!best)
        {
            best = cand;
            continue;
        }
        int cand_released = !cand->held;
        int best_released = !best->held;
        if (cand_released != best_released)
        {
            if (cand_released)
                best = cand;
            continue;
        }
        if (cand_released)
        {
            if (cand->env_level < best->env_level)
                best = cand;
            continue;
        }
        if (cand->age < best->age)
            best = cand;
    }
    return best;
}

/* 0x124a8 analogue -- note-on's forced-steal fallback only: ignores the mark,
 * and an older held candidate displaces a released best */
static Voice *find_steal_candidate_asymmetric(void)
{
    Voice *best = 0;
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *cand = &g_voices[i];
        if (!cand->active)
            continue;
        if (!best)
        {
            best = cand;
            continue;
        }
        if (cand->held)
        {
            if (cand->age < best->age)
                best = cand;
        }
        else if (best->held)
        {
            best = cand;
        }
        else if (cand->env_level < best->env_level)
        {
            best = cand;
        }
    }
    return best;
}

static void start_release(Voice *v, int fast)
{
    /* start_release: +0x138 set unconditionally (0x19ab6); this engine already
     * collapses ScheduleFastRelease's two internal paths into one */
    if (fast)
        v->fast_release_committed = 1;
    v->held = 0;
    v->sustain_deferred = 0;
    v->env_stage = ENV_RELEASE;
    double authored_s = timecents_to_seconds(v->artic->eg1_release_tc);
    double rel_s = authored_s;
    if (fast)
    {
        double lvl =
            1.0 + 20.0 * rt_log10(v->env_level > 1e-12 ? v->env_level : 1e-12) /
                      ENV_SPAN_DB;
        if (lvl > 1.0)
            lvl = 1.0;
        rel_s = (lvl > 1e-6) ? FAST_RELEASE_S / lvl : authored_s;
        if (rel_s > authored_s)
            rel_s = authored_s;
    }
    if (!fast && rel_s < RELEASE_FLOOR_S)
        rel_s = RELEASE_FLOOR_S;
    v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
    /* EG2 releases on both note-off paths; the fast-release clamp is
     * amplitude-only (0x19b16 vs 0x19b28) */
    if (v->eg2_stage != ENV_IDLE)
        v->eg2_stage = ENV_RELEASE;
}

/* [F] TOPUP_INTERVAL_FRAMES: the driver tops up once per service call
 * (0x12be7), whose length the audio stack picks. A wall-clock tick stands in.
 * Probes 20/21 need >=1024 frames (512 leaves 43 of 48 voices, 64 leaves 1);
 * HueArme-Weekend is flat from 512 up. 2048 ships --
 * INVESTIGATE.adoc#i-envelope-cadence. */
#ifndef TOPUP_INTERVAL_FRAMES
#define TOPUP_INTERVAL_FRAMES (2048 * RESAMPLE_FACTOR) /* ~92.9ms */
#endif

static void topup_reserve(void)
{
    int reserve_free = 0;
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (!g_voices[i].active && g_voices[i].in_reserve)
            reserve_free++;
    }
    int need = NUM_RESERVE - reserve_free;
    if (need <= 0)
        return;

    for (int i = 0; i < NUM_VOICES && need > 0; i++)
    {
        Voice *v = &g_voices[i];
        if (!v->active && !v->in_reserve)
        {
            v->in_reserve = 1;
            need--;
        }
    }

    while (need > 0)
    {
        Voice *victim = find_steal_candidate_symmetric();
        /* every active voice already committed to a fast release -- nothing
         * left to mark this call; NOT a leak (see voice_note_on's own final
         * fallback, which never reads this gate and can still repurpose a
         * draining voice immediately). */
        if (!victim)
            break;
        start_release(victim, 1);
        need--;
    }
}

void voice_topup_tick(uint32_t frames)
{
    g_topup_accum += frames;
    if (g_topup_accum < TOPUP_INTERVAL_FRAMES)
        return;
    /* no catch-up burst: one top-up
       per tick however coarsely render.c happens to slice its chunks. */
    g_topup_accum %= TOPUP_INTERVAL_FRAMES;
    topup_reserve();
}

void voice_note_on(int channel, int note, int velocity)
{
    if (channel < 0 || channel >= 16)
        return;
    if (note < 0 || note > 127)
        note = note < 0 ? 0 : 127;
    if (velocity <= 0)
    {
        voice_note_off(channel, note);
        return;
    }
    if (velocity > 127)
        velocity = 127;

    /* Mono: every held voice on the channel gets an ordinary note-off before
     * the lookup, pedal or not (0x12ddd-0x12e0e) */
    if (g_channels[channel].mono_mode)
    {
        for (int i = 0; i < NUM_VOICES; i++)
        {
            Voice *m = &g_voices[i];
            if (m->active && m->held && m->channel == channel)
                start_release(m, 0);
        }
    }

    uint32_t locale;
    Region *r =
        dls_find_region(synth_channel_locale(channel), (uint8_t) note, &locale);
    if (!r)
        return; /* silently dropped (0x1302a) */

    /* Same-note retrigger, held or released, unless the region is
     * self-non-exclusive (0x12e8c-0x12ec6) */
    if (!r->self_nonexclusive)
    {
        for (int i = 0; i < NUM_VOICES; i++)
        {
            Voice *t = &g_voices[i];
            if (t->active && t->channel == channel && t->note == note)
                start_release(t, 1);
        }
    }

    /* primary before reserve, forced steal (asymmetric comparator) only if
     * both free lists are empty (0x12ed1-0x12ef5); always finds a
     * victim since note_on is never reached with zero active voices. */
    Voice *v = find_free_primary();
    if (!v)
        v = find_free_reserve();
    if (!v)
        v = find_steal_candidate_asymmetric();
    if (!v)
        return;

    /* Key-group choke runs after allocation, so a voice just stolen for this
     * note is not also choked; scope is (channel, key group, locale after
     * fallback) (0x12efc-0x12f39) */
    if (r->key_group != 0)
    {
        for (int i = 0; i < NUM_VOICES; i++)
        {
            Voice *k = &g_voices[i];
            if (k != v && k->active && k->channel == channel &&
                k->key_group == r->key_group && k->locale == locale)
                start_release(k, 1);
        }
    }

    /* recycling always targets primary only (0x130f4) -- reset here so
     * whatever tier this voice occupied while free (or, for a stolen voice,
     * whatever it was tagged before going active) is irrelevant the moment it's
     * active again. */
    v->in_reserve = 0;
    /* cleared only by SetupNote (0x19d29) */
    v->fast_release_committed = 0;

    v->active = 1;
    v->channel = channel;
    v->note = note;
    v->locale = locale;
    v->key_group = r->key_group;
    v->wave = r->wave;
    v->artic = r->artic;
    v->age = ++g_voice_age_counter;
    v->held = 1;
    v->sustain_deferred = 0;

    /* base_cents excludes bend (re-read live); GS scale tuning by semitone
     * class and RPN1 always, RPN2 skipped on rhythm parts (0x12f54-0x12f78) */
    v->base_cents = (int) r->fine_tune + (note - (int) r->unity_note) * 100 +
                    g_channels[channel].scale_tuning[note % 12] +
                    (g_channels[channel].is_rhythm
                         ? 0
                         : g_channels[channel].rpn2_coarse_cents) +
                    g_channels[channel].rpn1_fine_cents;
    /* Latched here and clamped once, `[M: probe 30]` -- see voice_update_pitch.
     */
    v->base_ratio_q12 = (uint32_t) cents_to_ratio_q12(v->base_cents);

    /* LFO clock starts at note-on (0x19c48); advanced only by
     * voices_advance_lfo */
    v->lfo_elapsed = 0;

    voice_update_pitch(v);
    /* New note: plays at the true target from sample 0, no glide-in from
     * whatever phase_step this recycled Voice slot last held -- the ramp is for
     * modulation on an already-sounding voice, not note-on. */
    v->phase_step = v->phase_step_target;
    v->phase_step_ramp_step = 0;
    v->phase_step_ramp_acc = 0;
    v->ramp_left = 0;
    v->bend_cents_applied = synth_pitch_bend_cents(channel);
    v->phase_pos = 0;

    int no_loop = r->no_loop;
    if (no_loop)
    {
        v->loop_start_s = 0;
        v->loop_len_s = 0;
        v->sample_end_s = v->wave->sample_count;
    }
    else
    {
        v->loop_start_s = r->loop_start;
        v->sample_end_s = r->loop_end;
        v->loop_len_s = r->loop_end - r->loop_start;
        if (v->loop_len_s <= 0)
        {
            v->loop_len_s = 0;
            v->sample_end_s = v->wave->sample_count;
        }
    }

    /* Volume law: velocity/region attenuation fixed at note-on into
     * atten_const_hdb (0x19b94-0x19bcd); CC7/CC11/CC10/master-vol recomputed
     * live by voice_update_gain. depth -9600 = full velocity effect. */
    int32_t vel_atten = g_table_vel[velocity];
    int32_t depth = r->artic->vel_to_atten_depth;
    int32_t scaled = (int32_t)(((int64_t) vel_atten * depth) / -9600);
    /* Region's own wsmp attenuation only, not a separate wave-level term.
     * [A:0x19bcd]: +1200 folded into the sum moves the gain clamp's knee 12 dB
     * earlier, not a flat boost; re-anchoring it with the ceiling was a
     * uniform ~3 dB overshoot and lost on the corpus. */
    v->atten_const_hdb = scaled + (int32_t) r->attenuation_hdb + 1200;

    /* Pan: region's pan_cb fixed, channel CC10 combined live in
     * voice_update_gain; rising CC10 attenuates the left (probe 07) */
    voice_update_gain(v);
    v->gain_l = v->gain_l_target;
    v->gain_r = v->gain_r_target;
    v->amp_left = 0;
    v->amp_step_l = v->amp_step_r = 0.0;
    v->amp_retire = 0;
    v->start_delay = g_event_offset;
    /* amp_l/amp_r primed from envelope start level, matches driver's
     * nsamples==0 mixer priming [A:0x18fba]; at zero, every instant-attack
     * patch faded in over a segment (probe 14: 5x too quiet at 4 ms) */

    /* Envelope (EG1, amplitude) */
    double attack_s = timecents_to_seconds(scale_tc_by_source(
        r->artic->eg1_attack_tc, r->artic->eg1_attack_vel_tc, velocity));
    double decay_s = timecents_to_seconds(scale_tc_by_source(
        r->artic->eg1_decay_tc, r->artic->eg1_decay_kf_tc, note));
    /* [A:0x18d3d-0x18d49/0x18b4a-0x18b8b]: sustain permille is
     * a progress-domain marker on the 96dB scale at consumption time, not the
     * on-disk linear-amplitude storage reading. */
    int32_t sustain_permille = r->artic->eg1_sustain_permille;
    if (sustain_permille < 0)
        sustain_permille = 0;
    if (sustain_permille > 1000)
        sustain_permille = 1000;
    double sustain_hundredths_db = (double) sustain_permille * 9.6 - 9600.0;
    v->env_sustain_level = rt_pow(10.0, sustain_hundredths_db / 100.0 / 20.0);
    if (v->env_sustain_level < 0.0)
        v->env_sustain_level = 0.0;
    if (v->env_sustain_level > 1.0)
        v->env_sustain_level = 1.0;
    /* [A:0x19968-0x1997e]: note-setup rescales decay duration
     * to decay*(1000-sustainPermille)/1000 before the decay segment ever runs.
     */
    double decay_to_sustain_s =
        decay_s * (double) (1000 - sustain_permille) / 1000.0;
    /* decay is a geometric ramp (env_level *= coef)
     * from 1.0, not an asymptotic approach; env_decay_samples_left is the exact
     * countdown that snaps it to env_sustain_level. */
    int32_t decay_samples =
        (int32_t)(decay_to_sustain_s * (double) RENDER_RATE + 0.5);
    if (decay_samples > 0)
    {
        v->env_decay_samples_left = decay_samples;
        v->env_decay_coef =
            rt_pow(v->env_sustain_level, 1.0 / (double) decay_samples);
    }
    else
    {
        /* Rescaled duration is ~0 samples (e.g. sustain_permille at/near 1000)
         * -- go straight to sustain below, matching how decay_s<=0 already
         * worked pre-fix. */
        v->env_decay_samples_left = 0;
        v->env_decay_coef = 1.0;
    }
    if (attack_s <= 0.0)
    {
        v->env_level = 1.0;
        v->env_stage = (decay_samples > 0) ? ENV_DECAY : ENV_SUSTAIN;
    }
    else
    {
        v->env_level = 0.0;
        v->env_stage = ENV_ATTACK;
        v->env_attack_step = 1.0 / (attack_s * (double) RENDER_RATE);
        v->env_attack_elapsed = 0;
        v->env_attack_samples =
            (int32_t)(attack_s * (double) RENDER_RATE + 0.5);
        if (v->env_attack_samples < 1)
            v->env_attack_samples = 1;
    }

    /* EG2 [A:0x15838]: same segment structure as EG1 but scales
     * eg2_to_pitch_cents; zero depth (the default) leaves it inert */
    v->eg2_depth_cents = (double) r->artic->eg2_to_pitch_cents;
    v->eg2_sustain_level = (double) r->artic->eg2_sustain_permille / 1000.0;
    if (v->eg2_sustain_level < 0.0)
        v->eg2_sustain_level = 0.0;
    if (v->eg2_sustain_level > 1.0)
        v->eg2_sustain_level = 1.0;
    double eg2_atk_s = timecents_to_seconds(r->artic->eg2_attack_tc);
    double eg2_dec_s = timecents_to_seconds(scale_tc_by_source(
        r->artic->eg2_decay_tc, r->artic->eg2_decay_kf_tc, note));
    if (eg2_atk_s <= 0.0)
    {
        v->eg2_level = 1.0;
        v->eg2_stage = (eg2_dec_s > 0.0) ? ENV_DECAY : ENV_SUSTAIN;
    }
    else
    {
        v->eg2_level = 0.0;
        v->eg2_stage = ENV_ATTACK;
        v->eg2_attack_step = 1.0 / (eg2_atk_s * EG2_BLOCK_RATE);
    }
    v->eg2_decay_coef = eg2_coef(eg2_dec_s);
    v->eg2_release_coef =
        eg2_coef(timecents_to_seconds(r->artic->eg2_release_tc));

    /* Prime the amplitude ramp (see the amp_left reset above). env_level is
     * final only now: 1.0 for an instant attack, 0.0 for a real one. */
    v->amp_l = v->env_level * v->gain_l;
    v->amp_r = v->env_level * v->gain_r;
}

void voice_note_off(int channel, int note)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->note == note && v->held)
        {
            if (g_channels[channel].sustain != 0)
            {
                v->sustain_deferred = 1;
            }
            else
            {
                start_release(v, 0);
            }
        }
    }
}

void voice_all_sound_off(int channel)
{
    /* CC120: bypasses the sustain hold entirely (0x12cd6-0x12cf5). */
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->held)
        {
            start_release(v, 0);
        }
    }
}

void voice_all_notes_off(int channel)
{
    /* CC123: honours the sustain hold (0x12c78-0x12cb9). */
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->held)
        {
            if (g_channels[channel].sustain != 0)
                v->sustain_deferred = 1;
            else
                start_release(v, 0);
        }
    }
}

void voice_sustain_lift(int channel)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->sustain_deferred)
        {
            start_release(v, 0);
        }
    }
}

int voice_any_active(void)
{
    for (int i = 0; i < NUM_VOICES; i++)
    {
        if (g_voices[i].active)
            return 1;
    }
    return 0;
}

/* Frames until EG1's current segment ends (the driver's "next change" term,
 * [A:0x19490]); sustain/release return ENV_NO_CHANGE since neither has a
 * scheduled end. */
uint32_t voice_env_frames_to_change(const Voice *v)
{
    switch (v->env_stage)
    {
        case ENV_ATTACK:
        {
            if (v->env_attack_step <= 0.0)
                return 1;
            double n = (1.0 - v->env_level) / v->env_attack_step;
            if (n < 1.0)
                return 1;
            if (n >= (double) ENV_NO_CHANGE)
                return ENV_NO_CHANGE;
            return (uint32_t) n + 1;
        }
        case ENV_DECAY:
            return v->env_decay_samples_left > 0
                       ? (uint32_t) v->env_decay_samples_left
                       : 1;
        default:
            return ENV_NO_CHANGE;
    }
}

double voice_step_envelope(Voice *v)
{
    switch (v->env_stage)
    {
        case ENV_ATTACK:
#if ENV_ATTACK_TABLE_C
            v->env_attack_elapsed++;
            if (v->env_attack_elapsed >= v->env_attack_samples)
            {
                /* [A:0x18ac0-0x18ad1]: elapsed>=attackDuration leaves Table C
                 * entirely, so land on exactly 1.0 rather than t[200]=999 */
                v->env_level = 1.0;
                v->env_stage =
                    (v->env_decay_samples_left > 0) ? ENV_DECAY : ENV_SUSTAIN;
            }
            else
            {
                int32_t permille = (int32_t)((int64_t) v->env_attack_elapsed *
                                             1000 / v->env_attack_samples);
                int32_t idx = permille / 5; /* trunc, 0x18b0a-0x18b10 */
                if (idx > 200)
                    idx = 200;
                v->env_level = rt_pow(
                    10.0, (((double) g_table_envshape[idx] / 1000.0) - 1.0) *
                              ENV_SPAN_DB / 20.0);
            }
#else
            v->env_level += v->env_attack_step;
            if (v->env_level >= 1.0)
            {
                v->env_level = 1.0;
                v->env_stage =
                    (v->env_decay_samples_left > 0) ? ENV_DECAY : ENV_SUSTAIN;
            }
#endif
            break;
        case ENV_DECAY:
            /* geometric ramp, same mechanism as
             * ENV_RELEASE; env_decay_samples_left is the exact countdown,
             * avoiding both float drift and a threshold test that doesn't
             * generalize to a nonzero target. */
            v->env_level *= v->env_decay_coef;
            if (--v->env_decay_samples_left <= 0)
            {
                v->env_level = v->env_sustain_level;
                v->env_decay_samples_left = 0;
                v->env_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            v->env_level = v->env_sustain_level;
            break;
        case ENV_RELEASE:
            v->env_level *= v->env_release_coef;
            /* The driver's -80 dB finish floor [A:0x19733] was tried here
             * instead of -66 dB and measured no benefit. +0x13c is the bare
             * EG, not gain-inclusive [A:0x194da]. */
            if (v->env_level < 0.0005)
            {
                v->env_level = 0.0;
                v->active = 0;
                v->env_stage = ENV_IDLE;
            }
            break;
        default:
            v->env_level = 0.0;
            v->active = 0;
            break;
    }
    return v->env_level;
}
