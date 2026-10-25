/* tables.h -- the driver's runtime-built lookup tables. */
#ifndef TABLES_H
#define TABLES_H

#include <stdint.h>

extern int32_t g_table_cents[201];

extern int32_t g_table_semi[97];

extern int32_t g_table_vel[128];

extern int32_t g_table_lin[128];

extern int32_t g_table_dbamp[1001];

extern int16_t g_table_envshape[201];

extern int16_t g_table_sine[256];

extern uint8_t g_table_companding[2048];

void tables_build(void);

#endif /* TABLES_H */
