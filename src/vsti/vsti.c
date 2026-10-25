/* vsti.c -- GUI-less VST2 instrument: wraps the engine's single global synth
 * behind the VST2 ABI (vst2.h). Model is src/cli.c (init order, allocator);
 * opcode/canDo choices copied from confectionery's dispatcher. */
#include <stdlib.h>
#include <string.h>

#include "engine/dls.h"
#include "engine/render.h"
#include "engine/rt.h"
#include "engine/synth.h"
#include "engine/tables.h"
#include "engine/voice.h"
#include "vst2.h"

extern const unsigned char gm_dls_blob[];
extern const unsigned int gm_dls_blob_len;

/* allocator: malloc, never freed -- same rationale as cli.c's, gm.dls sample
 * data must outlive every render call for the process's whole life. Skipped
 * under VSTI_TEST_BUILD: the native test harness links src/unit_tap.c, which
 * already provides an identical one (shared with unit.c) -- the DLL build
 * links neither, so it needs this one. */
#ifndef VSTI_TEST_BUILD
static uint32_t g_total = 0;

void *rt_alloc(uint32_t nbytes)
{
    nbytes = (nbytes + 7u) & ~7u;
    void *p = calloc(1, nbytes ? nbytes : 8);
    if (!p)
        abort();
    g_total += nbytes;
    return p;
}

uint32_t rt_mem_size(void) { return g_total; }
#endif

static int g_engine_ready = 0;

/* One whole service block of engine output, and how far into it the resampler
 * has read. Starting "empty" makes the first read fill it. */
static int16_t g_block[SERVICE_BLOCK_FRAMES * 2];
static uint32_t g_block_read = SERVICE_BLOCK_FRAMES;
/* absolute engine frame of the next frame to read, and of g_block[0] */
static uint64_t g_next_frame = 0;
static uint64_t g_block_start = 0;
/* output frames produced, the clock MIDI timestamps are resolved against.
 * Deliberately not g_vsti_out_frames: that is a diagnostic the harness reads
 * across a whole run, and this has to restart with each master instance. */
static uint64_t g_out_pos = 0;

static void engine_init_once(void)
{
    if (g_engine_ready)
        return;
    tables_build();
    dls_load(gm_dls_blob, gm_dls_blob_len);
    synth_construct();
    g_engine_ready = 1;
}

/* ponytail: g_channels/g_voices/g_dls are one process-global engine, so two
 * instances in one host would fight over it. First instance to construct
 * wins and renders; later instances still hand back a valid AEffect and
 * silently zero their output. Ceiling: one sounding instance per host
 * process. Upgrade path: thread a context struct through the engine globals
 * so each instance gets its own state. */
static int g_have_master = 0;

#define VSTI_MIN_RATE 5512
#define VSTI_MAX_RATE 192000
#define VSTI_DEFAULT_RATE 44100
#define VSTI_MAX_EVENTS 256
#define VSTI_ABSURD_EVENTS 100000

/* Event-timing granularity within the service block below. 1 (the default):
 * every event acts at the block boundary, the coarseness the driver itself
 * has. 0: a note-on additionally carries its true sub-block offset, which is
 * what smf.c does with the sample-accurate timestamps an SMF gives it.
 * Neither setting can place a note-OFF finer than the block -- see
 * render_service_block: the driver can't either, and that is the mechanism
 * being reproduced, not a limitation being worked around. */
#ifndef VSTI_IMPRECISE_TIMING
#define VSTI_IMPRECISE_TIMING 1
#endif

/* Band-limited interpolation. Two-tap linear reconstruction leaves the images
 * of the 22050 Hz stream 13-25 dB down instead of gone -- measured against an
 * MSGS capture of the same riff, energy above 11 kHz was -42 dB of full band
 * where the reference is -63 -- and droops the top octave by the same
 * transfer's (1+cos(2*pi*f/rate))/2, -4.6 dB at 9.75 kHz.
 *
 * Blackman-windowed sinc at 1024 phases, nearest phase, no coefficient
 * interpolation. Tap count picked by measuring the riff through this plugin
 * against the MSGS capture of it -- energy above 11 kHz relative to full band,
 * and the 9-10.5 kHz passband the window droops:
 *
 *     taps    >11k     9-10.5k        (MSGS capture: -63.1, -3.0)
 *       16   -53.6        -4.8
 *       32   -58.3        -3.6
 *       64   -64.1        -3.3
 *
 * 64 is where both sides reach the reference. Past that the remaining gap is
 * the 11-12 kHz transition band, ours -23.1 against the reference's -28.2, and
 * that is kmixer's own FIR -- taps unrecovered, and the stated ceiling
 * on any 44.1 kHz comparison. Cost is 128 multiply-adds per output frame and
 * 256 KB of .bss.
 *
 * ponytail: nearest phase rather than interpolating between adjacent rows;
 * measured no worse than the reference, so the extra multiply per tap buys
 * nothing here. */
#define VSTI_TAPS 64
#define VSTI_PHASE_BITS 10
#define VSTI_PHASES (1 << VSTI_PHASE_BITS)
#define VSTI_CENTER (VSTI_TAPS / 2 - 1)

/* frame-accounting hooks for the native test harness only, not VST ABI.
 * g_vsti_engine_frames counts source frames the resampler has consumed, which
 * is what the drift check needs; it is not the number rendered, since the
 * engine always runs a whole block at a time. */
uint64_t g_vsti_engine_frames = 0;
uint64_t g_vsti_out_frames = 0;

typedef struct
{
    uint8_t status, d1, d2;
    /* absolute engine frame this event maps to, so an event can sit in the
     * queue across host blocks and still land on the right service block */
    uint64_t engine_frame;
} QueuedMidi;

typedef struct
{
    AEffect effect;
    int is_master;
    float sample_rate;
    /* Q32.32 phase within the interpolation window below. hist holds the last
     * VSTI_TAPS source frames, oldest first; the point being interpolated sits
     * at VSTI_TAPS/2 - 1, so the window TRAILS the newest frame read and
     * nothing is ever fetched before it is needed. */
    uint64_t resamp_acc;
    int16_t hist_l[VSTI_TAPS], hist_r[VSTI_TAPS];
    uint32_t queue_count;
    QueuedMidi queue[VSTI_MAX_EVENTS];
} VstiInstance;

static VstiInstance *vsti_of(AEffect *effect)
{
    return (VstiInstance *) effect->object;
}

static void copy_cstring(void *dst, const char *src, uint32_t limit)
{
    uint32_t n = 0;
    while (src[n] && n + 1 < limit)
        n++;
    memcpy(dst, src, n);
    ((char *) dst)[n] = '\0';
}

/* [A:0x19644],[A:0x18fba]: the mixer re-aims each voice once per segment and ramps
 * linearly to a gain sampled at the segment END, so a note-off inside a
 * segment starts falling at the segment's START -- up to one segment early.
 * That quantisation IS the driver's release taper and its 0-27 ms
 * early-release lattice (probe 46); it is not a separate mechanism.
 *
 * render_voice() clamps the segment to block_left, which render_frames() takes
 * from its own frame count -- so whoever calls render_frames() sets the
 * segment length. Driving it straight from the host's block would hand that
 * knob to the user's buffer setting: a 128-frame ASIO buffer is 64 engine
 * frames, a 2.9 ms segment, and both the taper and the lattice disappear.
 *
 * So the engine runs a whole SERVICE_BLOCK_FRAMES block at a time into
 * g_block regardless of what the host asked for, and the resampler reads out
 * of that. The block is where queued MIDI acts, matching smf.c's "drain a
 * whole service block's events before rendering any of it". The block is
 * filled on demand rather than ahead, so it costs no latency of its own --
 * the resampler's trailing window is the only delay, 33 frames at BASE_RATE
 * (test 18 measures it). */
static void render_service_block(VstiInstance *vi)
{
    /* Due by TIMESTAMP, not by arrival. A host delivers events batched per
     * audio block, so draining whatever happens to be queued would put the
     * host's block size back into the timing through the back door: the same
     * event delivered by a 128-frame host and a 512-frame host would land in
     * different service blocks. Anything not yet due stays queued. */
    uint64_t block_end = g_block_start + SERVICE_BLOCK_FRAMES;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < vi->queue_count; i++)
    {
        QueuedMidi *q = &vi->queue[i];
        if (q->engine_frame >= block_end)
        {
            vi->queue[kept++] = *q;
            continue;
        }
#if !VSTI_IMPRECISE_TIMING
        /* note-ons only: voice.c applies the offset as a per-voice start
         * delay, which has no meaning for any other message */
        if ((q->status & 0xf0) == 0x90 && q->d2 &&
            q->engine_frame > g_block_start)
        {
            uint64_t off = q->engine_frame - g_block_start;
            voice_set_event_offset(off < SERVICE_BLOCK_FRAMES
                                       ? (uint32_t) off
                                       : SERVICE_BLOCK_FRAMES - 1);
        }
#endif
        synth_midi(q->status, q->d1, q->d2);
        voice_set_event_offset(0);
    }
    vi->queue_count = kept;

    render_frames(g_block, SERVICE_BLOCK_FRAMES);
    g_block_read = 0;
}

/* next source frame, filling the block when this one runs out */
static void next_source(VstiInstance *vi, int16_t *l, int16_t *r)
{
    if (g_block_read >= SERVICE_BLOCK_FRAMES)
    {
        /* whole blocks are always read whole, so this is a block boundary */
        g_block_start = g_next_frame;
        render_service_block(vi);
    }
    *l = g_block[g_block_read * 2 + 0];
    *r = g_block[g_block_read * 2 + 1];
    g_block_read++;
    g_next_frame++;
    g_vsti_engine_frames++;
}

#define VSTI_PI 3.14159265358979323846

/* sin(pi*y)/(pi*y), exact at the zeros. rt_sin(pi*m) for integer m is only
 * near zero, and the phase-0 row has to be exactly a unit impulse or the
 * plugin stops being a pass-through at BASE_RATE (test 18). */
static double sinc_pi(double y)
{
    if (y == 0.0)
        return 1.0;
    double r = y - (double) (long) (y >= 0.0 ? y + 0.5 : y - 0.5);
    if (r > -1e-12 && r < 1e-12)
        return 0.0;
    return rt_sin(VSTI_PI * y) / (VSTI_PI * y);
}

static float g_filter[VSTI_PHASES][VSTI_TAPS];
static double g_filter_cutoff = -1.0;

/* cutoff is in units of the source Nyquist: 1.0 when the host runs at or
 * above BASE_RATE (the source band is the limit), lower when it runs below
 * (the output band is, and passing the difference through would alias). */
static void build_filter(double cutoff)
{
    if (cutoff == g_filter_cutoff)
        return;
    for (uint32_t p = 0; p < VSTI_PHASES; p++)
    {
        double frac = (double) p / (double) VSTI_PHASES;
        double sum = 0.0;
        for (uint32_t k = 0; k < VSTI_TAPS; k++)
        {
            double x = (double) k - (double) VSTI_CENTER - frac;
            double u = ((double) k - frac) / (double) (VSTI_TAPS - 1);
            /* Blackman, via sin because rt.h has no cos */
            double w = 0.42 - 0.5 * rt_sin(2.0 * VSTI_PI * u + VSTI_PI / 2.0) +
                       0.08 * rt_sin(4.0 * VSTI_PI * u + VSTI_PI / 2.0);
            g_filter[p][k] = (float) (sinc_pi(cutoff * x) * w);
            /* sum the STORED floats, not the doubles behind them: row 0 must
             * normalise to exactly 1.0, and x/x is exact only when the
             * numerator is the same value the denominator was built from */
            sum += (double) g_filter[p][k];
        }
        /* unity DC gain per row; a no-op on row 0, which is already the
         * impulse */
        if (sum != 0.0)
            for (uint32_t k = 0; k < VSTI_TAPS; k++)
                g_filter[p][k] = (float) (g_filter[p][k] / sum);
    }
    g_filter_cutoff = cutoff;
}

/* cutoff is 1.0 (the source band is the limit) unless the host runs below
 * BASE_RATE, where the output band is and passing the difference through
 * would alias. Called from the dispatcher, never from the render path: the
 * table is 65536 coefficients and three sines each, which is nothing at
 * setup and far too much inside processReplacing. */
static void build_filter_for(float rate)
{
    build_filter(rate < (float) BASE_RATE ? (double) rate / (double) BASE_RATE
                                          : 1.0);
}

/* Resamples the engine's fixed BASE_RATE stream to vi->sample_rate, 64-bit
 * Q32.32 phase accumulator (RESAMPLE_FACTOR stays 1, voice.h). */
static void vsti_resample(VstiInstance *vi, float *out_l, float *out_r,
                          uint32_t n)
{
    uint64_t step = ((uint64_t) BASE_RATE << 32) / (uint64_t) vi->sample_rate;
    uint64_t acc = vi->resamp_acc;

    for (uint32_t i = 0; i < n; i++)
    {
        /* advance at the HEAD: the frame that opens the next block is not
         * fetched, and so that block not rendered, until an output frame
         * actually needs it. The window trails by VSTI_TAPS/2 frames, so no
         * output ever waits on a frame that has not been read -- the cost is
         * that much latency, 363 us at BASE_RATE. */
        while (acc >> 32)
        {
            acc -= (uint64_t) 1 << 32;
            for (uint32_t k = 0; k + 1 < VSTI_TAPS; k++)
            {
                vi->hist_l[k] = vi->hist_l[k + 1];
                vi->hist_r[k] = vi->hist_r[k + 1];
            }
            next_source(vi, &vi->hist_l[VSTI_TAPS - 1],
                        &vi->hist_r[VSTI_TAPS - 1]);
        }

        const float *h = g_filter[acc >> (32 - VSTI_PHASE_BITS)];
        float l = 0.0f, r = 0.0f;
        for (uint32_t k = 0; k < VSTI_TAPS; k++)
        {
            l += h[k] * (float) vi->hist_l[k];
            r += h[k] * (float) vi->hist_r[k];
        }
        l /= 32768.0f;
        r /= 32768.0f;
        /* sinc ringing can overshoot what the engine's saturating mix already
         * clamped to full scale; the host is promised [-1,1] */
        out_l[i] = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        out_r[i] = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        acc += step;
    }

    vi->resamp_acc = acc;
    g_out_pos += n;
    g_vsti_out_frames += n;
}

/* effProcessEvents trust boundary, same checks as confectionery's
 * queueMidiEvents: null pointer, a hostile/corrupt count, non-MIDI events and
 * short byteSize are all dropped; nothing is read past numEvents. Queue is a
 * fixed array -- overflow beyond VSTI_MAX_EVENTS is dropped, never
 * allocated.
 *
 * Appends rather than replaces: the queue is drained by the service block,
 * not by processReplacing, so when the host's block is shorter than a service
 * block there are calls that queue events without draining any. */
static int32_t vsti_process_events(VstiInstance *vi, void *ptr)
{
    if (!ptr)
        return 1;
    VstEvents *events = (VstEvents *) ptr;
    int32_t n = events->numEvents;
    if (n <= 0 || n > VSTI_ABSURD_EVENTS)
        return 1;

    for (int32_t i = 0; i < n && vi->queue_count < VSTI_MAX_EVENTS; i++)
    {
        VstEvent *ev = events->events[i];
        if (!ev || ev->type != kVstMidiType)
            continue;
        if (ev->byteSize <
            (int32_t) (sizeof(VstMidiEvent) - 2 * sizeof(int32_t)))
            continue;
        VstMidiEvent *me = (VstMidiEvent *) ev;
        QueuedMidi *q = &vi->queue[vi->queue_count++];
        q->status = (uint8_t) me->midiData[0];
        q->d1 = (uint8_t) me->midiData[1];
        q->d2 = (uint8_t) me->midiData[2];
        int32_t d = me->deltaFrames < 0 ? 0 : me->deltaFrames;
        /* Absolute OUTPUT frame, converted once: g_vsti_out_frames is the
         * output frame this block starts on, so out+delta is the event's
         * position on the host's timeline whatever the block size, and the
         * multiply-then-divide keeps it that way. Anchoring to the resampler's
         * read head instead would reintroduce the block-size dependence, since
         * how far it has read at delivery time depends on the block size.
         * The read head leads the output by the filter's trailing window, a
         * constant sub-millisecond offset, which is far inside the block
         * quantisation this is all about. */
        uint32_t rate = vi->sample_rate > 1.0f ? (uint32_t) vi->sample_rate
                                               : VSTI_DEFAULT_RATE;
        q->engine_frame =
            (g_out_pos + (uint64_t) d) * BASE_RATE / (uint64_t) rate;
    }
    return 1;
}

static void VST2_CDECL vsti_process_replacing(AEffect *effect, float **inputs,
                                              float **outputs,
                                              int32_t sample_frames)
{
    (void) inputs;
    VstiInstance *vi = vsti_of(effect);
    uint32_t n = sample_frames > 0 ? (uint32_t) sample_frames : 0;
    if (n == 0 || !outputs || !outputs[0] || !outputs[1])
        return;
    float *out_l = outputs[0];
    float *out_r = outputs[1];

    if (!vi->is_master)
    {
        for (uint32_t i = 0; i < n; i++)
            out_l[i] = out_r[i] = 0.0f;
        vi->queue_count = 0;
        return;
    }

    /* Events are applied by render_service_block, not here -- see its comment.
     * The host's block size no longer reaches the engine at all. */
    vsti_resample(vi, out_l, out_r, n);
}

static void VST2_CDECL vsti_set_parameter(AEffect *effect, int32_t index,
                                          float parameter)
{
    /* no-op stub: numParams is 0, but not every host is polite about a NULL
     * setParameter/getParameter (confectionery's own comment records this). */
    (void) effect;
    (void) index;
    (void) parameter;
}

static float VST2_CDECL vsti_get_parameter(AEffect *effect, int32_t index)
{
    (void) effect;
    (void) index;
    return 0.0f;
}

static VstIntPtr VST2_CDECL vsti_dispatch(AEffect *effect, int32_t opcode,
                                          int32_t index, VstIntPtr value,
                                          void *ptr, float opt)
{
    (void) index;
    VstiInstance *vi = vsti_of(effect);

    switch (opcode)
    {
        case effOpen:
            return 1;

        case effClose:
            if (vi->is_master)
                g_have_master = 0;
            free(vi);
            return 1;

        case effSetSampleRate:
        {
            float rate = opt;
            if (!(rate > 0.0f) || rate != rate)
                rate = VSTI_DEFAULT_RATE;
            else if (rate < VSTI_MIN_RATE)
                rate = VSTI_MIN_RATE;
            else if (rate > VSTI_MAX_RATE)
                rate = VSTI_MAX_RATE;
            vi->sample_rate = rate;
            build_filter_for(rate);
            return 1;
        }

        case effSetBlockSize:
            return 1;

        case effMainsChanged:
            if (value == 0 && vi->is_master)
            {
                for (int ch = 0; ch < 16; ch++)
                    synth_midi((uint32_t) (0xB0 | ch), 120, 0);
                vi->queue_count = 0;
            }
            return 1;

        case effProcessEvents:
            return vsti_process_events(vi, ptr);

        case effGetEffectName:
            copy_cstring(ptr, "slopgs vsti", 32);
            return 1;

        case effGetProductString:
            copy_cstring(ptr, "slopgs MSGS VSTi", 64);
            return 1;

        case effGetVendorString:
            copy_cstring(ptr, "slopgs", 64);
            return 1;

        case effGetVendorVersion:
            return 1000;

        case effGetPlugCategory:
            return kPlugCategSynth;

        case effGetVstVersion:
            return 2400;

        case effCanDo:
        {
            const char *q = (const char *) ptr;
            if (!q)
                return 0;
            if (strcmp(q, "receiveVstEvents") == 0 ||
                strcmp(q, "receiveVstMidiEvent") == 0)
                return 1;
            return -1;
        }

        case effGetTailSize:
            return 0;

        default:
            return 0;
    }
}

#if defined(_WIN32)
#define VSTI_EXPORT __declspec(dllexport)
#else
#define VSTI_EXPORT
#endif

VSTI_EXPORT AEffect *VST2_CDECL VSTPluginMain(AudioMasterCallback host)
{
    (void) host;
    engine_init_once();

    VstiInstance *vi = (VstiInstance *) calloc(1, sizeof(VstiInstance));
    if (!vi)
        return NULL;

    vi->sample_rate = VSTI_DEFAULT_RATE;
    /* a host that never calls effSetSampleRate still gets a usable table */
    build_filter_for(VSTI_DEFAULT_RATE);
    if (!g_have_master)
    {
        vi->is_master = 1;
        g_have_master = 1;
        /* the grid's phase is arbitrary but must be reproducible: a fresh
         * master starts on a block boundary, so two runs of the same input
         * land the same way regardless of what a previous instance rendered */
        g_next_frame = g_block_start = g_out_pos = 0;
        g_block_read = SERVICE_BLOCK_FRAMES;
        /* Come up in GS mode. The driver only enters it on a GS Reset SysEx,
         * and a VST2 host delivers SysEx as kVstSysExType, which this plugin
         * does not accept -- so it could never be reached, and CC0/CC32 would
         * be dead for the plugin's whole life (synth.c gates bank-byte storage
         * on it, 0x1341e/0x13492). Set here rather than in
         * engine_init_once because synth_construct() runs synth_reset(), which
         * clears it, so it has to be re-established per master instance. */
        g_gs_mode = 1;
    }

    AEffect *e = &vi->effect;
    e->magic = kVstMagic;
    e->dispatcher = vsti_dispatch;
    e->process = NULL;
    e->setParameter = vsti_set_parameter;
    e->getParameter = vsti_get_parameter;
    e->numPrograms = 1;
    e->numParams = 0;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth;
    /* object, never user -- confectionery's getPlugin doc records a real
     * host overwriting `user` between VSTPluginMain returning and the first
     * dispatch. */
    e->object = vi;
    e->uniqueID = 0x736c7067; /* 's','l','p','g' */
    e->version = 1;
    e->processReplacing = vsti_process_replacing;
    e->processDoubleReplacing = NULL;

    return e;
}

#ifdef _WIN32
__asm__(".section .drectve\n\t.ascii \" -export:main=VSTPluginMain\"\n\t"
        ".section .text\n");
#endif
