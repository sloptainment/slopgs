/* rt.h -- freestanding runtime primitives
 * (memcpy/memset/pow/log10/sqrt/sin + allocator); rt.c implements the portable
 * math half. */
#ifndef RT_H
#define RT_H

#include <stddef.h>
#include <stdint.h>

double rt_pow(double base, double exponent);
double rt_log(double x);
double rt_log10(double x);
double rt_sqrt(double x);
double rt_sin(double x);
double rt_exp(double x);

/* one instance's bump allocator, over the tail of its msgs_create block */
typedef struct Arena
{
    uint8_t *base;
    uint32_t size, used;
} Arena;

/* zeroed, as the hosts' calloc used to hand it out; 0 once the block is full.
 * Inline so the oracle, which links smf.c alone, gets it too. */
static inline void *rt_alloc(Arena *a, uint32_t nbytes)
{
    uint64_t n = nbytes ? ((uint64_t) nbytes + 7u) & ~7ull : 8u;
    if (n > a->size - a->used)
        return 0;
    uint8_t *p = a->base + a->used;
    a->used += (uint32_t) n;
    for (uint32_t i = 0; i < n; i++)
        p[i] = 0;
    return p;
}

#endif /* RT_H */
