/* player.c -- plays a .mid through the default ALSA device in realtime.
 *
 *   player [gm.dls] file.mid
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine/dls.h"
#include "engine/render.h"
#include "engine/rt.h"
#include "engine/smf.h"
#include "engine/synth.h"
#include "engine/tables.h"
#include "engine/voice.h"

/* allocator: malloc, never freed -- gm.dls's sample data is referenced in
 * place and must outlive every render call. */
static uint32_t g_total = 0;

void *rt_alloc(uint32_t nbytes)
{
    nbytes = (nbytes + 7u) & ~7u;
    void *p = calloc(1, nbytes ? nbytes : 8);
    if (!p)
    {
        fprintf(stderr, "player: out of memory (%u bytes)\n", nbytes);
        exit(3);
    }
    g_total += nbytes;
    return p;
}

uint32_t rt_mem_size(void) { return g_total; }

static unsigned char *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *buf = malloc((size_t) n ? (size_t) n : 1);
    if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n)
    {
        fprintf(stderr, "player: could not read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (uint32_t) n;
    return buf;
}

static void die(const char *what, int err)
{
    fprintf(stderr, "player: %s: %s\n", what, snd_strerror(err));
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: %s [gm.dls] file.mid\n", argv[0]);
        return 2;
    }
    const char *dls_path = argc == 3 ? argv[1] : "dist/gm.dls";
    const char *smf_path = argv[argc - 1];

    uint32_t dls_len, smf_len;
    unsigned char *dls = read_file(dls_path, &dls_len);
    unsigned char *smf = read_file(smf_path, &smf_len);

    tables_build();
    // referenced in place: never freed
    if (dls_load(dls, dls_len) != 0)
    {
        fprintf(stderr, "player: %s is not a usable gm.dls\n", dls_path);
        return 2;
    }
    synth_construct();
    if (smf_load(smf, smf_len) != 0)
    {
        fprintf(stderr, "player: %s is not a usable MIDI file\n", smf_path);
        return 2;
    }
    smf_set_loop(0);

    snd_pcm_t *pcm;
    int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
        die("snd_pcm_open", err);
    // soft_resample: the engine's rate is not the device's
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, 2, RENDER_RATE, 1,
                             500000);
    if (err < 0)
        die("snd_pcm_set_params", err);

    static int16_t block[SERVICE_BLOCK_FRAMES * 2];
    for (;;)
    {
        uint32_t n = smf_render(block, SERVICE_BLOCK_FRAMES);
        // blocks until the device has room: this is the clock
        snd_pcm_sframes_t w = n ? snd_pcm_writei(pcm, block, n) : 0;
        if (w < 0 && snd_pcm_recover(pcm, (int) w, 1) < 0)
            die("snd_pcm_writei", (int) w);
        if (n == 0 || smf_is_finished())
            break;
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return 0;
}
