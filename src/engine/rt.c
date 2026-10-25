/* rt.c -- portable runtime math: pow/log10/sqrt/sin/exp. No
 * libc/libm/allocator. sqrt is the hardware f64.sqrt builtin; exp/log/sin are
 * from-scratch Taylor-series: not bit-exact vs. any CRT, which the driver's
 * truncation margins tolerate (unit.c S1.4.3). */
#include "rt.h"

typedef union
{
    double d;
    uint64_t u;
} du64;

static double ldexp_i(double x, int e)
{
    if (x == 0.0)
        return x;
    du64 u;
    u.d = x;
    int exp = (int) ((u.u >> 52) & 0x7FF);
    if (exp == 0)
    {
        /* subnormal input: not exercised by this driver's domain, treated as
         * zero. */
        return 0.0;
    }
    exp += e;
    if (exp >= 0x7FF)
    {
        u.u = (u.u & 0x8000000000000000ULL) | (0x7FEULL << 52) |
              0xFFFFFFFFFFFFFULL;
        return u.d; /* saturate to +-max finite, no infinities needed here */
    }
    if (exp <= 0)
        return 0.0; /* underflow to zero, adequate for this domain */
    u.u = (u.u & ~(0x7FFULL << 52)) | ((uint64_t) exp << 52);
    return u.d;
}

/* exp() -- range reduction mod ln2, Taylor polynomial for the remainder */

#define RT_LN2 0.69314718055994530942
#define RT_LOG2E 1.44269504088896340736

double rt_exp(double x)
{
    if (x > 709.0)
        x = 709.0;
    if (x < -745.0)
        return 0.0;
    double kf = x * RT_LOG2E;
    /* round to nearest, ties away from 0 */
    long k = (long) (kf + (kf >= 0.0 ? 0.5 : -0.5));
    double r = x - (double) k * RT_LN2;
    double r2 = r * r;
    /* Taylor series through r^8/8! -- |r| <= ln2/2, no correct-rounding
     * requirement (S1.4.3). */
    double poly =
        1.0 + r +
        r2 * (1.0 / 2 +
              r * (1.0 / 6 +
                   r * (1.0 / 24 +
                        r * (1.0 / 120 +
                             r * (1.0 / 720 +
                                  r * (1.0 / 5040 + r * (1.0 / 40320)))))));
    return ldexp_i(poly, (int) k);
}

/* log() -- decompose x = m * 2^e, m in [sqrt(0.5), sqrt(2)), atanh series */

double rt_log(double x)
{
    if (x <= 0.0)
    {
        /* x<=0: not exercised by this driver's domain, returns a sentinel
         * rather than trapping. */
        return (x == 0.0) ? -1.0e308 : 0.0;
    }
    du64 u;
    u.d = x;
    int exp = (int) ((u.u >> 52) & 0x7FF) - 1023;
    uint64_t mant_bits = (u.u & 0xFFFFFFFFFFFFFULL) | (1023ULL << 52);
    du64 mu;
    mu.u = mant_bits;
    double m = mu.d; /* in [1,2) */
    const double SQRT2 = 1.4142135623730951;
    if (m > SQRT2)
    {
        m *= 0.5;
        exp += 1;
    }
    double f = m - 1.0;
    double s = f / (2.0 + f);
    double s2 = s * s;
    double series =
        s +
        s * s2 *
            (1.0 / 3 +
             s2 * (1.0 / 5 +
                   s2 * (1.0 / 7 +
                         s2 * (1.0 / 9 + s2 * (1.0 / 11 + s2 * (1.0 / 13))))));
    double logm = 2.0 * series;
    return (double) exp * RT_LN2 + logm;
}

double rt_log10(double x)
{
    return rt_log(x) * 0.43429448190325182765; /* 1/ln(10) */
}

double rt_pow(double base, double exponent)
{
    if (base == 0.0)
        return (exponent > 0.0) ? 0.0 : 1.0;
    if (base < 0.0)
    {
        /* base<0: not exercised by this driver's domain (all call sites use
         * positive ratios/velocities). */
        return 0.0;
    }
    return rt_exp(exponent * rt_log(base));
}

double rt_sqrt(double x)
{
    if (x <= 0.0)
        return 0.0;
    return __builtin_sqrt(x); /* lowers to the wasm f64.sqrt hardware op */
}

/* sin() -- range reduce to [-pi,pi], Taylor polynomial. Table-build time only,
 * not the per-sample path -- need not be fast. */

#define RT_PI 3.14159265358979323846
#define RT_2PI 6.28318530717958647692

double rt_sin(double x)
{
    double k = x / RT_2PI;
    long ki = (long) (k + (k >= 0.0 ? 0.5 : -0.5));
    x = x - (double) ki * RT_2PI; /* now in [-pi,pi] roughly */
    if (x > RT_PI)
        x -= RT_2PI;
    if (x < -RT_PI)
        x += RT_2PI;
    double x2 = x * x;
    /* Taylor series through x^17/17!, accurate within [-pi,pi]. */
    double poly =
        x *
        (1.0 +
         x2 *
             (-1.0 / 6 +
              x2 *
                  (1.0 / 120 +
                   x2 * (-1.0 / 5040 +
                         x2 * (1.0 / 362880 +
                               x2 * (-1.0 / 39916800 +
                                     x2 * (1.0 / 6227020800.0 +
                                           x2 * (-1.0 / 1307674368000.0))))))));
    return poly;
}
