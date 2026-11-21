/* synth.c -- channel state, MIDI dispatch, CC/RPN/SysEx. */
#include "engine.h"

/* static table at 0x1a600: Part i's default physical channel; reloaded on
 * every MIDI-level reset (0x123a4). */
static const uint8_t kPartChannelDefault[16] = {9, 0, 1,  2,  3,  4,  5,  6,
                                                7, 8, 10, 11, 12, 13, 14, 15};

/* 0x150bc acquire half; guarded so a re-acquire can't resurrect a failed
 * load. */
static void acquire_collection(Msgs *e)
{
    if (e->dls.first_instrument)
        e->dls.valid = 1;
}

/* 0x16df4: recomputes the scheduled locale from current bank+program (drum
 * bit OR'd in live at note-on, 0x12dd9). Called only from
 * Program Change and the reset paths; CC0/CC32 deliberately do not (see
 * synth.h). */
static void relatch_locale(Channel *c)
{
    c->scheduled_locale = (uint32_t) c->program |
                          (((uint32_t) c->bank_lsb & 0x7f) << 7) |
                          (((uint32_t) c->bank_msb & 0x7f) << 14);
}

/* Fields the three reset paths touch; no reset writes RPN0, the RPN-select
 * register or the sustain byte (0x12354, 0x126c2, 0x12780). */
static void channel_defaults(Channel *c, int idx)
{
    c->bank_msb = 0;
    c->bank_lsb = 0;
    c->program = 0;
    relatch_locale(c); /* -> 0 */
    c->volume = 100;
    c->expression = 127;
    c->pan = 64;
    c->modulation = 0;
    c->pitch_bend = 8192;
    c->rpn1_fine_cents = 0;
    c->rpn2_coarse_cents = 0;
    c->data_entry_combined = 0;
    /* Part 0 (channel 10) is the default rhythm part (0x123ce) */
    c->is_rhythm = (idx == 9) ? 1 : 0;
    c->mono_mode = 0;
    for (int k = 0; k < 12; k++)
        c->scale_tuning[k] = 0;
}

static void channel_construct_only(Channel *c)
{
    c->pb_range_cents = 200;
    c->rpn_select = 0x3FFF;
    c->sustain = 0;
}

static void ctl_clear(Msgs *e);

void synth_service_tick(Msgs *e)
{
    for (int i = 0; i < 16; i++)
        e->pc_pending[i] = 0;
}

void synth_reset(Msgs *e)
{
    // every reset path deletes the queued nodes (0x1252a)
    synth_service_tick(e);
    e->gs_mode = 0;
    e->master_vol_hdb = 0;
    for (int i = 0; i < 16; i++)
    {
        channel_defaults(&e->channels[i], i);
        /* RCV CHANNEL reload */
        e->part_to_channel[i] = kPartChannelDefault[i];
    }
    voice_pool_reset(e);
    /* synth_reset() is reached by System Reset, GS Reset and GM System On/Off;
     * System Reset and GM System On both acquire the collection (0x13218,
     * 0x13700). */
    acquire_collection(e);
    ctl_clear(e);
}

/* Device construction/open: power-on defaults + synth_reset() body; resets
 * must not touch RPN0/RPN-select/sustain. */
void synth_construct(Msgs *e)
{
    for (int i = 0; i < 16; i++)
        channel_construct_only(&e->channels[i]);
    synth_reset(e);
}

/* A/B guard for measurement only: 0 recomputes the locale live from the bank
 * bytes, so the latch can be measured by flipping one macro on one tree. */
#ifndef SYNTH_SCHEDULE_LOCALE
#define SYNTH_SCHEDULE_LOCALE 1
#endif

uint32_t synth_channel_locale(Msgs *e, int ch)
{
    /* bank/program half is the latched value (relatch_locale); only the drum
     * bit is read live at query time (0x12dcf) */
    Channel *c = &e->channels[ch];
#if SYNTH_SCHEDULE_LOCALE
    uint32_t locale = c->scheduled_locale;
#else
    uint32_t locale = (uint32_t) c->program |
                      (((uint32_t) c->bank_lsb & 0x7f) << 7) |
                      (((uint32_t) c->bank_msb & 0x7f) << 14);
#endif
    if (c->is_rhythm)
        locale |= 0x80000000u;
    return locale;
}

int32_t synth_pitch_bend_cents(Msgs *e, int ch)
{
    Channel *c = &e->channels[ch];
    int32_t raw = (int32_t) c->pitch_bend - 8192;
    /* (raw*rangeCents)>>13, an arithmetic shift: floor, not truncation
     * (0x16d7c). `/` would differ by one cent on most downward bends. */
    return (int32_t)(((int64_t) raw * c->pb_range_cents) >> 13);
}

static uint16_t ctl_live(const Channel *c, int k)
{
    switch (k)
    {
        case CTL_MOD:
            return c->modulation;
        case CTL_VOL:
            return c->volume;
        case CTL_PAN:
            return c->pan;
        case CTL_EXPR:
            return c->expression;
        default:
            return c->pitch_bend;
    }
}

static void ctl_clear(Msgs *e)
{
    for (int ch = 0; ch < 16; ch++)
        for (int k = 0; k < CTL_COUNT; k++)
        {
            e->ctl[ch][k].n = 0;
            e->ctl[ch][k].cur = ctl_live(&e->channels[ch], k);
        }
}

/* after each event: whatever it changed is queued at the event's time,
 * however it was changed (CC, reset, SysEx) */
static void ctl_record(Msgs *e)
{
    uint64_t t = voice_event_time(e);
    for (int ch = 0; ch < 16; ch++)
        for (int k = 0; k < CTL_COUNT; k++)
        {
            uint16_t v = ctl_live(&e->channels[ch], k);
            uint8_t n = e->ctl[ch][k].n;
            if (v == (n ? e->ctl[ch][k].q[n - 1].v : e->ctl[ch][k].cur))
                continue;
            /* ponytail: a full queue folds its oldest node early; 64 is
               ~16 events per channel per ms at the 1024-frame call */
            if (n == CTL_QUEUE)
            {
                e->ctl[ch][k].cur = e->ctl[ch][k].q[0].v;
                for (uint8_t j = 1; j < n; j++)
                    e->ctl[ch][k].q[j - 1] = e->ctl[ch][k].q[j];
                n--;
            }
            e->ctl[ch][k].q[n].t = t;
            e->ctl[ch][k].q[n].v = v;
            e->ctl[ch][k].n = n + 1;
        }
}

void synth_ctl_fold(Msgs *e, uint64_t end)
{
    for (int ch = 0; ch < 16; ch++)
        for (int k = 0; k < CTL_COUNT; k++)
        {
            uint8_t n = e->ctl[ch][k].n, i = 0;
            while (i < n && e->ctl[ch][k].q[i].t < end)
                e->ctl[ch][k].cur = e->ctl[ch][k].q[i++].v;
            for (uint8_t j = i; j < n; j++)
                e->ctl[ch][k].q[j - i] = e->ctl[ch][k].q[j];
            e->ctl[ch][k].n = n - i;
        }
}

uint32_t synth_ctl_at(Msgs *e, int ch, int k, uint64_t t)
{
    /* nothing pending: the live field, which unit fixtures also write */
    if (!e->ctl[ch][k].n)
        return ctl_live(&e->channels[ch], k);
    for (int i = e->ctl[ch][k].n - 1; i >= 0; i--)
        if (e->ctl[ch][k].q[i].t <= t)
            return e->ctl[ch][k].q[i].v;
    return e->ctl[ch][k].cur;
}

int32_t synth_pitch_bend_cents_at(Msgs *e, int ch, uint64_t t)
{
    int32_t raw = (int32_t) synth_ctl_at(e, ch, CTL_BEND, t) - 8192;
    return (int32_t) (((int64_t) raw * e->channels[ch].pb_range_cents) >> 13);
}

static void recompute_rpn1(Channel *c)
{
    int32_t combined = c->data_entry_combined;
    c->rpn1_fine_cents = (int16_t)(((combined - 8192) * 100) / 8192);
}

static void cc_data_entry_msb(Channel *c, uint32_t d2)
{
    c->data_entry_combined =
        (uint16_t)((c->data_entry_combined & 0x7F) | ((d2 & 0x7F) << 7));
    if (c->rpn_select == 0)
    {
        c->pb_range_cents = (uint16_t)(d2 * 100);
    }
    else if (c->rpn_select == 1)
    {
        recompute_rpn1(c);
    }
    else if (c->rpn_select == 2)
    {
        c->rpn2_coarse_cents = (int16_t)(((int32_t) d2 - 64) * 100);
    }
}

static void cc_data_entry_lsb(Channel *c, uint32_t d2)
{
    c->data_entry_combined =
        (uint16_t)((c->data_entry_combined & (0x7F << 7)) | (d2 & 0x7F));
    if (c->rpn_select == 1)
    {
        recompute_rpn1(c);
    }
    /* RPN0/RPN2 do not consume the LSB half (0x13404, 0x133ed). */
}

/* CC121 [A:0x1351f]-[A:0x135a9]: Volume and Pan re-schedule only when the
 * value byte is non-zero; Expression/Bend/Modulation are unconditional. */
static void reset_all_channel_controllers(Channel *c, uint32_t val)
{
    c->modulation = 0;
    c->pitch_bend = 8192;
    if (val != 0)
    {
        c->volume = 100;
        c->pan = 64;
    }
    c->expression = 127;
}

/* ResetAllChannelControllers (0x126c2), reached by the reset paths and USE
 * RHYTHM PART. Unlike CC121 above, this always resets Channel Volume,
 * across every channel. */
static void reset_all_channel_controllers_device(Msgs *e)
{
    /* ponytail: voice_pool_reset() also zeroes the reserve top-up accumulator
     * and voice age counter, which 0x123de does not -- harmless since every
     * voice is already inactive here. */
    voice_pool_reset(e);
    for (int i = 0; i < 16; i++)
    {
        Channel *c = &e->channels[i];
        c->modulation = 0;
        c->pitch_bend = 8192;
        c->volume = 100;
        c->pan = 64;
        c->expression = 127;
        /* sustain byte not touched here (queues sentinel-0xFE only); no
         * voice_sustain_lift() needed since voice_pool_reset() above already
         * cut every voice */
    }
}

static void dispatch_cc(Msgs *e, int ch, uint32_t cc, uint32_t val)
{
    Channel *c = &e->channels[ch];
    switch (cc)
    {
        /* CC0/CC32 write bank bytes directly (0x16dc4/0x16ddc) without
         * re-latching the scheduled locale; only Program Change/reset does
         * that. */
        case 0:
            if (e->gs_mode)
                c->bank_msb = (uint8_t) val;
            break;
        case 1:
            c->modulation = (uint8_t) val;
            break;
        case 6:
            cc_data_entry_msb(c, val);
            break;
        case 7:
            c->volume = (uint8_t) val;
            break;
        case 10:
            c->pan = (uint8_t) val;
            break;
        case 11:
            c->expression = (uint8_t) val;
            break;
        case 32:
            if (e->gs_mode)
                c->bank_lsb = (uint8_t) val;
            break;
        case 38:
            cc_data_entry_lsb(c, val);
            break;
        case 64:
            c->sustain = (uint8_t) val;
            if (val == 0)
                voice_sustain_lift(e, ch);
            break;
        case 98:
        case 99:
            c->rpn_select = 0x3FFF;
            break;
        case 100:
            c->rpn_select =
                (uint16_t)((c->rpn_select & (0x7F << 7)) | (val & 0x7F));
            break;
        case 101:
            c->rpn_select =
                (uint16_t)((c->rpn_select & 0x7F) | ((val & 0x7F) << 7));
            break;
        case 120:
            voice_all_sound_off(e, ch);
            break;
        case 121:
            reset_all_channel_controllers(c, val);
            voice_sustain_lift(e, ch);
            break;
        case 123:
            voice_all_notes_off(e, ch);
            break;
        case 126:
            c->mono_mode = 1;
            voice_all_sound_off(e, ch);
            break;
        case 127:
            c->mono_mode = 0;
            voice_all_sound_off(e, ch);
            break;
            /* every other CC: discarded by the dispatch chain */
        default:
            break;
    }
}

static void midi(Msgs *e, uint32_t status, uint32_t d1, uint32_t d2)
{
    if (status == 0xFF)
    {
        /* 0x13202 resets programs, then 0x1320f's controller reset cancels
         * every queued event, the program-0 locale included: each channel
         * keeps its last folded scheduled locale and program, so a Program
         * Change no service call has seen yet is lost. */
        uint8_t program[16];
        uint32_t locale[16];
        for (int i = 0; i < 16; i++)
        {
            program[i] =
                e->pc_pending[i] ? e->pc_program[i] : e->channels[i].program;
            locale[i] = e->pc_pending[i] ? e->pc_locale[i]
                                         : e->channels[i].scheduled_locale;
        }
        synth_reset(e);
        for (int i = 0; i < 16; i++)
        {
            e->channels[i].program = program[i];
            e->channels[i].scheduled_locale = locale[i];
        }
        return;
    }
    uint32_t kind = status & 0xF0;
    int ch = (int) (status & 0x0F);

    switch (kind)
    {
        case 0x80:
            voice_note_off(e, ch, (int) d1);
            break;
        case 0x90:
            if (d2 == 0)
                voice_note_off(e, ch, (int) d1);
            else
                voice_note_on(e, ch, (int) d1, (int) d2);
            break;
        case 0xB0:
            dispatch_cc(e, ch, d1, d2);
            break;
        case 0xC0:
            if (!e->pc_pending[ch])
            {
                e->pc_pending[ch] = 1;
                e->pc_program[ch] = e->channels[ch].program;
                e->pc_locale[ch] = e->channels[ch].scheduled_locale;
            }
            e->channels[ch].program = (uint8_t) d1;
            /* schedules the new locale from the already-selected bank */
            relatch_locale(&e->channels[ch]);
            break;
        case 0xE0:
            e->channels[ch].pitch_bend =
                (uint16_t) (((d2 & 0x7F) << 7) | (d1 & 0x7F));
            break;
        default:
            break; /* poly pressure, channel pressure: not modeled */
    }
}

static void sysex(Msgs *e, const uint8_t *buf, uint32_t len)
{
    if (len < 1)
        return;
    uint8_t mfr = buf[0];

    if (mfr == 0x7E && len >= 4)
    { /* GM System On/Off */
        /* ResetDevice runs before sub-ID2 is examined (0x136e1), so System
         * Off clears GS mode exactly as System On does */
        if (buf[2] == 0x09)
        {
            synth_reset(e);
            /* System Off releases the collection (0x13700, argument 0): every
             * note-on is silent until GM System On or System Reset */
            if (buf[3] == 0x02)
                e->dls.valid = 0;
        }
        return;
    }
    if (mfr == 0x7F && len >= 6)
    { /* Master Volume, Universal Realtime */
        if (buf[2] == 0x04 && buf[3] == 0x01)
        {
            uint8_t msb = buf[5];
            if (msb > 127)
                msb = 127;
            e->master_vol_hdb = e->tab.vel[msb];
        }
        return;
    }
    if (mfr == 0x41 && len >= 8)
    { /* Roland GS family, DT1 */
        if (buf[2] == 0x42 && buf[3] == 0x12)
        {
            uint8_t a0 = buf[4], a1 = buf[5], a2 = buf[6];
            if (a0 == 0x40 && a1 == 0x00 && a2 == 0x7F)
            { /* GS Reset */
                /* GS Reset does not call the acquire/release at all; save and
                 * restore e->dls.valid around synth_reset() */
                int had_collection = e->dls.valid;
                synth_reset(e);
                e->dls.valid = had_collection;
                e->gs_mode = 1;
                return;
            }
            /* RCV CHANNEL/USE RHYTHM PART/Scale Tuning are each gated
             * individually in the driver; one guard ahead of this whole block
             * is behaviorally identical here */
            if (!e->gs_mode)
                return;
            /* GS part-parameter address: a0=0x40 fixed, a1=0x1<block>
             * (0x10-0x1F), a2=<param>. Guarding on a1's
             * range (not a0's low nibble) keeps this from misfiring on GS Reset
             * (a1=0x00), handled above. */
            if (a0 == 0x40 && a1 >= 0x10 && a1 <= 0x1F && len >= 9)
            {
                /* == Part index */
                uint8_t block = a1 & 0x0F;
                /* RCV CHANNEL: writes the per-Part channel map, no shared reset
                 * tail */
                if (a2 == 0x02)
                {
                    /* data 0 ignored, else channel data-1 (0x137c3/0x137c7) */
                    if (buf[7] != 0)
                        e->part_to_channel[block] = (uint8_t) (buf[7] - 1);
                }
                else if (a2 == 0x15)
                { /* USE RHYTHM PART */
                    /* Resolves through the RCV CHANNEL map, honoring an earlier
                     * remap. Masked to 0-15 as a defensive array bound. */
                    uint8_t ch = e->part_to_channel[block] & 0x0F;
                    e->channels[ch].is_rhythm = buf[7] ? 1 : 0;
                    /* fires the shared reset tail (0x137b5); Bank/Program
                     * survive since its second call is gated off on this path
                     * (0x13807) */
                    reset_all_channel_controllers_device(e);
                }
                else if (a2 == 0x40)
                {
                    /* Scale Tuning: the bit-7 stop test reads one byte ahead
                     * of the store, so the byte before F7 is never written and
                     * earlier entries are not rolled back (0x1377a-0x13797) */
                    Channel *c = &e->channels[e->part_to_channel[block] & 0x0F];
                    for (uint32_t i = 0; i < 12; i++)
                    {
                        if (8 + i >= len || (buf[8 + i] & 0x80))
                            break;
                        c->scale_tuning[i] = (int8_t) (buf[7 + i] - 0x40);
                    }
                }
            }
        }
        return;
    }
    /* any other manufacturer/model/command: unrecognized, dropped silently. */
}

void synth_midi(Msgs *e, uint32_t status, uint32_t d1, uint32_t d2)
{
    midi(e, status, d1, d2);
    ctl_record(e);
}

void synth_sysex(Msgs *e, const uint8_t *buf, uint32_t len)
{
    sysex(e, buf, len);
    ctl_record(e);
}
