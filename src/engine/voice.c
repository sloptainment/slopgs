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
        v->release_at = VOICE_NO_RELEASE;
        v->starting = 0;
        v->atten_const_hdb = 0;
        v->env_stage = ENV_IDLE;
        v->env_level = 0.0;
        v->env_decay_samples_left = 0;
        v->eg2_off = -1;
        v->eg2_t = 0;
        v->eg2_cents = 0;
        v->step_left = v->step_inc = v->step_acc = 0;
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
uint64_t g_now;

/* Where inside the current render call the event being dispatched actually
 * falls; smf.c sets it per event, direct msgs_midi injection leaves it at 0
 * ("now"). Note-on, note-off and the controller queues read it. */
static uint32_t g_event_offset = 0;

void voice_set_event_offset(uint32_t frames) { g_event_offset = frames; }

uint64_t voice_event_time(void) { return g_now + g_event_offset; }

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

/* ENV_ATTACK_TABLE_C: the driver's 1/200-quantized Table C attack
 * [A:0x18ad3]-[A:0x18b24]. Closer to the oracle in the waveform (09_chorus
 * 0-0.1 s -34.0 -> -41.6 dB); the spectral corpus mean reads 0.02 dB worse
 * (-39.58 -> -39.56), a floor effect of that metric */
#ifndef ENV_ATTACK_TABLE_C
#define ENV_ATTACK_TABLE_C 1
#endif

/* mult is a rate multiplier, 1.0 shipped; a 2.85 decay fit against probe 04
 * lost on the corpus mean, probe 04 and GENERAL_SERUM. Overridable for
 * sweeps. */
static double exp_coef_scaled(double seconds, double mult)
{
    if (seconds <= 0.0)
        return 0.0;
    double samples = seconds * (double) BASE_RATE;
    if (samples < 1.0)
        samples = 1.0;
    return rt_pow(10.0, (-(ENV_SPAN_DB / 20.0) * mult) / samples);
}

/* [A:0x18a7a], flag 0: linear in permille. Release falls from the level at
 * note-off (the t == off evaluation) to 0 over eg2_rel. */
static int32_t eg2_permille(const Voice *v, int64_t t)
{
    if (v->eg2_off >= 0 && t > v->eg2_off)
    {
        int64_t r = t - v->eg2_off;
        if (r >= (int64_t) v->eg2_rel)
            return 0;
        return (int32_t) (eg2_permille(v, v->eg2_off) *
                          ((int64_t) v->eg2_rel - r) / (int64_t) v->eg2_rel);
    }
    if (t < (int64_t) v->eg2_atk)
        return (int32_t) (t * 1000 / (int64_t) v->eg2_atk);
    t -= v->eg2_atk;
    if (t < (int64_t) v->eg2_dec)
        return (int32_t) (1000 -
                          (1000 - v->eg2_sus) * t / (int64_t) v->eg2_dec);
    return v->eg2_sus;
}

/* [A:0x18d58] depth * permille / 1000, clamped like the other pitch terms */
int32_t voice_eg2_cents(const Voice *v, uint64_t t)
{
    if (v->eg2_depth_cents == 0.0)
        return 0;
    int32_t c =
        (int32_t) v->eg2_depth_cents * eg2_permille(v, (int64_t) t) / 1000;
    if (c > CENTS_CLAMP)
        c = CENTS_CLAMP;
    if (c < -CENTS_CLAMP)
        c = -CENTS_CLAMP;
    return c;
}

/* a decay or release is read at its quarter, half and end, each only if 100
 * frames remain before it [A:0x18b98-0x18bf6] */
static uint32_t eg2_span_bound(int64_t span, int64_t t)
{
    if (t < (span >> 2) - 100)
        return (uint32_t) ((span >> 2) - t);
    if (t < (span >> 1) - 100)
        return (uint32_t) ((span >> 1) - t);
    return (uint32_t) (span - t);
}

uint32_t voice_eg2_frames_to_change(const Voice *v, uint64_t t_)
{
    int64_t t = (int64_t) t_;
    if (v->eg2_off >= 0 && t > v->eg2_off)
    {
        int64_t r = t - v->eg2_off;
        return r < (int64_t) v->eg2_rel ? eg2_span_bound(v->eg2_rel, r)
                                        : ENV_NO_CHANGE;
    }
    if (t < (int64_t) v->eg2_atk)
        return (uint32_t) (v->eg2_atk - t);
    t -= v->eg2_atk;
    if (t < (int64_t) v->eg2_dec)
        return eg2_span_bound(v->eg2_dec, t);
    // 0xac44
    return 44100;
}

uint32_t voice_eg2_render_frames_to_change(const Voice *v)
{
    if (v->eg2_depth_cents == 0.0)
        return ENV_NO_CHANGE;
    uint32_t n = voice_eg2_frames_to_change(v, v->eg2_t / RESAMPLE_FACTOR);
    if (n > ENV_NO_CHANGE / RESAMPLE_FACTOR)
        return ENV_NO_CHANGE;
    return n * RESAMPLE_FACTOR - (uint32_t) (v->eg2_t % RESAMPLE_FACTOR);
}

void voice_eg2_segment(Voice *v, uint32_t frames)
{
    v->eg2_cents = voice_eg2_cents(v, (v->eg2_t + frames) / RESAMPLE_FACTOR);
}

/* LFO output [A:0x18992],[A:0x189e4]: (CC1*cc1_depth/127 + inherent) *
 * sine/100, integer; cents for pitch, hdB for attenuation. 0 until the delay
 * has elapsed, then Table D index ((elapsed - delay) * step >> 12) & 255
 * [A:0x1891e]. step and delay are in BASE_RATE frames, elapsed in
 * RENDER_RATE frames; CC1 as due at time t. */
static int32_t voice_lfo(Voice *v, int32_t cc1_depth, int32_t inherent,
                         uint64_t t)
{
    uint64_t delay = (uint64_t) v->artic->lfo_delay_frames * RESAMPLE_FACTOR;
    if (v->lfo_elapsed < delay)
        return 0;
    int32_t depth =
        (int32_t) synth_ctl_at(v->channel, CTL_MOD, t) * cc1_depth / 127 +
        inherent;
    uint64_t e = v->lfo_elapsed - delay;
    int idx = (int) ((e * v->artic->lfo_step / RESAMPLE_FACTOR) >> 12) & 255;
    return depth * g_table_sine[idx] / 100;
}

static int32_t voice_lfo_cents(Voice *v, uint64_t t)
{
    if (!v->artic)
        return 0;
    return voice_lfo(v, v->artic->lfo_pitch_cc1_cents,
                     v->artic->lfo_pitch_inherent_cents, t);
}

/* the LFO's segment bound [A:0x1891e]: the delay left, else 1/8 cycle,
 * 0x20000/step (0x188fc), or 44100 when all four depths are 0 (0x188f0) */
static uint32_t lfo_frames_to_change(const Voice *v)
{
    const Artic *a = v->artic;
    uint64_t delay = (uint64_t) a->lfo_delay_frames * RESAMPLE_FACTOR;
    if (v->eg2_t < delay)
        return (uint32_t) (delay - v->eg2_t);
    if ((!a->lfo_pitch_cc1_cents && !a->lfo_pitch_inherent_cents &&
         !a->lfo_atten_cc1_hdb && !a->lfo_atten_inherent_hdb) ||
        !a->lfo_step)
        return 44100 * RESAMPLE_FACTOR;
    return 0x20000 / a->lfo_step * RESAMPLE_FACTOR;
}

/* 0x196f0 resets the bound to the device cap, (rate+19)/20 (0x12901), and
 * the LFO, EG1 and EG2 lower it (0x194cc, 0x1950a, 0x195dd, 0x19619) */
uint32_t voice_segment_frames(const Voice *v)
{
    uint32_t seg = (BASE_RATE + 19) / 20 * RESAMPLE_FACTOR;
    uint32_t n = lfo_frames_to_change(v);
    if (n < seg)
        seg = n;
    n = voice_env_frames_to_change(v);
    if (n < seg)
        seg = n;
    n = voice_eg2_render_frames_to_change(v);
    if (n < seg)
        seg = n;
    return seg;
}

void voice_update_pitch_at(Voice *v, uint64_t t)
{
    if (!v->active || !v->wave)
        return;
    /* Two-factor pitch: base (note-on, clamped, latched RPN1/RPN2) times live
     * bend/LFO/EG2 (clamped separately), the live delta from 0x195a2 */
    int32_t mod_cents = synth_pitch_bend_cents_at(v->channel, t)
                        /* `[A:0x189e4]` */
                        + voice_lfo_cents(v, t)
                        /* `[A:0x15838]` */
                        + (v->eg2_depth_cents == 0.0 ? 0 : v->eg2_cents);
    uint64_t raw =
        (uint64_t) v->wave->sample_rate * (uint64_t) v->base_ratio_q12;
    raw = (raw * (uint64_t)(uint32_t) cents_to_ratio_q12(mod_cents)) >> 12;
    v->mod_cents = mod_cents;
    v->phase_step_target = (uint32_t) (raw / BASE_RATE);
}

void voice_update_pitch(Voice *v) { voice_update_pitch_at(v, UINT64_MAX); }

/* GAIN_CEILING = 4095/8192: TABLE[0] of the hdB->gain table (0x1bfc0) is the
 * per-voice ceiling, ~0.5 of computed unity (pmulhw on a signed lane) */
#define GAIN_CEILING (4095.0 / 8192.0)

/* Gain law: read at every segment end (mirrors voice_update_pitch_at) so
 * CC7/CC10/CC11 changes reach a held note;
 * atten_const_hdb (master volume+velocity+region attenuation) is fixed at
 * note-on. */
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

void voice_update_gain_at(Voice *v, uint64_t t)
{
    if (!v->active)
        return;
    int32_t chan_vol = g_table_vel[synth_ctl_at(v->channel, CTL_VOL, t)];
    int32_t expr = g_table_vel[synth_ctl_at(v->channel, CTL_EXPR, t)];
    int32_t atten_hdb = chan_vol + expr + v->atten_const_hdb +
                        voice_lfo(v, v->artic->lfo_atten_cc1_hdb,
                                  v->artic->lfo_atten_inherent_hdb, t);

    int pan = (int) synth_ctl_at(v->channel, CTL_PAN, t) + v->artic->pan_cb;
    if (pan < 0)
        pan = 0;
    if (pan > 127)
        pan = 127;
    v->atten_l_hdb = atten_hdb + g_table_lin[127 - pan];
    v->atten_r_hdb = atten_hdb + g_table_lin[pan];
    v->gain_l_target = atten_to_gain(v->atten_l_hdb);
    v->gain_r_target = atten_to_gain(v->atten_r_hdb);
}

void voice_update_gain(Voice *v) { voice_update_gain_at(v, UINT64_MAX); }

int32_t voice_env_hdb(double env_level)
{
    // EG1 term of 0x18d28: -9600 at level 0, 0 at full scale
    if (env_level <= 0.0)
        return -9600;
    double e = 2000.0 * rt_log10(env_level);
    return e > -9600.0 ? (int32_t) e : -9600;
}

double voice_amp(int32_t atten_hdb, int32_t env_hdb)
{
    return atten_to_gain(atten_hdb + env_hdb);
}

uint32_t voice_stair_frames(uint32_t seg, int32_t d_att, int32_t d_cents)
{
    int32_t m = 2 * d_cents > d_att ? 2 * d_cents : d_att;
    uint32_t k = 512;
    if (m >> 1)
    {
        k = seg * 8 / (uint32_t) (m >> 1);
        if (k > 512)
            k = 512;
        if (k < 1)
            k = 1;
    }
    return (k + 3) & ~3u;
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

/* EG1 and EG2 enter release at EG2 frame eg2_t */
static void release_egs(Voice *v, int fast, uint64_t eg2_t)
{
    v->release_at = VOICE_NO_RELEASE;
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
    v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
    /* EG2 releases on both note-off paths; the fast-release clamp is
     * amplitude-only (0x19b16 vs 0x19b28) */
    if (v->eg2_off < 0)
        v->eg2_off = (int64_t) (eg2_t / RESAMPLE_FACTOR);
}

void voice_release_due(Voice *v, uint64_t eg2_t)
{
    release_egs(v, v->release_fast, eg2_t);
}

static void start_release(Voice *v, int fast)
{
    /* start_release: +0x138 set unconditionally (0x19ab6); this engine already
     * collapses ScheduleFastRelease's two internal paths into one */
    if (fast)
        v->fast_release_committed = 1;
    v->held = 0;
    v->sustain_deferred = 0;
    release_egs(v, fast, v->eg2_t);
}

/* an ordinary release from a dispatched event: the voice stops being held
 * now (0x19a7e) and its EGs release at the event's time (0x19a6f) */
static void release_at_event(Voice *v)
{
    if (!g_event_offset)
    {
        start_release(v, 0);
        return;
    }
    v->held = 0;
    v->sustain_deferred = 0;
    v->release_at = voice_event_time();
    v->release_fast = 0;
}

/* ScheduleFastRelease from a note-on: a voice still held, by key or pedal,
 * releases fast at the event's time (0x19ae5-0x19b11); one whose note-off
 * is still pending keeps that time (0x19ac7-0x19ade) */
static void fast_release_at_event(Voice *v)
{
    if (v->release_at != VOICE_NO_RELEASE)
    {
        v->fast_release_committed = 1;
        v->release_fast = 1;
        return;
    }
    if (!v->held || !g_event_offset)
    {
        start_release(v, 1);
        return;
    }
    v->fast_release_committed = 1;
    v->held = 0;
    v->sustain_deferred = 0;
    v->release_at = voice_event_time();
    v->release_fast = 1;
}

/* [F] TOPUP_INTERVAL_FRAMES: the driver tops up once per 1024-frame service
 * call (0x12be7, 0x181fa). A wall-clock tick stands in. Probes 20/21 need
 * >=1024 frames (512 leaves 43 of 48 voices, 64 leaves 1); 1024 scores
 * -55.29 against 2048's -55.31 dB mean, so 2048 ships --
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
                release_at_event(m);
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
                fast_release_at_event(t);
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
                fast_release_at_event(k);
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

    /* LFO clock starts at note-on (0x19c48); render_voice sets it to each
     * segment end */
    v->lfo_elapsed = 0;
    // EG2 reaches the pitch at the first segment (render.c), not at note-on
    v->eg2_off = -1;
    v->eg2_t = 0;
    v->eg2_cents = 0;
    v->step_left = v->step_inc = v->step_acc = 0;
    v->release_at = VOICE_NO_RELEASE;

    /* evaluated at the note's own time */
    uint64_t t0 = voice_event_time();
    voice_update_pitch_at(v, t0);
    /* New note: plays at the true target from sample 0, no glide-in from
     * whatever phase_step this recycled Voice slot last held */
    v->phase_step = v->phase_step_target;
    /* the driver zeroes its previous-segment values at note-on (0x18f23) */
    v->prev_att_l = v->prev_att_r = v->prev_mod_cents = 0;
    v->stair_left = 0;
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
     * atten_const_hdb (0x19b94-0x19bcd); CC7/CC11/CC10 recomputed live by
     * voice_update_gain. Master Volume is read here only: its one consumer
     * is the note-on call (0x12fa0, summed at 0x19b86), so a later change
     * never reaches a held note. depth -9600 = full velocity effect. */
    int32_t vel_atten = g_table_vel[velocity];
    int32_t depth = r->artic->vel_to_atten_depth;
    int32_t scaled = (int32_t)(((int64_t) vel_atten * depth) / -9600);
    /* Region's own wsmp attenuation only, not a separate wave-level term.
     * [A:0x19bcd]: +1200 folded into the sum moves the gain clamp's knee 12 dB
     * earlier, not a flat boost; re-anchoring it with the ceiling was a
     * uniform ~3 dB overshoot and lost on the corpus. */
    v->atten_const_hdb =
        g_master_vol_hdb + scaled + (int32_t) r->attenuation_hdb + 1200;

    /* Pan: region's pan_cb fixed, channel CC10 combined live in
     * voice_update_gain; rising CC10 attenuates the left (probe 07) */
    voice_update_gain_at(v, t0);
    v->gain_l = v->gain_l_target;
    v->gain_r = v->gain_r_target;
    v->amp_left = 0;
    v->amp_step_l = v->amp_step_r = 0.0;
    v->amp_retire = 0;
    v->start_delay = g_event_offset;
    v->starting = 1;
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
        (int32_t) (decay_to_sustain_s * (double) BASE_RATE + 0.5);
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
        v->env_attack_step = 1.0 / (attack_s * (double) BASE_RATE);
        v->env_attack_elapsed = 0;
        v->env_attack_samples = (int32_t) (attack_s * (double) BASE_RATE + 0.5);
        if (v->env_attack_samples < 1)
            v->env_attack_samples = 1;
    }

    v->env_sub = 0;

    /* EG2 [A:0x15838]: same segment structure as EG1 but scales
     * eg2_to_pitch_cents; zero depth (the default) leaves it inert */
    v->eg2_depth_cents = (double) r->artic->eg2_to_pitch_cents;
    int32_t eg2_sus = r->artic->eg2_sustain_permille;
    if (eg2_sus < 0)
        eg2_sus = 0;
    if (eg2_sus > 1000)
        eg2_sus = 1000;
    v->eg2_sus = eg2_sus;
    // [A:0x19968] scales the decay by 1000-sustain, as for EG1
    v->eg2_atk =
        (uint32_t) (timecents_to_seconds(r->artic->eg2_attack_tc) * BASE_RATE);
    v->eg2_dec = (uint32_t) (timecents_to_seconds(scale_tc_by_source(
                                 r->artic->eg2_decay_tc,
                                 r->artic->eg2_decay_kf_tc, note)) *
                             BASE_RATE) *
                 (uint32_t) (1000 - eg2_sus) / 1000;
    v->eg2_rel =
        (uint32_t) (timecents_to_seconds(r->artic->eg2_release_tc) * BASE_RATE);

    /* Prime the amplitude ramp (see the amp_left reset above). env_level is
     * final only now: 1.0 for an instant attack, 0.0 for a real one. */
    int32_t env_hdb = voice_env_hdb(v->env_level);
    v->amp_l = voice_amp(v->atten_l_hdb, env_hdb);
    v->amp_r = voice_amp(v->atten_r_hdb, env_hdb);
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
                release_at_event(v);
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
            release_at_event(v);
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
                release_at_event(v);
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
            release_at_event(v);
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

/* BASE_RATE frames until EG1's current segment ends (the driver's "next
 * change" term, [A:0x19490]); sustain/release return ENV_NO_CHANGE since
 * neither has a scheduled end. */
static uint32_t env_base_frames_to_change(const Voice *v)
{
    switch (v->env_stage)
    {
        case ENV_ATTACK:
        {
            if (v->env_attack_step <= 0.0)
                return 1;
#if ENV_ATTACK_TABLE_C
            double n = (double) (v->env_attack_samples - v->env_attack_elapsed);
#else
            double n = (1.0 - v->env_level) / v->env_attack_step;
#endif
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

/* the same boundary in RENDER_RATE frames, counting the held calls left
 * before the next step */
uint32_t voice_env_frames_to_change(const Voice *v)
{
    uint32_t n = env_base_frames_to_change(v);
    if (n > ENV_NO_CHANGE / RESAMPLE_FACTOR)
        return ENV_NO_CHANGE;
    return n * RESAMPLE_FACTOR - v->env_sub % RESAMPLE_FACTOR;
}

double voice_step_envelope(Voice *v)
{
    if (++v->env_sub % RESAMPLE_FACTOR)
        return v->env_level;
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
            /* The driver's finish floor: +0x13c < -8000 hundredths of a dB
             * [A:0x19733], the bare EG, not gain-inclusive [A:0x194da] */
            if (v->env_level < 0.0001)
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
