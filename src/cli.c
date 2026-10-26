/* cli.c -- the native interface: a standalone command-line renderer, same
 * role as wasm.c (drives the synth core directly; a host pointer doesn't
 * fit in wasm.c's uint32 ABI). msgs-render <dls> <smf|""> <loops> <max_frames>
 * <out_wav> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/dls.h"
#include "engine/render.h"
#include "engine/rt.h"
#include "engine/smf.h"
#include "engine/synth.h"
#include "engine/tables.h"
#include "engine/voice.h"

/* allocator: malloc, never freed -- gm.dls's sample data is referenced in
 * place and must outlive every render call; the process renders once and exits.
 */

static uint32_t g_total = 0;

void *rt_alloc(uint32_t nbytes)
{
    nbytes = (nbytes + 7u) & ~7u;
    void *p = calloc(1, nbytes ? nbytes : 8);
    if (!p)
    {
        fprintf(stderr, "msgs-render: out of memory (%u bytes)\n", nbytes);
        exit(3);
    }
    g_total += nbytes;
    return p;
}

uint32_t rt_mem_size(void) { return g_total; }

static unsigned char *read_file(const char *path, uint32_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *buf = (unsigned char *) malloc((size_t) n ? (size_t) n : 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t) n, f) != (size_t) n)
    {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len_out = (uint32_t) n;
    return buf;
}

/* Canonical 44-byte PCM WAV header, stereo 16-bit @ RENDER_RATE. Written as a
 * zero-size placeholder, rewritten once `frames` is known (streaming render).
 */
static void write_wav_header(FILE *f, unsigned long frames)
{
    uint32_t sample_rate = RENDER_RATE, byte_rate = RENDER_RATE * 4;
    uint32_t data_bytes = (uint32_t) frames * 4;
    uint32_t riff_size = 36 + data_bytes;
    uint32_t fmt_size = 16;
    uint16_t audio_fmt = 1, channels = 2, block_align = 4, bits = 16;

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

/* Inherited single-line JSON format from the legacy Node-based harness runner.
 */
static void emit(int abi, int init_ret, int load_ret, unsigned long frames,
                 int truncated, const char *error)
{
    printf("{\"abi_version\":%d,\"init_ret\":%d,\"load_ret\":", abi, init_ret);
    if (load_ret == 0x7fffffff)
        printf("null");
    else
        printf("%d", load_ret);
    printf(",\"frames\":%lu,\"truncated\":%s,\"error\":", frames,
           truncated ? "true" : "false");
    if (error)
        printf("\"%s\"", error);
    else
        printf("null");
    printf("}\n");
}

#define CHUNK 4096

/* --selftest: replay/reload check, guards a real bug -- before smf_rewind()
 * existed, msgs_is_finished()'s latch never cleared, so a second loaded song
 * reported "finished" immediately. */
static int selftest(const char *dls_path, const char *smf_path)
{
    uint32_t dls_len = 0, smf_len = 0;
    unsigned char *dls = read_file(dls_path, &dls_len);
    unsigned char *smf = read_file(smf_path, &smf_len);
    if (!dls || !smf)
    {
        fprintf(stderr, "selftest: could not read inputs\n");
        return 1;
    }

    tables_build();
    if (dls_load(dls, dls_len) != 0)
    {
        fprintf(stderr, "selftest: dls_load failed\n");
        return 1;
    }

    static int16_t buf[CHUNK * 2];
    int fail = 0;

    /* Two identical passes. Pass 2 differs only in that the synth has already
     * run a song to the end and latched finished. */
    unsigned long frames[2] = {0, 0};
    long long energy[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++)
    {
        synth_construct();
        if (smf_load(smf, smf_len) != 0)
        {
            fprintf(stderr, "selftest: smf_load failed\n");
            return 1;
        }
        smf_set_loop(0);

        if (smf_is_finished())
        {
            fprintf(
                stderr,
                "FAIL pass %d: finished latch still set right after smf_load\n",
                pass + 1);
            fail = 1;
        }
        for (;;)
        {
            uint32_t n = smf_render(buf, CHUNK);
            for (uint32_t i = 0; i < n * 2; i++)
            {
                long long s = buf[i];
                energy[pass] += s < 0 ? -s : s;
            }
            frames[pass] += n;
            if (n == 0 || smf_is_finished())
                break;
            /* 10 min cap */
            if (frames[pass] > (unsigned long) RENDER_RATE * 600UL)
                break;
        }
        if (!smf_is_finished())
        {
            fprintf(stderr, "FAIL pass %d: song never latched finished\n",
                    pass + 1);
            fail = 1;
        }
        if (energy[pass] == 0)
        {
            fprintf(stderr, "FAIL pass %d: rendered pure silence\n", pass + 1);
            fail = 1;
        }
    }

    if (frames[0] != frames[1] || energy[0] != energy[1])
    {
        fprintf(stderr,
                "FAIL: replay differs from first play -- frames %lu vs %lu, "
                "energy %lld vs %lld\n",
                frames[0], frames[1], energy[0], energy[1]);
        fail = 1;
    }

    /* Same latch, the other way a host clears it: rewinding in place rather
     * than reloading (what msgs_reset does for a looping <bg-sound>). */
    smf_rewind();
    if (smf_is_finished())
    {
        fprintf(stderr, "FAIL: finished latch still set after smf_rewind\n");
        fail = 1;
    }
    uint32_t n = smf_render(buf, CHUNK);
    long long e3 = 0;
    for (uint32_t i = 0; i < n * 2; i++)
    {
        long long s = buf[i];
        e3 += s < 0 ? -s : s;
    }
    if (n == 0)
    {
        fprintf(stderr, "FAIL: rewound song rendered no frames\n");
        fail = 1;
    }

    /* Pool saturation: Branch B fast-releases active voices, which keep
     * rendering for their full release before recycling -- exactly the defect
     * TOPUP_INTERVAL_FRAMES (voice.c) exists to prevent. Probes 20/21: 80
     * held note-ons must leave 48 sounding (unit.c S5.5). Renders via
     * render_frames, not smf_render: dispatching song events into a fixed-frame
     * window shifted it and cost one voice (47/48) in an earlier version of
     * this check. */
    synth_construct();
    for (int k = 0; k < 80; k++)
        voice_note_on(0, 36 + (k % 60), 100);
    /* ~7.4s */
    for (int k = 0; k < 40 * RESAMPLE_FACTOR; k++)
        render_frames(buf, CHUNK);
    int surviving = 0;
    for (int k = 0; k < NUM_VOICES; k++)
        if (g_voices[k].active)
            surviving++;
    if (surviving != 48)
    {
        fprintf(stderr,
                "FAIL: 80 held note-ons left %d voices sounding, expected 48"
                " (probes 20/21) -- reserve top-up cadence?\n",
                surviving);
        fail = 1;
    }

    /* --jitter: the draw is reseeded per load, so a jittered song replays
     * exactly. */
    long long ej[2] = {0, 0};
    smf_set_jitter_ms(22);
    for (int pass = 0; pass < 2; pass++)
    {
        synth_construct();
        smf_load(smf, smf_len);
        smf_set_loop(0);
        for (int k = 0; k < 64; k++)
        {
            uint32_t m = smf_render(buf, CHUNK);
            for (uint32_t i = 0; i < m * 2; i++)
                ej[pass] += buf[i] < 0 ? -buf[i] : buf[i];
        }
    }
    smf_set_jitter_ms(0);
    if (ej[0] != ej[1] || ej[0] == 0)
    {
        fprintf(stderr,
                "FAIL: jittered replay differs -- energy %lld vs %lld\n", ej[0],
                ej[1]);
        fail = 1;
    }

    printf("%s: pass1 %lu frames, pass2 %lu frames; rewind produced %u frames;"
           " %d/48 voices survive saturation\n",
           fail ? "FAIL" : "PASS", frames[0], frames[1], n, surviving);
    (void) e3;
    return fail;
}

int main(int argc, char **argv)
{
    /* --jitter=MS: capture realism (smf_set_jitter_ms), off by default */
    if (argc > 1 && strncmp(argv[1], "--jitter=", 9) == 0)
    {
        smf_set_jitter_ms((uint32_t) strtoul(argv[1] + 9, NULL, 10));
        argv[1] = argv[0];
        argv++;
        argc--;
    }
    if (argc == 4 && strcmp(argv[1], "--selftest") == 0)
    {
        return selftest(argv[2], argv[3]);
    }
    if (argc != 6)
    {
        fprintf(stderr,
                "usage: %s [--jitter=MS] <dls> <smf|\"\"> <loops> <max_frames> "
                "<out_wav>\n"
                "       %s --selftest <dls> <smf>\n",
                argv[0], argv[0]);
        return 1;
    }
    const char *dls_path = argv[1];
    const char *smf_path = argv[2];
    long loops = strtol(argv[3], NULL, 10);
    unsigned long max_frames = strtoul(argv[4], NULL, 10);
    const char *out_path = argv[5];

    tables_build();
    synth_construct();
    smf_set_loop(0);

    int init_ret = 0;
    if (dls_path[0])
    {
        uint32_t dls_len = 0;
        unsigned char *dls = read_file(dls_path, &dls_len);
        if (!dls)
        {
            emit(1, -1, 0x7fffffff, 0, 0, "could not read dls");
            return 2;
        }
        /* referenced in place: never freed */
        init_ret = dls_load(dls, dls_len);
    }

    /* sentinel -> JSON null, as node_runner emits */
    int load_ret = 0x7fffffff;
    if (smf_path[0])
    {
        uint32_t smf_len = 0;
        unsigned char *smf = read_file(smf_path, &smf_len);
        if (!smf)
        {
            emit(1, init_ret, 0x7fffffff, 0, 0, "could not read smf");
            return 2;
        }
        synth_construct();
        load_ret = smf_load(smf, smf_len);
    }

    smf_set_loop((int32_t) loops);

    FILE *out = fopen(out_path, "wb");
    if (!out)
    {
        emit(1, init_ret, load_ret, 0, 0, "could not open out_wav");
        return 2;
    }
    /* placeholder, patched below once total is known */
    write_wav_header(out, 0);

    static int16_t buf[CHUNK * 2]; /* stereo interleaved */
    unsigned long total = 0;
    int truncated = 0;
    for (;;)
    {
        uint32_t n = smf_render(buf, CHUNK);
        if (n > 0)
        {
            if (fwrite(buf, 4, n, out) != n)
            {
                fclose(out);
                emit(1, init_ret, load_ret, total, truncated,
                     "short write to out_wav");
                return 2;
            }
            total += n;
        }
        if (n == 0)
            break;
        if (smf_is_finished())
            break;
        if (total >= max_frames)
        {
            truncated = 1;
            break;
        }
    }
    write_wav_header(out, total);
    fclose(out);

    emit(1, init_ret, load_ret, total, truncated, NULL);
    return 0;
}
