/* alsa-synth.c -- the engine as a live ALSA sequencer synth, in the spirit of
 * `timidity -iA`: it opens one sequencer input port and plays whatever is
 * connected to it through an ALSA PCM.
 *
 *   alsa-synth [gm.dls [pcm]]      then: aconnect <src> slopgs
 *
 * Model is src/vsti/vsti.c: events act at the service block boundary, as the
 * driver's do, and the engine always runs a whole SERVICE_BLOCK_FRAMES block
 * at a time so the release taper and its early-release lattice do not depend
 * on the PCM buffer. */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine/dls.h"
#include "engine/render.h"
#include "engine/rt.h"
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
        fprintf(stderr, "alsa-synth: out of memory (%u bytes)\n", nbytes);
        exit(3);
    }
    g_total += nbytes;
    return p;
}

uint32_t rt_mem_size(void) { return g_total; }

/* A block and a half of buffer: writei blocks until a whole block fits, so
   this is also how far ahead of the speaker each block is rendered, and so
   how long an event waits. Rendering is ~1% of a block's time. */
#define LATENCY_US (3 * 1000000u * SERVICE_BLOCK_FRAMES / RENDER_RATE / 2)

static void die(const char *what, int err)
{
    fprintf(stderr, "alsa-synth: %s: %s\n", what, snd_strerror(err));
    exit(2);
}

// Pending sequencer events -> engine, none are held back for later.
static void drain_events(snd_seq_t *seq, snd_midi_event_t *dec)
{
    snd_seq_event_t *ev;
    static uint8_t msg[512];
    while (snd_seq_event_input(seq, &ev) >= 0)
    {
        long n = snd_midi_event_decode(dec, msg, sizeof msg, ev);
        if (n < 1)
            continue;
        if (msg[0] == 0xF0)
            synth_sysex(msg + 1, (uint32_t) n - 1);
        else if (msg[0] < 0xF0)
            synth_midi(msg[0], n > 1 ? msg[1] : 0, n > 2 ? msg[2] : 0);
    }
}

int main(int argc, char **argv)
{
    const char *dls_path = argc > 1 ? argv[1] : "dist/gm.dls";
    const char *pcm_name = argc > 2 ? argv[2] : "default";

    FILE *f = fopen(dls_path, "rb");
    if (!f)
    {
        perror(dls_path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long dls_len = ftell(f);
    rewind(f);
    unsigned char *dls = malloc((size_t) dls_len);
    if (!dls || fread(dls, 1, (size_t) dls_len, f) != (size_t) dls_len)
    {
        fprintf(stderr, "alsa-synth: could not read %s\n", dls_path);
        return 2;
    }
    fclose(f);

    tables_build();
    // referenced in place: never freed
    if (dls_load(dls, (uint32_t) dls_len) != 0)
    {
        fprintf(stderr, "alsa-synth: %s is not a usable gm.dls\n", dls_path);
        return 2;
    }
    synth_construct();

    snd_seq_t *seq;
    int err =
        snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (err < 0)
        die("snd_seq_open", err);
    snd_seq_set_client_name(seq, "slopgs");
    err = snd_seq_create_simple_port(
        seq, "slopgs", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_MIDI_GM |
            SND_SEQ_PORT_TYPE_SYNTHESIZER | SND_SEQ_PORT_TYPE_APPLICATION);
    if (err < 0)
        die("snd_seq_create_simple_port", err);

    snd_midi_event_t *dec;
    err = snd_midi_event_new(512, &dec);
    if (err < 0)
        die("snd_midi_event_new", err);
    // always emit the status byte; synth_midi takes no running status
    snd_midi_event_no_status(dec, 1);

    snd_pcm_t *pcm;
    err = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
        die("snd_pcm_open", err);
    // soft_resample: the engine's rate is not the device's
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, 2, RENDER_RATE, 1,
                             LATENCY_US);
    if (err < 0)
        die("snd_pcm_set_params", err);

    fprintf(stderr, "alsa-synth: client %d, port 0 (aconnect <src> %d:0)\n",
            snd_seq_client_id(seq), snd_seq_client_id(seq));

    static int16_t block[SERVICE_BLOCK_FRAMES * 2];
    for (;;)
    {
        drain_events(seq, dec);
        render_frames(block, SERVICE_BLOCK_FRAMES);
        // blocks until the device has room: this is the clock
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, block, SERVICE_BLOCK_FRAMES);
        if (w < 0 && snd_pcm_recover(pcm, (int) w, 1) < 0)
            die("snd_pcm_writei", (int) w);
    }
}
