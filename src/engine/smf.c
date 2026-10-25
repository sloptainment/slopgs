/* smf.c -- Standard MIDI File parser and sequencer. Not part of the original
 * driver: ordinary generic SMF handling. */
#include "smf.h"
#include "render.h"
#include "rt.h"
#include "synth.h"
#include "voice.h"

enum
{
    EKIND_MIDI = 0,
    EKIND_SYSEX,
    EKIND_TEMPO,
};

#define MAX_TRACKS 512

typedef struct Event
{
    uint32_t abs_tick;
    uint32_t seq;
    uint8_t kind; // EKIND_*
    uint8_t status, d1, d2;
    const uint8_t *data; // kind == EKIND_SYSEX
    uint32_t data_len;
    uint32_t tempo_usec;  // kind == EKIND_TEMPO
    uint32_t sample_time; // computed
} Event;

static Event *g_events = 0;
static uint32_t g_event_count = 0;
static uint32_t g_song_length_samples = 0;

static uint32_t g_event_index = 0;
static uint32_t g_sample_pos = 0;
static int32_t g_loop_remaining = 0; /* -1 = infinite, 0 = no more loops */
static int g_finished = 0;
static int g_loaded = 0;

static uint32_t rd_u32be(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
static uint16_t rd_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint32_t) p[0] << 8) | p[1]);
}
static int fourcc_is(const uint8_t *p, char a, char b, char c, char d)
{
    return p[0] == (uint8_t) a && p[1] == (uint8_t) b && p[2] == (uint8_t) c &&
           p[3] == (uint8_t) d;
}

static uint32_t read_vlq(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    const uint8_t *p = *pp;
    while (p < end)
    {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    *pp = p;
    return v;
}

/* Walks one MTrk's bytes, calling `emit` per event with abs_tick, or just
 * counting if `emit` is 0. Returns event count. */
typedef void (*EmitFn)(uint32_t abs_tick, uint32_t seq, uint8_t status,
                       uint8_t d1, uint8_t d2, const uint8_t *sysex_data,
                       uint32_t sysex_len, uint32_t tempo_usec, uint8_t kind,
                       void *ctx);

static uint32_t walk_track(const uint8_t *data, uint32_t len, EmitFn emit,
                           void *ctx, uint32_t seq_base)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t abs_tick = 0;
    uint8_t running_status = 0;
    uint32_t count = 0;

    while (p < end)
    {
        uint32_t delta = read_vlq(&p, end);
        abs_tick += delta;
        if (p >= end)
            break;
        uint8_t b = *p;

        if (b == 0xFF)
        { /* meta event */
            p++;
            if (p >= end)
                break;
            uint8_t type = *p++;
            uint32_t mlen = read_vlq(&p, end);
            if (p + mlen > end)
                break;
            if (type == 0x51 && mlen == 3)
            {
                uint32_t usec =
                    ((uint32_t) p[0] << 16) | ((uint32_t) p[1] << 8) | p[2];
                if (emit)
                    emit(abs_tick, seq_base + count, 0, 0, 0, 0, 0, usec,
                         EKIND_TEMPO, ctx);
                count++;
            }
            /* other meta types (track name, end-of-track, etc.): ignored */
            p += mlen;
            /* sysex (and sysex continuation, treated the same) */
        }
        else if (b == 0xF0 || b == 0xF7)
        {
            p++;
            uint32_t slen = read_vlq(&p, end);
            if (p + slen > end)
                break;
            if (b == 0xF0)
            {
                if (emit)
                    emit(abs_tick, seq_base + count, 0, 0, 0, p, slen, 0,
                         EKIND_SYSEX, ctx);
                count++;
            }
            p += slen;
        }
        else if (b & 0x80)
        { /* new status byte */
            running_status = b;
            p++;
            uint8_t kind = b & 0xF0;
            uint8_t d1 = (p < end) ? *p++ : 0;
            uint8_t d2 = 0;
            if (kind != 0xC0 && kind != 0xD0)
            {
                d2 = (p < end) ? *p++ : 0;
            }
            if (emit)
                emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0,
                     0, EKIND_MIDI, ctx);
            count++;
        }
        else
        {   /* running status data byte */
            /* malformed: skip byte defensively */
            if (running_status == 0)
            {
                p++;
                continue;
            }
            uint8_t kind = running_status & 0xF0;
            uint8_t d1 = b;
            p++;
            uint8_t d2 = 0;
            if (kind != 0xC0 && kind != 0xD0)
            {
                d2 = (p < end) ? *p++ : 0;
            }
            if (emit)
                emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0,
                     0, EKIND_MIDI, ctx);
            count++;
        }
    }
    return count;
}

typedef struct
{
    Event *arr;
    uint32_t idx;
} FillCtx;

static void fill_cb(uint32_t abs_tick, uint32_t seq, uint8_t status, uint8_t d1,
                    uint8_t d2, const uint8_t *sd, uint32_t sl, uint32_t tempo,
                    uint8_t kind, void *ctxp)
{
    FillCtx *ctx = (FillCtx *) ctxp;
    Event *e = &ctx->arr[ctx->idx++];
    e->abs_tick = abs_tick;
    e->seq = seq;
    e->kind = kind;
    e->status = status;
    e->d1 = d1;
    e->d2 = d2;
    e->data = sd;
    e->data_len = sl;
    e->tempo_usec = tempo;
    e->sample_time = 0;
}

/* [F] Bank/Program look-ahead: at one tick a note takes the last Program
 * Change before the NEXT note event of that tick, not the one before it in the
 * stream. Probe 40 scores 13/13 (stream order 11/13, highest-bank 8/13); probe
 * 38 7/8. Probe 38 case F contradicts probe 40 case G on identical input, and
 * the driver's own read (0x16c50: every pending value <= the query time) does
 * not predict this -- INVESTIGATE.adoc#i-bankprog-lookahead. */
#ifndef SMF_BANKPROG_LOOKAHEAD
#define SMF_BANKPROG_LOOKAHEAD 1
#endif

/* [A:0x13054]: drains every event due in the buffer (pos + nframes) before
 * rendering it -- the note-off early release probe 46 measures (0-27 ms).
 * Length = SERVICE_BLOCK_FRAMES (voice.h). */

static int is_note_event(const Event *e)
{
    if (e->kind != EKIND_MIDI)
        return 0;
    if ((e->status & 0xF0) == 0x80 || (e->status & 0xF0) == 0x90)
        return 1;
    return 0;
}

static int is_bankprog_event(const Event *e)
{
    if (e->kind != EKIND_MIDI)
        return 0;
    if ((e->status & 0xF0) == 0xC0)
        return 1;
    return (e->status & 0xF0) == 0xB0 && (e->d1 == 0 || e->d1 == 32);
}

/* One pass per equal-tick run: each note bubbles past the Bank Select/Program
 * Change run immediately after it, stopping at another note or anything else.
 */
static void slide_notes_past_bankprog(Event *arr, uint32_t n)
{
    uint32_t i = 0;
    while (i < n)
    {
        uint32_t end = i;
        while (end < n && arr[end].abs_tick == arr[i].abs_tick)
            end++;
        for (uint32_t k = i; k < end; k++)
        {
            if (!is_note_event(&arr[k]))
                continue;
            uint32_t j = k;
            while (j + 1 < end && is_bankprog_event(&arr[j + 1]))
            {
                Event tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
                j++;
            }
        }
        i = end;
    }
}

static int event_less(const Event *a, const Event *b)
{
    if (a->abs_tick != b->abs_tick)
        return a->abs_tick < b->abs_tick;
    return a->seq < b->seq;
}

/* Stable bottom-up merge sort using one scratch buffer (bump-allocated once).
 */
static void merge_sort_events(Event *arr, uint32_t n)
{
    if (n < 2)
        return;
    Event *tmp = (Event *) rt_alloc(n * sizeof(Event));
    for (uint32_t width = 1; width < n; width *= 2)
    {
        for (uint32_t i = 0; i < n; i += 2 * width)
        {
            uint32_t left = i, mid = i + width < n ? i + width : n,
                     right = i + 2 * width < n ? i + 2 * width : n;
            uint32_t a = left, b = mid, k = left;
            while (a < mid && b < right)
            {
                if (event_less(&arr[a], &arr[b]))
                    tmp[k++] = arr[a++];
                else
                    tmp[k++] = arr[b++];
            }
            while (a < mid)
                tmp[k++] = arr[a++];
            while (b < right)
                tmp[k++] = arr[b++];
        }
        for (uint32_t i = 0; i < n; i++)
            arr[i] = tmp[i];
    }
}

int smf_load(const uint8_t *data, uint32_t len)
{
    g_loaded = 0;
    g_events = 0;
    g_event_count = 0;
    g_song_length_samples = 0;
    /* a new song starts at its own tick 0, not wherever the previous one
     * stopped */
    smf_rewind();

    if (len < 14)
        return -1;
    if (!fourcc_is(data, 'M', 'T', 'h', 'd'))
        return -1;
    uint32_t hdr_len = rd_u32be(data + 4);
    if (hdr_len < 6 || 8 + hdr_len > len)
        return -1;
    uint16_t format = rd_u16be(data + 8);
    uint16_t ntracks = rd_u16be(data + 10);
    uint16_t division = rd_u16be(data + 12);
    (void) format;

    const uint8_t *p = data + 8 + hdr_len;
    const uint8_t *end = data + len;

    /* Pass 1: locate each track's byte range, and count events. */
    const uint8_t *track_ptrs[MAX_TRACKS];
    uint32_t track_lens[MAX_TRACKS];
    uint32_t nt = 0;
    while (p + 8 <= end && nt < MAX_TRACKS && nt < ntracks)
    {
        if (!fourcc_is(p, 'M', 'T', 'r', 'k'))
            break;
        uint32_t tlen = rd_u32be(p + 4);
        const uint8_t *tdata = p + 8;
        if (tdata + tlen > end)
            break;
        track_ptrs[nt] = tdata;
        track_lens[nt] = tlen;
        nt++;
        p = tdata + tlen;
    }

    uint32_t total = 0;
    uint32_t seq_base = 0;
    uint32_t track_seq_bases[MAX_TRACKS];
    for (uint32_t i = 0; i < nt; i++)
    {
        track_seq_bases[i] = seq_base;
        uint32_t c = walk_track(track_ptrs[i], track_lens[i], 0, 0, seq_base);
        total += c;
        seq_base += c + 1;
    }

    if (total == 0)
    {
        g_loaded = 1; /* an empty song is a valid (trivially silent) load */
        return 0;
    }

    Event *arr = (Event *) rt_alloc(total * sizeof(Event));
    FillCtx fc;
    fc.arr = arr;
    fc.idx = 0;
    for (uint32_t i = 0; i < nt; i++)
    {
        walk_track(track_ptrs[i], track_lens[i], fill_cb, &fc,
                   track_seq_bases[i]);
    }

    merge_sort_events(arr, total);
#if SMF_BANKPROG_LOOKAHEAD
    slide_notes_past_bankprog(arr, total);
#endif

    /* Tempo map pass: convert abs_tick -> sample_time. */
    int smpte = (division & 0x8000) != 0;
    double ticks_per_second = 0.0;
    if (smpte)
    {
        int fps = -(int8_t)(division >> 8);
        int tpf = division & 0xFF;
        if (fps <= 0)
            fps = 30;
        ticks_per_second = (double) fps * (double) tpf;
    }
    uint32_t division_q = smpte ? 1 : (division & 0x7FFF);
    if (division_q == 0)
        division_q = 24;

    double time_accum = 0.0;
    uint32_t last_tick = 0;
    uint32_t cur_tempo = 500000;

    for (uint32_t i = 0; i < total; i++)
    {
        Event *e = &arr[i];
        uint32_t delta_ticks = e->abs_tick - last_tick;
        if (smpte)
        {
            time_accum = (double) e->abs_tick / ticks_per_second;
        }
        else
        {
            double seconds_per_tick =
                ((double) cur_tempo / 1000000.0) / (double) division_q;
            time_accum += (double) delta_ticks * seconds_per_tick;
        }
        last_tick = e->abs_tick;
        double st = time_accum * (double) RENDER_RATE + 0.5;
        e->sample_time = (st < 0.0) ? 0 : (uint32_t) st;
        if (e->kind == EKIND_TEMPO)
            cur_tempo = e->tempo_usec;
    }

    g_events = arr;
    g_event_count = total;
    g_song_length_samples = arr[total - 1].sample_time;
    g_loaded = 1;
    return 0;
}

void smf_set_loop(int32_t loops) { g_loop_remaining = loops; }

static void dispatch_event(Event *e)
{
    switch (e->kind)
    {
        case EKIND_MIDI:
            return synth_midi(e->status, e->d1, e->d2);
        case EKIND_SYSEX:
            return synth_sysex(e->data, e->data_len);
        case EKIND_TEMPO:
            /* no action, already folded into sample_time precompute */
            break;
    }
}

/* Rewinds to tick 0 and clears the finished latch -- without this, a replayed
 * song would report "finished" on its very first render call. */
void smf_rewind(void)
{
    g_event_index = 0;
    g_sample_pos = 0;
    g_finished = 0;
}

uint32_t smf_render(int16_t *out, uint32_t frames)
{
    if (!g_loaded)
    {
        /* No SMF loaded: msgs_render still renders active voices from direct
         * msgs_midi() injection (ABI requirement). */
        render_frames(out, frames);
        return frames;
    }

    uint32_t produced = 0;
    while (produced < frames)
    {
        while (g_event_index < g_event_count &&
               g_events[g_event_index].sample_time <= g_sample_pos)
        {
            dispatch_event(&g_events[g_event_index]);
            g_event_index++;
        }

        int song_ended = (g_event_index >= g_event_count) &&
                         (g_sample_pos >= g_song_length_samples);
        if (song_ended)
        {
            if (g_loop_remaining != 0)
            {
                if (g_loop_remaining > 0)
                    g_loop_remaining--;
                smf_rewind();
                continue;
            }
            else
            {
                render_frames(out + produced * 2, frames - produced);
                produced = frames;
                if (!voice_any_active())
                    g_finished = 1;
                break;
            }
        }

        /* Blocks are anchored to the absolute sample position, not the call's
         * start, so the grid doesn't move with the host's buffer size. */
        uint32_t chunk =
            SERVICE_BLOCK_FRAMES - (g_sample_pos % SERVICE_BLOCK_FRAMES);
        if (chunk > frames - produced)
            chunk = frames - produced;

        if (chunk == 0)
            chunk = 1;

        /* Drain-ahead: every event due in this block acts now; only a note-on
         * uses its true sub-block offset; a USE RHYTHM PART cut lands at the
         * block top, as the reference's is instantaneous (probe 48). */
        while (g_event_index < g_event_count &&
               g_events[g_event_index].sample_time < g_sample_pos + chunk)
        {
            voice_set_event_offset(g_events[g_event_index].sample_time -
                                   g_sample_pos);
            dispatch_event(&g_events[g_event_index]);
            g_event_index++;
        }
        voice_set_event_offset(0);
        render_frames(out + produced * 2, chunk);
        produced += chunk;
        g_sample_pos += chunk;
    }
    return produced;
}

int32_t smf_is_finished(void) { return g_finished; }
