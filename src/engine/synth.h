#ifndef SYNTH_H
#define SYNTH_H

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

extern Channel g_channels[16];
/* gates CC0/CC32 storage and the per-part DT1 handlers (0x1341e, 0x13492,
 * 0x1376e, 0x137a2, 0x137b7) */
extern uint8_t g_gs_mode;
/* master volume attenuation, hundredths of a dB */
extern int32_t g_master_vol_hdb;

void synth_reset(void);
/* Device construction/open: power-on defaults + synth_reset() body;
 * MIDI-level resets don't touch RPN0/RPN-select/sustain (unit.c S4.6.4). */
void synth_construct(void);
/* program|bankLSB<<7|bankMSB<<14|drum<<31 */
uint32_t synth_channel_locale(int ch);
int32_t synth_pitch_bend_cents(int ch);

/* One short message with an explicit status byte; running-status expansion is
 * the caller's job. 0xFF is System Reset. */
void synth_midi(uint32_t status, uint32_t d1, uint32_t d2);

/* Internal-only SysEx handler, not public ABI: msgs_midi's 3-byte signature
 * can't carry a SysEx message. Invoked only from smf.c.
 * buf points right after the leading 0xF0. */
void synth_sysex(const uint8_t *buf, uint32_t len);

#endif /* SYNTH_H */
