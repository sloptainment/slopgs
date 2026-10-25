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

void *rt_alloc(uint32_t nbytes);
uint32_t rt_mem_size(void); /* total bytes handed out so far */

#endif /* RT_H */
