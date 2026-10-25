/* test.c -- native Linux harness for vsti.c: links the same vsti.c (its
 * Windows-only bits are #ifdef'd out) against the engine and gmdls.c, and
 * drives VSTPluginMain/dispatcher/processReplacing directly, no DLL needed.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/render.h"
#include "engine/synth.h"
#include "engine/voice.h"
#include "unit_tap.h"
#include "vst2.h"

/* Part 9 asserts opposite outcomes per mode, and an undefined macro is 0 to
 * `#if` -- silently the wrong branch. The Makefile passes this to both
 * translation units; refuse to build without it rather than restate vsti.c's
 * default here and let the two drift. */
#ifndef VSTI_IMPRECISE_TIMING
#error "build with -DVSTI_IMPRECISE_TIMING=<0|1> (see Makefile VSTI_TIMING)"
#endif

extern AEffect *VST2_CDECL VSTPluginMain(AudioMasterCallback host);
extern uint64_t g_vsti_engine_frames;
extern uint64_t g_vsti_out_frames;

static VstIntPtr VST2_CDECL host_callback(AEffect *effect, int32_t opcode,
                                          int32_t index, VstIntPtr value,
                                          void *ptr, float opt)
{
    (void) effect;
    (void) index;
    (void) value;
    (void) ptr;
    (void) opt;
    if (opcode == audioMasterVersion)
        return 2400;
    return 0;
}

static void send_midi(AEffect *effect, uint8_t status, uint8_t d1, uint8_t d2,
                      int32_t delta)
{
    VstMidiEvent me;
    memset(&me, 0, sizeof me);
    me.type = kVstMidiType;
    me.byteSize = (int32_t) (sizeof(VstMidiEvent) - 2 * sizeof(int32_t));
    me.deltaFrames = delta;
    me.midiData[0] = (char) status;
    me.midiData[1] = (char) d1;
    me.midiData[2] = (char) d2;

    VstEvents events;
    memset(&events, 0, sizeof events);
    events.numEvents = 1;
    events.events[0] = (VstEvent *) &me;

    effect->dispatcher(effect, effProcessEvents, 0, 0, &events, 0.0f);
}

static AEffect *open_master(float rate, int32_t block_size)
{
    AEffect *effect = VSTPluginMain(host_callback);
    effect->dispatcher(effect, effOpen, 0, 0, NULL, 0.0f);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, NULL, rate);
    effect->dispatcher(effect, effSetBlockSize, 0, block_size, NULL, 0.0f);
    return effect;
}

/* Renders `total` frames in chunks of `block_size` via processReplacing,
 * writing into caller-owned out_l/out_r. */
static void render_all(AEffect *effect, uint32_t total, uint32_t block_size,
                       float *out_l, float *out_r)
{
    uint32_t done = 0;
    while (done < total)
    {
        uint32_t n = block_size;
        if (n > total - done)
            n = total - done;
        float *outs[2] = {out_l + done, out_r + done};
        effect->processReplacing(effect, NULL, outs, (int32_t) n);
        done += n;
    }
}

/* One note-on, rendered at `rate`/`block_size`, total output frames
 * consumed and returned via g_vsti_engine_frames. Reuses a single
 * block-sized scratch buffer -- unlike render_all, nothing here needs the
 * full output retained. */
static uint64_t run_drift_pass(float rate, uint32_t total, uint32_t block_size)
{
    AEffect *effect = open_master(rate, (int32_t) block_size);
    send_midi(effect, 0x90, 60, 100, 0);
    g_vsti_engine_frames = 0;
    g_vsti_out_frames = 0;

    float *bl = (float *) malloc(block_size * sizeof(float));
    float *br = (float *) malloc(block_size * sizeof(float));
    float *outs[2] = {bl, br};
    uint32_t done = 0;
    while (done < total)
    {
        uint32_t n = block_size;
        if (n > total - done)
            n = total - done;
        effect->processReplacing(effect, NULL, outs, (int32_t) n);
        done += n;
    }
    free(bl);
    free(br);

    uint64_t engine_frames = g_vsti_engine_frames;
    effect->dispatcher(effect, effClose, 0, 0, NULL, 0.0f);
    return engine_frames;
}

int main(void)
{
    tap_begin();

    /* 1: construction */
    AEffect *effect = VSTPluginMain(host_callback);
    ok(effect != NULL, "VSTPluginMain returns a non-null AEffect");
    ok(effect->magic == kVstMagic, "AEffect.magic == kVstMagic");
    is_int(effect->numOutputs, 2, "numOutputs == 2");
    is_int(effect->numInputs, 0, "numInputs == 0");
    ok((effect->flags & effFlagsIsSynth) != 0, "flags has effFlagsIsSynth");
    ok((effect->flags & effFlagsCanReplacing) != 0,
       "flags has effFlagsCanReplacing");

    /* 2: open/configure */
    effect->dispatcher(effect, effOpen, 0, 0, NULL, 0.0f);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, NULL, 44100.0f);
    effect->dispatcher(effect, effSetBlockSize, 0, 512, NULL, 0.0f);

    /* 3: note on */
    send_midi(effect, 0x90, 60, 100, 0);

    /* 4: non-silent, finite, in-range output */
    enum
    {
        BLOCK = 512,
        NBLOCKS = 20
    };
    float bl[BLOCK], br[BLOCK];
    float outs_buf[2][BLOCK];
    float *outs[2] = {outs_buf[0], outs_buf[1]};
    float peak = 0.0f;
    int all_finite = 1, all_in_range = 1;
    for (int b = 0; b < NBLOCKS; b++)
    {
        effect->processReplacing(effect, NULL, outs, BLOCK);
        memcpy(bl, outs[0], sizeof bl);
        memcpy(br, outs[1], sizeof br);
        for (int i = 0; i < BLOCK; i++)
        {
            float l = bl[i], r = br[i];
            if (l != l || r != r || fabsf(l) > 1e6f || fabsf(r) > 1e6f)
                all_finite = 0;
            if (l < -1.0f || l > 1.0f || r < -1.0f || r > 1.0f)
                all_in_range = 0;
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (a > peak)
                peak = a;
        }
    }
    ok(all_finite, "note-on output is finite (no NaN/Inf)");
    ok(all_in_range, "note-on output stays within [-1,1]");
    ok(peak > 0.001, "note-on output is non-silent, peak=%f", (double) peak);

    /* 5: note off, release decays to silence */
    send_midi(effect, 0x80, 60, 0, 0);
    float last_peak = 1.0f;
    /* gm.dls releases can run several seconds; render 20s worth */
    int release_blocks = (44100 * 20) / BLOCK;
    for (int b = 0; b < release_blocks; b++)
    {
        effect->processReplacing(effect, NULL, outs, BLOCK);
        memcpy(bl, outs[0], sizeof bl);
        memcpy(br, outs[1], sizeof br);
        last_peak = 0.0f;
        for (int i = 0; i < BLOCK; i++)
        {
            float a = fabsf(bl[i]) > fabsf(br[i]) ? fabsf(bl[i]) : fabsf(br[i]);
            if (a > last_peak)
                last_peak = a;
        }
    }
    ok(last_peak < 1e-4, "release decays to silence, final block peak=%g",
       (double) last_peak);

    effect->dispatcher(effect, effClose, 0, 0, NULL, 0.0f);

    /* 6: resampler frame-accounting, thousands of awkward-size blocks */
    uint32_t block = 511;
    uint32_t k = 5000;
    uint32_t total = block * k;

    uint64_t got44 = run_drift_pass(44100.0f, total, block);
    uint64_t want44 = ((uint64_t) BASE_RATE * total) / 44100;
    long long diff44 = (long long) got44 - (long long) want44;
    ok(diff44 >= -1 && diff44 <= 1,
       "44100Hz frame accounting over %u blocks of %u: got=%llu want=%llu "
       "diff=%lld",
       k, block, (unsigned long long) got44, (unsigned long long) want44,
       diff44);

    uint64_t got48 = run_drift_pass(48000.0f, total, block);
    uint64_t want48 = ((uint64_t) BASE_RATE * total) / 48000;
    long long diff48 = (long long) got48 - (long long) want48;
    ok(diff48 >= -1 && diff48 <= 1,
       "48000Hz frame accounting over %u blocks of %u: got=%llu want=%llu "
       "diff=%lld",
       k, block, (unsigned long long) got48, (unsigned long long) want48,
       diff48);

    /* block-boundary independence: same total, one call vs many */
    uint32_t total2 = block * 200;
    uint64_t many = run_drift_pass(44100.0f, total2, block);
    uint64_t one = run_drift_pass(44100.0f, total2, total2);
    long long diff_chunking = (long long) many - (long long) one;
    ok(diff_chunking >= -1 && diff_chunking <= 1,
       "engine frames match whether output is one block or %u blocks of %u: "
       "many=%llu one=%llu diff=%lld",
       total2 / block, block, (unsigned long long) many,
       (unsigned long long) one, diff_chunking);

    /* 7: block-boundary continuity, sample-domain */
    uint32_t total3 = 44100 * 2;
    float *long_l = (float *) malloc(total3 * sizeof(float));
    float *long_r = (float *) malloc(total3 * sizeof(float));
    float *short_l = (float *) malloc(total3 * sizeof(float));
    float *short_r = (float *) malloc(total3 * sizeof(float));

    /* Both passes must start from the same engine state or this compares two
     * different renders: neither pass sends note-off, so without the reset the
     * second one begins with the first one's voice still sounding. */
    /* Note-off halfway, so the compared span covers the release and not just
     * the held note -- the release is where the gain segmentation shows, and
     * where a host-block-sized segment would diverge. Both passes have
     * rendered exactly total3/2 frames when it arrives, so it lands on the
     * same engine frame in both. */
    synth_construct();
    AEffect *e_long = open_master(44100.0f, (int32_t) total3);
    send_midi(e_long, 0x90, 60, 100, 0);
    render_all(e_long, total3 / 2, total3, long_l, long_r);
    send_midi(e_long, 0x80, 60, 0, 0);
    render_all(e_long, total3 / 2, total3, long_l + total3 / 2,
               long_r + total3 / 2);
    e_long->dispatcher(e_long, effClose, 0, 0, NULL, 0.0f);

    synth_construct();
    AEffect *e_short = open_master(44100.0f, 137);
    send_midi(e_short, 0x90, 60, 100, 0);
    render_all(e_short, total3 / 2, 137, short_l, short_r);
    send_midi(e_short, 0x80, 60, 0, 0);
    render_all(e_short, total3 / 2, 137, short_l + total3 / 2,
               short_r + total3 / 2);
    e_short->dispatcher(e_short, effClose, 0, 0, NULL, 0.0f);

    double max_diff = 0.0;
    for (uint32_t i = 0; i < total3; i++)
    {
        double dl = fabs((double) long_l[i] - (double) short_l[i]);
        double dr = fabs((double) long_r[i] - (double) short_r[i]);
        if (dl > max_diff)
            max_diff = dl;
        if (dr > max_diff)
            max_diff = dr;
    }
    /* Exact, not "close": the engine runs on its own service-block grid now,
     * so the host's block size cannot reach it and the two passes must be the
     * same render sample for sample. Any drift here means the host's buffer
     * size is leaking into the gain segmentation again. */
    ok(max_diff == 0.0,
       "host block size cannot change the render: 1 block vs 137-frame blocks "
       "differ by %.6f",
       max_diff);

    free(long_l);
    free(long_r);
    free(short_l);
    free(short_r);

    /* 8: effMainsChanged(value=0) stops a held note. Program 19 (Church
     * Organ) is picked deliberately: unlike the default piano patch used
     * above, its EG1 sustains flat while held (measured: no natural decay
     * over 4+ seconds), so a drop to silence can only come from the release
     * CC120 triggers, not from the instrument's own envelope. synth_construct
     * is test isolation, not the fix under test: it clears voices leaked by
     * parts 6/7 above, which never sent note-off, so the periodic
     * voice-reserve top-up (voice.c's topup_reserve) can't steal this test's
     * own voice and produce a false pass. */
    synth_construct();
    AEffect *e_mains = open_master(44100.0f, 512);
    float mains_l[512], mains_r[512];
    float *mains_outs[2] = {mains_l, mains_r};
    send_midi(e_mains, 0xC0, 19, 0, 0);
    e_mains->processReplacing(e_mains, NULL, mains_outs, 512);
    send_midi(e_mains, 0x90, 60, 100, 0);
    float mains_peak = 0.0f;
    /* past the attack ramp (measured: ~5 blocks to reach sustain level) */
    for (int b = 0; b < 10; b++)
    {
        e_mains->processReplacing(e_mains, NULL, mains_outs, 512);
        for (int i = 0; i < 512; i++)
        {
            float a = fabsf(mains_l[i]) > fabsf(mains_r[i]) ? fabsf(mains_l[i])
                                                            : fabsf(mains_r[i]);
            if (a > mains_peak)
                mains_peak = a;
        }
    }
    ok(mains_peak > 0.01, "note-on before suspend is audible, peak=%f",
       (double) mains_peak);

    e_mains->dispatcher(e_mains, effMainsChanged, 0, 0, NULL, 0.0f);
    float mains_peak2 = 1.0f;
    int mains_decay_blocks = (44100 * 2) / 512;
    for (int b = 0; b < mains_decay_blocks; b++)
    {
        e_mains->processReplacing(e_mains, NULL, mains_outs, 512);
        mains_peak2 = 0.0f;
        for (int i = 0; i < 512; i++)
        {
            float a = fabsf(mains_l[i]) > fabsf(mains_r[i]) ? fabsf(mains_l[i])
                                                            : fabsf(mains_r[i]);
            if (a > mains_peak2)
                mains_peak2 = a;
        }
    }
    ok(mains_peak2 < 1e-4,
       "effMainsChanged(value=0) silences a held note, final block peak=%g",
       (double) mains_peak2);

    e_mains->dispatcher(e_mains, effClose, 0, 0, NULL, 0.0f);

    /* Part 9: event-timing granularity. A note-on placed at the very last
     * frame of the block is the case the two modes disagree on -- imprecise
     * drains it at the block boundary so the block's opening frames already
     * sound, sample-accurate places it at deltaFrames so they cannot. Same
     * synth_construct isolation as part 8, same reason.
     *
     * The host block is sized to exactly one service block (2x at 44100 vs
     * BASE_RATE) so the event lands inside the block being rendered next
     * whatever SERVICE_BLOCK_FRAMES is set to. A smaller host block would make
     * the event wait for a service block that has not been reached yet, and
     * the imprecise case would read as silence for a legitimate reason. */
    uint32_t gn = SERVICE_BLOCK_FRAMES * 2;
    synth_construct();
    AEffect *e_grid = open_master(44100.0f, (int32_t) gn);
    float *grid_l = (float *) malloc(gn * sizeof(float));
    float *grid_r = (float *) malloc(gn * sizeof(float));
    float *grid_outs[2] = {grid_l, grid_r};
    send_midi(e_grid, 0xC0, 19, 0, 0);
    e_grid->processReplacing(e_grid, NULL, grid_outs, (int32_t) gn);
    send_midi(e_grid, 0x90, 60, 100, (int32_t) gn - 1);
    e_grid->processReplacing(e_grid, NULL, grid_outs, (int32_t) gn);
    float head_peak = 0.0f;
    /* the first half: entirely ahead of the event's own deltaFrames, but wide
     * enough not to hinge on the attack ramp crossing one LSB in the first
     * millisecond */
    for (uint32_t i = 0; i < gn / 2; i++)
    {
        float a = fabsf(grid_l[i]) > fabsf(grid_r[i]) ? fabsf(grid_l[i])
                                                      : fabsf(grid_r[i]);
        if (a > head_peak)
            head_peak = a;
    }
#if VSTI_IMPRECISE_TIMING
    ok(head_peak > 1e-5,
       "imprecise: note-on at the block's last frame sounds from frame 0, "
       "peak=%g",
       (double) head_peak);
#else
    ok(head_peak == 0.0f,
       "sample-accurate: note-on at the block's last frame leaves frame 0 "
       "silent, "
       "peak=%g",
       (double) head_peak);
#endif
    send_midi(e_grid, 0x80, 60, 0, 0);
    e_grid->dispatcher(e_grid, effClose, 0, 0, NULL, 0.0f);
    free(grid_l);
    free(grid_r);

    /* Part 10: at BASE_RATE the resampler is a pass-through, so the plugin
     * should be nothing but the engine driven on the service-block grid --
     * which is what smf.c does and what the corpus scores against MSGS. Any
     * difference here is the plugin editing the engine's output. The only
     * freedom allowed is a constant delay -- the interpolation window trails
     * the read head -- so search for it rather than hard-coding the tap count,
     * and still demand an exact match once it is found. */
    uint32_t rn = SERVICE_BLOCK_FRAMES * 8;
    int16_t *ref = (int16_t *) malloc(rn * 2 * sizeof(int16_t));
    float *plug_l = (float *) malloc(rn * sizeof(float));
    float *plug_r = (float *) malloc(rn * sizeof(float));

    synth_construct();
    synth_midi(0x90, 60, 100);
    for (uint32_t b = 0; b < rn / SERVICE_BLOCK_FRAMES; b++)
        render_frames(ref + (size_t) b * SERVICE_BLOCK_FRAMES * 2,
                      SERVICE_BLOCK_FRAMES);

    synth_construct();
    AEffect *e_id = open_master((float) BASE_RATE, 64);
    send_midi(e_id, 0x90, 60, 100, 0);
    render_all(e_id, rn, 64, plug_l, plug_r);
    e_id->dispatcher(e_id, effClose, 0, 0, NULL, 0.0f);

    uint32_t best = rn, best_d = 0;
    for (uint32_t d = 0; d < 64; d++)
    {
        uint32_t bad = 0;
        for (uint32_t i = 0; i + d < rn; i++)
        {
            if (plug_l[i + d] != (float) ref[i * 2 + 0] / 32768.0f ||
                plug_r[i + d] != (float) ref[i * 2 + 1] / 32768.0f)
                bad++;
        }
        if (bad < best)
        {
            best = bad;
            best_d = d;
        }
    }
    ok(best == 0,
       "at BASE_RATE the plugin is the engine on its own grid, delayed %u "
       "frames: %u/%u differ",
       best_d, best, rn - best_d);

    free(ref);
    free(plug_l);
    free(plug_r);

    /* Part 11: the plugin comes up in GS mode, checked through the MIDI path
     * rather than by reading the flag -- what matters is that synth.c's gate on
     * CC0/CC32 bank-byte storage is open, since without a SysEx route nothing
     * else could ever open it. synth_construct() first, to prove the plugin
     * re-establishes it rather than inheriting it from an earlier part. */
    synth_construct();
    AEffect *e_gs = open_master(44100.0f, 512);
    send_midi(e_gs, 0xB1, 0, 3, 0);  /* CC0  bank MSB = 3 on channel 1 */
    send_midi(e_gs, 0xB1, 32, 5, 0); /* CC32 bank LSB = 5 */
    float gs_l[512], gs_r[512];
    float *gs_outs[2] = {gs_l, gs_r};
    e_gs->processReplacing(e_gs, NULL, gs_outs, 512);
    ok(g_channels[1].bank_msb == 3 && g_channels[1].bank_lsb == 5,
       "GS mode is on by default: CC0/CC32 stored, bank_msb=%u bank_lsb=%u",
       g_channels[1].bank_msb, g_channels[1].bank_lsb);
    e_gs->dispatcher(e_gs, effClose, 0, 0, NULL, 0.0f);

    return tap_done();
}
