#ifndef SYNTH_H
#define SYNTH_H

#include "msgs.h"
#include <stdint.h>

typedef struct Channel
{
    uint8_t bank_msb, bank_lsb, program;
    /* scheduled 21-bit locale (0x16df4), latched at Program Change/reset
     * only, not recomputed live from bank bytes */
    uint32_t scheduled_locale;
    uint8_t volume;      /* CC7, default 100 */
    uint8_t expression;  /* CC11, default 127 */
    uint8_t pan;         /* CC10, default 64 */
    uint8_t modulation;  /* CC1, default 0; scales the CC1 LFO depth */
    uint16_t pitch_bend; /* raw 14-bit, center 8192 */
    uint16_t pb_range_cents; /* RPN0, default 200 */
    int16_t rpn1_fine_cents;
    int16_t rpn2_coarse_cents;
    uint16_t rpn_select; /* MSB<<7|LSB, 0x3FFF = Null */
    uint16_t data_entry_combined;
    uint8_t sustain; /* CC64 raw value */
    /* USE RHYTHM PART (driver: per Part, 0x130c); kept per channel here */
    uint8_t is_rhythm;
    uint8_t mono_mode;
    /* GS Scale Tuning, cents per semitone class, added at note-on */
    int8_t scale_tuning[12];
} Channel;

void synth_reset(Msgs *e);
/* Device construction/open: power-on defaults + synth_reset() body;
 * MIDI-level resets don't touch RPN0/RPN-select/sustain (unit.c S4.6.4). */
void synth_construct(Msgs *e);
/* program|bankLSB<<7|bankMSB<<14|drum<<31 */
uint32_t synth_channel_locale(Msgs *e, int ch);
int32_t synth_pitch_bend_cents(Msgs *e, int ch);

/* the live controllers a voice reads at its segment ends */
enum
{
    CTL_MOD,
    CTL_VOL,
    CTL_PAN,
    CTL_EXPR,
    CTL_BEND,
    CTL_COUNT
};
/* RecordMIDI queues each live controller with its event time (0x16bae);
 * a voice reads the value due at its segment end (0x16c50) and Mix folds
 * every node older than its end into the current value (0x16b46). Here the
 * Channel fields hold the newest value and the queue what came before. */
#define CTL_QUEUE 64

typedef struct CtlQueue
{
    /* value before q[0]; n pending nodes in time order */
    uint16_t cur;
    uint8_t n;
    struct
    {
        uint64_t t;
        uint16_t v;
    } q[CTL_QUEUE];
} CtlQueue;

/* the controller's value at time t (RENDER_RATE frames) [A:0x16c50] */
uint32_t synth_ctl_at(Msgs *e, int ch, int ctl, uint64_t t);
int32_t synth_pitch_bend_cents_at(Msgs *e, int ch, uint64_t t);
/* Mix's end: fold what is older than `end` into the current value */
void synth_ctl_fold(Msgs *e, uint64_t end);

/* One short message with an explicit status byte; running-status expansion is
 * the caller's job. 0xFF is System Reset. */
void synth_midi(Msgs *e, uint32_t status, uint32_t d1, uint32_t d2);

/* A service call ran: Program Changes posted so far are folded into the
 * locale queue default (0x16b46) and a later System Reset keeps them. Called
 * by render_frames. */
void synth_service_tick(Msgs *e);

/* Internal-only SysEx handler, not public ABI: msgs_midi's 3-byte signature
 * can't carry a SysEx message, and the ABI stays fixed. The driver has no
 * separate SysEx entry: its buffer walker (0x18057) takes raw variable-length
 * MIDI events and sends 0xF0 to 0x1367a, anything else to 0x131c0. In-tree
 * hosts that link the engine (src/vsti) may call this directly.
 * buf points right after the leading 0xF0. */
void synth_sysex(Msgs *e, const uint8_t *buf, uint32_t len);

#endif /* SYNTH_H */
