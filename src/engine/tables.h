/* tables.h -- the driver's runtime-built lookup tables. */
#ifndef TABLES_H
#define TABLES_H

#include <stdint.h>

typedef struct Tables
{
    int32_t cents[201];
    int32_t semi[97];
    int32_t vel[128];
    int32_t lin[128];
    int32_t dbamp[1001];
    int16_t envshape[201];
    int16_t sine[256];
    uint8_t companding[2048];
} Tables;

void tables_build(Tables *t);

#endif /* TABLES_H */
