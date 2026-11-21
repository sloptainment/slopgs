/* engine.h -- one engine instance: every piece of state that used to be a C
 * global, so instances share nothing. msgs.h hands it out opaque. */
#ifndef ENGINE_H
#define ENGINE_H

#include "dls.h"
#include "msgs.h"
#include "rt.h"
#include "synth.h"
#include "tables.h"
#include "voice.h"

struct Msgs
{
    Tables tab;

    /* dls.c */
    DlsCollection dls;
    /* Sticky malformed-chunk HRESULT (S2.2.2 error table); negative as
     * int32, so dls_load returns it directly. Any nonzero value aborts the
     * whole load. */
    int32_t parse_err;

    /* synth.c */
    Channel channels[16];
    /* gates CC0/CC32 storage and the per-part DT1 handlers (0x1341e,
     * 0x13492, 0x1376e, 0x137a2, 0x137b7) */
    uint8_t gs_mode;
    /* master volume attenuation, hundredths of a dB */
    int32_t master_vol_hdb;
    /* RCV CHANNEL map (device+0x12fc). The driver keeps all part state per
     * Part and fans each channel message out to every Part mapped to it
     * (0x13267); here channels own the state and only the per-part SysEx
     * handlers read this map -- equivalent while it stays a permutation,
     * INVESTIGATE.adoc#i-part-model. */
    uint8_t part_to_channel[16];
    /* Program Change locale nodes a service call has not folded yet
     * (0x16b46), with the program and locale from before the first of them */
    uint8_t pc_pending[16];
    uint8_t pc_program[16];
    uint32_t pc_locale[16];
    CtlQueue ctl[16][CTL_COUNT];

    /* voice.c */
    Voice voices[NUM_VOICES];
    uint32_t voice_age_counter;
    /* top-up tick clock, see voice_topup_tick */
    uint32_t topup_accum;
    /* RENDER_RATE frames rendered so far; render_frames advances it */
    uint64_t now;
    /* Where inside the current render call the event being dispatched
     * actually falls; smf.c sets it per event, direct msgs_midi injection
     * leaves it at 0 ("now"). Note-on, note-off and the controller queues
     * read it. */
    uint32_t event_offset;

    /* smf.c */
    struct
    {
        struct Event *events;
        uint32_t event_count;
        uint32_t song_length_samples;
        uint32_t event_index;
        uint32_t sample_pos;
        int32_t loop_remaining; /* -1 = infinite, 0 = no more loops */
        int finished;
        int loaded;
        uint32_t jitter_ms; /* smf_set_jitter_ms */
    } smf;

    /* the rest of the msgs_create block; a song's events start at song_mark,
     * just past the DLS, so each load reuses the previous song's space */
    Arena arena;
    uint32_t song_mark;
};

#endif /* ENGINE_H */
