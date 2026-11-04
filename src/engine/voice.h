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

/* one sample in phase units: Q12 scaled by RESAMPLE_FACTOR, so phase_step is
 * the BASE_RATE step and frame R*n lands on frame n's exact position */
#define PHASE_ONE (4096u * RESAMPLE_FACTOR)

#ifndef SERVICE_BLOCK_FRAMES
/* one Mix call: FillBuffer allocates 0x1000 bytes and mixes 0x1000>>2
 * frames into them (0x184c0, 0x181fa); smf.c drains each call's events at
 * its top, and no segment runs past its end (0x196de) */
#define SERVICE_BLOCK_FRAMES (1024u * RESAMPLE_FACTOR)
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

    uint32_t phase_pos; /* PHASE_ONE units: index * PHASE_ONE + fraction */
    /* per-output-sample increment, PHASE_ONE units; moved toward
     * phase_step_target in stairs by render_voice (0x18fba) */
    uint32_t phase_step;
    /* the step as of the current segment's end (bend+LFO+EG2), set by
     * voice_update_pitch */
    uint32_t phase_step_target;
    /* the segment's summed pitch modulation (bend+LFO+EG2), cents */
    int32_t mod_cents;
    /* the previous segment's attenuation sums and pitch cents; they size
     * the stairs (0x19077) */
    int32_t prev_att_l, prev_att_r, prev_mod_cents;
    /* BASE_RATE frames between step refreshes this segment; frames left to the
     * next one; render frames into the segment, for the RESAMPLE_FACTOR grid */
    uint32_t stair_k, stair_left, stair_sub;
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
    /* set at note-on: the first render evaluates pitch and gain again at
     * the note's time, once the whole call's events are queued (0x12bd6
     * sets notes up at Mix start, after RecordMIDI) */
    int starting;
    /* g_now frame a dispatched note-off releases EG1/EG2 at; VOICE_NO_RELEASE
     * if none is pending [A:0x19a3d]-[A:0x19a6f] */
    uint64_t release_at;
    /* ...and whether that release is a fast one (0x19aa4) */
    int release_fast;

    /* gain in force for the CURRENT amplitude segment: gain_*_target sampled at
     * the segment boundary and held for its duration `[A:0x19490]` */
    double gain_l, gain_r;

    /* frames left in this segment; 0 = re-aim. Amplitude ramp segment: envelope
     * x gain sampled once per segment END, ramped linearly `[A:0x18fba]` */
    uint32_t amp_left;
    double amp_l, amp_r;           /* applied amplitude, envelope x gain */
    /* increment towards the target, per BASE_RATE frame */
    double amp_step_l, amp_step_r;
    /* frames into this segment; the step lands on every RESAMPLE_FACTOR-th */
    uint32_t amp_sub;
    /* envelope finished inside this segment; voice dies at the segment's end,
     * not the envelope's instant, so the committed ramp lands */
    int amp_retire;
    /* live target gain, recomputed every block by voice_update_gain from
     * atten_const_hdb plus current channel vol/expr/pan */
    double gain_l_target, gain_r_target;
    /* the same sums before the clamp (pan included, EG1 not): the envelope
     * joins them in dB, see voice_amp */
    int32_t atten_l_hdb, atten_r_hdb;
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
    /* EG1 runs on the BASE_RATE grid: voice_step_envelope advances it on every
     * RESAMPLE_FACTOR-th call and holds it between */
    uint32_t env_sub;

    /* EG2, the PITCH envelope [A:0x15838]: EG2 to any destination other than
     * PITCH is ignored. Linear in permille, BASE_RATE frames: attack, decay
     * (already scaled by 1000-sustain), release, sustain permille; eg2_off is
     * the note-off time or -1 while held. eg2_t counts RENDER_RATE frames
     * since the voice began; eg2_cents is the pitch term as of the end of the
     * current segment (render.c). */
    uint32_t eg2_atk, eg2_dec, eg2_rel;
    int32_t eg2_sus;
    int64_t eg2_off;
    uint64_t eg2_t;
    int32_t eg2_cents;
    /* the step's change still to land this segment, its stair increment and
     * accumulator (Q8): the mixer adds (S1-S0)*K/N every K frames
     * [A:0x190c8, 0x1a41d] */
    int32_t step_left, step_inc, step_acc;
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

    /* LFO clock: frames since note-on at the evaluation time (0x1891e) */
    uint64_t lfo_elapsed;
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
/* g_now plus the offset of the event being dispatched */
uint64_t voice_event_time(void);
#define VOICE_NO_RELEASE UINT64_MAX

/* "no scheduled boundary" sentinel for voice_env_frames_to_change, below */
#define ENV_NO_CHANGE 0xFFFFFFFFu
uint32_t voice_env_frames_to_change(const Voice *v);

int voice_any_active(void);

/* phase_step_target from v->base_cents plus every live pitch source (bend
 * and CC1 as due at time t, LFO at lfo_elapsed, EG2) -- the ONLY place it is
 * computed; voice_update_pitch reads the newest values */
void voice_update_pitch_at(Voice *v, uint64_t t);
void voice_update_pitch(Voice *v);

/* gain_l/r_target and atten_l/r_hdb from atten_const_hdb plus master and
 * CC7/CC11/CC10/CC1 as due at time t -- the ONLY place gain is computed */
void voice_update_gain_at(Voice *v, uint64_t t);
void voice_update_gain(Voice *v);
/* the release a note-off scheduled, reached at EG2 frame eg2_t */
void voice_release_due(Voice *v, uint64_t eg2_t);

/* gain for a pre-clamp attenuation sum with EG1's level added in dB before
 * the ceiling clamp [A:0x18ff7]; env_hdb from voice_env_hdb */
double voice_amp(int32_t atten_hdb, int32_t env_hdb);
int32_t voice_env_hdb(double env_level);

/* Frames between phase-step refreshes in a segment of `seg` BASE_RATE frames,
 * given the largest attenuation change (hdB) and the pitch change (cents)
 * since the previous segment [A:0x19077-0x190a2] */
/* EG2's pitch term at t BASE_RATE frames, and the frames from t to its next
 * breakpoint, both as the driver reads them [A:0x18d58, 0x18a7a] */
int32_t voice_eg2_cents(const Voice *v, uint64_t t);
uint32_t voice_eg2_frames_to_change(const Voice *v, uint64_t t);
/* the segment about to start is `frames` long: read EG2 at its end */
void voice_eg2_segment(Voice *v, uint32_t frames);
/* RENDER_RATE frames to EG2's next breakpoint */
uint32_t voice_eg2_render_frames_to_change(const Voice *v);

uint32_t voice_stair_frames(uint32_t seg, int32_t d_att, int32_t d_cents);

/* RENDER_RATE frames rendered so far; render_frames advances it */
extern uint64_t g_now;

/* frames from the voice's clock (eg2_t) to its next segment end, before the
 * Mix end clips it [A:0x196cd]-[A:0x196f0] */
uint32_t voice_segment_frames(const Voice *v);

#endif /* VOICE_H */
