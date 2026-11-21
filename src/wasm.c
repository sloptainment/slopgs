/* wasm.c -- freestanding wasm32 interface: the host-side bump allocator and
 * the libc-named mem* symbols this target has no libc for. The exported ABI
 * is msgs.h (engine/msgs.c); cli.c is the other interface (native). */
#include "engine/engine.h"

#define WASM_EXPORT __attribute__((visibility("default")))

/* Real libc-named symbols so compiler-synthesized calls (struct copies,
 * zero-init) resolve. */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *) dst;
    const unsigned char *s = (const unsigned char *) src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *) dst;
    unsigned char v = (unsigned char) c;
    for (size_t i = 0; i < n; i++)
        d[i] = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *) dst;
    const unsigned char *s = (const unsigned char *) src;
    if (d == s || n == 0)
        return dst;
    if (d < s)
    {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    }
    else
    {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

/* bump allocator for the host's own buffers (gm.dls, SMFs, output, each
 * instance's msgs_create block), growing this module's linear memory via
 * memory.grow */

extern unsigned char __heap_base;

#define WASM_PAGE_SIZE 65536u

static uint32_t g_arena_next = 0;

/* ponytail: bump-only, never frees -- a host that keeps creating instances
 * or loading songs into fresh buffers grows without bound. Fine for a page
 * that switches songs by hand; add a free list if something ever loops. */
WASM_EXPORT
uint32_t msgs_alloc(uint32_t nbytes)
{
    if (!g_arena_next)
        g_arena_next = ((uint32_t) (uintptr_t) &__heap_base + 7u) & ~7u;
    nbytes = (nbytes + 7u) & ~7u;
    uint32_t start = g_arena_next;
    uint32_t end = start + nbytes;
    uint32_t cur_bytes =
        (uint32_t) __builtin_wasm_memory_size(0) * WASM_PAGE_SIZE;
    if (end > cur_bytes)
    {
        uint32_t need = end - cur_bytes;
        uint32_t pages = (need + WASM_PAGE_SIZE - 1) / WASM_PAGE_SIZE;
        __builtin_wasm_memory_grow(0, pages);
    }
    g_arena_next = end;
    return start;
}

WASM_EXPORT
uint32_t msgs_mem_size(void) { return g_arena_next; }

/* Temporary debug export (active voice count), additive to the required
 * ABI; remove if a stricter export surface is desired. */
WASM_EXPORT
uint32_t msgs_debug_active_count(Msgs *e)
{
    uint32_t n = 0;
    for (int i = 0; i < NUM_VOICES; i++)
        if (e->voices[i].active)
            n++;
    return n;
}
