/* dls.h -- RIFF/DLS parse of gm.dls into instruments, regions, waves. */
/* Field names are chosen for clarity in this clean-room reimplementation, not a
 * claim of matching the driver's in-memory offsets. */
#ifndef DLS_H
#define DLS_H

#include <stdint.h>

typedef struct Wave
{
    uint32_t sample_rate; /* fmt.nSamplesPerSec, wave scope */
    /* referenced in place into the gm.dls buffer, NOT copied */
    int16_t *samples;
    int32_t sample_count; /* data chunk size/2 for 16-bit PCM */
    int32_t loop_start;   /* wsmp loop start, wave scope, samples */
                          /* wsmp loop start+length, wave scope, samples */
    int32_t loop_end;
    int16_t fine_tune; /* wsmp sFineTune, wave scope */
    /* wsmp lAttenuation, (lScale*10)>>16, hundredths of a dB, wave scope */
    int16_t attenuation_hdb;
    /* wsmp usUnityNote, byte-truncated, wave scope */
    uint8_t unity_note;
    uint8_t no_loop; /* wsmp cSampleLoops==0, wave scope */
} Wave;

/* art1 usSource values the driver dispatches on (0x157da; not the full DLS-1
 * enumeration -- just the rows apply_art1() switches on). */
enum ArtSource
{
    ART_SRC_NONE = 0x0000, /* unconditional (RPN-less) block */
    ART_SRC_LFO = 0x0001,
    ART_SRC_KEYONVELOCITY = 0x0002,
    ART_SRC_KEYNUMBER = 0x0003,
    ART_SRC_EG2 = 0x0005,
};

/* art1 usDestination values; ART_DST_PAN_COARSE is DLS-1's reserved 0x0002,
 * which the driver writes into the same pan field as 0x0004 (0x15a26). */
enum ArtDest
{
    ART_DST_ATTENUATION = 0x0001,
    ART_DST_PAN_COARSE = 0x0002,
    ART_DST_PITCH = 0x0003,
    ART_DST_PAN = 0x0004,
    ART_DST_LFO_FREQUENCY = 0x0104,
    ART_DST_LFO_DELAY = 0x0105,
    ART_DST_EG1_ATTACKTIME = 0x0206,
    ART_DST_EG1_DECAYTIME = 0x0207,
    ART_DST_EG1_SUSTAINLEVEL_LO = 0x0208,
    ART_DST_EG1_RELEASETIME = 0x0209,
    ART_DST_EG1_SUSTAINLEVEL_HI = 0x020a,
    ART_DST_EG2_ATTACKTIME = 0x030a,
    ART_DST_EG2_DECAYTIME = 0x030b,
    ART_DST_EG2_SUSTAINLEVEL_LO = 0x030c,
    ART_DST_EG2_RELEASETIME = 0x030d,
    ART_DST_EG2_SUSTAINLEVEL_HI = 0x030e,
};

/* art1 usControl with src=ART_SRC_LFO; any other value is dropped
 * (0x158e8). */
enum ArtLfoControl
{
    ART_CTRL_NONE = 0x0000,
    ART_CTRL_CC1 = 0x0081, /* modwheel */
};

/* The 0x68-byte resolved articulation block, distinct from Wave: a region's
 * own lart, else its instrument's shared one (0x162a6). */
typedef struct Artic
{
    /* raw DLS timecents */
    int32_t eg1_attack_tc, eg1_decay_tc, eg1_release_tc;
    int16_t eg1_sustain_permille; /* 0..1000 */
    int32_t eg2_attack_tc, eg2_decay_tc, eg2_release_tc;
    int16_t eg2_sustain_permille;
    /* src=ART_SRC_EG2 -> dest=ART_DST_PITCH */
    int16_t eg2_to_pitch_cents;
    /* src=ART_SRC_KEYONVELOCITY -> dest=ART_DST_ATTENUATION, default -9600 */
    int16_t vel_to_atten_depth;
    /* dest=ART_DST_PAN_COARSE/ART_DST_PAN, small correction to CC10 pan */
    int16_t pan_cb;
    /* src=ART_SRC_NONE -> dest=ART_DST_LFO_FREQUENCY/ART_DST_LFO_DELAY
     * (art+0x40/+0x48): per-sample phase step (Hz*2^20/22050) and delay in
     * frames, both truncated at parse; default 5 Hz, no delay */
    uint32_t lfo_step, lfo_delay_frames;
    /* src=LFO,ctrl=NONE -> PITCH: ungated pitch-LFO depth, hi word of lScale,
     * clamped +-1200 */
    int16_t lfo_pitch_inherent_cents;
    /* src=LFO,ctrl=CC1 -> PITCH: CC1-scaled pitch-LFO depth, same formula/clamp
     */
    int16_t lfo_pitch_cc1_cents;
    /* src=LFO,ctrl=NONE/CC1 -> ATTENUATION: tremolo depth, lScale*10>>16 (hdB),
     * clamped +-1200 */
    int16_t lfo_atten_inherent_hdb, lfo_atten_cc1_hdb;
    /* src=KEYONVELOCITY -> EG1_ATTACKTIME: velocity attack scaling, hi word of
     * lScale, full-scale (vel 127) cent offset */
    int16_t eg1_attack_vel_tc;
    /* src=KEYNUMBER -> EG1/EG2_DECAYTIME: decay key-follow, hi word of lScale,
     * full-scale (key 127) cent offset */
    int16_t eg1_decay_kf_tc, eg2_decay_kf_tc;
} Artic;

typedef struct Region
{
    struct Region *next;
    Wave *wave;
    Artic *artic;
    int32_t loop_start, loop_end; /* region-scope wsmp override, samples */
    int16_t fine_tune;
    /* art1 lScale, (lScale*10)>>16, hundredths of a dB, region scope */
    int16_t attenuation_hdb;
    uint8_t unity_note;
    uint8_t no_loop;
    uint8_t low_key, high_key;
    uint8_t key_group;
    /* rgnh.fusOptions bit 0: may sound over itself (no same-note retrigger) */
    uint8_t self_nonexclusive;
    uint16_t wave_pool_index;
    uint8_t has_own_wsmp; /* region carried its own wsmp chunk (S2.3.4) */
} Region;

typedef struct Instrument
{
    struct Instrument *next;
    Region *first_region;
    uint32_t locale; /* program | bankLSB<<7 | bankMSB<<14 | drum<<31 */
    int region_count;
} Instrument;

enum
{
    /* middle C, wave/region wsmp-absent default */
    DLS_DEFAULT_UNITY_NOTE = 60,
    /* wlnk ulChannel: only mono/left link is used */
    WLNK_CHANNEL_LEFT = 1,
};

typedef struct DlsCollection
{
    Instrument *first_instrument;
    Wave **wave_array;
    uint32_t wave_count;
    int valid;
} DlsCollection;

extern DlsCollection g_dls;

/* Parses a DLS-1 RIFF collection in place; sample data is referenced into
 * `data`, never copied (the driver reads the file into one never-freed
 * buffer, 0x15efa). */
int dls_load(const uint8_t *data, uint32_t len);

/* Three-tier bank/program/drum fallback + region lookup (0x12e19-0x12e7a). */
/* `resolved` (nullable) receives the locale after fallback stripping. */
Region *dls_find_region(uint32_t locale, uint8_t note, uint32_t *resolved);

#endif /* DLS_H */
