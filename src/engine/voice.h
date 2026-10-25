/* voice.h -- voice pool, allocation, stealing, key-group choke, per-voice
 * parameters, envelopes. */
#ifndef VOICE_H
#define VOICE_H

#include "dls.h"
#include <stdint.h>

/* every fitted constant here is measured against it; RESAMPLE_FACTOR ships at
 * 1, see invariant below */
#define BASE_RATE 22050
#ifndef RESAMPLE_FACTOR
#define RESAMPLE_FACTOR 1
#endif
#define RENDER_RATE (BASE_RATE * RESAMPLE_FACTOR)

/* shared modulation cadence for render.c's LFO and voice.c's EG2 stepping */
#define LFO_UPDATE_FRAMES (64 * RESAMPLE_FACTOR)

#ifndef SERVICE_BLOCK_FRAMES
/* audio service block: one clock for re-aim/gain ramp/reserve top-up, see
 * render.c's GAIN_SEGMENT_FRAMES */
#define SERVICE_BLOCK_FRAMES (256u * RESAMPLE_FACTOR)
#endif

/* RESAMPLE_FACTOR must stay an INTEGER multiple of BASE_RATE: a non-integer
 * ratio silently detunes RENDER_RATE, and a non-multiple render rate loses
 * probe 02's real, audible synthesis-rate aliasing (keys 125-127); the
 * _Static_assert below guards it. */
_Static_assert(
    RENDER_RATE % BASE_RATE == 0 && RENDER_RATE >= BASE_RATE,
    "RESAMPLE_FACTOR must be a positive integer (internal-rate invariant)");

/* 54 voice objects, 48 primary + 6 reserve (0x12a78-0x12aef), kept here as
 * one array with a reserve tag */
#define NUM_VOICES 54

typedef enum
{
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
    ENV_IDLE
} EnvStage;

typedef struct Voice
{
    int active; /* producing sound */
    int channel;
    int note;
    /* resolved instrument locale, for key-group scope match */
    uint32_t locale;
    uint8_t key_group;
    Wave *wave;
    Artic *artic;

    uint32_t phase_pos; /* Q12: bits31:12 index, bits11:0 fraction */
    /* Q12 per-output-sample increment; ramped towards phase_step_target one
     * sample at a time by render.c's render_voice, never written directly
     * outside voice_note_on's snap and that ramp step (0x18fba) */
    uint32_t phase_step;
    /* bend the in-flight ramp is aimed at; LFO/EG2 changes are carried through
     * instantly instead */
    int32_t bend_cents_applied;
    /* live target voice_update_pitch recomputes every block (bend/LFO/EG2);
     * phase_step glides toward this, mirroring gain_l/gain_l_target */
    uint32_t phase_step_target;
    /* HELD per-sample slope towards phase_step_target, in 1/256 phase_step LSBs
     * (whole LSBs too coarse below ~a semitone); sized by ramp_reaim over a
     * fixed horizon, held until ramp_left expires */
    int32_t phase_step_ramp_step;
    /* leftover 1/256ths from the slope above, carried sample to sample by
     * render_voice */
    int32_t phase_step_ramp_acc;
    /* frames left on the current ramp; 0 = settled. voice_ramp_tick snaps
     * phase_step to target on expiry */
    uint32_t ramp_left;
    /* note-on base pitch ratio, clamped once at note-on (incl. latched
     * RPN1/RPN2); live bend/LFO multiply this OUTSIDE the clamp -- `[M: probe
     * 30]` */
    uint32_t base_ratio_q12;
    /* fine_tune + (note-unity_note)*100: fixed for the voice's life; live
     * modulation added on top each block by voice_update_pitch */
    int32_t base_cents;
    int32_t loop_start_s, loop_len_s; /* samples; loop_len_s==0 => one-shot */
    int32_t sample_end_s;             /* one-shot end / loop end, samples */

    /* frames before this voice produces anything, so a note-on lands on its
     * exact sample despite being dispatched at the buffer top */
    uint32_t start_delay;

    /* gain in force for the CURRENT amplitude segment: gain_*_target sampled at
     * the segment boundary and held for its duration `[A:0x19490]` */
    double gain_l, gain_r;

    /* frames left in this segment; 0 = re-aim. Amplitude ramp segment: envelope
     * x gain sampled once per segment END, ramped linearly `[A:0x18fba]` */
    uint32_t amp_left;
    double amp_l, amp_r;           /* applied amplitude, envelope x gain */
    double amp_step_l, amp_step_r; /* per-frame increment towards the target */
    /* envelope finished inside this segment; voice dies at the segment's end,
     * not the envelope's instant, so the committed ramp lands */
    int amp_retire;
    /* live target gain, recomputed every block by voice_update_gain from
     * atten_const_hdb plus current channel vol/expr/pan/master vol */
    double gain_l_target, gain_r_target;
    /* velocity + region/wsmp attenuation + 1200: fixed for the voice's life
     * (0x19bb9-0x19bcd); CC-driven terms NOT baked in here, see
     * voice_update_gain */
    int32_t atten_const_hdb;

    EnvStage env_stage;
    double env_level;       /* 0..1 amplitude envelope multiplier */
    double env_attack_step; /* linear increment per sample during attack */
    /* ENV_ATTACK_TABLE_C only: samples advanced so far into the attack segment
     */
    int32_t env_attack_elapsed;
    /* ENV_ATTACK_TABLE_C only: attack segment length in samples (>=1 whenever
     * env_stage==ENV_ATTACK) */
    int32_t env_attack_samples;
    /* geometric per-sample ratio toward env_sustain_level,
     * terminated exactly by env_decay_samples_left, not by this coefficient
     * asymptoting */
    double env_decay_coef;
    double env_release_coef;  /* multiplicative approach-to-zero coefficient */
    double env_sustain_level; /* 0..1 */
    /* exact countdown so the decay ramp snaps to
     * env_sustain_level at the rescaled duration rather than a threshold test.
     * 0 while not in ENV_DECAY. */
    int32_t env_decay_samples_left;

    /* EG2, the PITCH envelope [A:0x15838]: EG2 to any destination other than
     * PITCH is ignored */
    EnvStage eg2_stage;
    double eg2_level;       /* 0..1 */
    double eg2_attack_step; /* linear increment per modulation block */
                            /* multiplicative approach-to-sustain, per block */
    double eg2_decay_coef;
    double eg2_release_coef;  /* multiplicative approach-to-zero, per block */
    double eg2_sustain_level; /* 0..1 */
    /* connection scale; 0 = no EG2->pitch (the default) */
    double eg2_depth_cents;

    /* note not yet released (CC64/CC120/CC123 semantics) */
    int held;
    int sustain_deferred; /* CC64 held down at note-off time */
    /* allocation sequence number, for oldest-first steal */
    uint32_t age;

    /* 1 while this voice is a FREE node tagged
     * reserve-tier; meaningless while active==1. Recycling always clears it to
     * 0 (primary). See voice_topup_reserve. */
    int in_reserve;
    /* driver voice+0x138: 1 once committed to a fast release by choke,
     * retrigger, or top-up Branch B; gates find_steal_candidate_symmetric so
     * the same voice isn't re-marked mid-drain. Cleared at voice_note_on. */
    int fast_release_committed;

    /* Pitch LFO (vibrato) `[M: probe 06]`; rate/delay cached at note-on from
     * this voice's own artic */
    double lfo_phase;
    double lfo_freq_hz;
    double lfo_delay_s;
    /* seconds since note-on; phase stays 0 until this exceeds lfo_delay_s */
    double lfo_elapsed_s;
} Voice;

extern Voice g_voices[NUM_VOICES];
extern uint32_t g_voice_age_counter;

void voice_pool_reset(void);
void voice_note_on(int channel, int note, int velocity);

/* TopUpReserve (0x12b6a): tops the reserve via Branch A (retag free primary
 * nodes) then Branch B (fast-release active voices), on a wall-clock tick */
void voice_topup_tick(uint32_t frames);
void voice_note_off(int channel, int note);
void voice_all_sound_off(int channel); /* CC120: bypasses sustain hold */
void voice_all_notes_off(int channel); /* CC123: honours sustain hold */
/* CC64 released: release deferred voices */
void voice_sustain_lift(int channel);

double voice_step_envelope(Voice *v);

void voice_set_event_offset(uint32_t frames);

/* "no scheduled boundary" sentinel for voice_env_frames_to_change, below */
#define ENV_NO_CHANGE 0xFFFFFFFFu
uint32_t voice_env_frames_to_change(const Voice *v);

int voice_any_active(void);

/* recomputes v->phase_step from v->base_cents plus every live pitch source
 * (bend, LFO, EG2) -- the ONLY place phase_step is computed */
void voice_update_pitch(Voice *v);

void voices_update_modulation(void);

/* retire pitch ramps whose horizon has elapsed; call once per rendered
 * sub-chunk, AFTER render_voice has actually rendered those frames */
void voice_ramp_tick(uint32_t frames);
void voices_advance_lfo(uint32_t frames);

/* recomputes gain_l/gain_r from atten_const_hdb plus live master/CC7/CC11/CC10
 * -- the ONLY place gain is computed */
void voice_update_gain(Voice *v);

#endif /* VOICE_H */
