/* smf.c -- Standard MIDI File parser and sequencer. Not part of the original
 * driver: ordinary generic SMF handling. */
#include "smf.h"
#include "engine.h"
#include "render.h"

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
static int merge_sort_events(Msgs *e, Event *arr, uint32_t n)
{
    if (n < 2)
        return 0;
    Event *tmp = (Event *) rt_alloc(&e->arena, n * sizeof(Event));
    if (!tmp)
        return MSGS_ENOMEM;
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
    return 0;
}

/* every event takes at least two track bytes (delta + one data byte), and a
 * load holds the event array and its sort scratch at once */
uint32_t smf_mem_bound(uint32_t len)
{
    uint64_t n = (uint64_t) len * sizeof(Event) + 16;
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t) n;
}

int smf_load(Msgs *e, const uint8_t *data, uint32_t len)
{
    /* the previous song's events are the only thing past the DLS */
    e->arena.used = e->song_mark;
    e->smf.loaded = 0;
    e->smf.events = 0;
    e->smf.event_count = 0;
    e->smf.song_length_samples = 0;
    /* a new song starts at its own tick 0, not wherever the previous one
     * stopped */
    smf_rewind(e);

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
        e->smf.loaded =
            1; /* an empty song is a valid (trivially silent) load */
        return 0;
    }

    Event *arr = (Event *) rt_alloc(&e->arena, total * sizeof(Event));
    if (!arr)
        return MSGS_ENOMEM;
    FillCtx fc;
    fc.arr = arr;
    fc.idx = 0;
    for (uint32_t i = 0; i < nt; i++)
    {
        walk_track(track_ptrs[i], track_lens[i], fill_cb, &fc,
                   track_seq_bases[i]);
    }

    if (merge_sort_events(e, arr, total))
        return MSGS_ENOMEM;
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
        /* rounded on the BASE_RATE grid, so every rate sees the same frames */
        double st = time_accum * (double) BASE_RATE + 0.5;
        e->sample_time = (st < 0.0) ? 0 : (uint32_t) st * RESAMPLE_FACTOR;
        if (e->kind == EKIND_TEMPO)
            cur_tempo = e->tempo_usec;
    }

    /* Each tick's events arrive late by one uniform draw, never reordered.
     * xorshift32 reseeded per load, so a render replays exactly; drawn in
     * 1/1024ths of the span, so the realization is rate-independent. */
    if (e->smf.jitter_ms)
    {
        uint32_t span = e->smf.jitter_ms * BASE_RATE / 1000;
        uint32_t x = 2463534242u, late = 0, last = 0;
        for (uint32_t i = 0; i < total; i++)
        {
            if (i == 0 || arr[i].abs_tick != arr[i - 1].abs_tick)
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                late = (((x >> 22) * span) >> 10) * RESAMPLE_FACTOR;
            }
            uint32_t t = arr[i].sample_time + late;
            last = t > last ? t : last;
            arr[i].sample_time = last;
        }
    }

    e->smf.events = arr;
    e->smf.event_count = total;
    e->smf.song_length_samples = arr[total - 1].sample_time;
    e->smf.loaded = 1;
    return 0;
}

void smf_set_loop(Msgs *e, int32_t loops) { e->smf.loop_remaining = loops; }

/* clamped to 1 s, which keeps the span arithmetic within 32 bits */
void smf_set_jitter_ms(Msgs *e, uint32_t ms)
{
    e->smf.jitter_ms = ms > 1000 ? 1000 : ms;
}

static void dispatch_event(Msgs *e, Event *ev)
{
    switch (ev->kind)
    {
        case EKIND_MIDI:
            return synth_midi(e, ev->status, ev->d1, ev->d2);
        case EKIND_SYSEX:
            return synth_sysex(e, ev->data, ev->data_len);
        case EKIND_TEMPO:
            /* no action, already folded into sample_time precompute */
            break;
    }
}

/* Rewinds to tick 0 and clears the finished latch -- without this, a replayed
 * song would report "finished" on its very first render call. */
void smf_rewind(Msgs *e)
{
    e->smf.event_index = 0;
    e->smf.sample_pos = 0;
    e->smf.finished = 0;
}

uint32_t smf_render(Msgs *e, int16_t *out, uint32_t frames)
{
    if (!e->smf.loaded)
    {
        /* No SMF loaded: msgs_render still renders active voices from direct
         * msgs_midi() injection (ABI requirement). */
        render_frames(e, out, frames);
        return frames;
    }

    uint32_t produced = 0;
    while (produced < frames)
    {
        while (e->smf.event_index < e->smf.event_count &&
               e->smf.events[e->smf.event_index].sample_time <=
                   e->smf.sample_pos)
        {
            dispatch_event(e, &e->smf.events[e->smf.event_index]);
            e->smf.event_index++;
        }

        int song_ended = (e->smf.event_index >= e->smf.event_count) &&
                         (e->smf.sample_pos >= e->smf.song_length_samples);
        if (song_ended)
        {
            if (e->smf.loop_remaining != 0)
            {
                if (e->smf.loop_remaining > 0)
                    e->smf.loop_remaining--;
                smf_rewind(e);
                continue;
            }
            else
            {
                /* the tail too renders in Mix-sized calls on the block grid:
                   a segment never runs past a call's end */
                uint32_t tail = SERVICE_BLOCK_FRAMES -
                                (e->smf.sample_pos % SERVICE_BLOCK_FRAMES);
                if (tail > frames - produced)
                    tail = frames - produced;
                render_frames(e, out + produced * 2, tail);
                produced += tail;
                e->smf.sample_pos += tail;
                if (produced < frames)
                    continue;
                if (!voice_any_active(e))
                    e->smf.finished = 1;
                break;
            }
        }

        /* Blocks are anchored to the absolute sample position, not the call's
         * start, so the grid doesn't move with the host's buffer size. */
        uint32_t chunk =
            SERVICE_BLOCK_FRAMES - (e->smf.sample_pos % SERVICE_BLOCK_FRAMES);
        if (chunk > frames - produced)
            chunk = frames - produced;

        if (chunk == 0)
            chunk = 1;

        /* Drain-ahead: every event due in this block is dispatched now with
         * its sub-block offset; note-ons, note-offs and the live controllers
         * act at that time, everything else (a USE RHYTHM PART cut, probe 48)
         * at the block top. */
        while (e->smf.event_index < e->smf.event_count &&
               e->smf.events[e->smf.event_index].sample_time <
                   e->smf.sample_pos + chunk)
        {
            voice_set_event_offset(
                e, e->smf.events[e->smf.event_index].sample_time -
                       e->smf.sample_pos);
            dispatch_event(e, &e->smf.events[e->smf.event_index]);
            e->smf.event_index++;
        }
        voice_set_event_offset(e, 0);
        render_frames(e, out + produced * 2, chunk);
        produced += chunk;
        e->smf.sample_pos += chunk;
    }
    return produced;
}

int32_t smf_is_finished(Msgs *e) { return e->smf.finished; }
