/* msgs.c -- the public ABI (msgs.h) over one engine instance. */
#include "engine.h"
#include "render.h"
#include "smf.h"

/* gm.dls parses into ~150 KB of arena (measured, 64-bit) */
#define DLS_ARENA (1u << 20)

uint32_t msgs_abi_version(void) { return 2; }

/* Exported so a host never keeps its own stale copy of RENDER_RATE --
 * dist/compare.js once did, and played every file at the wrong speed. */
uint32_t msgs_sample_rate(void) { return RENDER_RATE; }

uint32_t msgs_size(uint32_t max_smf_len)
{
    uint64_t n =
        (uint64_t) sizeof(Msgs) + DLS_ARENA + smf_mem_bound(max_smf_len);
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t) n;
}

Msgs *msgs_create(void *mem, uint32_t len)
{
    if (!mem || ((uintptr_t) mem & 7u) || len < sizeof(Msgs))
        return 0;
    Msgs *e = (Msgs *) mem;
    /* every field starts zeroed, as the C globals it replaces did */
    for (uint32_t i = 0; i < sizeof(Msgs); i++)
        ((uint8_t *) mem)[i] = 0;
    e->arena.base = (uint8_t *) mem + sizeof(Msgs);
    e->arena.size = len - (uint32_t) sizeof(Msgs);
    tables_build(&e->tab);
    synth_construct(e);
    return e;
}

int32_t msgs_init(Msgs *e, const uint8_t *dls, uint32_t dls_len)
{
    synth_construct(e);
    smf_set_loop(e, 0);
    return dls_load(e, dls, dls_len);
}

/* Rewinds the loaded song and clears the finished latch too -- resetting
 * only the control plane would leave msgs_is_finished() stuck on. */
void msgs_reset(Msgs *e)
{
    synth_construct(e);
    smf_rewind(e);
}

int32_t msgs_load_smf(Msgs *e, const uint8_t *smf, uint32_t smf_len)
{
    synth_construct(e);
    return smf_load(e, smf, smf_len);
}

void msgs_set_loop(Msgs *e, int32_t loops) { smf_set_loop(e, loops); }

uint32_t msgs_render(Msgs *e, int16_t *out, uint32_t frames)
{
    return smf_render(e, out, frames);
}

int32_t msgs_is_finished(Msgs *e) { return smf_is_finished(e); }

void msgs_midi(Msgs *e, uint32_t status, uint32_t d1, uint32_t d2)
{
    synth_midi(e, status, d1, d2);
}
