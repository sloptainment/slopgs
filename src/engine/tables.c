/* tables.c -- the driver's runtime-built tables. All truncate toward zero (the
 * driver's 0x106e0 helper) via a plain (int32_t) cast; x87 control word 0x027F
 * (53-bit, round-to-nearest) makes plain double arithmetic reproduce them. */
#include "tables.h"
#include "rt.h"

int32_t g_table_cents[201];
int32_t g_table_semi[97];
int32_t g_table_vel[128];
int32_t g_table_lin[128];
int32_t g_table_dbamp[1001];
int16_t g_table_envshape[201];
int16_t g_table_sine[256];
uint8_t g_table_companding[2048];

static int32_t trunc_i32(double x)
{
    return (int32_t) x; /* C (int) cast: truncates toward zero */
}

void tables_build(void)
{
    int i;

    for (i = -100; i <= 100; i++)
    {
        double v = 4096.0 * rt_pow(2.0, (double) i / 1200.0);
        g_table_cents[i + 100] = trunc_i32(v);
    }

    for (i = -48; i <= 48; i++)
    {
        double v = 4096.0 * rt_pow(2.0, (double) i / 12.0);
        g_table_semi[i + 48] = trunc_i32(v);
    }

    /* Velocity/attenuation table (squared law), hundredths of a dB */
    g_table_vel[0] = -9600;
    for (i = 1; i <= 127; i++)
    {
        double ratio = (double) i / 127.0;
        double v = 1000.0 * rt_log10(rt_pow(ratio, 4.0));
        g_table_vel[i] = trunc_i32(v);
    }

    /* Linear/sqrt-law table: index 0 = v=0 floor, indices 1..127 = curve */
    g_table_lin[0] = -2500;
    for (i = 1; i <= 127; i++)
    {
        double ratio = (double) i / 127.0;
        double v = 1000.0 * rt_log10(ratio);
        g_table_lin[i] = trunc_i32(v);
    }

    /* T1: dB -> linear amplitude, n = -1000..0 */
    for (i = -1000; i <= 0; i++)
    {
        double v = 4095.0 * rt_sqrt(rt_pow(10.0, (double) i / 100.0));
        g_table_dbamp[i + 1000] = trunc_i32(v);
    }

    /* Table C, builder 0x1685e-0x168b7: the 1/200 and 1/96 constants are
     * float32 in the original and load-bearing; an earlier build missed the
     * log10 and produced a parabola. */
    g_table_envshape[0] = 0;
    for (i = 1; i <= 200; i++)
    {
        /* 1/200 float32, 0x11d04 */
        double x = (double) i * 0.004999999888241291;
        /* 1/96 float32, 0x11cfc */
        double v = rt_log10(x * x) * 10000.0 * 0.010416666977107525 + 1000.0;
        g_table_envshape[i] = (int16_t) trunc_i32(v);
    }

    /* Table D: sine LFO, i = 0..255, amplitude +-100 */
    for (i = 0; i < 256; i++)
    {
        double phase = (double) i * 6.28318530717958647692 * (1.0 / 256.0);
        double v = rt_sin(phase) * 100.0;
        g_table_sine[i] = (int16_t) trunc_i32(v);
    }

    /* Table E: log-companding curve, i = 0..2047, range [0,127] */
    {
        double inv = 1.0 / rt_log10(8.0);
        for (i = 0; i < 2048; i++)
        {
            double x = 1.0 + (double) i * 7.0 * (1.0 / 2048.0);
            double v = (rt_log10(x) * 128.0) * inv;
            int32_t iv = trunc_i32(v);
            if (iv < 0)
                iv = 0;
            if (iv > 255)
                iv = 255;
            g_table_companding[i] = (uint8_t) iv;
        }
    }
}
