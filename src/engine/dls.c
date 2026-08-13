/* dls.c -- RIFF/DLS parse of gm.dls, SPEC.adoc Part 2. Chunk walk: next =
 * cur+8+size, no odd-size padding except inside INFO lists (S2.2.1). */
/* Internal-mechanism deviations: two-pass ptbl/wvpl resolution, eager wave
 * decode vs SPEC.adoc S2.8/S2.8.6 -- SPEC_LOG item53 */
#include "dls.h"
#include "rt.h"

DlsCollection g_dls;

/* Sticky malformed-chunk HRESULT (S2.2.2 error table); negative as int32, so
 * dls_load returns it directly. Any nonzero value aborts the whole load. */
static int32_t parse_err;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t) rd_u32(p); }
static int16_t rd_i16(const uint8_t *p) { return (int16_t) rd_u16(p); }

static int fourcc_is(const uint8_t *p, char a, char b, char c, char d)
{
    return p[0] == (uint8_t) a && p[1] == (uint8_t) b && p[2] == (uint8_t) c &&
           p[3] == (uint8_t) d;
}

/* wsmp chunk sizes (S2.3.4/S2.7): fixed header through cSampleLoops, or +one
 * loop record. Parsed identically for a wave's own wsmp and a region's
 * override. */
#define WSMP_MIN_SIZE 0x14
#define WSMP_LOOP_MIN_SIZE (WSMP_MIN_SIZE + 0x10)

/* Wave-chunk contents: fmt / data / wsmp / edit (SPEC.adoc S2.3.4, S2.7) */

static void wave_defaults(Wave *w)
{
    w->sample_rate = 22050;
    w->samples = 0;
    w->sample_count = 0;
    w->loop_start = 0;
    w->loop_end = 0;
    w->fine_tune = 0;
    w->attenuation_hdb = 0;
    w->unity_note = DLS_DEFAULT_UNITY_NOTE;
    w->no_loop = 1;
}

static void parse_wave_contents(const uint8_t *content, uint32_t clen, Wave *w)
{
    const uint8_t *p = content;
    const uint8_t *end = content + clen;
    int have_fmt = 0;
    uint16_t bits_per_sample = 16;
    wave_defaults(w);

    while (p + 8 <= end)
    {
        uint32_t size = rd_u32(p + 4);
        const uint8_t *cdata = p + 8;
        /* truncated chunk: stop, no error (S2.2.1) */
        if (cdata + size > end)
            break;

        if (fourcc_is(p, 'f', 'm', 't', ' '))
        {
            if (size >= 0x10)
            {
                uint16_t fmt_tag = rd_u16(cdata + 0);
                uint16_t nchan = rd_u16(cdata + 2);
                uint32_t rate = rd_u32(cdata + 4);
                uint16_t bps = rd_u16(cdata + 0xe);
                if (have_fmt)
                    parse_err = (int32_t) 0x80041389; /* duplicate fmt, D-27 */
                else if (fmt_tag != 1)
                    parse_err = (int32_t) 0x8004138a; /* not PCM, D-26 */
                else if (nchan != 1)
                    parse_err = (int32_t) 0x8004138b; /* not mono, D-26 */
                else
                {
                    w->sample_rate = rate;
                    bits_per_sample = bps;
                    have_fmt = 1;
                }
            }
        }
        else if (fourcc_is(p, 'd', 'a', 't', 'a'))
        {
            if (!have_fmt)
                parse_err = (int32_t) 0x80041389; /* data before fmt, D-27 */
            else
            {
                if (bits_per_sample == 16)
                {
                    w->sample_count = (int32_t)(size / 2);
                    /* Referenced in place, never copied (SPEC.adoc S1.5.5). */
                    w->samples = (int16_t *) (uintptr_t) cdata;
                }
                else if (bits_per_sample == 8)
                {
                    /* Hypothetical 8-bit path (gm.dls is 495/495 16-bit,
                     * S2.7.3): converted to int16, not referenced in place --
                     * render only implements the 16-bit fetch tap
                     * (SPEC_LOG.adoc item12). */
                    uint32_t n = size;
                    int16_t *buf = (int16_t *) rt_alloc(n * 2);
                    for (uint32_t i = 0; i < n; i++)
                    {
                        int v = (int) cdata[i] - 128;
                        buf[i] = (int16_t)(v * 256);
                    }
                    w->samples = buf;
                    w->sample_count = (int32_t) n;
                }
            }
        }
        else if (fourcc_is(p, 'w', 's', 'm', 'p'))
        {
            if (size >= WSMP_MIN_SIZE)
            {
                w->unity_note = cdata[4]; /* low byte only, S2.3.4 */
                w->fine_tune = rd_i16(cdata + 6);
                int32_t latten = rd_i32(cdata + 8);
                w->attenuation_hdb =
                    (int16_t)((int32_t)(((int64_t) latten * 10) >> 16));
                uint32_t nloops = rd_u32(cdata + 0x10);
                if (nloops > 1)
                    parse_err = (int32_t) 0x80041389; /* S2.2.2, D-21 */
                else if (nloops == 0)
                {
                    w->no_loop = 1;
                }
                else
                {
                    w->no_loop = 0;
                    if (size >= WSMP_LOOP_MIN_SIZE)
                    {
                        /* loop record:
                         * cbSize(0x14)+ulLoopType(0x18)+ulStart(0x1c)+ulLength(0x20)
                         */
                        uint32_t lstart = rd_u32(cdata + 0x1c);
                        uint32_t llen = rd_u32(cdata + 0x20);
                        w->loop_start = (int32_t) lstart;
                        w->loop_end = (int32_t)(lstart + llen);
                    }
                }
            }
        }
        /* 'edit' and unrecognized chunks: silently skipped (S2.2.2). */
        p = cdata + size;
    }
}

/* art1 connection-block decoder, SPEC.adoc S2.4 */

static void artic_defaults(Artic *a)
{
    a->eg1_attack_tc = a->eg1_decay_tc = a->eg1_release_tc =
        (int32_t) 0x80000000;
    a->eg1_sustain_permille = 1000;
    a->eg2_attack_tc = a->eg2_decay_tc = a->eg2_release_tc =
        (int32_t) 0x80000000;
    a->eg2_sustain_permille = 1000;
    a->eg2_to_pitch_cents = 0;
    /* -9600; sign/meaning per SPEC_LOG.adoc item3 (S2.5 vs S3.5) */
    a->vel_to_atten_depth = (int16_t) 0xda80;
    a->pan_cb = 0;
    a->lfo_freq_tc = (int32_t) 0x80000000;
    a->lfo_delay_tc = (int32_t) 0x80000000;
    a->lfo_pitch_inherent_cents = 0;
    a->lfo_pitch_cc1_cents = 0;
    a->eg1_attack_vel_tc = 0;
    a->eg1_decay_kf_tc = 0;
    a->eg2_decay_kf_tc = 0;
}

static int16_t clamp_cents(int32_t v)
{
    if (v > 1200)
        v = 1200;
    if (v < -1200)
        v = -1200;
    return (int16_t) v;
}

/* Applies one art1 CONNECTIONLIST chunk's connection blocks onto `a`. */
static void apply_art1(const uint8_t *cdata, uint32_t csize, Artic *a)
{
    if (csize < 8)
        return;
    uint32_t cb_size = rd_u32(cdata + 0);
    uint32_t n_blocks = rd_u32(cdata + 4);
    if (cb_size < 8)
    {
        parse_err = (int32_t) 0x8004138c; /* S2.2.2, D-23 */
        return;
    }
    /* SPEC.adoc S2.3.6: array unconditionally at chunk_data+8, ignoring the
     * chunk's own declared cbSize beyond the >=8 gate. */
    const uint8_t *block = cdata + 8;
    for (uint32_t i = 0; i < n_blocks; i++)
    {
        if (block + 12 > cdata + csize)
            break;
        uint16_t usrc = rd_u16(block + 0);
        uint16_t uctrl = rd_u16(block + 2);
        uint16_t udest = rd_u16(block + 4);
        int32_t lscale = rd_i32(block + 8);
        block += 12;

        if (usrc == ART_SRC_NONE)
        {
            switch (udest)
            {
                case ART_DST_PAN_COARSE:
                case ART_DST_PAN:
                    if (udest == ART_DST_PAN_COARSE)
                        a->pan_cb = (int16_t)((lscale << 4) / 125);
                    else
                        a->pan_cb = (int16_t)((lscale >> 12) / 125);
                    break;
                case ART_DST_LFO_FREQUENCY:
                    a->lfo_freq_tc = lscale;
                    break;
                case ART_DST_LFO_DELAY:
                    a->lfo_delay_tc = lscale;
                    break;
                case ART_DST_EG1_ATTACKTIME:
                    a->eg1_attack_tc = lscale;
                    break;
                case ART_DST_EG1_DECAYTIME:
                    a->eg1_decay_tc = lscale;
                    break;
                case ART_DST_EG1_SUSTAINLEVEL_LO:
                    a->eg1_sustain_permille =
                        (int16_t)(uint16_t)(lscale & 0xFFFF);
                    break;
                case ART_DST_EG1_SUSTAINLEVEL_HI:
                    a->eg1_sustain_permille =
                        (int16_t)(uint16_t)((lscale >> 16) & 0xFFFF);
                    break;
                case ART_DST_EG1_RELEASETIME:
                    a->eg1_release_tc = lscale;
                    break;
                case ART_DST_EG2_ATTACKTIME:
                    a->eg2_attack_tc = lscale;
                    break;
                case ART_DST_EG2_DECAYTIME:
                    a->eg2_decay_tc = lscale;
                    break;
                case ART_DST_EG2_SUSTAINLEVEL_LO:
                    a->eg2_sustain_permille =
                        (int16_t)(uint16_t)(lscale & 0xFFFF);
                    break;
                case ART_DST_EG2_SUSTAINLEVEL_HI:
                    a->eg2_sustain_permille =
                        (int16_t)(uint16_t)((lscale >> 16) & 0xFFFF);
                    break;
                case ART_DST_EG2_RELEASETIME:
                    a->eg2_release_tc = lscale;
                    break;
                default:
                    break;
            }
        }
        else if (usrc == ART_SRC_KEYONVELOCITY)
        {
            if (udest == ART_DST_ATTENUATION)
            {
                if (lscale == (int32_t) 0x80000000)
                    a->vel_to_atten_depth = (int16_t) 0xda80;
                else
                {
                    int32_t v = (int32_t)(((int64_t) lscale * 10) >> 16);
                    if (v > 0)
                        v = 0;
                    if (v < -9600)
                        v = -9600;
                    a->vel_to_atten_depth = (int16_t) v;
                }
            }
            /* SPEC.adoc S2.4.3 Source=2: velocity->EG1 attack (27/235 gm.dls
             * instruments carry one, SPEC_LOG.adoc item28). */
            else if (udest == ART_DST_EG1_ATTACKTIME)
                a->eg1_attack_vel_tc = (int16_t)(lscale >> 16);
            /* SPEC.adoc S2.4.3 Source=3: key-follow decay (169/235 gm.dls
             * instruments, SPEC_LOG.adoc entry15) */
        }
        else if (usrc == ART_SRC_KEYNUMBER)
        {
            if (udest == ART_DST_EG1_DECAYTIME)
                a->eg1_decay_kf_tc = (int16_t)(lscale >> 16);
            else if (udest == ART_DST_EG2_DECAYTIME)
                a->eg2_decay_kf_tc = (int16_t)(lscale >> 16);
        }
        else if (usrc == ART_SRC_EG2)
        {
            if (udest == ART_DST_PITCH)
                a->eg2_to_pitch_cents = clamp_cents(lscale >> 16);
            /* SPEC.adoc S2.4.3 Source=1 (LFO->PITCH), probe 06 rate/depth
             * model; ATTENUATION (tremolo) left unparsed, out of scope
             * (SPEC_LOG.adoc entry3/item10) */
        }
        else if (usrc == ART_SRC_LFO)
        {
            if (udest == ART_DST_PITCH)
            {
                int16_t v = clamp_cents(lscale >> 16);
                if (uctrl == ART_CTRL_NONE)
                    a->lfo_pitch_inherent_cents = v;
                else if (uctrl == ART_CTRL_CC1)
                    a->lfo_pitch_cc1_cents = v;
                /* any other usControl: dropped, matches SPEC.adoc S2.4.4 (gate
                 * only accepts 0 or 0x81). */
            }
        }
        /* KEYNUMBER to any destination but the two decay rows is dropped
         * (SPEC.adoc S2.4.3's own table; gm.dls authors nothing else).
         * usSource==4 dropped per S2.4 (matches original's own quirk). */
    }
}

static void region_defaults(Region *r)
{
    r->next = 0;
    r->wave = 0;
    r->artic = 0;
    r->loop_start = 0;
    r->loop_end = 0;
    r->fine_tune = 0;
    r->attenuation_hdb = 0;
    r->unity_note = DLS_DEFAULT_UNITY_NOTE;
    r->no_loop = 1;
    r->low_key = 0;
    r->high_key = 127;
    r->key_group = 0;
    r->wave_pool_index = 0;
    r->has_own_wsmp = 0;
}

/* Parses one 'rgn '/'rgn2' body. Returns the built Region. */
static Region *parse_region(const uint8_t *content, uint32_t clen)
{
    Region *r = (Region *) rt_alloc(sizeof(Region));
    region_defaults(r);

    const uint8_t *p = content;
    const uint8_t *end = content + clen;
    while (p + 8 <= end)
    {
        uint32_t size = rd_u32(p + 4);
        const uint8_t *cdata = p + 8;
        if (cdata + size > end)
            break;

        if (fourcc_is(p, 'r', 'g', 'n', 'h'))
        {
            if (size >= 0xc)
            {
                r->low_key = cdata[0];
                r->high_key = cdata[2];
                r->key_group = cdata[0xa];
            }
            else
                parse_err = (int32_t) 0x8004138d; /* S2.2.2, D-23 */
        }
        else if (fourcc_is(p, 'w', 's', 'm', 'p'))
        {
            if (size >= WSMP_MIN_SIZE)
            {
                r->has_own_wsmp = 1;
                r->unity_note = cdata[4];
                r->fine_tune = rd_i16(cdata + 6);
                int32_t latten = rd_i32(cdata + 8);
                r->attenuation_hdb =
                    (int16_t)((int32_t)(((int64_t) latten * 10) >> 16));
                uint32_t nloops = rd_u32(cdata + 0x10);
                if (nloops == 0)
                {
                    r->no_loop = 1;
                }
                else
                {
                    r->no_loop = 0;
                    if (size >= WSMP_LOOP_MIN_SIZE)
                    {
                        uint32_t lstart = rd_u32(cdata + 0x1c);
                        uint32_t llen = rd_u32(cdata + 0x20);
                        r->loop_start = (int32_t) lstart;
                        r->loop_end = (int32_t)(lstart + llen);
                    }
                }
            }
        }
        else if (fourcc_is(p, 'w', 'l', 'n', 'k'))
        {
            if (size >= 0xc)
            {
                uint32_t ulchannel = rd_u32(cdata + 4);
                if (ulchannel == WLNK_CHANNEL_LEFT)
                {
                    r->wave_pool_index = rd_u16(cdata + 8);
                }
                else
                    parse_err = (int32_t) 0x8004138b; /* S2.2.2, D-23 */
            }
        }
        else if (fourcc_is(p, 'L', 'I', 'S', 'T'))
        {
            if (size >= 4 && fourcc_is(cdata, 'l', 'a', 'r', 't'))
            {
                Artic *a = (Artic *) rt_alloc(sizeof(Artic));
                artic_defaults(a);
                const uint8_t *lp = cdata + 4;
                const uint8_t *lend = cdata + size;
                while (lp + 8 <= lend)
                {
                    uint32_t lsize = rd_u32(lp + 4);
                    const uint8_t *ldata = lp + 8;
                    if (ldata + lsize > lend)
                        break;
                    if (fourcc_is(lp, 'a', 'r', 't', '1'))
                    {
                        apply_art1(ldata, lsize, a);
                    }
                    lp = ldata + lsize;
                }
                r->artic = a;
            }
        }
        p = cdata + size;
    }
    return r;
}

static Instrument *parse_instrument(const uint8_t *content, uint32_t clen)
{
    Instrument *inst = (Instrument *) rt_alloc(sizeof(Instrument));
    inst->next = 0;
    inst->first_region = 0;
    inst->locale = 0;
    inst->region_count = 0;

    Artic *inst_default_artic = 0;
    Region *region_tail = 0;

    const uint8_t *p = content;
    const uint8_t *end = content + clen;
    while (p + 8 <= end)
    {
        uint32_t size = rd_u32(p + 4);
        const uint8_t *cdata = p + 8;
        if (cdata + size > end)
            break;

        if (fourcc_is(p, 'i', 'n', 's', 'h'))
        {
            if (size >= 0xc)
            {
                uint32_t ulbank = rd_u32(cdata + 4);
                uint32_t ulinstrument = rd_u32(cdata + 8);
                uint32_t locale = ulinstrument;
                locale |= (ulbank & 0x7f) << 7;
                locale |= (ulbank & 0x7f00) << 6;
                if (ulbank & 0x80000000u)
                    locale |= 0x80000000u;
                inst->locale = locale;
            }
            else
                parse_err = (int32_t) 0x8004138f; /* S2.2.2, D-23 */
        }
        else if (fourcc_is(p, 'L', 'I', 'S', 'T'))
        {
            if (size >= 4 && (fourcc_is(cdata, 'l', 'r', 'g', 'n')))
            {
                const uint8_t *lp = cdata + 4;
                const uint8_t *lend = cdata + size;
                while (lp + 8 <= lend)
                {
                    uint32_t lsize = rd_u32(lp + 4);
                    const uint8_t *ldata = lp + 8;
                    if (ldata + lsize > lend)
                        break;
                    if (fourcc_is(lp, 'L', 'I', 'S', 'T') && lsize >= 4 &&
                        (fourcc_is(ldata, 'r', 'g', 'n', ' ') ||
                         fourcc_is(ldata, 'r', 'g', 'n', '2')))
                    {
                        Region *r = parse_region(ldata + 4, lsize - 4);
                        if (region_tail)
                            region_tail->next = r;
                        else
                            inst->first_region = r;
                        region_tail = r;
                        inst->region_count++;
                    }
                    lp = ldata + lsize;
                }
            }
            else if (size >= 4 && fourcc_is(cdata, 'l', 'a', 'r', 't'))
            {
                Artic *a = (Artic *) rt_alloc(sizeof(Artic));
                artic_defaults(a);
                const uint8_t *lp = cdata + 4;
                const uint8_t *lend = cdata + size;
                while (lp + 8 <= lend)
                {
                    uint32_t lsize = rd_u32(lp + 4);
                    const uint8_t *ldata = lp + 8;
                    if (ldata + lsize > lend)
                        break;
                    if (fourcc_is(lp, 'a', 'r', 't', '1'))
                    {
                        apply_art1(ldata, lsize, a);
                    }
                    lp = ldata + lsize;
                }
                inst_default_artic = a;
            }
        }
        p = cdata + size;
    }

    /* SPEC.adoc S3.2.2: regions lacking their own lart adopt the instrument's
     * shared default block. */
    if (inst_default_artic)
    {
        for (Region *r = inst->first_region; r; r = r->next)
        {
            if (!r->artic)
                r->artic = inst_default_artic;
        }
    }
    return inst;
}

int dls_load(const uint8_t *data, uint32_t len)
{
    g_dls.first_instrument = 0;
    g_dls.wave_array = 0;
    g_dls.wave_count = 0;
    g_dls.valid = 0;
    parse_err = 0;

    if (len < 12)
        return -1;
    if (!fourcc_is(data, 'R', 'I', 'F', 'F'))
        return -1;
    if (!fourcc_is(data + 8, 'D', 'L', 'S', ' '))
        return -1;

    const uint8_t *p = data + 12;
    const uint8_t *end = data + len;

    const uint8_t *ptbl_offsets_raw = 0; /* the file's raw ulOffset[] array */
    uint32_t ptbl_cues = 0;
    /* per SPEC.adoc S2.8.3: wvpl chunk data + 4 */
    const uint8_t *wvpl_data_base = 0;
    uint32_t wvpl_data_size = 0;
    Instrument *inst_tail = 0;

    while (p + 8 <= end)
    {
        uint32_t size = rd_u32(p + 4);
        const uint8_t *cdata = p + 8;
        if (cdata + size > end)
            break;

        if (fourcc_is(p, 'p', 't', 'b', 'l'))
        {
            if (size >= 8)
            {
                uint32_t cb_size = rd_u32(cdata + 0);
                uint32_t ccues = rd_u32(cdata + 4);
                const uint8_t *arr = cdata + cb_size;
                if (arr + (uint64_t) ccues * 4 <= end)
                {
                    ptbl_offsets_raw = arr;
                    ptbl_cues = ccues;
                }
                else if (arr <= end)
                {
                    /* array runs past end mid-array: take as many cues as fit,
                     * silently (SPEC.adoc S2.2.2's stated non-error case). */
                    ptbl_offsets_raw = arr;
                    ptbl_cues = (uint32_t)((end - arr) / 4);
                }
            }
        }
        else if (fourcc_is(p, 'L', 'I', 'S', 'T'))
        {
            if (size >= 4 && fourcc_is(cdata, 'w', 'v', 'p', 'l'))
            {
                wvpl_data_base = cdata + 4;
                wvpl_data_size = size - 4;
            }
            else if (size >= 4 && fourcc_is(cdata, 'l', 'i', 'n', 's'))
            {
                const uint8_t *lp = cdata + 4;
                const uint8_t *lend = cdata + size;
                while (lp + 8 <= lend)
                {
                    uint32_t lsize = rd_u32(lp + 4);
                    const uint8_t *ldata = lp + 8;
                    if (ldata + lsize > lend)
                        break;
                    if (fourcc_is(lp, 'L', 'I', 'S', 'T') && lsize >= 4 &&
                        fourcc_is(ldata, 'i', 'n', 's', ' '))
                    {
                        Instrument *inst =
                            parse_instrument(ldata + 4, lsize - 4);
                        if (inst_tail)
                            inst_tail->next = inst;
                        else
                            g_dls.first_instrument = inst;
                        inst_tail = inst;
                    }
                    lp = ldata + lsize;
                }
            }
            /* INFO and any other LIST subtype: skipped. */
        }
        else if (fourcc_is(p, 'c', 'o', 'l', 'h'))
        {
            if (size < 4)
                parse_err = (int32_t) 0x80041392; /* S2.2.2, D-23 */
        }
        /* vers, msyn, edit: parsed off, size-validated leniently -- SPEC_LOG
         * item53 */
        p = cdata + size;
    }

    /* S2.2.2: any malformed chunk aborts the whole load. */
    if (parse_err)
        return (int) parse_err;

    /* Post-pass mirrors SPEC.adoc S2.8.4's own post-pass (0x15dde); see file
     * header for the mechanism deviation. */
    if (ptbl_offsets_raw && wvpl_data_base && ptbl_cues > 0)
    {
        Wave **arr = (Wave **) rt_alloc(ptbl_cues * sizeof(Wave *));
        for (uint32_t i = 0; i < ptbl_cues; i++)
        {
            uint32_t rel_off = rd_u32(ptbl_offsets_raw + i * 4);
            Wave *w = (Wave *) rt_alloc(sizeof(Wave));
            wave_defaults(w);
            if ((uint64_t) rel_off + 8 <= wvpl_data_size)
            {
                const uint8_t *wchunk = wvpl_data_base + rel_off;
                const uint8_t *wend = wvpl_data_base + wvpl_data_size;
                if (wchunk + 8 <= wend && fourcc_is(wchunk, 'L', 'I', 'S', 'T'))
                {
                    uint32_t wsize = rd_u32(wchunk + 4);
                    const uint8_t *wdata = wchunk + 8;
                    if (wdata + wsize <= wend && wsize >= 4 &&
                        (fourcc_is(wdata, 'w', 'a', 'v', 'e') ||
                         fourcc_is(wdata, 'W', 'A', 'V', 'E')))
                    {
                        parse_wave_contents(wdata + 4, wsize - 4, w);
                    }
                }
            }
            arr[i] = w;
        }
        if (parse_err)
            return (int) parse_err;
        g_dls.wave_array = arr;
        g_dls.wave_count = ptbl_cues;

        for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
        {
            for (Region *r = inst->first_region; r; r = r->next)
            {
                if (r->wave_pool_index < g_dls.wave_count)
                {
                    r->wave = g_dls.wave_array[r->wave_pool_index];
                }
                /* DLS-1 wsmp inheritance fallback, [O], never exercised by
                 * gm.dls (1498 regions each carry their own wsmp) -- SPEC_LOG
                 * item53 */
                if (!r->has_own_wsmp && r->wave)
                {
                    r->unity_note = r->wave->unity_note;
                    r->fine_tune = r->wave->fine_tune;
                }
            }
        }
        g_dls.valid = 1;
        return 0;
    }

    g_dls.valid = 0;
    return -2;
}

/* Instrument/region lookup, SPEC.adoc S3.1 */

static Region *find_region_for_note(Instrument *inst, uint8_t note)
{
    for (Region *r = inst->first_region; r; r = r->next)
    {
        if (note >= r->low_key && note <= r->high_key)
            return r;
    }
    return 0;
}

/* Key-range walk is part of FindInstrument itself [A:0x147b7]; gm.dls SFX kit
 * (program 56) example -- SPEC_LOG item53 */
static Instrument *find_instrument_exact(uint32_t locale, uint8_t note)
{
    for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
    {
        if (inst->locale == locale && inst->region_count > 0 &&
            find_region_for_note(inst, note))
            return inst;
    }
    return 0;
}

Region *dls_find_region(uint32_t locale, uint8_t note)
{
    if (!g_dls.valid)
        return 0;
    Instrument *inst = find_instrument_exact(locale, note);
    if (!inst)
    {
        if (locale & 0x80000000u)
        {
            locale = 0x80000000u;
            inst = find_instrument_exact(locale, note);
        }
        if (!inst)
        {
            if (locale == 0x80000000u)
                return 0;
            locale &= 0x7f;
            inst = find_instrument_exact(locale, note);
            if (!inst)
                return 0;
        }
    }
    Region *r = find_region_for_note(inst, note);
    if (!r || !r->artic || !r->wave)
        return 0;
    return r;
}
