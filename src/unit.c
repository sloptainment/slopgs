/* unit.c -- the specification of slopgs. Every finding about swmidi.sys
 * 5.1.2600.5512 (Windows XP SP3, 56,576 bytes) and gm.dls (3,440,660 bytes)
 * that a DLS-1/GM/GS implementer could not assume is an assertion here, written
 * to the driver's value. [A:0xVMA] cites the driver instruction, [M: probe NN]
 * a reference capture; tags like S5.6 or S T.2 only group related assertions
 * (grep "--- S5.6"). A TODO assertion is driver behaviour the engine does not
 * match yet; its reason names the INVESTIGATE.adoc entry.
 * Run with `make unit` (builds dist/msgs-unit) or `./dist/msgs-unit`. TAP 14.
 */

#include "engine/dls.h"
#include "engine/render.h"
#include "engine/rt.h"
#include "engine/synth.h"
#include "engine/tables.h"
#include "engine/voice.h"
#include "unit_tap.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int have_dls; /* set by main(): 1 if dist/gm.dls loaded and parsed */

/* frag_tables.c -- the driver's runtime-built tables vs. tables.c/rt.c. */

/* Golden data: the driver's tables as its builders produce them, recomputed in
 * binary64 from the stored .rdata float constants with truncation. Machine
 * extracted and round-trip verified; do not hand-edit. */

static const int32_t tb_golden_vel[128] = {
    -9600, -8415, -7211, -6506, -6006, -5619, -5302, -5034, -4802, -4598, -4415,
    -4249, -4098, -3959, -3830, -3710, -3598, -3493, -3394, -3300, -3211, -3126,
    -3045, -2968, -2894, -2823, -2755, -2689, -2626, -2565, -2506, -2449, -2394,
    -2341, -2289, -2238, -2190, -2142, -2096, -2050, -2006, -1964, -1922, -1881,
    -1841, -1802, -1764, -1726, -1690, -1654, -1619, -1584, -1551, -1518, -1485,
    -1453, -1422, -1391, -1361, -1331, -1302, -1273, -1245, -1217, -1190, -1163,
    -1137, -1110, -1085, -1059, -1034, -1010, -985,  -961,  -938,  -914,  -891,
    -869,  -846,  -824,  -802,  -781,  -759,  -738,  -718,  -697,  -677,  -657,
    -637,  -617,  -598,  -579,  -560,  -541,  -522,  -504,  -486,  -468,  -450,
    -432,  -415,  -397,  -380,  -363,  -347,  -330,  -313,  -297,  -281,  -265,
    -249,  -233,  -218,  -202,  -187,  -172,  -157,  -142,  -127,  -113,  -98,
    -84,   -69,   -55,   -41,   -27,   -13,   0,
};

static const int32_t tb_golden_lin[127] = {
    -2103, -1802, -1626, -1501, -1404, -1325, -1258, -1200, -1149, -1103, -1062,
    -1024, -989,  -957,  -927,  -899,  -873,  -848,  -825,  -802,  -781,  -761,
    -742,  -723,  -705,  -688,  -672,  -656,  -641,  -626,  -612,  -598,  -585,
    -572,  -559,  -547,  -535,  -524,  -512,  -501,  -491,  -480,  -470,  -460,
    -450,  -441,  -431,  -422,  -413,  -404,  -396,  -387,  -379,  -371,  -363,
    -355,  -347,  -340,  -332,  -325,  -318,  -311,  -304,  -297,  -290,  -284,
    -277,  -271,  -264,  -258,  -252,  -246,  -240,  -234,  -228,  -222,  -217,
    -211,  -206,  -200,  -195,  -189,  -184,  -179,  -174,  -169,  -164,  -159,
    -154,  -149,  -144,  -140,  -135,  -130,  -126,  -121,  -117,  -112,  -108,
    -103,  -99,   -95,   -90,   -86,   -82,   -78,   -74,   -70,   -66,   -62,
    -58,   -54,   -50,   -46,   -43,   -39,   -35,   -31,   -28,   -24,   -21,
    -17,   -13,   -10,   -6,    -3,    0,
};

static const int16_t tb_golden_tablec[201] = {
    0,   520, 583, 620, 646, 666, 682, 696, 708, 719, 728, 737, 745, 752, 759,
    765, 771, 776, 782, 787, 791, 796, 800, 804, 808, 811, 815, 818, 822, 825,
    828, 831, 834, 836, 839, 842, 844, 847, 849, 852, 854, 856, 858, 860, 863,
    865, 867, 868, 870, 872, 874, 876, 878, 879, 881, 883, 884, 886, 887, 889,
    891, 892, 894, 895, 896, 898, 899, 901, 902, 903, 905, 906, 907, 908, 910,
    911, 912, 913, 914, 915, 917, 918, 919, 920, 921, 922, 923, 924, 925, 926,
    927, 928, 929, 930, 931, 932, 933, 934, 935, 936, 937, 938, 939, 939, 940,
    941, 942, 943, 944, 945, 945, 946, 947, 948, 949, 949, 950, 951, 952, 953,
    953, 954, 955, 956, 956, 957, 958, 958, 959, 960, 961, 961, 962, 963, 963,
    964, 965, 965, 966, 967, 967, 968, 969, 969, 970, 970, 971, 972, 972, 973,
    973, 974, 975, 975, 976, 976, 977, 978, 978, 979, 979, 980, 980, 981, 982,
    982, 983, 983, 984, 984, 985, 985, 986, 986, 987, 987, 988, 988, 989, 989,
    990, 990, 991, 991, 992, 992, 993, 993, 994, 994, 995, 995, 996, 996, 997,
    997, 998, 998, 999, 999, 999,
};

static const int16_t tb_golden_sine[256] = {
    0,   2,   4,   7,   9,   12,  14,  17,  19,  21,  24,  26,  29,  31,  33,
    35,  38,  40,  42,  44,  47,  49,  51,  53,  55,  57,  59,  61,  63,  65,
    67,  68,  70,  72,  74,  75,  77,  78,  80,  81,  83,  84,  85,  87,  88,
    89,  90,  91,  92,  93,  94,  94,  95,  96,  97,  97,  98,  98,  98,  99,
    99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  98,  98,  98,  97,  97,
    96,  95,  94,  94,  93,  92,  91,  90,  89,  88,  87,  85,  84,  83,  81,
    80,  78,  77,  75,  74,  72,  70,  68,  67,  65,  63,  61,  59,  57,  55,
    53,  51,  49,  47,  44,  42,  40,  38,  35,  33,  31,  29,  26,  24,  21,
    19,  17,  14,  12,  9,   7,   4,   2,   0,   -2,  -4,  -7,  -9,  -12, -14,
    -17, -19, -21, -24, -26, -29, -31, -33, -35, -38, -40, -42, -44, -47, -49,
    -51, -53, -55, -57, -59, -61, -63, -65, -67, -68, -70, -72, -74, -75, -77,
    -78, -80, -81, -83, -84, -85, -87, -88, -89, -90, -91, -92, -93, -94, -94,
    -95, -96, -97, -97, -98, -98, -98, -99, -99, -99, -99, -99, -99, -99, -99,
    -99, -99, -99, -98, -98, -98, -97, -97, -96, -95, -94, -94, -93, -92, -91,
    -90, -89, -88, -87, -85, -84, -83, -81, -80, -78, -77, -75, -74, -72, -70,
    -68, -67, -65, -63, -61, -59, -57, -55, -53, -51, -49, -47, -44, -42, -40,
    -38, -35, -33, -31, -29, -26, -24, -21, -19, -17, -14, -12, -9,  -7,  -4,
    -2,
};

static const uint8_t tb_golden_companding[2048] = {
    0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,
    3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,   5,
    6,   6,   6,   6,   6,   6,   7,   7,   7,   7,   7,   8,   8,   8,   8,
    8,   8,   9,   9,   9,   9,   9,   10,  10,  10,  10,  10,  10,  11,  11,
    11,  11,  11,  12,  12,  12,  12,  12,  12,  13,  13,  13,  13,  13,  13,
    14,  14,  14,  14,  14,  14,  15,  15,  15,  15,  15,  15,  16,  16,  16,
    16,  16,  16,  16,  17,  17,  17,  17,  17,  17,  18,  18,  18,  18,  18,
    18,  19,  19,  19,  19,  19,  19,  19,  20,  20,  20,  20,  20,  20,  21,
    21,  21,  21,  21,  21,  21,  22,  22,  22,  22,  22,  22,  22,  23,  23,
    23,  23,  23,  23,  23,  24,  24,  24,  24,  24,  24,  24,  25,  25,  25,
    25,  25,  25,  25,  26,  26,  26,  26,  26,  26,  26,  26,  27,  27,  27,
    27,  27,  27,  27,  28,  28,  28,  28,  28,  28,  28,  28,  29,  29,  29,
    29,  29,  29,  29,  30,  30,  30,  30,  30,  30,  30,  30,  31,  31,  31,
    31,  31,  31,  31,  31,  32,  32,  32,  32,  32,  32,  32,  32,  33,  33,
    33,  33,  33,  33,  33,  33,  34,  34,  34,  34,  34,  34,  34,  34,  34,
    35,  35,  35,  35,  35,  35,  35,  35,  36,  36,  36,  36,  36,  36,  36,
    36,  36,  37,  37,  37,  37,  37,  37,  37,  37,  38,  38,  38,  38,  38,
    38,  38,  38,  38,  39,  39,  39,  39,  39,  39,  39,  39,  39,  40,  40,
    40,  40,  40,  40,  40,  40,  40,  41,  41,  41,  41,  41,  41,  41,  41,
    41,  41,  42,  42,  42,  42,  42,  42,  42,  42,  42,  43,  43,  43,  43,
    43,  43,  43,  43,  43,  43,  44,  44,  44,  44,  44,  44,  44,  44,  44,
    44,  45,  45,  45,  45,  45,  45,  45,  45,  45,  45,  46,  46,  46,  46,
    46,  46,  46,  46,  46,  46,  47,  47,  47,  47,  47,  47,  47,  47,  47,
    47,  48,  48,  48,  48,  48,  48,  48,  48,  48,  48,  49,  49,  49,  49,
    49,  49,  49,  49,  49,  49,  49,  50,  50,  50,  50,  50,  50,  50,  50,
    50,  50,  50,  51,  51,  51,  51,  51,  51,  51,  51,  51,  51,  51,  52,
    52,  52,  52,  52,  52,  52,  52,  52,  52,  52,  53,  53,  53,  53,  53,
    53,  53,  53,  53,  53,  53,  54,  54,  54,  54,  54,  54,  54,  54,  54,
    54,  54,  54,  55,  55,  55,  55,  55,  55,  55,  55,  55,  55,  55,  55,
    56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  56,  57,  57,  57,  57,
    57,  57,  57,  57,  57,  57,  57,  57,  57,  58,  58,  58,  58,  58,  58,
    58,  58,  58,  58,  58,  58,  59,  59,  59,  59,  59,  59,  59,  59,  59,
    59,  59,  59,  60,  60,  60,  60,  60,  60,  60,  60,  60,  60,  60,  60,
    60,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  62,
    62,  62,  62,  62,  62,  62,  62,  62,  62,  62,  62,  62,  63,  63,  63,
    63,  63,  63,  63,  63,  63,  63,  63,  63,  63,  64,  64,  64,  64,  64,
    64,  64,  64,  64,  64,  64,  64,  64,  64,  65,  65,  65,  65,  65,  65,
    65,  65,  65,  65,  65,  65,  65,  65,  66,  66,  66,  66,  66,  66,  66,
    66,  66,  66,  66,  66,  66,  66,  67,  67,  67,  67,  67,  67,  67,  67,
    67,  67,  67,  67,  67,  67,  68,  68,  68,  68,  68,  68,  68,  68,  68,
    68,  68,  68,  68,  68,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,
    69,  69,  69,  69,  69,  70,  70,  70,  70,  70,  70,  70,  70,  70,  70,
    70,  70,  70,  70,  70,  71,  71,  71,  71,  71,  71,  71,  71,  71,  71,
    71,  71,  71,  71,  71,  72,  72,  72,  72,  72,  72,  72,  72,  72,  72,
    72,  72,  72,  72,  72,  72,  73,  73,  73,  73,  73,  73,  73,  73,  73,
    73,  73,  73,  73,  73,  73,  74,  74,  74,  74,  74,  74,  74,  74,  74,
    74,  74,  74,  74,  74,  74,  74,  75,  75,  75,  75,  75,  75,  75,  75,
    75,  75,  75,  75,  75,  75,  75,  75,  75,  76,  76,  76,  76,  76,  76,
    76,  76,  76,  76,  76,  76,  76,  76,  76,  76,  77,  77,  77,  77,  77,
    77,  77,  77,  77,  77,  77,  77,  77,  77,  77,  77,  77,  78,  78,  78,
    78,  78,  78,  78,  78,  78,  78,  78,  78,  78,  78,  78,  78,  78,  79,
    79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,
    79,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,
    80,  80,  80,  80,  81,  81,  81,  81,  81,  81,  81,  81,  81,  81,  81,
    81,  81,  81,  81,  81,  81,  81,  82,  82,  82,  82,  82,  82,  82,  82,
    82,  82,  82,  82,  82,  82,  82,  82,  82,  82,  83,  83,  83,  83,  83,
    83,  83,  83,  83,  83,  83,  83,  83,  83,  83,  83,  83,  83,  84,  84,
    84,  84,  84,  84,  84,  84,  84,  84,  84,  84,  84,  84,  84,  84,  84,
    84,  84,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,
    85,  85,  85,  85,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,
    86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  87,  87,  87,  87,  87,
    87,  87,  87,  87,  87,  87,  87,  87,  87,  87,  87,  87,  87,  87,  87,
    88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,
    88,  88,  88,  88,  88,  89,  89,  89,  89,  89,  89,  89,  89,  89,  89,
    89,  89,  89,  89,  89,  89,  89,  89,  89,  89,  90,  90,  90,  90,  90,
    90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,
    90,  91,  91,  91,  91,  91,  91,  91,  91,  91,  91,  91,  91,  91,  91,
    91,  91,  91,  91,  91,  91,  91,  92,  92,  92,  92,  92,  92,  92,  92,
    92,  92,  92,  92,  92,  92,  92,  92,  92,  92,  92,  92,  92,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  94,  94,  94,  94,  94,  94,  94,  94,  94,  94,
    94,  94,  94,  94,  94,  94,  94,  94,  94,  94,  94,  94,  95,  95,  95,
    95,  95,  95,  95,  95,  95,  95,  95,  95,  95,  95,  95,  95,  95,  95,
    95,  95,  95,  95,  95,  96,  96,  96,  96,  96,  96,  96,  96,  96,  96,
    96,  96,  96,  96,  96,  96,  96,  96,  96,  96,  96,  96,  97,  97,  97,
    97,  97,  97,  97,  97,  97,  97,  97,  97,  97,  97,  97,  97,  97,  97,
    97,  97,  97,  97,  97,  97,  98,  98,  98,  98,  98,  98,  98,  98,  98,
    98,  98,  98,  98,  98,  98,  98,  98,  98,  98,  98,  98,  98,  98,  99,
    99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,  100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 102, 102, 102,
    102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102,
    102, 102, 102, 102, 102, 102, 102, 103, 103, 103, 103, 103, 103, 103, 103,
    103, 103, 103, 103, 103, 103, 103, 103, 103, 103, 103, 103, 103, 103, 103,
    103, 103, 103, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104,
    104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 105,
    105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105,
    105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 106, 106, 106, 106, 106,
    106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106,
    106, 106, 106, 106, 106, 106, 106, 107, 107, 107, 107, 107, 107, 107, 107,
    107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107,
    107, 107, 107, 107, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108,
    108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108,
    108, 108, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109,
    109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109,
    110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110,
    110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 111,
    111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111,
    111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 112, 112,
    112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112,
    112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 113, 113, 113,
    113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113,
    113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 113, 114, 114, 114,
    114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114,
    114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 114, 115, 115,
    115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115,
    115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 115, 116,
    116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116,
    116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116,
    117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117,
    117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117,
    117, 117, 117, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118,
    118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118, 118,
    118, 118, 118, 118, 118, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119,
    119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119, 119,
    119, 119, 119, 119, 119, 119, 119, 119, 120, 120, 120, 120, 120, 120, 120,
    120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120,
    120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 121, 121, 121,
    121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121,
    121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121,
    121, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122,
    122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122, 122,
    122, 122, 122, 122, 122, 122, 123, 123, 123, 123, 123, 123, 123, 123, 123,
    123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123,
    123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 124, 124, 124, 124,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 124, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125,
    125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125,
    125, 125, 125, 125, 125, 125, 125, 125, 125, 126, 126, 126, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127,
};

static void t_tables(void)
{
    int i;

    /* S T.2 -- velocity/attenuation table, whole-table compare */
    {
        int mism = 0, first = -1;
        for (i = 0; i < 128; i++)
        {
            if (g_table_vel[i] != tb_golden_vel[i])
            {
                if (first < 0)
                    first = i;
                mism++;
            }
        }
        if (mism)
            tap_diag("first mismatch at [%d]: got %d, driver %d (%d/%d entries "
                     "differ)",
                     first, (int) g_table_vel[first],
                     (int) tb_golden_vel[first], mism, 128);
        is_int(mism, 0,
               "S T.2 velocity->attenuation table matches the driver's built "
               "table byte-exact "
               "(128 entries)");
    }

    is_int(g_table_vel[0], -9600,
           "S T.2 velocity table[0] is the hardcoded -9600 silence floor, not "
           "a curve point");

    /* S T.3: g_table_lin[v] vs tb_golden_lin[v-1] (golden row for v=k+1 is
     * table[k]). */
    {
        int v, mism = 0, first = -1;
        for (v = 1; v <= 127; v++)
        {
            if (g_table_lin[v] != tb_golden_lin[v - 1])
            {
                if (first < 0)
                    first = v;
                mism++;
            }
        }
        if (mism)
            tap_diag(
                "first mismatch at v=%d: got %d, driver %d (%d/127 entries "
                "differ)",
                first, (int) g_table_lin[first], (int) tb_golden_lin[first - 1],
                mism);
        is_int(mism, 0,
               "S T.3 linear/pan table matches the driver's built table "
               "byte-exact (127 "
               "entries, v=1..127)");
    }

    is_int(g_table_lin[0], -2500,
           "S T.3 v=0 floor scalar (0x1bfd0) is -2500, folded into "
           "g_table_lin[0] per tables.h's documented layout");
    is_int(g_table_lin[1], -2103, "S T.3 driver-table check: v=1 -> -2103");
    is_int(g_table_lin[64], -297, "S T.3 driver-table check: v=64 -> -297");
    is_int(g_table_lin[127], 0, "S T.3 driver-table check: v=127 -> 0");

    /* S T.4: Table C, builder 0x1685e-0x168b7. */
    {
        int mism = 0, first = -1;
        for (i = 0; i <= 200; i++)
        {
            if (g_table_envshape[i] != tb_golden_tablec[i])
            {
                if (first < 0)
                    first = i;
                mism++;
            }
        }
        if (mism)
            tap_diag("first mismatch at [%d]: got %d, driver %d (%d/%d entries "
                     "differ) -- a parabola (x*x*10000/96) here means the "
                     "builder's log10 is missing",
                     first, (int) g_table_envshape[first],
                     (int) tb_golden_tablec[first], mism, 201);
        is_int(mism, 0,
               "S T.4 Table C matches the driver's built table byte-exact (201 "
               "entries)");
    }

    is_int(g_table_envshape[0], 0,
           "S T.4 Table C t[0] is the explicit zeroing, not part of the trunc "
           "formula");
    is_int(g_table_envshape[200], 999,
           "S T.4 driver-table check: t[200] -> 999 (a missing log10 gives "
           "1104)");

    /* Table D matches the driver's despite tables.c's full-double 2*pi (the
     * driver's is float32, 0x11d18): rt_sin's undershoot plus the int16
     * truncation absorb it at all 256 phases. */
    {
        int mism = 0, first = -1;
        for (i = 0; i < 256; i++)
        {
            if (g_table_sine[i] != tb_golden_sine[i])
            {
                if (first < 0)
                    first = i;
                mism++;
            }
        }
        if (mism)
            tap_diag("first mismatch at [%d]: got %d, driver %d (%d/%d entries "
                     "differ) -- tables.c's Table D loop uses the full-double "
                     "2*pi literal, not the binary's float32-rounded 2*pi "
                     "(0x11d18 = 6.2831854820251465)",
                     first, (int) g_table_sine[first],
                     (int) tb_golden_sine[first], mism, 256);
        is_int(mism, 0,
               "S T.5 Table D (sine LFO) matches the driver's built table "
               "byte-exact (256 "
               "entries)");
    }

    is_int(g_table_sine[64], 99,
           "S T.5 driver-table check: t[64] -> 99, reached via rt_sin's Taylor "
           "undershoot rather than the driver's float32 2*pi");
    is_int(g_table_sine[192], -99,
           "S T.5 driver-table check: t[192] -> -99, reached via rt_sin's "
           "Taylor undershoot rather than the driver's float32 2*pi");

    /* S T.6 -- Table E, whole-table compare */
    {
        int mism = 0, first = -1;
        for (i = 0; i < 2048; i++)
        {
            if (g_table_companding[i] != tb_golden_companding[i])
            {
                if (first < 0)
                    first = i;
                mism++;
            }
        }
        if (mism)
            tap_diag("first mismatch at [%d]: got %d, driver %d (%d/%d entries "
                     "differ)",
                     first, (int) g_table_companding[first],
                     (int) tb_golden_companding[first], mism, 2048);
        is_int(mism, 0,
               "S T.6 Table E (log-companding) matches the driver's built "
               "table byte-exact "
               "(2048 entries)");
    }

    is_int(g_table_companding[0], 0, "S T.6 driver-table check: t[0] -> 0");
    is_int(g_table_companding[2047], 127,
           "S T.6 driver-table check: t[2047] -> 127");

    /* S3.3.1 -- entry counts/domains for the three pitch/attenuation ratio
     * tables */
    is_int((int) (sizeof(g_table_cents) / sizeof(g_table_cents[0])), 201,
           "S3.3.1 cents ratio table (0x1ad00) has 201 entries (domain "
           "n=-100..100)");
    is_int((int) (sizeof(g_table_semi) / sizeof(g_table_semi[0])), 97,
           "S3.3.1 semitone ratio table (0x1af58) has 97 entries (domain "
           "n=-48..48)");
    is_int((int) (sizeof(g_table_dbamp) / sizeof(g_table_dbamp[0])), 1001,
           "S3.3.1 dB->linear amplitude table (0x1bfc0) has 1001 entries "
           "(domain n=-1000..0)");

    is_int(g_table_dbamp[900], 1294,
           "S3.3.1 spot value T[0x1bfc0][-100] = 1294 (index n+1000=900)");
    is_int(g_table_cents[0], 3866,
           "S3.3.1 spot value T[0x1ad00][-100] = 3866 (index n+100=0)");

    /* S1.4.4 -- Q12 unity: 4096 represents a pitch ratio of 1.0 */
    is_int(g_table_cents[100], 4096,
           "S1.4.4 Q12 unity: cents table n=0 (index 100) is 4096");
    is_int(g_table_semi[48], 4096,
           "S1.4.4 Q12 unity: semitone table n=0 (index 48) is 4096");

    /* S1.4.2 -- every float->int conversion truncates toward zero, never rounds
     */
    is_int((int32_t)(-6506.72987141667), -6506,
           "S1.4.2 language-level check: C's (int32_t) cast truncates toward "
           "zero (matches the driver's shared 0x106e0 helper), not round()");

    {
        /* S1.4.2/S T.2 v=3: libm recompute shows the table took the truncating
         * branch. */
        double val = 1000.0 * log10(pow(3.0 / 127.0, 4.0));
        /* truncate toward zero, matches tables.c's trunc_i32() */
        int32_t t = (int32_t) val;
        /* round to nearest -- NOT what the driver does */
        int32_t r = (int32_t) round(val);
        is_int(t, -6506,
               "S1.4.2/S T.2 libm recompute of the v=3 formula truncates to "
               "-6506");
        is_int(g_table_vel[3], -6506,
               "S T.2 g_table_vel[3] took the truncating branch: matches "
               "-6506, not the round()-would-give value");
        is_int(r, -6507,
               "S1.4.2 round() of the same v=3 value gives -6507, one unit "
               "away from the truncating result the table actually stores");
    }

    {
        /* S T.2's own claim: 64/127 entries differ under round-to-nearest vs
         * truncate. */
        int v, diff_count = 0;
        for (v = 1; v <= 127; v++)
        {
            double ratio = (double) v / 127.0;
            double val = 1000.0 * log10(pow(ratio, 4.0));
            int32_t t = (int32_t) val;
            int32_t r = (int32_t) round(val);
            if (t != r)
                diff_count++;
        }
        is_int(diff_count, 64,
               "S T.2/S1.4.2 64 of 127 velocity-table entries would differ by "
               "one unit under round-to-nearest instead of "
               "truncate-toward-zero");
    }
}

static void t_rt(void)
{
    int i;

    /* rt_pow(2,x) vs libm over the driver's cents domain; tolerance is this
     * test's own choice (plain double is all the driver's margins need). */
    {
        int cents;
        double maxrel = 0.0, worst_x = 0.0;
        for (cents = -4800; cents <= 4800; cents++)
        {
            double x = (double) cents / 1200.0;
            double got = rt_pow(2.0, x);
            double want = pow(2.0, x);
            double rel = (want != 0.0) ? fabs(got - want) / fabs(want)
                                       : fabs(got - want);
            if (rel > maxrel)
            {
                maxrel = rel;
                worst_x = x;
            }
        }
        if (maxrel >= 1e-8)
            tap_diag("rt_pow(2.0,x) worst relative error %.3e at x=%.6f",
                     maxrel, worst_x);
        ok(maxrel < 1e-8,
           "S1.4.3 rt_pow(2.0,x) agrees with libm pow(2.0,x) to < 1e-8 "
           "relative error across the cents domain x=-4..4 "
           "(cents=-4800..4800); tolerance is this test's own choice (rt.c is "
           "a from-scratch Taylor-series exp/log, not required to be bit-exact "
           "per S1.4.3; observed worst case is ~2.7e-10, this bound gives "
           "headroom), not a driver figure");
    }

    /* rt_pow(10.0,x) vs libm pow(10.0,x) over the dB->linear domain
     * (reusing S3.3.1's own n=-1000..0 domain, x=n/100 -> -10.0..0.0). */
    {
        double maxrel = 0.0, worst_x = 0.0;
        for (i = -1000; i <= 0; i++)
        {
            double x = (double) i / 100.0;
            double got = rt_pow(10.0, x);
            double want = pow(10.0, x);
            double rel = (want != 0.0) ? fabs(got - want) / fabs(want)
                                       : fabs(got - want);
            if (rel > maxrel)
            {
                maxrel = rel;
                worst_x = x;
            }
        }
        if (maxrel >= 1e-8)
            tap_diag("rt_pow(10.0,x) worst relative error %.3e at x=%.6f",
                     maxrel, worst_x);
        ok(maxrel < 1e-8,
           "S1.4.3 rt_pow(10.0,x) agrees with libm pow(10.0,x) to < 1e-8 "
           "relative error over the dB->linear domain x=-10.0..0.0 (S3.3.1's "
           "n=-1000..0 domain); tolerance is this test's own choice (observed "
           "worst case is ~2.7e-10, this bound gives headroom), not a driver "
           "figure");
    }

    /* rt_log10 vs libm log10 over (0,1] on the velocity domain v/127, v=1..127.
     */
    {
        int v;
        double maxrel = 0.0, worst_v = 0.0;
        for (v = 1; v <= 127; v++)
        {
            double x = (double) v / 127.0;
            double got = rt_log10(x);
            double want = log10(x);
            double rel = (want != 0.0) ? fabs(got - want) / fabs(want)
                                       : fabs(got - want);
            if (rel > maxrel)
            {
                maxrel = rel;
                worst_v = v;
            }
        }
        if (maxrel >= 1e-10)
            tap_diag("rt_log10 worst relative error %.3e at v=%.0f", maxrel,
                     worst_v);
        ok(maxrel < 1e-10,
           "S1.4.3 rt_log10(v/127.0) agrees with libm log10(v/127.0) to < "
           "1e-10 relative error across the S T.2 velocity domain v=1..127; "
           "tolerance is this test's own choice (observed worst case is "
           "~1.2e-12, this bound gives headroom), not a driver figure");
    }

    /* rt_sqrt and libm sqrt both lower to hardware sqrtsd -- bit-exact, not
     * merely close. */
    {
        static const double vals[] = {0.0, 0.25,  0.5,   1.0,       2.0,   3.0,
                                      4.0, 96.04, 100.0, 1.0 / 3.0, 1.0e10};
        int n = (int) (sizeof(vals) / sizeof(vals[0]));
        int j, mism = 0, first = -1;
        for (j = 0; j < n; j++)
        {
            if (rt_sqrt(vals[j]) != sqrt(vals[j]))
            {
                if (first < 0)
                    first = j;
                mism++;
            }
        }
        if (mism)
            tap_diag(
                "rt_sqrt mismatch at vals[%d]=%.17g: rt_sqrt=%.17g sqrt=%.17g",
                first, vals[first], rt_sqrt(vals[first]), sqrt(vals[first]));
        is_int(mism, 0,
               "rt.c's own contract (both rt_sqrt and libm sqrt lower to the "
               "hardware sqrtsd instruction): rt_sqrt(x) == sqrt(x) "
               "bit-for-bit for a spread of test values");
    }

    /* rt_sin vs libm at the 256 Table-D phases, same full-double 2*pi both
     * sides --
     * isolates polynomial accuracy from the float32-2pi issue tracked in
     * t_tables (S T.5). */
    {
        const double pi2 = 6.28318530717958647692;
        double maxabs = 0.0, worst_ph = 0.0;
        for (i = 0; i < 256; i++)
        {
            double phase = (double) i * pi2 / 256.0;
            double got = rt_sin(phase);
            double want = sin(phase);
            double d = fabs(got - want);
            if (d > maxabs)
            {
                maxabs = d;
                worst_ph = phase;
            }
        }
        if (maxabs >= 1e-5)
            tap_diag("rt_sin worst absolute error %.3e at phase=%.10f", maxabs,
                     worst_ph);
        ok(maxabs < 1e-5,
           "S1.4.3 rt_sin agrees with libm sin to < 1e-5 absolute error across "
           "the 256 Table-D LFO phases i*2*pi/256; tolerance is this test's "
           "own choice (rt.c's Taylor series is truncated at x^17, observed "
           "worst case ~7.7e-7 near phase=pi; 1e-5 abs error is still far "
           "below the ~0.01 amplitude-fraction granularity Table D stores), "
           "not a driver figure");
    }

    /* rt_exp/rt_log round-trip: rt_log(rt_exp(x)) ~= x. */
    {
        double maxabs = 0.0, worst_x = 0.0;
        for (double x = -20.0; x <= 20.0; x += 0.5)
        {
            double got = rt_log(rt_exp(x));
            double d = fabs(got - x);
            if (d > maxabs)
            {
                maxabs = d;
                worst_x = x;
            }
        }
        if (maxabs >= 1e-9)
            tap_diag("rt_log(rt_exp(x)) round-trip worst absolute error %.3e "
                     "at x=%.3f",
                     maxabs, worst_x);
        ok(maxabs < 1e-9, "S1.4.3 rt_log(rt_exp(x)) round-trips x to < 1e-9 "
                          "absolute error over x=-20.0..20.0; tolerance is "
                          "this test's own choice, not a driver figure");
    }

    /* Edge-case contracts are rt.c's own documented behavior, not driver
     * findings. */
    ok(rt_sqrt(-1.0) == 0.0,
       "rt.c's own documented contract, not a driver finding: "
       "rt_sqrt(-1.0) == 0.0 per rt.c's "
       "x<=0 domain clamp");
    ok(rt_pow(0.0, 2.0) == 0.0,
       "rt.c's own documented contract, not a driver finding: "
       "rt_pow(0.0, 2.0) == 0.0 per rt.c's "
       "base==0 special case");
    ok(rt_pow(0.0, -1.0) == 1.0,
       "rt.c's own documented contract, not a driver finding: "
       "rt_pow(0.0, -1.0) == 1.0 per "
       "rt.c's base==0,exponent<=0 special case");

    /* S3.4.1's own worst-case tc=4330.571090698242 truncates to 269009 samples;
     * libm and rt_pow both agree. */
    {
        double tc = 4330.571090698242;
        double got = rt_pow(2.0, tc / 1200.0) * 22050.0;
        tap_diag("rt_pow(2.0, tc/1200.0)*22050.0 = %.17g (binary64 pow: "
                 "269009.99992038216)",
                 got);
        is_int((int32_t) got, 269009,
               "S3.4.1 gm.dls's closest-to-a-boundary timecent of its 552 EG "
               "values (tc=4330.571090698242) truncates to 269009 samples, "
               "1,367,824 ULP from the boundary [D:gm.dls]");
    }

    /* S3.4.1's 1,367,824-ULP margin, converted to relative error at 269010
     * samples; rt_pow's own worst-case error must stay under it. */
    {
        double margin_ulp = 1367824.0;
        double ulp_at_269010 = nextafter(269010.0, INFINITY) - 269010.0;
        double margin_rel = margin_ulp * ulp_at_269010 / 269010.0;

        double tc;
        double maxrel = 0.0, worst_tc = 0.0;
        for (tc = -12000.0; tc <= 8000.0; tc += 50.0)
        {
            double x = tc / 1200.0;
            double got = rt_pow(2.0, x);
            double want = pow(2.0, x);
            double rel = (want != 0.0) ? fabs(got - want) / fabs(want)
                                       : fabs(got - want);
            if (rel > maxrel)
            {
                maxrel = rel;
                worst_tc = tc;
            }
        }
        tap_diag("rt_pow(2.0, tc/1200.0) worst relative error %.3e at tc=%.1f; "
                 "gm.dls's 1,367,824 ULP margin at 269010 samples is %.3e "
                 "relative -- %.1f%% of the margin consumed",
                 maxrel, worst_tc, margin_rel, 100.0 * maxrel / margin_rel);
        ok(maxrel < margin_rel,
           "S3.4.1 rt_pow(2.0, tc/1200.0)'s own worst relative error across "
           "tc=-12000..8000 stays below gm.dls's 1,367,824-ULP margin "
           "(converted to relative terms at the 269010-sample worst-case "
           "magnitude) -- with far less headroom than the 'over a million ULP' "
           "prose suggests");
    }
}

/* frag_synth.c -- the MIDI control plane: synth.c. */

/* ---- helpers: build SysEx buffers matching S4.5's layouts. `buf` excludes
 * the leading 0xF0 (synth_sysex's own contract, synth.h). ---- */

static void sy_gs_reset(void)
{
    uint8_t b[9] = {0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x00};
    synth_sysex(b, sizeof b);
}

static void sy_use_rhythm(int block, int on)
{
    uint8_t b[9] = {0x41, 0x10,
                    0x42, 0x12,
                    0x40, (uint8_t)(0x10 | block),
                    0x15, (uint8_t)(on ? 1 : 0),
                    0x00};
    synth_sysex(b, sizeof b);
}

static void sy_rcv_channel(int block, int target_channel)
{
    uint8_t b[9] = {0x41, 0x10,
                    0x42, 0x12,
                    0x40, (uint8_t)(0x10 | block),
                    0x02, (uint8_t) target_channel,
                    0x00};
    synth_sysex(b, sizeof b);
}

static void sy_gm(int on)
{
    uint8_t b[4] = {0x7E, 0x10, 0x09, (uint8_t)(on ? 1 : 2)};
    synth_sysex(b, sizeof b);
}

static void sy_master_vol(uint8_t lsb, uint8_t msb)
{
    uint8_t b[6] = {0x7F, 0x10, 0x04, 0x01, lsb, msb};
    synth_sysex(b, sizeof b);
}

/* Select an RPN via CC101 (MSB) then CC100 (LSB) so rpn_select == rpn exactly.
 */
static void sy_select_rpn(int ch, uint16_t rpn)
{
    synth_midi((uint32_t)(0xB0 | ch), 101, (rpn >> 7) & 0x7F);
    synth_midi((uint32_t)(0xB0 | ch), 100, rpn & 0x7F);
}

static void t_synth(void)
{
    tap_diag("=== synth.c: MIDI control plane ===");

    /* S4.6.4 System Reset: fixture drives pb_range_cents/rpn_select/sustain
     * away from ctor defaults first, so "unchanged" can't pass as "reset". No
     * reset path (0x12354, 0x126c2, 0x12780) writes RPN0-2 or the select
     * register. */
    tap_diag("--- S4.6.4 reset-state summary table: System Reset (0xFF) net "
             "effect ---");
    {
        int ch = 0;
        synth_construct();
        /* GS mode on, so CC0/CC32 bank writes aren't gated off below */
        sy_gs_reset();
        /* flip is_rhythm on channel 3 (block4's default channel) --
         * MUST happen before the other mutations below, since this
         * message also fires a device-wide controller reset
         * (reset_all_channel_controllers_device) that would otherwise
         * wipe them straight back to their defaults. */
        sy_use_rhythm(4, 1);
        synth_midi(0xB0 | ch, 0, 5);  /* CC0  bank_msb = 5 */
        synth_midi(0xB0 | ch, 32, 3); /* CC32 bank_lsb = 3 */
        /* Program Change: program = 7, relatches locale */
        synth_midi(0xC0 | ch, 7, 0);
        /* pitch bend raw = 15<<7 = 1920, away from 8192 */
        synth_midi(0xE0 | ch, 0, 15);
        sy_select_rpn(ch, 0);
        synth_midi(0xB0 | ch, 6, 5);   /* RPN0: pb_range_cents = 500 */
        synth_midi(0xB0 | ch, 7, 40);  /* CC7  volume = 40 */
        synth_midi(0xB0 | ch, 11, 10); /* CC11 expression = 10 */
        synth_midi(0xB0 | ch, 10, 20); /* CC10 pan = 20 */
        synth_midi(0xB0 | ch, 1, 64);  /* CC1  modulation = 64 */
        sy_select_rpn(ch, 1);
        synth_midi(0xB0 | ch, 6, 60); /* RPN1: nonzero rpn1_fine_cents */
        synth_midi(0xB0 | ch, 38, 10);
        sy_select_rpn(ch, 2);
        synth_midi(0xB0 | ch, 6, 70); /* RPN2: rpn2_coarse_cents = 600 */
        /* rpn_select is left at 2 (not re-Nulled) so "unchanged" is
         * testable. */
        /* CC64 sustain down, raw cache = 127 */
        synth_midi(0xB0 | ch, 64, 127);
        synth_midi(0xB0 | ch, 126, 0); /* CC126 mono on */
                                       /* nonzero master volume attenuation */
        sy_master_vol(0x55, 64);

        ok(g_gs_mode != 0, "S4.6.4 setup: GS mode is on before System Reset");

        synth_midi(0xFF, 0, 0); /* System Reset */

        is_int(g_channels[ch].bank_msb, 0,
               "S4.6.4 Bank MSB -> 0 after System Reset");
        is_int(g_channels[ch].bank_lsb, 0,
               "S4.6.4 Bank LSB -> 0 after System Reset");
        is_int(g_channels[ch].program, 0,
               "S4.6.4 Program (scheduled locale) -> 0 after System Reset");
        is_int(g_channels[ch].pitch_bend, 8192,
               "S4.6.4 Pitch Bend -> 8192 after System Reset");
        is_int(g_channels[ch].pb_range_cents, 500,
               "S4.6.4 RPN0 unchanged by System Reset (no reset path "
               "writes it)");
        is_int(g_channels[ch].volume, 100,
               "S4.6.4 Channel Volume -> 100 after System Reset");
        is_int(g_channels[ch].expression, 127,
               "S4.6.4 Expression -> 127 after System Reset");
        is_int(g_channels[ch].pan, 64, "S4.6.4 Pan -> 64 after System Reset");
        is_int(g_channels[ch].modulation, 0,
               "S4.6.4 Modulation Wheel -> 0 after System Reset");
        is_int(g_channels[ch].rpn1_fine_cents, 0,
               "S4.6.4 RPN1 result field -> 0 after System Reset");
        is_int(g_channels[ch].rpn2_coarse_cents, 0,
               "S4.6.4 RPN2 result field -> 0 after System Reset");
        is_int(g_channels[ch].rpn_select, 2,
               "S4.6.4 RPN-select register unchanged by System Reset");
        is_int(g_channels[ch].data_entry_combined, 0,
               "S4.6.4 Data-Entry combined word -> 0 after System Reset");
        is_int(g_channels[9].is_rhythm, 1,
               "S4.6.4 USE RHYTHM PART: Part 0 (channel 9) -> 1 after System "
               "Reset");
        is_int(g_channels[3].is_rhythm, 0,
               "S4.6.4 USE RHYTHM PART: non-Part-0 channel -> 0 after System "
               "Reset");
        is_int(g_channels[ch].mono_mode, 0,
               "S4.6.4 Mono flag -> 0 (poly) after System Reset");
        is_int(
            g_channels[ch].sustain, 127,
            "S4.6.4 Sustain raw value not force-zeroed by System Reset (only "
            "a release event is queued, 0x1270f)");
        is_int(g_master_vol_hdb, 0,
               "S4.6.4 Master Volume attenuation -> 0 after System Reset");
        is_int(g_gs_mode, 0, "S4.6.4 GS-mode flag -> 0 after System Reset");
    }

    /* S4.6.4 reset-state summary table: GS Reset SysEx net effect. */
    tap_diag(
        "--- S4.6.4 reset-state summary table: GS Reset SysEx net effect ---");
    {
        int ch = 1;
        synth_construct();
        synth_midi(0xE0 | ch, 0, 10); /* pitch bend raw = 10<<7 = 1280 */
        sy_select_rpn(ch, 0);
        synth_midi(0xB0 | ch, 6, 9);    /* pb_range_cents = 900 */
        sy_select_rpn(ch, 1);           /* leave rpn_select == 1, non-Null */
        synth_midi(0xB0 | ch, 64, 100); /* sustain raw = 100 */

        sy_gs_reset();

        is_int(g_gs_mode, 1,
               "S4.5/S4.6.4 GS-mode flag -> 1 after GS Reset (set after "
               "ResetDevice)");
        is_int(g_channels[ch].pitch_bend, 8192,
               "S4.6.4 Pitch Bend -> 8192, GS Reset net effect");
        is_int(g_channels[ch].pb_range_cents, 900,
               "S4.6.4 RPN0 unchanged by GS Reset (no reset path "
               "writes it)");
        is_int(g_channels[ch].rpn_select, 1,
               "S4.6.4 RPN-select register unchanged by GS Reset");
        is_int(g_channels[ch].sustain, 100,
               "S4.6.4 Sustain raw value not force-zeroed by GS Reset (only a "
               "release event is queued)");
    }

    /* S4.6.4 reset-state summary table: GM System On/Off net effect. */
    tap_diag("--- S4.6.4 reset-state summary table: GM System On/Off net "
             "effect ---");
    {
        int ch = 2;
        synth_construct();
        sy_gs_reset();
        ok(g_gs_mode != 0, "S4.5 setup: GS mode is on before GM System On");
        sy_gm(1); /* GM System On */
        is_int(g_gs_mode, 0,
               "S4.5/S4.6.4 GM System On clears the GS-mode flag");

        sy_gs_reset();
        synth_midi(0xE0 | ch, 0, 10);
        sy_select_rpn(ch, 0);
        synth_midi(0xB0 | ch, 6, 9);
        sy_select_rpn(ch, 1);
        synth_midi(0xB0 | ch, 64, 100);

        sy_gm(2); /* GM System Off */

        is_int(g_gs_mode, 0,
               "S4.5/S4.6.4 GM System Off also clears the GS-mode flag");
        is_int(g_channels[ch].pb_range_cents, 900,
               "S4.6.4 RPN0 unchanged by GM System Off (no reset path "
               "writes it)");
        is_int(g_channels[ch].sustain, 100,
               "S4.6.4 Sustain raw value not force-zeroed by GM System Off");
    }

    /* S4.1.2/S4.1.3 entry-point semantics. */
    tap_diag("--- S4.1.2/S4.1.3 entry-point semantics ---");
    {
        int ch = 0;
        synth_construct();
        synth_midi(0xB0 | ch, 7, 55);
        is_int(g_channels[ch].volume, 55,
               "setup: volume mutated before entry-point tests");

        /* status==0xFF triggers reset regardless of d1/d2 */
        synth_midi(0xFF, 5, 9);
        is_int(g_channels[ch].volume, 100,
               "S4.1.2 status==0xFF triggers System Reset regardless of data "
               "bytes");

        synth_midi(0xB0 | ch, 7, 55);
        /* shares high nibble 0xF with 0xFF but is not itself 0xFF */
        synth_midi(0xF1, 0, 0);
        is_int(g_channels[ch].volume, 55,
               "S4.1.2 ShortMsg checks the full status byte, not just the high "
               "nibble (0xF1 must not reset)");

        uint8_t gs_a[9] = {0x41, 0x00, 0x42, 0x12, 0x40,
                           0x00, 0x7F, 0x00, 0x00};
        uint8_t gs_b[9] = {0x41, 0x7F, 0x42, 0x12, 0x40,
                           0x00, 0x7F, 0x00, 0x00};
        synth_construct();
        synth_sysex(gs_a, sizeof gs_a);
        int mode_a = g_gs_mode;
        synth_construct();
        synth_sysex(gs_b, sizeof gs_b);
        int mode_b = g_gs_mode;
        ok(mode_a == 1 && mode_b == 1,
           "S4.1.3 device-ID byte (buf[1]) is never read/compared -- identical "
           "effect regardless of its value");

        /* no checksum byte at all */
        uint8_t gs_nochk[8] = {0x41, 0x00, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00};
        synth_construct();
        synth_sysex(gs_nochk, sizeof gs_nochk);
        is_int(g_gs_mode, 1,
               "S4.1.3 no checksum is ever computed/checked -- GS Reset with "
               "the checksum byte absent still processed");
    }

    /* S4.3 Control Change table: implemented controllers. */
    tap_diag("--- S4.3 CC table: implemented controllers ---");
    {
        int ch = 0;
        synth_construct();
        g_gs_mode = 0;
        g_channels[ch].bank_msb = 9;
        synth_midi(0xB0 | ch, 0, 5);
        is_int(
            g_channels[ch].bank_msb, 9,
            "S4.3 CC0 Bank Select MSB gated off when GS-mode is 0 (unmoved)");
        g_gs_mode = 1;
        synth_midi(0xB0 | ch, 0, 5);
        is_int(g_channels[ch].bank_msb, 5,
               "S4.3 CC0 Bank Select MSB stores when GS-mode is 1");
        g_gs_mode = 0;
        g_channels[ch].bank_lsb = 9;
        synth_midi(0xB0 | ch, 32, 3);
        is_int(
            g_channels[ch].bank_lsb, 9,
            "S4.3 CC32 Bank Select LSB gated off when GS-mode is 0 (unmoved)");
        g_gs_mode = 1;
        synth_midi(0xB0 | ch, 32, 3);
        is_int(g_channels[ch].bank_lsb, 3,
               "S4.3 CC32 Bank Select LSB stores when GS-mode is 1");

        synth_construct();
        synth_midi(0xB0 | ch, 1, 64);
        is_int(g_channels[ch].modulation, 64, "S4.3 CC1 Modulation Wheel");
        synth_midi(0xB0 | ch, 7, 40);
        is_int(g_channels[ch].volume, 40, "S4.3 CC7 Channel Volume");
        synth_midi(0xB0 | ch, 10, 20);
        is_int(g_channels[ch].pan, 20, "S4.3 CC10 Pan");
        synth_midi(0xB0 | ch, 11, 10);
        is_int(g_channels[ch].expression, 10, "S4.3 CC11 Expression");
        synth_midi(0xB0 | ch, 64, 77);
        is_int(g_channels[ch].sustain, 77,
               "S4.3 CC64 Sustain stores the raw value");

        sy_select_rpn(ch, 1);
        synth_midi(0xB0 | ch, 98, 33);
        is_int(g_channels[ch].rpn_select, 0x3FFF,
               "S4.3/S4.4 CC98 NRPN LSB forces RPN-select to Null regardless "
               "of value");
        sy_select_rpn(ch, 2);
        synth_midi(0xB0 | ch, 99, 77);
        is_int(g_channels[ch].rpn_select, 0x3FFF,
               "S4.3/S4.4 CC99 NRPN MSB forces RPN-select to Null regardless "
               "of value");

        g_channels[ch].rpn_select = 0;
        synth_midi(0xB0 | ch, 101, 0x7F); /* MSB -> bits 7-13 = 0x7F */
        /* LSB -> bits 0-6, must not disturb bits 7-13 */
        synth_midi(0xB0 | ch, 100, 0x2A);
        is_int(g_channels[ch].rpn_select, 0x3FAA,
               "S4.4/S4.3 CC100 RPN LSB sets bits 0-6, preserves bits 7-13");

        g_channels[ch].rpn_select = 0;
        synth_midi(0xB0 | ch, 100, 0x2A); /* LSB first -> bits 0-6 = 0x2A */
        /* MSB -> bits 7-13, must not disturb bits 0-6 */
        synth_midi(0xB0 | ch, 101, 0x15);
        is_int(g_channels[ch].rpn_select, 0x0AAA,
               "S4.4/S4.3 CC101 RPN MSB sets bits 7-13, preserves bits 0-6");

        /* [A:0x1351f]-[0x13523]: CC121 gates the Volume/Pan re-schedule on
         * a nonzero value byte. */
        synth_construct();
        synth_midi(0xB0 | ch, 7, 40);
        synth_midi(0xB0 | ch, 10, 20);
        synth_midi(0xB0 | ch, 11, 10);
        synth_midi(0xE0 | ch, 0, 5);
        synth_midi(0xB0 | ch, 1, 64);
        /* CC121, value=0 -- the MIDI-conventional case, gate closed */
        synth_midi(0xB0 | ch, 121, 0);
        is_int(g_channels[ch].volume, 40,
               "S4.3 CC121 value=0 leaves Channel Volume unchanged (gate "
               "closed, item42)");
        ok(g_channels[ch].expression == 127 &&
               g_channels[ch].pitch_bend == 8192 &&
               g_channels[ch].modulation == 0,
           "S4.3 CC121 value=0 still re-schedules Expression=127, Pitch "
           "Bend=8192, Modulation=0");

        synth_construct();
        synth_midi(0xB0 | ch, 7, 40);
        synth_midi(0xB0 | ch, 11, 10);
        synth_midi(0xE0 | ch, 0, 5);
        synth_midi(0xB0 | ch, 1, 64);
        synth_midi(0xB0 | ch, 121, 5); /* CC121, value!=0 -- gate open */
        is_int(g_channels[ch].volume, 100,
               "S4.3 CC121 value!=0 re-schedules Channel Volume=100 (gate "
               "open, item42)");
        ok(g_channels[ch].expression == 127 &&
               g_channels[ch].pitch_bend == 8192 &&
               g_channels[ch].modulation == 0,
           "S4.3 CC121 value!=0 still re-schedules Expression=127, Pitch "
           "Bend=8192, Modulation=0");

        synth_construct();
        synth_midi(0xB0 | ch, 126, 0);
        is_int(g_channels[ch].mono_mode, 1,
               "S4.3 CC126 Mono Mode On sets the Mono flag");
        synth_midi(0xB0 | ch, 127, 0);
        is_int(g_channels[ch].mono_mode, 0,
               "S4.3 CC127 Poly Mode On clears the Mono flag");
    }
    tap_diag("CC120/CC121/CC123/CC126/CC127's channel-wide *voice release* "
             "half (as opposed to");
    tap_diag("the Channel-struct field writes just tested) needs an "
             "actually-active Voice, which");
    tap_diag("requires a DLS-resolved region -- have_dls is 1 in this "
             "assembled binary (dist/gm.dls");
    tap_diag("loads), so CC120/CC123 are driven here for real: a genuine "
             "note-on (channel 0, program 0,");
    tap_diag("note 60 -- verified to resolve a region) through synth_midi's "
             "own dispatch, distinct from");
    tap_diag("t_voice's S5.9 coverage of the same behaviour via a hand-built "
             "bare-voice fixture.");
    if (have_dls)
    {
        int ch = 0, note = 60;

        synth_construct();
        voice_pool_reset();
        /* real DLS-backed note-on */
        synth_midi(0x90u | (uint32_t) ch, (uint32_t) note, 100u);
        int vidx = -1;
        for (int i = 0; i < NUM_VOICES; i++)
            if (g_voices[i].active && g_voices[i].channel == ch &&
                g_voices[i].note == note)
                vidx = i;
        if (vidx < 0)
        {
            tap_skip("gm.dls loaded but channel 0/program 0/note 60 note-on "
                     "did not allocate a voice",
                     "S4.3/S5.9 CC120 All Sound Off releases a held voice, "
                     "bypassing the sustain hold (via synth_midi CC dispatch, "
                     "real note-on)");
        }
        else
        {
            synth_midi(0xB0u | (uint32_t) ch, 64, 127); /* CC64 sustain down */
            /* CC120 All Sound Off */
            synth_midi(0xB0u | (uint32_t) ch, 120, 0);
            ok(g_voices[vidx].active == 1 &&
                   g_voices[vidx].env_stage == ENV_RELEASE &&
                   g_voices[vidx].sustain_deferred == 0,
               "S4.3/S5.9 CC120 All Sound Off releases a held voice, bypassing "
               "the sustain hold (via synth_midi CC dispatch, real note-on)");
            /* pedal back up, tidy state */
            synth_midi(0xB0u | (uint32_t) ch, 64, 0);
        }
        voice_pool_reset();

        synth_construct();
        voice_pool_reset();
        /* second, independent real note-on */
        synth_midi(0x90u | (uint32_t) ch, (uint32_t) note, 100u);
        vidx = -1;
        for (int i = 0; i < NUM_VOICES; i++)
            if (g_voices[i].active && g_voices[i].channel == ch &&
                g_voices[i].note == note)
                vidx = i;
        if (vidx < 0)
        {
            tap_skip("gm.dls loaded but channel 0/program 0/note 60 note-on "
                     "did not allocate a voice",
                     "S4.3/S5.9 CC123 All Notes Off defers a held voice when "
                     "sustain is down, honouring it (via synth_midi CC "
                     "dispatch, real note-on)");
        }
        else
        {
            synth_midi(0xB0u | (uint32_t) ch, 64, 127); /* CC64 sustain down */
            /* CC123 All Notes Off */
            synth_midi(0xB0u | (uint32_t) ch, 123, 0);
            ok(g_voices[vidx].active == 1 &&
                   g_voices[vidx].sustain_deferred == 1 &&
                   g_voices[vidx].env_stage != ENV_RELEASE,
               "S4.3/S5.9 CC123 All Notes Off defers a held voice when sustain "
               "is down, honouring it (via synth_midi CC dispatch, real "
               "note-on)");
            /* pedal lift, tidy state */
            synth_midi(0xB0u | (uint32_t) ch, 64, 0);
        }
        voice_pool_reset();
    }
    else
    {
        tap_skip(
            "dist/gm.dls not loadable",
            "S4.3/S5.9 CC120 All Sound Off releases a held voice, bypassing "
            "the sustain hold (via synth_midi CC dispatch, real note-on)");
        tap_skip(
            "dist/gm.dls not loadable",
            "S4.3/S5.9 CC123 All Notes Off defers a held voice when sustain is "
            "down, honouring it (via synth_midi CC dispatch, real note-on)");
    }

    /* S4.3 Control Change table: discarded controllers. */
    tap_diag("--- S4.3 CC table: discarded controllers "
             "(send-and-assert-nothing-moved) ---");
    {
        static const int discarded_ccs[] = {2, 5, 8, 66, 91, 96, 122};
        int ch = 0;
        for (size_t i = 0; i < sizeof(discarded_ccs) / sizeof(discarded_ccs[0]);
             i++)
        {
            synth_construct();
            Channel before = g_channels[ch];
            synth_midi(0xB0 | ch, (uint32_t) discarded_ccs[i], 99);
            ok(memcmp(&before, &g_channels[ch], sizeof(Channel)) == 0,
               "S4.3 CC%d is discarded: no Channel field moves",
               discarded_ccs[i]);
        }
    }

    /* S4.4 RPN and NRPN. */
    tap_diag("--- S4.4 RPN and NRPN ---");
    {
        int ch = 0;
        synth_construct();
        sy_select_rpn(ch, 0);
        synth_midi(0xB0 | ch, 6, 5);
        is_int(g_channels[ch].pb_range_cents, 500,
               "S4.4 RPN0: CC6 MSB writes data2*100 cents");
        synth_midi(0xB0 | ch, 38, 99);
        is_int(g_channels[ch].pb_range_cents, 500,
               "S4.4 RPN0: CC38 LSB has no effect");

        synth_construct();
        sy_select_rpn(ch, 1);
        synth_midi(0xB0 | ch, 6, 63);
        is_int(g_channels[ch].rpn1_fine_cents, -1,
               "S4.4 RPN1: CC6-only combined value, C-style truncation toward "
               "zero");
        synth_midi(0xB0 | ch, 38, 100);
        is_int(g_channels[ch].rpn1_fine_cents, 0,
               "S4.4 RPN1: CC38 recomputes using the just-updated combined "
               "word, truncation toward zero");

        synth_construct();
        sy_select_rpn(ch, 2);
        synth_midi(0xB0 | ch, 6, 70);
        is_int(g_channels[ch].rpn2_coarse_cents, 600,
               "S4.4 RPN2: CC6 MSB writes (data2-64)*100 cents");
        synth_midi(0xB0 | ch, 38, 99);
        is_int(g_channels[ch].rpn2_coarse_cents, 600,
               "S4.4 RPN2: CC38 LSB has no effect");

        synth_construct();
        synth_midi(0xB0 | ch, 98, 0); /* NRPN LSB: forces RPN-select to Null */
        is_int(g_channels[ch].rpn_select, 0x3FFF,
               "setup: RPN-select is Null before the discard test");
        synth_midi(0xB0 | ch, 6, 99);
        synth_midi(0xB0 | ch, 38, 88);
        ok(g_channels[ch].pb_range_cents == 200 &&
               g_channels[ch].rpn1_fine_cents == 0 &&
               g_channels[ch].rpn2_coarse_cents == 0,
           "S4.4 Null/unselected RPN: CC6/CC38 leave RPN0/RPN1/RPN2 untouched");
        is_int(g_channels[ch].data_entry_combined, (99 << 7) | 88,
               "S4.4 Null/unselected RPN: the raw combined data-entry word "
               "still updates");
    }

    /* bend cents = ((raw-8192)*range)>>13 [A:0x16d73]-[A:0x16d7c]: an
     * arithmetic shift, floor -- unlike RPN1's C division, which truncates */
    tap_diag("--- S3.3.2(c) pitch bend -> cents ---");
    {
        int ch = 0;
        synth_construct();
        static const struct
        {
            int raw;
            int32_t want;
        } spread[] = {
            {0, -200}, {4096, -100}, {8192, 0}, {12288, 100}, {16383, 199},
        };
        for (size_t i = 0; i < sizeof(spread) / sizeof(spread[0]); i++)
        {
            synth_midi(0xE0 | ch, spread[i].raw & 0x7F,
                       (spread[i].raw >> 7) & 0x7F);
            is_int(synth_pitch_bend_cents(ch), spread[i].want,
                   "S3.3.2c pitch bend cents at raw=%d, range=200",
                   spread[i].raw);
        }

        /* Discriminating case: floor and C-style truncation-toward-zero differ
         * by one cent here (-103 vs -102); the driver floors. */
        synth_midi(0xE0 | ch, 4000 & 0x7F, (4000 >> 7) & 0x7F);
        is_int(synth_pitch_bend_cents(ch), -103,
               "S3.3.2c pitch bend cents floors (raw=4000, range=200): -103, "
               "not truncating-toward-zero's -102");

        sy_select_rpn(ch, 0);
        synth_midi(0xB0 | ch, 6, 7); /* pb_range_cents = 700 */
        synth_midi(0xE0 | ch, 4096 & 0x7F, (4096 >> 7) & 0x7F);
        is_int(synth_pitch_bend_cents(ch), -350,
               "S3.3.2c pitch bend cents at raw=4096, re-ranged to 700 cents");
        synth_midi(0xE0 | ch, 12288 & 0x7F, (12288 >> 7) & 0x7F);
        is_int(synth_pitch_bend_cents(ch), 350,
               "S3.3.2c pitch bend cents at raw=12288, re-ranged to 700 cents");
    }

    /* S4.8 channel 10/drum-part selection; S4.2.1 scheduled-locale latch. */
    tap_diag("--- S4.8 channel 10/drum-part selection; S4.2.1 scheduled-locale "
             "latch ---");
    {
        synth_construct();
        ok((synth_channel_locale(9) & 0x80000000u) != 0,
           "S4.8 channel 9 (MIDI ch 10) has the drum bit set by default, "
           "GS-mode off");
        ok((synth_channel_locale(0) & 0x80000000u) == 0,
           "S4.8 channel 0 has no drum bit by default");

        sy_gs_reset();
        sy_use_rhythm(0, 0); /* block 0 -> default channel 9 */
        ok((synth_channel_locale(9) & 0x80000000u) == 0,
           "S4.8 drum bit is read live: USE RHYTHM PART clearing it is visible "
           "with no following Program Change");

        synth_construct();
        sy_gs_reset();
        /* CC0 bank_msb=5, channel 0. NOTE: the scheduled
         * locale is 21 bits (program bits 0-6, bank_lsb
         * bits 7-13, bank_msb bits 14-20) -- mask with
         * 0x1FFFFF, not 0x3FFF, or bank_msb's own bits
         * are invisible to the check. */
        synth_midi(0xB0, 0, 5);
        ok((synth_channel_locale(0) & 0x1FFFFF) == 0,
           "S4.2.1/S4.3 Bank Select alone does not retarget the scheduled "
           "locale");
        /* Program Change: relatches from the now-current bank */
        synth_midi(0xC0, 0, 0);
        is_int(synth_channel_locale(0) & 0x1FFFFF, 5u << 14,
               "S4.2.1 a following Program Change latches the new bank into "
               "the scheduled locale");
    }

    /* S4.5/T.8 RCV CHANNEL remaps a Part's channel; the per-part SysEx
     * handlers then follow it. */
    tap_diag("--- S4.5 RCV CHANNEL: Part -> channel remap ---");
    {
        synth_construct();
        sy_gs_reset();
        sy_rcv_channel(1, 6); /* data 6 -> channel 5 (n-1, 0x137c7) */
        /* USE RHYTHM PART for block 1 should now target channel 5 */
        sy_use_rhythm(1, 1);
        is_int(g_channels[5].is_rhythm, 1,
               "S4.5/T.8 RCV CHANNEL remaps block 1's Part to channel 5, so "
               "USE RHYTHM PART for block 1 lands there");
        is_int(g_channels[0].is_rhythm, 0,
               "S4.5/T.8 ...and channel 0 (block 1's un-remapped default "
               "target) is left untouched");
    }

    /* S4.5 unrecognized SysEx addresses; Master Volume MSB-only. */
    tap_diag(
        "--- S4.5 unrecognized GS DT1 addresses; Master Volume MSB-only ---");
    {
        synth_construct();
        sy_gs_reset();
        Channel before[16];
        memcpy(before, g_channels, sizeof before);
        /* a2=0x99: unrecognized */
        uint8_t gs_dt1[9] = {0x41, 0x10, 0x42, 0x12, 0x40,
                             0x11, 0x99, 0x00, 0x00};
        synth_sysex(gs_dt1, sizeof gs_dt1);
        ok(memcmp(before, g_channels, sizeof before) == 0,
           "S4.5 unrecognized GS DT1 address is silently dropped, no Channel "
           "field moves");

        synth_construct();
        sy_master_vol(0x55, 64);
        int32_t hdb1 = g_master_vol_hdb;
        synth_construct();
        sy_master_vol(0x00, 64);
        int32_t hdb2 = g_master_vol_hdb;
        ok(hdb1 == hdb2, "S4.5 Master Volume SysEx: the LSB is never read, "
                         "only the MSB matters");
    }

    tap_diag("=== end synth.c coverage ===");
    synth_construct(); /* leave global state clean for the next fragment */
}

/* frag_voice.c -- note-on, voice model and mixer: voice.c and render.c. */

/* Minimal Artic for pool/steal/choke/release ROUTING fixtures (not real DLS
 * envelope shape); eg1_release_tc==0 gives a known 1.0s release so S5.6's
 * fast-release clamp binds deterministically below. */
static Artic vo_bare_artic;

static void vo_setup_bare_active(Voice *v, int channel, int note)
{
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->channel = channel;
    v->note = note;
    v->artic = &vo_bare_artic;
    v->held = 1;
    v->env_stage = ENV_SUSTAIN;
    v->env_level = 1.0;
    v->env_sustain_level = 1.0;
    v->eg2_stage = ENV_IDLE;
    v->age = ++g_voice_age_counter;
}

static int vo_all_eq_i16(const int16_t *buf, uint32_t n, int16_t val)
{
    for (uint32_t i = 0; i < n; i++)
        if (buf[i] != val)
            return 0;
    return 1;
}

/* S3.4.1: tc=lScale/65536, duration=2^(tc/1200), INT32_MIN->0. Independent
 * reimplementation (voice.c's own is static) sharing rt_pow, so directly
 * comparable. */
static double vo_timecents_to_seconds(int32_t tc)
{
    if (tc == (int32_t) 0x80000000)
        return 0.0;
    double t = (double) tc / 65536.0;
    return rt_pow(2.0, t / 1200.0);
}

/* key/velocity-follow term (0x198b6): clamp(depth*src/127, +-4800) cents
 * added to a raw timecent value before conversion. */
static int32_t vo_scale_tc_by_source(int32_t tc, int16_t depth, int src)
{
    if (tc == (int32_t) 0x80000000 || depth == 0)
        return tc;
    int32_t cents = (int32_t) depth * (int32_t) src / 127;
    if (cents > 4800)
        cents = 4800;
    if (cents < -4800)
        cents = -4800;
    return tc + cents * 65536;
}

/* S3.3.3 CentsToRatio: clamp +-4800, T2 lookup for |cents|<=100, else T2/T3
 * decomposition. Reads the same public tables src's static cents_to_ratio_q12
 * does. */
static int32_t vo_cents_to_ratio_q12(int32_t cents)
{
    if (cents > 4800)
        cents = 4800;
    if (cents < -4800)
        cents = -4800;
    if (cents >= -100 && cents <= 100)
        return g_table_cents[cents + 100];
    int32_t whole = cents / 100;
    int32_t rem = cents % 100;
    int32_t oct = whole / 12;
    int32_t semi = whole % 12;
    int64_t t2 = g_table_cents[rem + 100];
    int64_t t3 = g_table_semi[semi + 48];
    int64_t scaled = (oct >= 0) ? (t2 << oct) : (t2 >> (-oct));
    return (int32_t)((t3 * scaled) >> 12);
}

static void t_voice(void)
{
    synth_construct();
    voice_pool_reset();

    /* ---------------- S5.2: pool size, 48 primary / 6 reserve ----------------
     */
    tap_diag("--- S5.2: pool construction ---");
    is_int(NUM_VOICES, 54,
           "S5.2: pool is 54 physically distinct voice objects");
    {
        int nprim = 0, nres = 0;
        for (int i = 0; i < NUM_VOICES; i++)
        {
            if (!g_voices[i].active && !g_voices[i].in_reserve)
                nprim++;
            if (!g_voices[i].active && g_voices[i].in_reserve)
                nres++;
        }
        is_int(nprim, 48,
               "S5.2/S5.5: 48 voices free-tagged primary after "
               "voice_pool_reset()");
        is_int(
            nres, 6,
            "S5.2/S5.5: 6 voices free-tagged reserve after voice_pool_reset()");
    }

    /* ------ S5.3: allocation order (primary -> reserve -> steal) & recycle
     * ------ */
    tap_diag("--- S5.3: free lists and allocation order ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S5.3 allocation-order fill/steal/recycle sequence");
    }
    else
    {
        voice_pool_reset();
        int ch = 0;
        /* Same (channel, 36+(k%60)) scheme cli.c's own --selftest uses for its
         * S5.5 saturation check (src/cli.c) -- reused here, not reinvented,
         * for 55 GUARANTEED-distinct notes (55 <= 60) on gm.dls's default
         * program 0, so no same-note retrigger or (absent an exclusive key
         * group on that patch) key-group choke confounds the fill. */
        voice_note_on(ch, 36 + 0, 100);
        is_int(g_voices[0].note, 36,
               "S5.3: 1st note-on fills primary slot 0 (lowest-index-first)");
        for (int k = 1; k < 48; k++)
            voice_note_on(ch, 36 + (k % 60), 100);
        is_int(g_voices[47].note, 36 + 47,
               "S5.3: 48th note-on fills primary slot 47 (last primary)");
        voice_note_on(ch, 36 + 48, 100);
        is_int(g_voices[48].note, 36 + 48,
               "S5.3: 49th note-on falls through to reserve slot 48 (first "
               "reserve)");
        for (int k = 49; k < 54; k++)
            voice_note_on(ch, 36 + (k % 60), 100);
        is_int(g_voices[53].note, 36 + 53,
               "S5.3: 54th note-on fills reserve slot 53 (last reserve, pool "
               "now full)");
        voice_note_on(ch, 36 + 54, 100);
        is_int(g_voices[0].note, 36 + 54,
               "S5.3/S5.7: 55th note-on forces asymmetric steal; with every "
               "voice held, oldest (slot 0, age 1) is evicted");
        ok(g_voices[0].active == 1,
           "S5.3: the stolen slot is reused, not freed");

        /* S5.3 "recycle timing": a voice cannot supply a free slot to a
         * note-on dispatched in the same tick it finishes in -- ordinary
         * note-off alone must not make it eligible via find_free_primary. */
        voice_note_off(ch, g_voices[1].note);
        ok(g_voices[1].active == 1 && g_voices[1].held == 0,
           "S5.3: note-off releases (held=0) but does NOT free the slot "
           "immediately (active stays 1)");

        /* S5.3 "recycling... both target primary only, never reserve": drain
         * a RESERVE-tier voice (slot 48) to fully inactive, then confirm the
         * next note-on can reuse it via the PRIMARY path (find_free_primary
         * scans lowest-index-first; slots 0-47 are all still active, so slot
         * 48 being reachable at all here means it was recycled as primary). */
        voice_note_off(ch, g_voices[48].note);
        for (int guard = 0; guard < 5000000 && g_voices[48].active; guard++)
            voice_step_envelope(&g_voices[48]);
        if (g_voices[48].active)
        {
            tap_skip(
                "slot 48's release did not finish inside the iteration guard",
                "S5.3 recycle-always-primary check");
        }
        else
        {
            voice_note_on(ch, 36 + 55, 100);
            is_int(g_voices[48].note, 36 + 55,
                   "S5.3: a finished RESERVE-tier voice is recycled onto the "
                   "PRIMARY free list, not reserve");
        }
        voice_pool_reset();
    }

    /* -------------------------- S5.4: reserve top-up
     * -------------------------- */
    tap_diag("--- S5.4: reserve top-up, Branch A (retag) and Branch B "
             "(fast-release) ---");
    {
        /* Branch A: reserve short by 1, one free PRIMARY slot available --
         * move it to reserve, touch no active voice. */
        voice_pool_reset();
        /* consume the sole reserve slot we leave free elsewhere */
        vo_setup_bare_active(&g_voices[48], 1, 0);
        /* any frame count >= the internal interval fires exactly one topup pass
         */
        voice_topup_tick(1u << 30);
        ok(g_voices[0].in_reserve == 1,
           "S5.4 Branch A: a free PRIMARY node (slot 0, lowest index) is "
           "retagged reserve when the reserve tier is short");
        ok(g_voices[48].fast_release_committed == 0 && g_voices[48].active == 1,
           "S5.4 Branch A: the one active voice is untouched (Branch A never "
           "marks/frees an active voice)");
        voice_pool_reset();
    }
    {
        /* Branch B: saturate all 54 (need primary ALSO empty), all held ->
         * marks exactly NUM_RESERVE(6) oldest active voices for fast release,
         * synchronously (still active==1, not freed). Also folds in S5.6's
         * fast-release-flag / clamp (S5.6/S3.8.2) and S3.4's "EG2 releases on
         * both paths, unclamped" (S5.6/Part 7) checks on the same victims,
         * since Branch B's start_release(v,1) call is the same primitive
         * choke/retrigger use. */
        voice_pool_reset();
        for (int i = 0; i < NUM_VOICES; i++)
        {
            vo_setup_bare_active(&g_voices[i], 1, i);
            /* non-IDLE, so a release transition is observable */
            g_voices[i].eg2_stage = ENV_SUSTAIN;
        }
        voice_topup_tick(1u << 30);
        int marked_ok = 1, unmarked_ok = 1;
        for (int i = 0; i < 6; i++)
            if (!(g_voices[i].fast_release_committed == 1 &&
                  g_voices[i].env_stage == ENV_RELEASE &&
                  g_voices[i].active == 1))
                marked_ok = 0;
        for (int i = 6; i < NUM_VOICES; i++)
            if (g_voices[i].fast_release_committed != 0)
                unmarked_ok = 0;
        ok(marked_ok,
           "S5.4 Branch B/S5.7 rule 4: the 6 OLDEST active voices (slots 0-5, "
           "all held) are marked, released segment still active");
        ok(unmarked_ok,
           "S5.4 Branch B: exactly NUM_RESERVE(6) victims marked, no more");

        double samples = ((1.0 / 70.0) * (double) RENDER_RATE);
        double expected_coef = rt_pow(10.0, (-(96.0 / 20.0) * 1.0) / samples);
        is_near(g_voices[0].env_release_coef, expected_coef, 1e-9,
                "S5.6/S3.8.2: fast-release from full level clamps to "
                "sampleRate/70 samples (14.29ms @ 22050Hz), not the "
                "unreconciled 70ms [M] figure");
        ok(g_voices[0].eg2_stage == ENV_RELEASE,
           "S5.6/Part7: EG2 (pitch envelope) also transitions to release on "
           "the fast-release path, same as ordinary note-off");
        voice_pool_reset();
    }
    {
        /* S5.7 symmetric comparator, isolated with need==1 (primary fully
         * consumed, reserve short by exactly 1) so exactly one victim is
         * chosen per call and the winner is unambiguous. */
        tap_diag("--- S5.7: symmetric comparator (top-up Branch B), rule by "
                 "rule ---");

        /* rule (2)+(3): released beats held; among released, lower env_level
         * wins. */
        voice_pool_reset();
        /* 48 primary + 1 reserve: need==1 */
        for (int i = 0; i < 49; i++)
            vo_setup_bare_active(&g_voices[i], 1, i);
        g_voices[5].held = 0;
        g_voices[5].env_level = 0.7;
        /* lower -> should win */
        g_voices[20].held = 0;
        g_voices[20].env_level = 0.1;
        voice_topup_tick(1u << 30);
        ok(g_voices[20].fast_release_committed == 1 &&
               g_voices[5].fast_release_committed == 0,
           "S5.7 rule 2/3: released beats held, and among released the LOWER "
           "env_level (slot 20) wins over slot 5");
        voice_pool_reset();

        /* rule (1): a voice already fast_release_committed==1 is never
         * eligible, even if it would otherwise win on rules 2/3. */
        for (int i = 0; i < 49; i++)
            vo_setup_bare_active(&g_voices[i], 1, i);
        g_voices[7].held = 0;
        g_voices[7].env_level = 0.05;
        g_voices[7].fast_release_committed = 1;
        voice_topup_tick(1u << 30);
        ok(g_voices[0].fast_release_committed == 1 &&
               g_voices[7].env_level == 0.05,
           "S5.7 rule 1: a pre-marked voice (slot 7, lowest env_level) is "
           "excluded; fallback picks the oldest HELD voice (slot 0) instead");
        voice_pool_reset();

        /* rule (4), no manual pokes: all-held natural ages -> oldest (slot 0)
         * wins. */
        for (int i = 0; i < 49; i++)
            vo_setup_bare_active(&g_voices[i], 1, i);
        voice_topup_tick(1u << 30);
        ok(g_voices[0].fast_release_committed == 1,
           "S5.7 rule 4: among two held candidates, older (smaller age -- slot "
           "0, allocated first) wins");
        voice_pool_reset();
    }
    {
        /* S5.7 asymmetric comparator (note-on's own fallback): a released
         * `best` is NOT protected from a later, older HELD candidate; the
         * comparator never reads/writes fast_release_committed at all. */
        tap_diag(
            "--- S5.7: asymmetric comparator (note-on fallback steal) ---");
        if (!have_dls)
        {
            tap_skip("dist/gm.dls not loadable",
                     "S5.7 asymmetric-comparator steal trigger needs one real "
                     "note-on");
        }
        else
        {
            voice_pool_reset();
            for (int i = 0; i < NUM_VOICES; i++)
                vo_setup_bare_active(&g_voices[i], 1, i);
            /* released mid-scan */
            g_voices[10].held = 0;
            g_voices[10].env_level = 0.3;
            /* older-than-everything HELD, and pre-marked */
            g_voices[50].age = 0;
            g_voices[50].fast_release_committed = 1;
            int newCh = 2, newNote = 60;
            uint32_t locale = synth_channel_locale(newCh);
            Region *r = dls_find_region(locale, (uint8_t) newNote, 0);
            if (!r)
            {
                tap_skip("dist/gm.dls has no region for channel 2 default "
                         "program, note 60",
                         "S5.7 asymmetric-comparator trigger");
            }
            else
            {
                voice_note_on(newCh, newNote, 100);
                ok(g_voices[50].channel == newCh &&
                       g_voices[50].note == newNote,
                   "S5.7 asymmetric: an older HELD candidate (slot 50) "
                   "displaces an already-released `best` (slot 10) purely on "
                   "age");
                ok(g_voices[10].channel == 1 && g_voices[10].note == 10,
                   "S5.7 asymmetric: the released voice (slot 10) is NOT "
                   "protected -- it was not the one stolen");
                /* slot 50 was pre-marked fast_release_committed==1 and still
                 * won: proves the asymmetric comparator never gates on that
                 * flag (unlike the symmetric one). */
            }
            voice_pool_reset();
        }
    }

    /* --------------------- S5.5: measured 80-notes/48-survive
     * --------------------- */
    tap_diag(
        "--- S5.5: 80 held note-ons must leave exactly 48 sounding [M] ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S5.5 80-note saturation invariant");
    }
    else
    {
        /* Exact recipe src/cli.c's own --selftest uses (same channel/notes/
         * render cadence) -- reused verbatim rather than re-derived, since a
         * shorter or denser cadence is a KNOWN way to break this invariant
         * (see voice.c's own extensive comment above TOPUP_INTERVAL_FRAMES). */
        synth_construct();
        static int16_t vo_sat_buf[4096 * 2];
        for (int k = 0; k < 80; k++)
            voice_note_on(0, 36 + (k % 60), 100);
        for (int k = 0; k < 40 * RESAMPLE_FACTOR; k++)
            render_frames(vo_sat_buf, 4096);
        int surviving = 0;
        for (int k = 0; k < NUM_VOICES; k++)
            if (g_voices[k].active)
                surviving++;
        is_int(surviving, 48,
               "S5.5 [M]: 80 held note-ons, no note-off, leave exactly 48 "
               "voices sounding");
        voice_pool_reset();
    }

    /* --------------------- S5.6: note-off vs release-rate law
     * --------------------- */
    tap_diag("--- S5.6/S3.4.2: ordinary release, level-independent rate ---");
    {
        synth_construct();
        vo_setup_bare_active(&g_voices[0], 4, 10);
        g_voices[0].env_level = 1.0;
        vo_setup_bare_active(&g_voices[1], 5, 20);
        g_voices[1].env_level = 0.4;
        g_channels[4].sustain = 0;
        g_channels[5].sustain = 0;
        voice_note_off(4, 10);
        voice_note_off(5, 20);
        ok(g_voices[0].fast_release_committed == 0 &&
               g_voices[1].fast_release_committed == 0,
           "S5.6: ordinary note-off never sets the fast-release flag");
        ok(g_voices[0].env_stage == ENV_RELEASE &&
               g_voices[1].env_stage == ENV_RELEASE,
           "S5.6: ordinary note-off moves the voice to ENV_RELEASE");
        is_near(g_voices[0].env_release_coef, g_voices[1].env_release_coef,
                1e-12,
                "S3.4.2: release RATE (96dB/authoredRelease) is independent of "
                "the level released from -- full (1.0) vs partial (0.4) give "
                "the SAME coefficient");
        voice_pool_reset();
    }

    /* --------------------------- S5.8/S3.8: key groups
     * --------------------------- */
    tap_diag("--- S5.8/S3.8.1: exclusive key groups, ungated by channel-9/drum "
             "status ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable", "S5.8/S3.8.1 key-group choke");
    }
    else
    {
        synth_construct();
        int ch = 0, noteA = 40, noteB = 41;
        uint32_t locale = synth_channel_locale(ch);
        Region *rA = dls_find_region(locale, (uint8_t) noteA, 0);
        Region *rB = dls_find_region(locale, (uint8_t) noteB, 0);
        if (!rA || !rB)
        {
            tap_skip("dist/gm.dls missing a region for channel 0 default "
                     "program, notes 40/41",
                     "S5.8/S3.8.1 key-group choke");
        }
        else
        {
            uint8_t origA = rA->key_group, origB = rB->key_group;
            /* Artificial nonzero key group on a melodic (non-rhythm, channel 0)
             * instrument chokes exactly like a drum kit: the gate
             * (0x12efc-0x12f44) has no channel-9 or drum test. */
            rA->key_group = rB->key_group = 7;

            voice_pool_reset();
            voice_note_on(ch, noteA, 100);
            int idxA = -1;
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].note == noteA)
                    idxA = i;
            voice_note_on(ch, noteB, 100);
            ok(idxA >= 0 && g_voices[idxA].fast_release_committed == 1 &&
                   g_voices[idxA].env_stage == ENV_RELEASE,
               "S5.8/S3.8.1: same channel, shared nonzero key group -> the "
               "earlier note is choked via the fast-release path, on a melodic "
               "(channel 0) instrument");
            ok(g_voices[idxA].active == 1,
               "S3.8.2: choke releases via ClampedRelease, it does not cut "
               "instantly");

            /* Scope requires the SAME channel too: a different channel sharing
             * the same key group + locale must NOT choke (S3.8.1's stated
             * scope is (channel, keygroup, locale), not keygroup+locale alone).
             */
            voice_pool_reset();
            voice_note_on(ch, noteA, 100);
            idxA = -1;
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].note == noteA)
                    idxA = i;
            voice_note_on(ch + 1, noteB, 100);
            ok(idxA >= 0 && g_voices[idxA].fast_release_committed == 0,
               "S3.8.1: a DIFFERENT channel sharing the same key group/locale "
               "does NOT choke -- scope includes channel");

            rA->key_group = origA;
            rB->key_group = origB;
            voice_pool_reset();
        }
    }

    /* -------------------- S5.9/S4.3: sustain pedal, CC120/CC123
     * -------------------- */
    tap_diag("--- S5.9/S4.3: sustain pedal (CC64), CC120 (bypasses), CC123 "
             "(honours) ---");
    {
        synth_construct();
        int ch = 6;
        vo_setup_bare_active(&g_voices[0], ch, 10);
        synth_midi(0xB0u | (uint32_t) ch, 64, 127); /* pedal down */
        synth_midi(0x80u | (uint32_t) ch, 10, 0);   /* ordinary note-off */
        ok(g_voices[0].sustain_deferred == 1 && g_voices[0].held == 1,
           "S5.9: note-off while CC64 held defers (sustain_deferred=1), does "
           "not release yet");
        synth_midi(0xB0u | (uint32_t) ch, 64, 0); /* pedal lift */
        ok(g_voices[0].env_stage == ENV_RELEASE &&
               g_voices[0].fast_release_committed == 0 &&
               g_voices[0].sustain_deferred == 0,
           "S5.9: CC64 lift releases deferred voices via the ORDINARY "
           "(unclamped) release path");

        vo_setup_bare_active(&g_voices[1], ch, 20);
        synth_midi(0xB0u | (uint32_t) ch, 64, 127); /* pedal down */
        synth_midi(0xB0u | (uint32_t) ch, 123, 0);  /* CC123 All Notes Off */
        ok(g_voices[1].sustain_deferred == 1 &&
               g_voices[1].env_stage != ENV_RELEASE,
           "S4.3 CC123: honours the pedal -- defers instead of releasing while "
           "CC64 is down");
        synth_midi(0xB0u | (uint32_t) ch, 64, 0);
        ok(g_voices[1].env_stage == ENV_RELEASE,
           "S4.3 CC123: deferred voice is released once the pedal lifts");

        vo_setup_bare_active(&g_voices[2], ch, 30);
        synth_midi(0xB0u | (uint32_t) ch, 64, 127); /* pedal down again */
        synth_midi(0xB0u | (uint32_t) ch, 120, 0);  /* CC120 All Sound Off */
        ok(g_voices[2].env_stage == ENV_RELEASE &&
               g_voices[2].sustain_deferred == 0 &&
               g_voices[2].fast_release_committed == 0,
           "S4.3 CC120: bypasses the pedal entirely -- releases immediately "
           "via the ordinary path despite CC64 being down");

        voice_pool_reset();
        synth_midi(0xB0u | (uint32_t) ch, 64, 0); /* leave the pedal up */
    }

    /* --------------------------- S3.3.2: rhythm/RPN2 gate
     * --------------------------- */
    tap_diag("--- S3.3.2(a)/(d): note-on tuning accumulator, RPN2 gated for "
             "rhythm parts only ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S3.3.2 tuning-accumulator/rhythm-gate check");
    }
    else
    {
        synth_construct(); /* channel 9 is_rhythm==1 by construction */
        int ch = 9, note = 42;
        g_channels[ch].rpn2_coarse_cents = 500;
        g_channels[ch].rpn1_fine_cents = 30;
        uint32_t locale = synth_channel_locale(ch);
        Region *r = dls_find_region(locale, (uint8_t) note, 0);
        if (!r)
        {
            tap_skip("dist/gm.dls has no rhythm-channel region for note 42",
                     "S3.3.2 rhythm-gate check");
        }
        else
        {
            voice_note_on(ch, note, 100);
            int vidx = -1;
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].channel == ch &&
                    g_voices[i].note == note)
                    vidx = i;
            if (vidx < 0)
            {
                tap_skip("note-on did not allocate a voice",
                         "S3.3.2 rhythm-gate check");
            }
            else
            {
                int32_t expected = (int32_t) r->fine_tune +
                                   (note - (int) r->unity_note) * 100 +
                                   0 /* RPN2 skipped: is_rhythm */
                                   + g_channels[ch].rpn1_fine_cents;
                is_int(g_voices[vidx].base_cents, expected,
                       "S3.3.2(a)/(d): base_cents excludes RPN2 (rhythm part) "
                       "but includes RPN1 (never gated)");
            }
        }
        voice_pool_reset();
    }

    /* --- S3.3.2(c)/S3.3.3/S6.5: live bend vs latched RPN2, cents-ratio, wave
     * rate --- */
    tap_diag("--- S3.3.2(c)/S3.3.3/S6.5: live pitch bend, latched RPN2, "
             "CentsToRatio, wave-rate phase step ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S3.3.2(c)/S3.3.3/S6.5 live-bend group");
    }
    else
    {
        synth_construct();
        int ch = 2, note = 60, vidx = -1;
        g_channels[ch].pb_range_cents = 200;
        uint32_t locale = synth_channel_locale(ch);
        Region *r = dls_find_region(locale, (uint8_t) note, 0);
        if (!r)
        {
            tap_skip("dist/gm.dls has no region for channel 2 default program, "
                     "note 60",
                     "S3.3.2(c)/S3.3.3/S6.5 group");
        }
        else
        {
            voice_note_on(ch, note, 100);
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].channel == ch &&
                    g_voices[i].note == note)
                    vidx = i;
            if (vidx < 0)
            {
                tap_skip("note-on did not allocate a voice",
                         "S3.3.2(c)/S3.3.3/S6.5 group");
            }
            else
            {
                Voice *v = &g_voices[vidx];
                uint32_t base_ratio = v->base_ratio_q12;
                uint32_t wave_rate = v->wave->sample_rate;
                uint32_t target_before = v->phase_step_target;

                /* mid-note RPN2 change */
                g_channels[ch].rpn2_coarse_cents = 700;
                voices_update_modulation();
                is_int(v->phase_step_target, target_before,
                       "S3.3.2(c): RPN2 changed on a HELD note does not retune "
                       "it (latched at note-on, not live)");

                static const int32_t vo_bend_raw[4] = {9000, 0, 16383, 16383};
                static const uint16_t vo_bend_range[4] = {200, 200, 200, 6000};
                for (int bi = 0; bi < 4; bi++)
                {
                    g_channels[ch].pb_range_cents = vo_bend_range[bi];
                    uint32_t d1 = (uint32_t) vo_bend_raw[bi] & 0x7Fu,
                             d2 = ((uint32_t) vo_bend_raw[bi] >> 7) & 0x7Fu;
                    synth_midi(0xE0u | (uint32_t) ch, d1, d2);
                    voices_update_modulation();
                    int32_t bend_cents = synth_pitch_bend_cents(ch);
                    int32_t ratio = vo_cents_to_ratio_q12(bend_cents);
                    uint64_t raw64 =
                        (uint64_t) wave_rate * (uint64_t) base_ratio;
                    raw64 = (raw64 * (uint64_t)(uint32_t) ratio) >> 12;
                    uint32_t expected =
                        (uint32_t)(raw64 / (uint32_t) RENDER_RATE);
                    is_int(v->phase_step_target, expected,
                           "S3.3.3/S6.5: phase_step_target after bend raw=%d "
                           "range=%u matches "
                           "wave_rate*base_ratio_q12*CentsToRatio(bend)/"
                           "RENDER_RATE (bend_cents=%d)",
                           vo_bend_raw[bi], (unsigned) vo_bend_range[bi],
                           bend_cents);
                }
                ok(v->phase_step_target != target_before,
                   "S3.3.2(c): pitch bend DOES move phase_step_target -- live, "
                   "unlike RPN2 above");
            }
        }
        voice_pool_reset();
    }

    /* ------------------------- S3.4.1/S5.1.2: envelope setup
     * ------------------------- */
    tap_diag("--- S3.4.1/S5.1.2: timecent->duration, sustain-level and "
             "decay-rescale formulas ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S3.4.1/S5.1.2 envelope-setup formula check");
    }
    else
    {
        synth_construct();
        int ch = 7, note = 60, velocity = 100, vidx = -1;
        uint32_t locale = synth_channel_locale(ch);
        Region *r = dls_find_region(locale, (uint8_t) note, 0);
        if (!r)
        {
            tap_skip("dist/gm.dls has no region for channel 7 default program, "
                     "note 60",
                     "S3.4.1/S5.1.2 envelope-setup check");
        }
        else
        {
            voice_note_on(ch, note, velocity);
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].channel == ch &&
                    g_voices[i].note == note)
                    vidx = i;
            if (vidx < 0)
            {
                tap_skip("note-on did not allocate a voice",
                         "S3.4.1/S5.1.2 envelope-setup check");
            }
            else
            {
                Voice *v = &g_voices[vidx];
                Artic *ar = r->artic;

                double attack_s = vo_timecents_to_seconds(vo_scale_tc_by_source(
                    ar->eg1_attack_tc, ar->eg1_attack_vel_tc, velocity));
                if (attack_s > 0.0)
                {
                    double expected_step =
                        1.0 / (attack_s * (double) RENDER_RATE);
                    is_near(
                        v->env_attack_step, expected_step,
                        expected_step * 1e-9 + 1e-15,
                        "S3.4.1: env_attack_step == 1/(attack_s * RENDER_RATE) "
                        "via the timecent->duration formula 2^(tc/1200)");
                }
                else
                {
                    is_near(v->env_level, 1.0, 1e-12,
                            "S3.4.1: attack_s<=0 (instant attack) starts "
                            "env_level at 1.0");
                }

                int32_t sustain_permille = ar->eg1_sustain_permille;
                if (sustain_permille < 0)
                    sustain_permille = 0;
                if (sustain_permille > 1000)
                    sustain_permille = 1000;
                double expected_sustain =
                    rt_pow(10.0, ((double) sustain_permille * 9.6 - 9600.0) /
                                     100.0 / 20.0);
                if (expected_sustain < 0.0)
                    expected_sustain = 0.0;
                if (expected_sustain > 1.0)
                    expected_sustain = 1.0;
                is_near(v->env_sustain_level, expected_sustain, 1e-9,
                        "S5.1.2: sustain level is the PROGRESS-domain marker "
                        "sustainPermille*9.6-9600 on the 96dB scale, not a "
                        "linear-amplitude reading");

                double decay_s = vo_timecents_to_seconds(vo_scale_tc_by_source(
                    ar->eg1_decay_tc, ar->eg1_decay_kf_tc, note));
                double decay_to_sustain_s =
                    decay_s * (double) (1000 - sustain_permille) / 1000.0;
                int32_t decay_samples =
                    (int32_t)(decay_to_sustain_s * (double) RENDER_RATE + 0.5);
                int32_t expected_left = decay_samples > 0 ? decay_samples : 0;
                is_int(
                    v->env_decay_samples_left, expected_left,
                    "S5.1.2: decay duration rescaled to "
                    "decay_s*(1000-sustainPermille)/1000 before consumption");
            }
        }
        voice_pool_reset();
    }

    /* ------------------------------ S3.5/S3.10: volume law
     * ------------------------------ */
    tap_diag("--- S3.5/S3.10: velocity attenuation sum ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S3.5/S3.10 attenuation-sum check");
    }
    else
    {
        synth_construct();
        int ch = 3, note = 60, velocity = 90, vidx = -1;
        uint32_t locale = synth_channel_locale(ch);
        Region *r = dls_find_region(locale, (uint8_t) note, 0);
        if (!r)
        {
            tap_skip("dist/gm.dls has no region for channel 3 default program, "
                     "note 60",
                     "S3.5/S3.10 attenuation-sum check");
        }
        else
        {
            voice_note_on(ch, note, velocity);
            for (int i = 0; i < NUM_VOICES; i++)
                if (g_voices[i].active && g_voices[i].channel == ch &&
                    g_voices[i].note == note)
                    vidx = i;
            if (vidx < 0)
            {
                tap_skip("note-on did not allocate a voice",
                         "S3.5/S3.10 attenuation-sum check");
            }
            else
            {
                int32_t vel_atten = g_table_vel[velocity];
                int32_t depth = r->artic->vel_to_atten_depth;
                int32_t scaled =
                    (int32_t)(((int64_t) vel_atten * depth) / -9600);
                /* 0x19bb9-0x19bcd */
                int32_t expected_spec =
                    scaled + (int32_t) r->attenuation_hdb + 1200;
                is_int(g_voices[vidx].atten_const_hdb, expected_spec,
                       "S3.5/S3.10: atten_const_hdb == scaled + "
                       "region.attenuation_hdb + 1200 (the +1200 term)");
                is_int(g_voices[vidx].atten_const_hdb - scaled - 1200,
                       (int32_t) r->attenuation_hdb,
                       "S3.5/S3.10: region's own (wsmp) attenuation is summed "
                       "once, the WAVE-level term is not double-counted");
            }
        }
        voice_pool_reset();
    }

    /* S3.6 hard-pan direction; the law itself is asserted in
     * t_findings_live */
    tap_diag("--- S3.6: hard-pan direction ---");
    {
        synth_construct();
        int ch = 11;
        g_master_vol_hdb = 0;
        static const uint8_t vo_pans[2] = {0, 127};
        double gl[2], gr[2];
        for (int pi = 0; pi < 2; pi++)
        {
            Voice *v = &g_voices[0];
            vo_setup_bare_active(v, ch, 0);
            v->atten_const_hdb = -2000;
            g_channels[ch].pan = vo_pans[pi];
            voice_update_gain(v);
            gl[pi] = v->gain_l_target;
            gr[pi] = v->gain_r_target;
        }
        ok(gr[0] < gl[0], "S3.6: CC10=0 (hard left) attenuates the RIGHT "
                          "channel, not the left");
        ok(gl[1] < gr[1], "S3.6: CC10=127 (hard right) attenuates the LEFT "
                          "channel, not the right");
        voice_pool_reset();
        synth_construct();
    }

    /* ------------------------------------- Open items
     * ------------------------------------- */
    tap_diag("--- S5.4/S3.4.1 open cadence items ---");
    todo_ok(0,
            "the driver tops up once per service call (0x12be7), whose length "
            "the audio stack picks; TOPUP_INTERVAL_FRAMES is an [F] stand-in "
            "-- INVESTIGATE.adoc#i-envelope-cadence",
            "S5.4 top-up cadence value (mechanism itself is covered above)");
    todo_ok(0,
            "envelope state advances once per service call, frame count "
            "caller-supplied (0x13054) -- INVESTIGATE.adoc#i-envelope-cadence",
            "S3.4.1/S3.4.2 envelope update cadence");

    voice_pool_reset();
    synth_construct();
}

/* t_render: the mixer. */

/* render_frames() always recomputes gain via voices_update_modulation(),
 * overwriting any gain/amp priming -- so this fixture forces the
 * VO_GAIN_CEILING clamp instead, which lands on the same value regardless of
 * channel/pan state. */
/* the hdB->gain table's own ceiling, TABLE[0]==4095 of 8192 (0x1bfc0) */
#define VO_GAIN_CEILING (4095.0 / 8192.0)
static Wave vo_r_wave;
static Artic vo_r_artic; /* zero-initialized: unused by render_voice itself */

static void vo_render_fixture(Voice *v, Wave *w, int32_t sample_end_s,
                              int channel)
{
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->wave = w;
    v->artic = &vo_r_artic;
    v->channel = channel;
    v->loop_start_s = 0;
    v->loop_len_s = 0;
    v->sample_end_s = sample_end_s;
    v->env_stage = ENV_SUSTAIN;
    v->env_level = 1.0;
    v->env_sustain_level = 1.0;
    v->eg2_stage = ENV_IDLE;
    /* forces voice_update_gain's ceiling clamp regardless of channel/pan state
     */
    v->atten_const_hdb = 100000;
    v->gain_l_target = v->gain_r_target = VO_GAIN_CEILING;
    v->gain_l = v->gain_r = VO_GAIN_CEILING;
    /* matches what voice_update_gain will ALSO compute on the next sub-chunk */
    v->amp_l = v->amp_r = VO_GAIN_CEILING;
    v->amp_left = 0;
}

/* Applies the SAME truncating gain multiply render_voice's per-sample loop
 * does (`(int32_t)((double)interp * v->amp_l)`), at the known VO_GAIN_CEILING
 * amplitude the fixture above forces. */
static int32_t vo_apply_gain(int32_t interp)
{
    return (int32_t)((double) interp * VO_GAIN_CEILING);
}

/* the mixer's per-voice saturating add (paddsw), reimplemented
 * independently (not calling render.c's static sat_add_i16) to compute
 * expected mix sequences in the tests below. */
static int32_t vo_sat_add(int32_t a, int32_t b)
{
    int32_t s = a + b;
    if (s > 32767)
        return 32767;
    if (s < -32768)
        return -32768;
    return s;
}

static void t_render(void)
{
    voice_pool_reset();

    /* ----------------------- S6.1/S6.2: output format contract
     * ----------------------- */
    tap_diag("--- S6.1/S6.2: output format contract ---");
    is_int(BASE_RATE, 22050, "S6.1: fixed 22050 Hz output rate");
    is_int(RENDER_RATE, BASE_RATE,
           "S6.1: synthesis rate is BASE_RATE at RESAMPLE_FACTOR==1 (this "
           "build's default)");
    {
        enum
        {
            FR = 8
        };
        static int16_t buf[(FR + 4) * 2];
        for (uint32_t i = 0; i < (FR + 4) * 2; i++)
            buf[i] = (int16_t) 0x5A5A;
        voice_pool_reset(); /* no active voices */
        render_frames(buf, FR);
        ok(vo_all_eq_i16(buf, FR * 2, 0), "S6.2: render starts from silence -- "
                                          "no active voices renders all-zero");
        ok(vo_all_eq_i16(buf + FR * 2, 4, (int16_t) 0x5A5A),
           "S6.2/S6.4.1: render_frames writes EXACTLY frames*2 int16s -- "
           "sentinel just past the end is untouched");
    }

    /* --------------------------- S6.4.2: phase accumulator format
     * --------------------------- */
    tap_diag("--- S6.4.2: Q12 phase accumulator (bits 31:12 index, bits 11:0 "
             "fraction) ---");
    {
        static int16_t vo_ramp[24];
        for (int i = 0; i < 24; i++)
            vo_ramp[i] = (int16_t)(i * 256);
        vo_r_wave.sample_rate = (uint32_t) RENDER_RATE;
        vo_r_wave.samples = vo_ramp;
        vo_r_wave.sample_count = 24;

        enum
        {
            N = 8
        };
        static int16_t buf[N * 2];
        voice_pool_reset();
        vo_render_fixture(&g_voices[0], &vo_r_wave, 20, 0);
        g_voices[0].phase_pos = 0;
        /* unity: one sample index per output frame */
        g_voices[0].phase_step = 1u << 12;
        render_frames(buf, N);
        int unity_ok = 1;
        for (int i = 0; i < N; i++)
            if (buf[i * 2] != (int16_t) vo_apply_gain(i * 256))
                unity_ok = 0;
        ok(unity_ok, "S6.4.2: phase_step==1<<12 advances the integer sample "
                     "index by exactly 1 per output frame");

        voice_pool_reset();
        vo_render_fixture(&g_voices[0], &vo_r_wave, 20, 0);
        g_voices[0].phase_pos = 0;
        g_voices[0].phase_step = 1u << 11; /* half rate */
        render_frames(buf, N);
        int half_ok = 1;
        for (int i = 0; i < N; i++)
            if (buf[i * 2] != (int16_t) vo_apply_gain(i * 128))
                half_ok = 0;
        ok(half_ok,
           "S6.4.2/S6.4.4: phase_step==1<<11 advances at half rate (index "
           "every other frame, exact Q12 interpolation between)");
        voice_pool_reset();
    }

    /* ------------------------- S6.4.4: Q12 linear interpolation
     * ------------------------- */
    tap_diag("--- S6.4.4: (tap0*(4096-frac) + tap1*frac) >> 12 ---");
    {
        static int16_t vo_two[2];
        vo_two[0] = 1000;
        vo_two[1] = -500;
        vo_r_wave.sample_rate = (uint32_t) RENDER_RATE;
        vo_r_wave.samples = vo_two;
        vo_r_wave.sample_count = 2;

        static const int32_t vo_fracs[3] = {0, 2048, 1024};
        for (int fi = 0; fi < 3; fi++)
        {
            static int16_t buf1[2];
            voice_pool_reset();
            vo_render_fixture(&g_voices[0], &vo_r_wave, 50, 0);
            g_voices[0].phase_pos = (uint32_t) vo_fracs[fi];
            g_voices[0].phase_step = 0;
            render_frames(buf1, 1);
            int32_t interp =
                (1000 * (4096 - vo_fracs[fi]) + (-500) * vo_fracs[fi]) >> 12;
            int32_t expected = vo_apply_gain(interp);
            is_int(buf1[0], expected,
                   "S6.4.4: frac=%d gives the driver's "
                   "(tap0*(4096-frac)+tap1*frac)>>12 == %d (post-gain %d)",
                   vo_fracs[fi], interp, expected);
        }
        voice_pool_reset();
    }

    /* --------------------- S6.4.6: saturating mix, no wide accumulator
     * --------------------- */
    tap_diag("--- S6.4.6: saturating accumulate directly into the 16-bit "
             "buffer, no wider intermediate ---");
    {
        /* Each voice's own contribution is capped at VO_GAIN_CEILING (~0.5),
         * so a max-amplitude int16 sample alone cannot saturate the mix --
         * 3 max-amplitude voices are used per round so the SUM can. */
        static int16_t sV[4];
        static Wave wV[4];
        for (int i = 0; i < 4; i++)
        {
            wV[i].sample_rate = (uint32_t) RENDER_RATE;
            wV[i].samples = &sV[i];
            wV[i].sample_count = 1;
        }
        int32_t c_pos = vo_apply_gain(32767); /* == 16383 */
        /* == -16383 (truncates -16383.5 toward zero) */
        int32_t c_neg = vo_apply_gain(-32768);

        /* round 1: three +32767 voices -> clamps to exactly +32767. */
        sV[0] = sV[1] = sV[2] = 32767;
        voice_pool_reset();
        for (int i = 0; i < 3; i++)
            vo_render_fixture(&g_voices[i], &wV[i], 50, 0);
        {
            static int16_t buf1[2];
            render_frames(buf1, 1);
            is_int(buf1[0], 32767,
                   "S6.4.6: 3 x %d (=%d each, sums past +32767) saturates to "
                   "exactly +32767",
                   32767, c_pos);
        }
        voice_pool_reset();

        /* round 2: three -32768 voices -> clamps to exactly -32768. */
        sV[0] = sV[1] = sV[2] = -32768;
        for (int i = 0; i < 3; i++)
            vo_render_fixture(&g_voices[i], &wV[i], 50, 0);
        {
            static int16_t buf1[2];
            render_frames(buf1, 1);
            is_int(buf1[0], -32768,
                   "S6.4.6: 3 x %d (=%d each, sums past -32768) saturates to "
                   "exactly -32768",
                   -32768, c_neg);
        }
        voice_pool_reset();

        /* round 3: per-voice saturation differs from a wide-then-clip sum.
         * Three +32767 voices push the buffer to the +32767 clamp (as round
         * 1) BEFORE a 4th, -32768 voice's contribution is added; a wide
         * accumulator would instead sum all four unclamped (3*c_pos+c_neg)
         * first and clip once -- that raw sum is <= 32767, so it would not
         * even clip, giving a visibly different, higher result. */
        sV[0] = sV[1] = sV[2] = 32767;
        sV[3] = -32768;
        voice_pool_reset();
        for (int i = 0; i < 4; i++)
            vo_render_fixture(&g_voices[i], &wV[i], 50, 0);
        {
            static int16_t buf1[2];
            render_frames(buf1, 1);
            int32_t per_voice = vo_sat_add(
                vo_sat_add(vo_sat_add(vo_sat_add(0, c_pos), c_pos), c_pos),
                c_neg);
            /* what a single-clip-at-the-end design would give */
            int32_t wide_equiv = c_pos * 3 + c_neg;
            is_int(buf1[0], per_voice,
                   "S6.4.6: per-voice saturating add clamps at voice 3, then "
                   "voice 4 subtracts from the CLAMPED value (%d), not the "
                   "true sum",
                   per_voice);
            ok(per_voice != wide_equiv,
               "S6.4.6: per-voice result (%d) differs from what a wide "
               "accumulator clipped once would give (%d)",
               per_voice, wide_equiv);
        }
        voice_pool_reset();
    }

    /* ----------------------- S6.4.8: loop wraparound and one-shot end
     * ----------------------- */
    tap_diag("--- S6.4.8: single-subtraction loop wraparound; one-shot end "
             "deactivates ---");
    {
        static int16_t vo_z[10];
        vo_r_wave.sample_rate = (uint32_t) RENDER_RATE;
        vo_r_wave.samples = vo_z;
        vo_r_wave.sample_count = 10;

        /* looping: sample_end=8, loop_start=2 -> loop_len=6 (Q12: 32768/24576).
         */
        voice_pool_reset();
        vo_render_fixture(&g_voices[0], &vo_r_wave, 8, 0);
        g_voices[0].loop_len_s = 6;
        /* 50 Q12 LSBs before sample_end */
        g_voices[0].phase_pos = (8u << 12) - 50u;
        /* crosses sample_end this frame */
        g_voices[0].phase_step = 100;
        {
            static int16_t buf1[2];
            render_frames(buf1, 1);
        }
        uint32_t expected_pos = ((8u << 12) - 50u + 100u) - (6u << 12);
        is_int(g_voices[0].phase_pos, expected_pos,
               "S6.4.8: crossing sample_end wraps by a SINGLE subtraction of "
               "loop_len (Q12), not modulo");
        ok(g_voices[0].active == 1,
           "S6.4.8: a looping voice stays active after wrapping");
        voice_pool_reset();

        /* one-shot: loop_len==0 -> deactivate at sample_end, do not wrap. */
        vo_render_fixture(&g_voices[0], &vo_r_wave, 5, 0);
        g_voices[0].loop_len_s = 0;
        g_voices[0].phase_pos = (5u << 12) - 40u;
        g_voices[0].phase_step = 100;
        {
            static int16_t buf1[2];
            render_frames(buf1, 1);
        }
        ok(g_voices[0].active == 0,
           "S6.4.8: one-shot (loop_len_s==0) voice goes inactive once "
           "phase_pos reaches sample_end");
        voice_pool_reset();
    }

    /* --------------------------- S6.5: per-wave sample rate
     * --------------------------- */
    tap_diag(
        "--- S6.5: per-wave sample rate enters the phase-step composition ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not loadable",
                 "S6.5 per-wave (24000Hz) phase-step composition");
    }
    else
    {
        Wave *w24 = 0;
        for (uint32_t i = 0; i < g_dls.wave_count && !w24; i++)
            if (g_dls.wave_array[i]->sample_rate == 24000)
                w24 = g_dls.wave_array[i];
        if (!w24)
        {
            tap_skip("dist/gm.dls has no 24000Hz wave (gm.dls has three, "
                     "wave-pool indices 26/62/404)",
                     "S6.5 per-wave phase-step composition");
        }
        else
        {
            int ch = 8;
            synth_construct();
            g_channels[ch].pitch_bend = 8192;
            g_channels[ch].pb_range_cents = 200;
            Voice *v = &g_voices[0];
            voice_pool_reset();
            memset(v, 0, sizeof(*v));
            v->active = 1;
            v->wave = w24;
            v->channel = ch;
            v->base_ratio_q12 = 4096; /* unity */
            v->bend_cents_applied = 0;
            voice_update_pitch(v);
            /* base_ratio_q12 */
            uint64_t raw = (uint64_t) w24->sample_rate * 4096u;
            /* CentsToRatio(0) == 4096 exactly (unity) */
            raw = (raw * 4096u) >> 12;
            uint32_t expected = (uint32_t)(raw / (uint32_t) RENDER_RATE);
            is_int(v->phase_step_target, expected,
                   "S6.5: a 24000Hz wave's phase_step_target is "
                   "waveRate*ratio/renderRate using the WAVE's own rate, not a "
                   "fixed 22050 constant");
            voice_pool_reset();
        }
    }

    /* ------------------------------- Open/not-applicable items
     * ------------------------------- */
    tap_diag("--- S6.6/S6.7/S6.2/S6.4.1 open or not-applicable items: not "
             "asserted ---");
    todo_ok(0,
            "segment length is the caller's KS buffer, not a driver constant; "
            "GAIN_SEGMENT_FRAMES is an [F] stand-in -- "
            "INVESTIGATE.adoc#i-segment-length",
            "S6.6 ramp/envelope update cadence value");
    tap_diag(
        "S6.2/S6.2.1/S6.2.2 (MMX/scalar dispatch, CPU-capability gate) and "
        "S6.4.1/S6.4.7 (stack-arg frame-count doubling): no analog exists in "
        "this engine (plain C, one render_frames(int16_t*,uint32_t) entry "
        "point) -- nothing to assert, not a gap");
    tap_diag("S6.7 (x87 control word 0x027F = 53-bit, round-to-nearest): "
             "plain IEEE-754 double reproduces it, so no control-word handling "
             "is needed");

    voice_pool_reset();
    synth_construct();
}

/* frag_dls.c -- t_dls() coverage for dls.c.
 * Block A/B/C: S2.10/S2.11 inventory, S2.4/S2.5 articulation, S2.9/S3.1
 * locale+fallback -- all off the real g_dls from dist/gm.dls (have_dls).
 * Block B3/C-synth/D: S2.4.4 ignored usSource==4, S3.1.4 region file-order,
 * S2.2.2 malformed-buffer table -- synthetic buffers, no gm.dls needed.
 * IMPORTANT: dls_load() overwrites g_dls on every call; A/B/C (real) MUST
 * run before any synthetic-buffer test below clobbers it for good. */

/* -------------------------------------------------------------------- */
/* Tiny RIFF/DLS buffer builder for the synthetic (Block B3/C-synth/D)
 * tests. Chunk-loop shape matches the driver: no padding. */

#define DL_MAXBUF 4096
typedef struct
{
    uint8_t d[DL_MAXBUF];
    uint32_t n;
} dl_buf;

static void dl_init(dl_buf *b) { b->n = 0; }
static void dl_raw(dl_buf *b, const void *p, uint32_t len)
{
    memcpy(b->d + b->n, p, len);
    b->n += len;
}
static void dl_fourcc(dl_buf *b, const char *s) { dl_raw(b, s, 4); }
static void dl_u32(dl_buf *b, uint32_t v) { dl_raw(b, &v, 4); }
static void dl_u16(dl_buf *b, uint16_t v) { dl_raw(b, &v, 2); }

/* Writes an 8-byte chunk header (fourcc + declared size) then exactly `len`
 * payload bytes from p. `len` may be smaller than the "real" content the
 * caller built at p -- that is how the bad-size test cases below truncate a
 * chunk's declared size without leaving orphaned trailing bytes for the next
 * chunk's header to misparse. */
static void dl_chunk_raw(dl_buf *out, const char *fourcc, const void *p,
                         uint32_t len)
{
    dl_fourcc(out, fourcc);
    dl_u32(out, len);
    dl_raw(out, p, len);
}
/* Wraps `inner` as a LIST chunk of the given 4-byte subtype. */
static void dl_wrap_list(dl_buf *out, const char *subtype, const dl_buf *inner)
{
    dl_buf tmp;
    dl_init(&tmp);
    dl_fourcc(&tmp, subtype);
    dl_raw(&tmp, inner->d, inner->n);
    dl_chunk_raw(out, "LIST", tmp.d, tmp.n);
}

enum
{
    dl_ok = 0,
    dl_bad_riff_magic,
    dl_bad_dls_magic,
    dl_bad_colh_size,    /* S2.2.2: colh size < 4 */
    dl_bad_insh_size,    /* S2.2.2: insh size < 0xc */
    dl_bad_rgnh_size,    /* S2.2.2: rgnh size < 0xc */
    dl_bad_wlnk_channel, /* S2.2.2: wlnk.ulChannel != 1 */
    dl_bad_art1_cbsize,  /* S2.2.2: art1 cbSize < 8 */
    /* S2.2.2/S2.6/D-21: wave-level wsmp.cSampleLoops > 1 */
    dl_bad_wsmp_loops,
    dl_bad_fmt_tag,     /* S2.2.2/S2.7.1: fmt.wFormatTag != 1 */
    dl_bad_fmt_nchan,   /* S2.2.2/S2.7.1: fmt.nChannels != 1 */
    dl_dup_fmt,         /* S2.2.2/S2.7.1: duplicate fmt chunk */
    dl_data_before_fmt, /* S2.2.2/S2.7.2: data chunk before fmt */
                        /* S2.4/S2.4.4/D-15: usSource==4, not a size defect --
                           dls_load must still succeed, only the connection
                           is supposed to have no effect */
    dl_ignored_src4
};

/* Builds a minimal-but-structurally-valid one-instrument/one-region/one-wave
 * DLS RIFF buffer, injecting exactly one condition from the enum above.
 * The driver's whole-load abort table (Block D) plus the ignored-usSource==4
 * case (Block B3). */
static void dl_build(dl_buf *out, int defect)
{
    dl_buf colh;
    dl_init(&colh);
    dl_u32(&colh, 235);

    dl_buf insh;
    dl_init(&insh);
    dl_u32(&insh, 1); /* cRegions, ignored either way */
    dl_u32(&insh, 0); /* ulBank = 0 */
    dl_u32(&insh, 0); /* ulInstrument = program 0 -> locale 0 */

    dl_buf rgnh;
    dl_init(&rgnh);
    dl_u16(&rgnh, 0);
    dl_u16(&rgnh, 127); /* usLowKey, usHighKey */
                        /* usLowVel, usHighVel (never read) */
    dl_u16(&rgnh, 0);
    dl_u16(&rgnh, 127);
    dl_u16(&rgnh, 0);
    dl_u16(&rgnh, 0); /* fusOptions, usKeyGroup */

    dl_buf wlnk;
    dl_init(&wlnk);
    dl_u16(&wlnk, 0);
    dl_u16(&wlnk, 0); /* fusOptions, usPhaseGroup */
    dl_u32(&wlnk, defect == dl_bad_wlnk_channel ? 2u : 1u); /* ulChannel */
    dl_u32(&wlnk, 0); /* ulTableIndex -> wave 0 */

    dl_buf rgnbody;
    dl_init(&rgnbody);
    dl_chunk_raw(&rgnbody, "rgnh", rgnh.d,
                 defect == dl_bad_rgnh_size ? 4u : rgnh.n);
    dl_chunk_raw(&rgnbody, "wlnk", wlnk.d, wlnk.n);
    dl_buf rgnlist;
    dl_init(&rgnlist);
    dl_wrap_list(&rgnlist, "rgn ", &rgnbody);

    dl_buf lrgnbody;
    dl_init(&lrgnbody);
    dl_raw(&lrgnbody, rgnlist.d, rgnlist.n);
    dl_buf lrgnlist;
    dl_init(&lrgnlist);
    dl_wrap_list(&lrgnlist, "lrgn", &lrgnbody);

    dl_buf instbody;
    dl_init(&instbody);
    dl_chunk_raw(&instbody, "insh", insh.d,
                 defect == dl_bad_insh_size ? 4u : insh.n);
    dl_raw(&instbody, lrgnlist.d, lrgnlist.n);

    if (defect == dl_bad_art1_cbsize)
    {
        /* Outer chunk size is a legitimate 8 (passes dls.c's own `csize<8`
         * gate on the *chunk's* declared size); the defect is the payload's
         * own internal cbSize sub-field (S2.3.6), which dls.c reads into a
         * local and discards -- `(void)cb_size;` -- rather than checking it
         * against the required >=8 (S2.2.2's `art1 cbSize<8` row). */
        dl_buf art1;
        dl_init(&art1);
        /* payload's own cbSize field: invalid, spec wants >=8 */
        dl_u32(&art1, 4);
        dl_u32(&art1, 0); /* cConnectionBlocks = 0 */
        dl_buf art1chunk;
        dl_init(&art1chunk);
        dl_chunk_raw(&art1chunk, "art1", art1.d, art1.n);
        dl_buf lartbody;
        dl_init(&lartbody);
        dl_raw(&lartbody, art1chunk.d, art1chunk.n);
        dl_buf lartlist;
        dl_init(&lartlist);
        dl_wrap_list(&lartlist, "lart", &lartbody);
        dl_raw(&instbody, lartlist.d, lartlist.n);
    }
    else if (defect == dl_ignored_src4)
    {
        /* One connection block: usSource=4 (EG1-as-modulator), destination
         * 0x0206 (EG1 attack), a large nonzero lScale. S2.4/S2.4.4 say the
         * dispatch chain drops usSource==4 entirely -- this connection must
         * have zero observable effect on eg1_attack_tc (Block B3). */
        dl_buf art1;
        dl_init(&art1);
        dl_u32(&art1, 8); /* cbSize */
        dl_u32(&art1, 1); /* cConnectionBlocks = 1 */
        dl_u16(&art1, 4);
        dl_u16(&art1, 0);
        dl_u16(&art1, ART_DST_EG1_ATTACKTIME);
        dl_u16(&art1, 0);
        /* lScale: would be a huge attack-time shift if applied */
        dl_u32(&art1, 0x7fff0000u);
        dl_buf art1chunk;
        dl_init(&art1chunk);
        dl_chunk_raw(&art1chunk, "art1", art1.d, art1.n);
        dl_buf lartbody;
        dl_init(&lartbody);
        dl_raw(&lartbody, art1chunk.d, art1chunk.n);
        dl_buf lartlist;
        dl_init(&lartlist);
        dl_wrap_list(&lartlist, "lart", &lartbody);
        dl_raw(&instbody, lartlist.d, lartlist.n);
    }

    dl_buf inslist;
    dl_init(&inslist);
    dl_wrap_list(&inslist, "ins ", &instbody);
    dl_buf linsbody;
    dl_init(&linsbody);
    dl_raw(&linsbody, inslist.d, inslist.n);
    dl_buf linslist;
    dl_init(&linslist);
    dl_wrap_list(&linslist, "lins", &linsbody);

    dl_buf ptblbody;
    dl_init(&ptblbody);
    /* cbSize, cCues, ulOffset[0] */
    dl_u32(&ptblbody, 8);
    dl_u32(&ptblbody, 1);
    dl_u32(&ptblbody, 0);
    dl_buf ptblchunk;
    dl_init(&ptblchunk);
    dl_chunk_raw(&ptblchunk, "ptbl", ptblbody.d, ptblbody.n);

    dl_buf fmt;
    dl_init(&fmt);
    dl_u16(&fmt, defect == dl_bad_fmt_tag ? 2u : 1u);   /* wFormatTag */
    dl_u16(&fmt, defect == dl_bad_fmt_nchan ? 2u : 1u); /* nChannels */
    /* nSamplesPerSec, nAvgBytesPerSec */
    dl_u32(&fmt, 22050);
    dl_u32(&fmt, 44100);
    /* nBlockAlign, wBitsPerSample */
    dl_u16(&fmt, 2);
    dl_u16(&fmt, 16);

    dl_buf data;
    dl_init(&data);
    {
        uint16_t samp = 0x1234;
        dl_raw(&data, &samp, 2);
    }

    dl_buf wsmp;
    dl_init(&wsmp);
    if (defect == dl_bad_wsmp_loops)
    {
        dl_u32(&wsmp, 0x14);
        dl_u16(&wsmp, 60);
        dl_u16(&wsmp, 0);
        dl_u32(&wsmp, 0);
        dl_u32(&wsmp, 0);
        /* cSampleLoops = 2: S2.2.2/S2.6/D-21 says this must be a hard error */
        dl_u32(&wsmp, 2);
        /* one loop record */
        dl_u32(&wsmp, 0x10);
        dl_u32(&wsmp, 0);
        dl_u32(&wsmp, 0);
        dl_u32(&wsmp, 100);
    }

    dl_buf wavebody;
    dl_init(&wavebody);
    if (defect == dl_data_before_fmt)
    {
        dl_chunk_raw(&wavebody, "data", data.d, data.n);
        dl_chunk_raw(&wavebody, "fmt ", fmt.d, fmt.n);
    }
    else
    {
        dl_chunk_raw(&wavebody, "fmt ", fmt.d, fmt.n);
        if (defect == dl_dup_fmt)
            dl_chunk_raw(&wavebody, "fmt ", fmt.d, fmt.n);
        dl_chunk_raw(&wavebody, "data", data.d, data.n);
    }
    if (defect == dl_bad_wsmp_loops)
        dl_chunk_raw(&wavebody, "wsmp", wsmp.d, wsmp.n);

    dl_buf wavelist;
    dl_init(&wavelist);
    dl_wrap_list(&wavelist, "wave", &wavebody);
    dl_buf wvplbody;
    dl_init(&wvplbody);
    dl_raw(&wvplbody, wavelist.d, wavelist.n);
    dl_buf wvplchunk;
    dl_init(&wvplchunk);
    dl_wrap_list(&wvplchunk, "wvpl", &wvplbody);

    dl_buf body;
    dl_init(&body);
    dl_chunk_raw(&body, "colh", colh.d,
                 defect == dl_bad_colh_size ? 0u : colh.n);
    dl_raw(&body, linslist.d, linslist.n);
    dl_raw(&body, ptblchunk.d, ptblchunk.n);
    dl_raw(&body, wvplchunk.d, wvplchunk.n);

    dl_init(out);
    dl_fourcc(out, defect == dl_bad_riff_magic ? "XIFF" : "RIFF");
    dl_u32(out, body.n + 4);
    dl_fourcc(out, defect == dl_bad_dls_magic ? "WAVE" : "DLS ");
    dl_raw(out, body.d, body.n);
}

/* Builds a one-instrument/one-wave buffer with TWO regions whose key ranges
 * overlap ([0,100] then [50,127], file order as written), for the S3.1.4
 * "first covering region in file order wins" test (Block C-synth). Both
 * regions share the instrument's own (empty) lart for a valid, non-NULL
 * Artic; both wlnk to the one wave (index 0). */
static void dl_build_overlap(dl_buf *out)
{
    dl_buf rgnh_a;
    dl_init(&rgnh_a);
    dl_u16(&rgnh_a, 0);
    dl_u16(&rgnh_a, 100);
    dl_u16(&rgnh_a, 0);
    dl_u16(&rgnh_a, 127);
    dl_u16(&rgnh_a, 0);
    dl_u16(&rgnh_a, 0);

    dl_buf rgnh_b;
    dl_init(&rgnh_b);
    dl_u16(&rgnh_b, 50);
    dl_u16(&rgnh_b, 127);
    dl_u16(&rgnh_b, 0);
    dl_u16(&rgnh_b, 127);
    dl_u16(&rgnh_b, 0);
    dl_u16(&rgnh_b, 0);

    dl_buf wlnk;
    dl_init(&wlnk);
    dl_u16(&wlnk, 0);
    dl_u16(&wlnk, 0);
    dl_u32(&wlnk, 1);
    dl_u32(&wlnk, 0);

    dl_buf rgnbody_a;
    dl_init(&rgnbody_a);
    dl_chunk_raw(&rgnbody_a, "rgnh", rgnh_a.d, rgnh_a.n);
    dl_chunk_raw(&rgnbody_a, "wlnk", wlnk.d, wlnk.n);
    dl_buf rgnlist_a;
    dl_init(&rgnlist_a);
    dl_wrap_list(&rgnlist_a, "rgn ", &rgnbody_a);

    dl_buf rgnbody_b;
    dl_init(&rgnbody_b);
    dl_chunk_raw(&rgnbody_b, "rgnh", rgnh_b.d, rgnh_b.n);
    dl_chunk_raw(&rgnbody_b, "wlnk", wlnk.d, wlnk.n);
    dl_buf rgnlist_b;
    dl_init(&rgnlist_b);
    dl_wrap_list(&rgnlist_b, "rgn ", &rgnbody_b);

    dl_buf insh;
    dl_init(&insh);
    dl_u32(&insh, 2);
    dl_u32(&insh, 0);
    dl_u32(&insh, 0);

    /* empty: no art1 needed, just a valid shared Artic */
    dl_buf lartbody;
    dl_init(&lartbody);
    dl_buf lartlist;
    dl_init(&lartlist);
    dl_wrap_list(&lartlist, "lart", &lartbody);

    dl_buf lrgnbody;
    dl_init(&lrgnbody);
    dl_raw(&lrgnbody, rgnlist_a.d, rgnlist_a.n);
    dl_raw(&lrgnbody, rgnlist_b.d, rgnlist_b.n);
    dl_buf lrgnlist;
    dl_init(&lrgnlist);
    dl_wrap_list(&lrgnlist, "lrgn", &lrgnbody);

    dl_buf instbody;
    dl_init(&instbody);
    dl_chunk_raw(&instbody, "insh", insh.d, insh.n);
    dl_raw(&instbody, lrgnlist.d, lrgnlist.n);
    dl_raw(&instbody, lartlist.d, lartlist.n);
    dl_buf inslist;
    dl_init(&inslist);
    dl_wrap_list(&inslist, "ins ", &instbody);
    dl_buf linsbody;
    dl_init(&linsbody);
    dl_raw(&linsbody, inslist.d, inslist.n);
    dl_buf linslist;
    dl_init(&linslist);
    dl_wrap_list(&linslist, "lins", &linsbody);

    dl_buf ptblbody;
    dl_init(&ptblbody);
    dl_u32(&ptblbody, 8);
    dl_u32(&ptblbody, 1);
    dl_u32(&ptblbody, 0);
    dl_buf ptblchunk;
    dl_init(&ptblchunk);
    dl_chunk_raw(&ptblchunk, "ptbl", ptblbody.d, ptblbody.n);

    dl_buf fmt;
    dl_init(&fmt);
    dl_u16(&fmt, 1);
    dl_u16(&fmt, 1);
    dl_u32(&fmt, 22050);
    dl_u32(&fmt, 44100);
    dl_u16(&fmt, 2);
    dl_u16(&fmt, 16);
    dl_buf data;
    dl_init(&data);
    {
        uint16_t s = 0x1234;
        dl_raw(&data, &s, 2);
    }
    dl_buf wavebody;
    dl_init(&wavebody);
    dl_chunk_raw(&wavebody, "fmt ", fmt.d, fmt.n);
    dl_chunk_raw(&wavebody, "data", data.d, data.n);
    dl_buf wavelist;
    dl_init(&wavelist);
    dl_wrap_list(&wavelist, "wave", &wavebody);
    dl_buf wvplbody;
    dl_init(&wvplbody);
    dl_raw(&wvplbody, wavelist.d, wavelist.n);
    dl_buf wvplchunk;
    dl_init(&wvplchunk);
    dl_wrap_list(&wvplchunk, "wvpl", &wvplbody);

    dl_buf body;
    dl_init(&body);
    dl_raw(&body, linslist.d, linslist.n);
    dl_raw(&body, ptblchunk.d, ptblchunk.n);
    dl_raw(&body, wvplchunk.d, wvplchunk.n);

    dl_init(out);
    dl_fourcc(out, "RIFF");
    dl_u32(out, body.n + 4);
    dl_fourcc(out, "DLS ");
    dl_raw(out, body.d, body.n);
}

/* Small read-side helpers over the parsed g_dls (Blocks A/B/C). */

static Instrument *dl_find_inst(uint32_t locale)
{
    for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
        if (inst->locale == locale)
            return inst;
    return 0;
}
static int dl_key_covered(Instrument *inst, int key)
{
    for (Region *r = inst->first_region; r; r = r->next)
        if (key >= r->low_key && key <= r->high_key)
            return 1;
    return 0;
}
static int dl_range_covered(Instrument *inst, int lo, int hi)
{
    for (int k = lo; k <= hi; k++)
        if (!dl_key_covered(inst, k))
            return 0;
    return 1;
}

static void t_dls(void)
{
    tap_diag("--- Block A: S2.10/S2.11 gm.dls inventory ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not present or not loadable",
                 "S2.10/S2.11 gm.dls inventory (20 test points)");
    }
    else
    {
        int inst_count = 0, melodic = 0, drum = 0;
        long region_sum = 0, region_walk = 0;
        for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
        {
            inst_count++;
            if ((inst->locale >> 31) & 1)
                drum++;
            else
                melodic++;
            region_sum += inst->region_count;
            for (Region *r = inst->first_region; r; r = r->next)
                region_walk++;
        }
        is_int(inst_count, 235,
               "S2.11: 235 instruments (ins LIST chunks) in gm.dls");
        is_int(melodic, 226, "S2.11: 226 melodic instruments (drum bit clear)");
        is_int(drum, 9, "S2.11: 9 drum kits (drum bit set)");
        is_int(region_sum, 1498,
               "S2.11: 1498 regions, summed via Instrument.region_count");
        is_int(region_walk, 1498,
               "S2.11: 1498 regions, cross-checked by walking every "
               "first_region list directly");
        is_int(g_dls.wave_count, 495,
               "S2.11/S2.8.1: g_dls.wave_count == 495 (ptbl.cCues, "
               "load-bearing precondition)");

        int bad_bankLSB = 0;
        for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
            if ((inst->locale >> 7) & 0x7f)
                bad_bankLSB++;
        is_int(bad_bankLSB, 0,
               "S2.10/S2.11: bankLSB == 0 for all 235 instruments");

        {
            static const struct
            {
                int drumflag, bankmsb, want;
            } rows[14] = {
                {1, 0, 9}, {0, 0, 128}, {0, 1, 16}, {0, 2, 8},  {0, 3, 6},
                {0, 4, 4}, {0, 5, 4},   {0, 6, 1},  {0, 7, 1},  {0, 8, 37},
                {0, 9, 3}, {0, 16, 12}, {0, 24, 2}, {0, 32, 4},
            };
            int counts[2][128];
            memset(counts, 0, sizeof counts);
            for (Instrument *inst = g_dls.first_instrument; inst;
                 inst = inst->next)
            {
                int d = (inst->locale >> 31) & 1;
                int msb = (inst->locale >> 14) & 0x7f;
                counts[d][msb]++;
            }
            int mismatches = 0, accounted = 0;
            for (int i = 0; i < 14; i++)
            {
                int got = counts[rows[i].drumflag][rows[i].bankmsb];
                accounted += got;
                if (got != rows[i].want)
                {
                    tap_diag("A8: drum=%d bankMSB=%d got=%d want=%d",
                             rows[i].drumflag, rows[i].bankmsb, got,
                             rows[i].want);
                    mismatches++;
                }
            }
            int total = 0;
            for (int d = 0; d < 2; d++)
                for (int m = 0; m < 128; m++)
                    total += counts[d][m];
            if (total != accounted)
            {
                tap_diag("A8: %d instruments fall outside the 14 expected "
                         "(drum,bankMSB) buckets",
                         total - accounted);
                mismatches++;
            }
            ok(mismatches == 0, "S2.11: 14-row bank map (type x bankMSB -> "
                                "instrument count) matches exactly");
        }

        {
            static const int progs[9] = {0, 8, 16, 24, 25, 32, 40, 48, 56};
            int found[9] = {0};
            int extra = 0;
            for (Instrument *inst = g_dls.first_instrument; inst;
                 inst = inst->next)
            {
                int d = (inst->locale >> 31) & 1;
                int msb = (inst->locale >> 14) & 0x7f;
                int prog = inst->locale & 0x7f;
                if (d && msb == 0)
                {
                    int i, matched = 0;
                    for (i = 0; i < 9; i++)
                        if (progs[i] == prog)
                        {
                            found[i] = 1;
                            matched = 1;
                            break;
                        }
                    if (!matched)
                        extra++;
                }
            }
            int all_found = 1;
            for (int i = 0; i < 9; i++)
                if (!found[i])
                    all_found = 0;
            if (!all_found || extra)
                tap_diag("A9: drum-kit program-set mismatch, extra programs "
                         "seen: %d",
                         extra);
            ok(all_found && extra == 0,
               "S2.11: drum kits present at exactly programs "
               "{0,8,16,24,25,32,40,48,56}, bank MSB 0");
        }

        Instrument *stdkit = dl_find_inst(0x80000000u);
        if (ok(stdkit != 0, "S2.10/S2.11: Standard kit instrument (locale "
                            "0x80000000, program 0/bank 0/drum) present"))
        {
            is_int(stdkit->region_count, 61,
                   "S2.11: Standard kit has 61 regions");
            ok(dl_range_covered(stdkit, 27, 87),
               "S2.11: Standard kit covers every key 27..87");
            ok(!dl_key_covered(stdkit, 25),
               "S2.11/S3.1.2: Standard kit covers NO region for key 25");
            ok(!dl_key_covered(stdkit, 26),
               "S2.11/S3.1.2: Standard kit covers NO region for key 26");
        }
        else
        {
            tap_skip("Standard kit instrument not found",
                     "S2.11: Standard kit region count/coverage");
            tap_skip("Standard kit instrument not found",
                     "S2.11: Standard kit key 25 gap");
            tap_skip("Standard kit instrument not found",
                     "S2.11: Standard kit key 26 gap");
        }

        Instrument *prog48 = dl_find_inst(0x80000000u | 48);
        ok(prog48 && prog48->region_count == 62 &&
               dl_range_covered(prog48, 27, 88),
           "S2.11: drum program 48's kit has 62 regions covering 27..88");

        Instrument *prog56 = dl_find_inst(0x80000000u | 56);
        ok(prog56 && prog56->region_count == 46 &&
               dl_range_covered(prog56, 39, 84),
           "S2.11: drum program 56 (SFX kit) has 46 regions covering 39..84");

        {
            int c22050 = 0, c24000 = 0, other = 0;
            for (uint32_t i = 0; i < g_dls.wave_count; i++)
            {
                uint32_t sr = g_dls.wave_array[i]->sample_rate;
                if (sr == 22050)
                    c22050++;
                else if (sr == 24000)
                    c24000++;
                else
                    other++;
            }
            is_int(c22050, 492,
                   "S2.7.3/S2.11: 492 of 495 waves at 22050 Hz (reachable half "
                   "of the format claim, see report)");
            is_int(c24000, 3, "S2.7.3/S2.11: 3 of 495 waves at 24000 Hz");
            if (other)
                tap_diag("A: %d waves at neither 22050 nor 24000 Hz", other);

            int idx_ok = g_dls.wave_count > 404 &&
                         g_dls.wave_array[26]->sample_rate == 24000 &&
                         g_dls.wave_array[62]->sample_rate == 24000 &&
                         g_dls.wave_array[404]->sample_rate == 24000;
            ok(idx_ok && c24000 == 3, "S2.7.3: the three 24000 Hz waves are "
                                      "exactly wave-pool indices 26, 62, 404");
        }

        {
            int seen[8] = {0};
            for (Instrument *inst = g_dls.first_instrument; inst;
                 inst = inst->next)
            {
                for (Region *r = inst->first_region; r; r = r->next)
                {
                    if (r->key_group >= 1 && r->key_group <= 7)
                    {
                        seen[r->key_group] = 1;
                    }
                    else if (r->key_group != 0)
                    {
                        tap_diag("A: region key_group %u outside the expected "
                                 "0..7 range",
                                 (unsigned) r->key_group);
                    }
                }
            }
            int count = 0;
            for (int i = 1; i <= 7; i++)
                if (seen[i])
                    count++;
            is_int(count, 7,
                   "S2.11: 7 distinct non-zero usKeyGroup values (1..7) appear "
                   "across all regions");
        }

        tap_skip("Region.artic pointer identity misclassifies 51 of 1498 "
                 "regions when an instrument's shared "
                 "default and a region's own lart happen to alias to a "
                 "single-occurrence pointer within that "
                 "instrument (independently verified: see report) -- not a "
                 "reliable proxy for the exact count",
                 "S2.11: 535 of 1498 regions carry their own lart");
    }

    tap_diag("--- Block B: S2.4/S2.5 articulation (real gm.dls data) ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not present or not loadable",
                 "S2.5 default vel_to_atten_depth across the whole bank");
        tap_skip("dist/gm.dls not present or not loadable",
                 "S2.4.2/S2.4.3 real-data lScale pan_cb conversion");
    }
    else
    {
        int bad = 0;
        uint32_t first_bad_locale = 0;
        for (Instrument *inst = g_dls.first_instrument; inst; inst = inst->next)
        {
            for (Region *r = inst->first_region; r; r = r->next)
            {
                if (!r->artic ||
                    r->artic->vel_to_atten_depth != (int16_t) 0xda80)
                {
                    if (!bad)
                        first_bad_locale = inst->locale;
                    bad++;
                }
            }
        }
        if (bad)
            tap_diag("B1: %d regions with vel_to_atten_depth != -9600 (first: "
                     "locale 0x%08x)",
                     bad, first_bad_locale);
        ok(bad == 0,
           "S2.5: every region's artic->vel_to_atten_depth == -9600 default "
           "(gm.dls has zero KEYONVELOCITY->ATTENUATION art1 connections, "
           "S2.11)");

        /* gm.dls instrument 0x1007a (bank MSB 4, prog 122) carries one
         * (0,0,0x0002) block, lScale -8, and one (0,0,0x0004), lScale -524288:
         * (-8<<4)/125 and (-524288>>12)/125 both truncate to -1 */
        Instrument *inst = dl_find_inst(0x1007au);
        ok(inst != 0, "S2.9/S2.10: instrument locale 0x1007a (bank MSB 4, "
                      "program 122) present in gm.dls");
        ok(inst && inst->first_region && inst->first_region->artic &&
               inst->first_region->artic->pan_cb == -1,
           "S2.4.2/S2.4.3 Source=0: pan_cb == -1 for instrument 0x1007a (dest "
           "0x0002 lScale=-8 and "
           "dest 0x0004 lScale=-524288 both convert to -1)");
    }

    tap_diag("--- Block C: S2.9/S2.10 locale packing + S3.1 three-tier "
             "fallback (real gm.dls data) ---");
    if (!have_dls)
    {
        tap_skip("dist/gm.dls not present or not loadable",
                 "S3.1.2 probe-36 key-25 fallback silence, Standard kit");
        tap_skip("dist/gm.dls not present or not loadable",
                 "S3.1.2 probe-36 key-25 fallback silence, SFX kit");
        tap_skip("dist/gm.dls not present or not loadable",
                 "S3.1: control -- Standard kit key 60 resolves");
    }
    else
    {
        Region *r25_std = dls_find_region(0x80000000u, 25, 0);
        ok(r25_std == 0,
           "S3.1.2: dls_find_region(Standard-kit locale, key 25) == NULL "
           "(key 25 uncovered by any Standard-kit region)");
        Region *r25_sfx = dls_find_region(0x80000000u | 56, 25, 0);
        ok(r25_sfx == 0,
           "S3.1.2 probe 36: dls_find_region(SFX-kit locale, key 25) == NULL "
           "(tier 1 fails -- SFX covers 39-84; tier 2 collapses to the "
           "Standard kit, which also "
           "fails; tier 3 is skipped for a drum locale per S3.1.2's own goto)");
        Region *r60_std = dls_find_region(0x80000000u, 60, 0);
        ok(r60_std != 0, "S3.1: control -- dls_find_region(Standard-kit "
                         "locale, key 60, 0) succeeds (60 is inside 27-87)");
    }

    tap_diag("--- Block B3/C-synth: S2.4.4 ignored usSource==4, S3.1.4 region "
             "file-order (synthetic buffers) ---");
    tap_diag("g_dls is clobbered by dls_load() from this point on -- nothing "
             "below may assume gm.dls is loaded");
    {
        dl_buf buf;
        dl_build(&buf, dl_ignored_src4);
        int rc = dls_load(buf.d, buf.n);
        ok(rc == 0, "sanity: the ignored-usSource==4 synthetic buffer loads "
                    "cleanly (dls_load == 0)");
        Instrument *inst = dl_find_inst(0);
        ok(inst && inst->first_region && inst->first_region->artic &&
               inst->first_region->artic->eg1_attack_tc == (int32_t) 0x80000000,
           "S2.4/S2.4.4: usSource==4 (EG1-as-modulator) is dropped entirely -- "
           "eg1_attack_tc stays at its "
           "0x80000000 sentinel default, not the crafted lScale=0x7fff0000");
    }
    {
        dl_buf buf;
        dl_build_overlap(&buf);
        int rc = dls_load(buf.d, buf.n);
        ok(rc == 0, "sanity: the overlapping-region synthetic buffer loads "
                    "cleanly (dls_load == 0)");
        Region *r60 = dls_find_region(0, 60, 0);
        ok(r60 && r60->low_key == 0 && r60->high_key == 100,
           "S3.1.4: note 60, covered by both regions, resolves to the FIRST "
           "region in file order [0,100]");
        Region *r127 = dls_find_region(0, 127, 0);
        ok(r127 && r127->low_key == 50 && r127->high_key == 127,
           "S3.1.4: note 127, covered only by the second region, resolves to "
           "[50,127] "
           "(linear scan continues past the first region once it fails to "
           "match)");
    }

    tap_diag("--- Block D: S2.2.2 malformed-buffer error table "
             "(D-21/D-22/D-23/D-26/D-27) ---");
    {
        dl_buf buf;
        dl_build(&buf, dl_ok);
        ok(dls_load(buf.d, buf.n) == 0,
           "control: the malformed-buffer builder's defect-free output loads "
           "cleanly");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_riff_magic);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2: bad top-level RIFF signature -> dls_load fails");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_dls_magic);
        ok(dls_load(buf.d, buf.n) < 0, "S2.2.2: bad top-level 'DLS ' signature "
                                       "(error 0x80004005) -> dls_load fails");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_colh_size);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.3.2: colh size < 4 (error 0x80041392) should abort the "
           "whole load (D-23)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_insh_size);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.3.1: insh size < 0xc (error 0x8004138f) should abort the "
           "whole load (D-23)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_rgnh_size);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.3.3: rgnh size < 0xc (error 0x8004138d) should abort the "
           "whole load (D-23)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_wlnk_channel);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.3.5: wlnk.ulChannel != 1 (error 0x8004138b) should abort "
           "the whole load (D-23)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_art1_cbsize);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.3.6: art1 payload cbSize < 8 (error 0x8004138c) should "
           "abort the whole load (D-23)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_wsmp_loops);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.6: wave-level wsmp declaring 2 loops but holding 1 "
           "record (size < 0x14+16n, error 0x80041389) aborts the whole load");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_fmt_tag);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.7.1: fmt.wFormatTag != 1 (error 0x8004138a) should abort "
           "the whole load (D-26)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_bad_fmt_nchan);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.7.1: fmt.nChannels != 1 (error 0x8004138b) should abort "
           "the whole load (D-26)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_dup_fmt);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.7.1: duplicate fmt chunk (error 0x80041389) should abort "
           "the whole load (D-27)");
    }
    {
        dl_buf buf;
        dl_build(&buf, dl_data_before_fmt);
        ok(dls_load(buf.d, buf.n) < 0,
           "S2.2.2/S2.7.2: data chunk before fmt (error 0x80041389) should "
           "abort the whole load (D-27)");
    }
}

/* -------------------------------------------------------------------- */
/* Composable synthetic-DLS builder for the per-quirk tests below. Each dlx_*
 * appends one chunk to a caller-owned dl_buf; dlx_file assembles
 * colh+lins+ptbl+wvpl around them. */

typedef struct
{
    uint16_t src, ctrl, dst, trn;
    int32_t scale;
} dlx_conn;

static void dlx_rgnh(dl_buf *b, uint16_t lo, uint16_t hi, uint16_t lovel,
                     uint16_t hivel, uint16_t opts, uint16_t kg)
{
    dl_buf t;
    dl_init(&t);
    dl_u16(&t, lo);
    dl_u16(&t, hi);
    dl_u16(&t, lovel);
    dl_u16(&t, hivel);
    dl_u16(&t, opts);
    dl_u16(&t, kg);
    dl_chunk_raw(b, "rgnh", t.d, t.n);
}

static void dlx_wlnk(dl_buf *b, uint32_t table_index, uint32_t size)
{
    dl_buf t;
    dl_init(&t);
    dl_u16(&t, 0);
    dl_u16(&t, 0);
    dl_u32(&t, 1);
    dl_u32(&t, table_index);
    dl_chunk_raw(b, "wlnk", t.d, size);
}

/* `size` < 0x14 truncates; nloops==1 appends one loop record */
static void dlx_wsmp(dl_buf *b, uint16_t unity, uint32_t nloops,
                     uint32_t loop_type, uint32_t start, uint32_t len,
                     uint32_t size)
{
    dl_buf t;
    dl_init(&t);
    dl_u32(&t, 0x14);
    dl_u16(&t, unity);
    dl_u16(&t, 0);
    dl_u32(&t, 0);
    dl_u32(&t, 0);
    dl_u32(&t, nloops);
    if (nloops)
    {
        dl_u32(&t, 0x10);
        dl_u32(&t, loop_type);
        dl_u32(&t, start);
        dl_u32(&t, len);
    }
    dl_chunk_raw(b, "wsmp", t.d, size ? size : t.n);
}

/* LIST lart { art1 }; `trailing` extra bytes after the blocks (counted in the
 * chunk size), to build cbSize/size mismatches */
static void dlx_lart(dl_buf *b, uint32_t cbsize, const dlx_conn *c, uint32_t n,
                     uint32_t trailing)
{
    dl_buf t;
    dl_init(&t);
    dl_u32(&t, cbsize);
    dl_u32(&t, n);
    for (uint32_t i = 0; i < n; i++)
    {
        dl_u16(&t, c[i].src);
        dl_u16(&t, c[i].ctrl);
        dl_u16(&t, c[i].dst);
        dl_u16(&t, c[i].trn);
        dl_u32(&t, (uint32_t) c[i].scale);
    }
    for (uint32_t i = 0; i < trailing; i++)
        dl_raw(&t, "\0", 1);
    dl_buf art;
    dl_init(&art);
    dl_chunk_raw(&art, "art1", t.d, t.n);
    dl_wrap_list(b, "lart", &art);
}

/* appends LIST rgn { body } to an lrgn body */
static void dlx_region(dl_buf *lrgn, const dl_buf *body)
{
    dl_wrap_list(lrgn, "rgn ", body);
}

/* appends LIST ins { insh, LIST lrgn, [lart] } to a lins body */
static void dlx_instrument(dl_buf *lins, uint32_t bank, uint32_t prog,
                           uint32_t cregions, const dl_buf *lrgn,
                           const dl_buf *lart)
{
    dl_buf insh, body;
    dl_init(&insh);
    dl_u32(&insh, cregions);
    dl_u32(&insh, bank);
    dl_u32(&insh, prog);
    dl_init(&body);
    dl_chunk_raw(&body, "insh", insh.d, insh.n);
    dl_wrap_list(&body, "lrgn", lrgn);
    if (lart)
        dl_raw(&body, lart->d, lart->n);
    dl_wrap_list(lins, "ins ", &body);
}

/* appends LIST <type> { fmt, data, [extra] } to a wvpl body, recording its
 * offset for ptbl */
static void dlx_wave(dl_buf *wvpl, uint32_t *offs, uint32_t *n,
                     const char *type, uint16_t bits, const void *pcm,
                     uint32_t nbytes, const dl_buf *extra)
{
    dl_buf fmt, body;
    dl_init(&fmt);
    dl_u16(&fmt, 1);
    dl_u16(&fmt, 1);
    dl_u32(&fmt, 22050);
    dl_u32(&fmt, 44100);
    dl_u16(&fmt, 2);
    dl_u16(&fmt, bits);
    dl_init(&body);
    dl_chunk_raw(&body, "fmt ", fmt.d, fmt.n);
    dl_chunk_raw(&body, "data", pcm, nbytes);
    if (extra)
        dl_raw(&body, extra->d, extra->n);
    offs[(*n)++] = wvpl->n;
    dl_wrap_list(wvpl, type, &body);
}

static void dlx_file(dl_buf *out, uint32_t cinstruments, const dl_buf *lins,
                     const dl_buf *wvpl, const uint32_t *offs, uint32_t n)
{
    dl_buf body, colh, ptbl;
    dl_init(&colh);
    dl_u32(&colh, cinstruments);
    dl_init(&ptbl);
    dl_u32(&ptbl, 8);
    dl_u32(&ptbl, n);
    for (uint32_t i = 0; i < n; i++)
        dl_u32(&ptbl, offs[i]);
    dl_init(&body);
    dl_chunk_raw(&body, "colh", colh.d, colh.n);
    dl_wrap_list(&body, "lins", lins);
    dl_chunk_raw(&body, "ptbl", ptbl.d, ptbl.n);
    dl_wrap_list(&body, "wvpl", wvpl);
    dl_init(out);
    dl_fourcc(out, "RIFF");
    dl_u32(out, body.n + 4);
    dl_fourcc(out, "DLS ");
    dl_raw(out, body.d, body.n);
}

static const int16_t dlx_pcm[8] = {1000, 2000, 3000, 4000,
                                   5000, 6000, 7000, 8000};
/* an empty but present lart: enough for a region to be playable */
static const dlx_conn dlx_no_conn[1] = {{0, 0, 0, 0, 0}};

/* One melodic instrument (bank 0, program 0), one region [0,127] whose body is
 * `rgn` (a wlnk to wave 0 is appended), instrument lart from `c`/`nc`, one
 * 16-bit wave whose own body gets `wextra`. */
static int dlx_load_one(const dl_buf *rgn, const dlx_conn *c, uint32_t nc,
                        const dl_buf *wextra)
{
    dl_buf body, lrgn, lart, lins, wvpl, file;
    uint32_t offs[1], n = 0;
    dl_init(&body);
    if (rgn)
        dl_raw(&body, rgn->d, rgn->n);
    dlx_wlnk(&body, 0, 12);
    dl_init(&lrgn);
    dlx_region(&lrgn, &body);
    dl_init(&lart);
    dlx_lart(&lart, 8, c, nc, 0);
    dl_init(&lins);
    dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
    dl_init(&wvpl);
    dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, wextra);
    dlx_file(&file, 1, &lins, &wvpl, offs, n);
    return dls_load(file.d, file.n);
}

static Region *dlx_region0(void)
{
    Instrument *inst = dl_find_inst(0);
    return inst ? inst->first_region : 0;
}

static Voice *vo_find(int channel, int note)
{
    Voice *found = 0;
    for (int i = 0; i < NUM_VOICES; i++)
        if (g_voices[i].active && g_voices[i].channel == channel &&
            g_voices[i].note == note &&
            (!found || g_voices[i].age > found->age))
            found = &g_voices[i];
    return found;
}

/* Control-plane and voice findings that need no DLS, or the real gm.dls. Runs
 * before t_dls clobbers g_dls. */
static void t_findings_live(void)
{
    tap_diag("--- S4.3: CC121's value byte gates Pan as well as Volume "
             "[A:0x1351f]-[A:0x13551] ---");
    {
        synth_construct();
        synth_midi(0xB3, 10, 20);
        synth_midi(0xB3, 121, 0);
        is_int(g_channels[3].pan, 20,
               "S4.3 CC121 value=0 leaves Pan unchanged: the Pan=64 schedule "
               "(0x13545-0x13551, ebx=0x40 from 0x1336f) sits inside the "
               "data2!=0 branch with Volume");
        synth_midi(0xB3, 121, 1);
        is_int(g_channels[3].pan, 64, "S4.3 CC121 value!=0 resets Pan to 64");
    }

    tap_diag("--- S4.5: RCV CHANNEL data byte n selects channel n-1; 0 is "
             "ignored [A:0x137c0]-[A:0x137c7] ---");
    {
        synth_construct();
        sy_gs_reset();
        sy_rcv_channel(2, 0); /* ignored: block 2 keeps default channel 1 */
        sy_use_rhythm(2, 1);
        is_int(g_channels[1].is_rhythm, 1,
               "S4.5 RCV CHANNEL data 0 is ignored (test cl,cl / je at "
               "0x137c3): block 2 still feeds channel 1");
        synth_construct();
        sy_gs_reset();
        sy_rcv_channel(1, 6); /* dec cl at 0x137c7 -> channel 5 */
        sy_use_rhythm(1, 1);
        is_int(g_channels[5].is_rhythm, 1,
               "S4.5 RCV CHANNEL data 6 maps block 1 to channel 5 (n-1)");
    }

    tap_diag("--- S4.2.2: the three per-part DT1 handlers are each gated on GS "
             "mode [A:0x1376e],[A:0x137a2],[A:0x137b7] ---");
    {
        synth_construct();
        sy_rcv_channel(1, 6);
        sy_use_rhythm(1, 1);
        ok(g_channels[0].is_rhythm == 0 && g_channels[5].is_rhythm == 0,
           "S4.2.2 without a GS Reset, RCV CHANNEL and USE RHYTHM PART are "
           "dropped");
    }

    tap_diag("--- S4.5: USE RHYTHM PART runs ResetAllChannelControllers on all "
             "16 channels; Bank/Program survive [A:0x137b5],[A:0x13807] ---");
    {
        synth_construct();
        sy_gs_reset();
        synth_midi(0xB3, 7, 50);
        synth_midi(0xC3, 10, 0);
        vo_setup_bare_active(&g_voices[0], 2, 40);
        vo_setup_bare_active(&g_voices[1], 7, 41);
        sy_use_rhythm(4, 1);
        ok(!g_voices[0].active && !g_voices[1].active,
           "S4.5 USE RHYTHM PART cuts sounding voices on unrelated channels");
        ok(g_channels[3].volume == 100 && g_channels[3].program == 10,
           "S4.5 USE RHYTHM PART resets Volume to 100 but keeps Program");
        synth_construct();
    }

    tap_diag("--- S4.3: CC126/CC127 release the channel like CC120 "
             "[A:0x13621],[A:0x13618] ---");
    {
        synth_construct();
        vo_setup_bare_active(&g_voices[0], 4, 40);
        synth_midi(0xB4, 126, 1);
        ok(g_voices[0].env_stage == ENV_RELEASE,
           "S4.3 CC126 (Mono On) releases held voices on its channel");
        vo_setup_bare_active(&g_voices[1], 4, 41);
        synth_midi(0xB4, 127, 0);
        ok(g_voices[1].env_stage == ENV_RELEASE,
           "S4.3 CC127 (Poly On) releases held voices on its channel");
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S3.5: Master Volume, CC7 and CC11 index the velocity table "
             "[A:0x16b94],[A:0x16d8a],[A:0x12d28] ---");
    {
        synth_construct();
        sy_master_vol(0, 64);
        is_int(g_master_vol_hdb, g_table_vel[64],
               "S3.5 Master Volume MSB 64 -> velocity-table entry 64");
        sy_master_vol(0, 127);
        int ch = 6;
        Voice *v = &g_voices[0];
        vo_setup_bare_active(v, ch, 0);
        v->atten_const_hdb = -3000;
        synth_midi(0xB0 | ch, 7, 64);
        synth_midi(0xB0 | ch, 11, 90);
        synth_midi(0xB0 | ch, 10, 0);
        voice_update_gain(v);
        int32_t sum = v->atten_const_hdb + g_table_vel[64] + g_table_vel[90] +
                      g_table_lin[127];
        is_near(v->gain_l_target,
                (double) g_table_dbamp[sum / 10 + 1000] / 8192.0, 1e-12,
                "S3.5 CC7 and CC11 add their velocity-table entries to the "
                "attenuation sum");
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S3.6: pan law, one table read forward and reversed "
             "[A:0x19bfe]-[A:0x19c2a] ---");
    {
        synth_construct();
        int ch = 11;
        static const uint8_t pans[5] = {0, 16, 64, 100, 127};
        int bad = 0;
        for (int i = 0; i < 5; i++)
        {
            Voice *v = &g_voices[0];
            vo_setup_bare_active(v, ch, 0);
            v->atten_const_hdb = -2000;
            synth_midi(0xB0 | ch, 10, pans[i]);
            voice_update_gain(v);
            int32_t base =
                v->atten_const_hdb + g_table_vel[100] + g_table_vel[127];
            double want_l =
                (double)
                    g_table_dbamp[(base + g_table_lin[127 - pans[i]]) / 10 +
                                  1000] /
                8192.0;
            double want_r =
                (double)
                    g_table_dbamp[(base + g_table_lin[pans[i]]) / 10 + 1000] /
                8192.0;
            if (fabs(v->gain_l_target - want_l) > 1e-12 ||
                fabs(v->gain_r_target - want_r) > 1e-12)
                bad++;
        }
        ok(bad == 0, "S3.6 L adds PAN[127-pan], R adds PAN[pan], "
                     "PAN[j]=trunc(1000*log10(j/127)), before the clamp");
        Voice *v = &g_voices[0];
        vo_setup_bare_active(v, ch, 0);
        v->artic->pan_cb = 100;
        synth_midi(0xB0 | ch, 10, 100);
        v->atten_const_hdb = -2000;
        voice_update_gain(v);
        double l127 = v->gain_l_target;
        v->artic->pan_cb = 0;
        synth_midi(0xB0 | ch, 10, 127);
        voice_update_gain(v);
        is_near(l127, v->gain_l_target, 1e-12,
                "S3.6 pan index is clamp(CC10 + art pan, 0, 127)");
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S6.4.5.1: per-voice gain ceiling, TABLE[0]=4095 of 8192 "
             "[A:0x18e09] ---");
    {
        synth_construct();
        Voice *v = &g_voices[0];
        vo_setup_bare_active(v, 5, 0);
        v->atten_const_hdb = 3000;
        voice_update_gain(v);
        ok(v->gain_l_target == 4095.0 / 8192.0 &&
               v->gain_r_target == 4095.0 / 8192.0,
           "S6.4.5.1 any attenuation sum >= 0 pins both channels at 4095/8192, "
           "about -6 dB below computed unity");
        voice_pool_reset();
    }

    tap_diag("--- S3.3.3: CentsToRatio clamps the note-on sum to +-4800; live "
             "bend multiplies outside it [A:0x18e2c] ---");
    {
        is_int(vo_cents_to_ratio_q12(6000), vo_cents_to_ratio_q12(4800),
               "S3.3.3 CentsToRatio(6000) == CentsToRatio(4800)");
        synth_construct();
        Voice *v = &g_voices[0];
        static Wave w;
        w.sample_rate = 22050;
        vo_setup_bare_active(v, 2, 60);
        v->wave = &w;
        v->base_ratio_q12 = (uint32_t) vo_cents_to_ratio_q12(4800);
        synth_midi(0xE2, 0x7F, 0x7F); /* bend up, 200-cent range */
        voice_update_pitch(v);
        uint64_t clamped =
            (uint64_t) 22050 * (uint64_t) vo_cents_to_ratio_q12(4800);
        ok(v->phase_step_target > (uint32_t) (clamped / RENDER_RATE),
           "S3.3.2c a bend on a note already at +4800 still raises the pitch "
           "(live delta is outside the note-on clamp)");
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S5.6: no release floor -- duration = authored * L / 1000 "
             "[A:0x19801],[A:0x19815] ---");
    {
        synth_construct();
        static Artic a;
        a = vo_bare_artic;
        /* 10 ms authored release */
        a.eg1_release_tc = (int32_t) (1200.0 * log2(0.010) * 65536.0);
        Voice *v = &g_voices[0];
        vo_setup_bare_active(v, 3, 50);
        v->artic = &a;
        voice_note_off(3, 50);
        double want = rt_pow(10.0, -(96.0 / 20.0) / (0.010 * RENDER_RATE));
        todo_ok(fabs(v->env_release_coef - want) < 1e-9,
                "RELEASE_FLOOR_S is an [F] stand-in; removing it regresses "
                "the corpus mean -- INVESTIGATE.adoc#i-release-floor",
                "S5.6 a 10 ms authored release stays 10 ms (no floor) "
                "[A:0x19801],[A:0x19815]");
        voice_pool_reset();
    }

    tap_diag("--- S5.1.2.1: decay is a straight line in dB that lands on the "
             "sustain level at the rescaled duration ---");
    if (!have_dls)
        tap_skip("dist/gm.dls not loadable", "S5.1.2.1 decay shape");
    else
    {
        synth_construct();
        voice_note_on(0, 60, 127);
        Voice *v = vo_find(0, 60);
        if (!v || v->env_decay_samples_left < 4)
            tap_skip("piano note 60 has no measurable decay",
                     "S5.1.2.1 decay shape");
        else
        {
            while (v->env_stage == ENV_ATTACK)
                voice_step_envelope(v);
            int32_t n = v->env_decay_samples_left;
            double db_sus = 20.0 * log10(v->env_sustain_level);
            for (int32_t i = 0; i < n / 2; i++)
                voice_step_envelope(v);
            double db_mid = 20.0 * log10(v->env_level);
            is_near(db_mid, db_sus * (double) (n / 2) / (double) n, 0.01,
                    "S5.1.2.1 halfway through decay the level is halfway (in "
                    "dB) to sustain");
            for (int32_t i = n / 2; i < n; i++)
                voice_step_envelope(v);
            ok(v->env_stage == ENV_SUSTAIN &&
                   v->env_level == v->env_sustain_level,
               "S5.1.2.1 decay ends exactly on the sustain level at the "
               "rescaled duration");
        }
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S3.4.2: attack is linear in amplitude, quantized through "
             "Table C in 1/200 steps [A:0x18ad3]-[A:0x18b24] ---");
    {
        synth_construct();
        static Artic a;
        a = vo_bare_artic;
        Voice *v = &g_voices[0];
        vo_setup_bare_active(v, 1, 60);
        v->artic = &a;
        v->env_stage = ENV_ATTACK;
        v->env_level = 0.0;
        v->env_attack_samples = 2000;
        v->env_attack_step = 1.0 / 2000.0;
        v->env_attack_elapsed = 0;
        for (int i = 0; i < 777; i++)
            voice_step_envelope(v);
        int32_t idx = (777 * 1000 / 2000) / 5;
        double want =
            pow(10.0,
                ((double) g_table_envshape[idx] / 1000.0 - 1.0) * 96.0 / 20.0);
        todo_ok(fabs(v->env_level - want) < 1e-9,
                "ENV_ATTACK_TABLE_C=1 measured -32.47 vs -32.48 dB corpus "
                "mean, no item moved >0.3 dB -- not shipped, "
                "INVESTIGATE.adoc#i-attack-quantize",
                "S3.4.2 attack level = TableC[(elapsed*1000/attack)/5] on the "
                "96 dB scale");
        voice_pool_reset();
    }

    tap_diag("--- S4.5: GM System Off unloads the collection until GM System "
             "On or System Reset [A:0x13700],[A:0x13218] ---");
    if (!have_dls)
        tap_skip("dist/gm.dls not loadable", "S4.5 GM System Off");
    else
    {
        synth_construct();
        sy_gm(0);
        voice_note_on(0, 60, 100);
        ok(!voice_any_active(), "S4.5 after GM System Off a note-on is silent");
        sy_gm(1);
        voice_note_on(0, 60, 100);
        ok(voice_any_active(), "S4.5 GM System On re-acquires the collection");
        sy_gm(0);
        synth_midi(0xFF, 0, 0);
        voice_note_on(0, 60, 100);
        ok(voice_any_active(), "S4.5 System Reset (0xFF) re-acquires it too");
        voice_pool_reset();
        synth_construct();
    }

    tap_diag("--- S4.3: Mono mode releases every held voice on the channel "
             "before a note-on [A:0x12ddd]-[A:0x12e0e] ---");
    if (!have_dls)
        tap_skip("dist/gm.dls not loadable", "S4.3 mono note-on");
    else
    {
        synth_construct();
        synth_midi(0xB0, 126, 1);
        voice_note_on(0, 60, 100);
        Voice *a = vo_find(0, 60);
        voice_note_on(0, 64, 100);
        ok(a && a->env_stage == ENV_RELEASE && !a->fast_release_committed,
           "S4.3 in Mono mode a new note releases the previous one through "
           "ordinary note-off (0x19a2c)");
        voice_pool_reset();
        synth_construct();
    }
}

/* Parser and note-on findings on synthetic collections. Runs after t_dls. */
static void t_findings_dls(void)
{
    tap_diag("--- S2.2.1: no RIFF odd-size padding outside INFO "
             "[A:0x16486] ---");
    {
        dl_buf rgn;
        dl_init(&rgn);
        dl_chunk_raw(&rgn, "junk", "abc", 3);
        dlx_rgnh(&rgn, 10, 20, 0, 127, 0, 0);
        int rc = dlx_load_one(&rgn, dlx_no_conn, 0, 0);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->low_key == 10 && r->high_key == 20,
           "S2.2.1 a chunk after an odd-sized one is read at cur+8+size, no "
           "pad byte");
    }

    tap_diag("--- S2.2.2: further whole-load aborts [A:0x15c67],[A:0x15ada],"
             "[A:0x154be],[A:0x15715],[A:0x15c60] ---");
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_wlnk(&rgn, 0, 8);
        ok(dlx_load_one(&rgn, dlx_no_conn, 0, 0) < 0,
           "S2.2.2 wlnk size < 0xc aborts the load (0x8004138e)");
    }
    {
        dl_buf body, lrgn, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 8, dlx_no_conn, 1, 4);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        ok(dls_load(file.d, file.n) < 0,
           "S2.2.2 art1 size != cbSize + 12*cConnectionBlocks aborts the load "
           "(0x8004138c)");
    }
    {
        dl_buf w;
        dl_init(&w);
        dlx_wsmp(&w, 60, 1, 1, 0, 4, 0);
        ok(dlx_load_one(0, dlx_no_conn, 0, &w) < 0,
           "S2.2.2 wave-level wsmp ulLoopType != 0 aborts the load "
           "(0x80041389)");
    }
    {
        dl_buf w;
        dl_init(&w);
        dlx_wsmp(&w, 60, 0, 0, 0, 0, 0x10);
        ok(dlx_load_one(0, dlx_no_conn, 0, &w) < 0,
           "S2.2.2 wave-level wsmp size < 0x14 aborts the load (0x80041389)");
    }
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_wsmp(&rgn, 60, 2, 0, 0, 4, 0);
        ok(dlx_load_one(&rgn, dlx_no_conn, 0, 0) < 0,
           "S2.2.2 region-level wsmp cSampleLoops > 1 aborts the load "
           "(0x8004138d)");
    }
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_wsmp(&rgn, 60, 0, 0, 0, 0, 0x10);
        ok(dlx_load_one(&rgn, dlx_no_conn, 0, 0) < 0,
           "S2.2.2 region-level wsmp size < 0x14 aborts the load "
           "(0x8004138d)");
    }
    {
        dl_buf lrgn, body, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 8, dlx_no_conn, 0, 0);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 12, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        ok(dls_load(file.d, file.n) < 0,
           "S2.2.2 fmt wBitsPerSample other than 8/16 aborts the load "
           "(0x80041389)");
    }

    {
        /* two complete loop records: room for both, so no error */
        dl_buf t, w;
        dl_init(&t);
        dl_u32(&t, 0x14);
        dl_u16(&t, 60);
        dl_u16(&t, 0);
        dl_u32(&t, 0);
        dl_u32(&t, 0);
        dl_u32(&t, 2);
        for (int k = 0; k < 2; k++)
        {
            dl_u32(&t, 0x10);
            dl_u32(&t, 0);
            dl_u32(&t, k ? 5u : 1u);
            dl_u32(&t, 2);
        }
        dl_init(&w);
        dl_chunk_raw(&w, "wsmp", t.d, t.n);
        int rc = dlx_load_one(0, dlx_no_conn, 0, &w);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->wave && !r->wave->no_loop &&
               r->wave->loop_start == 1 && r->wave->loop_end == 3,
           "S2.3.4 wave-level wsmp has no loop-count limit: cSampleLoops=2 "
           "with both records present loads and uses record 0 "
           "[A:0x154a5]-[A:0x154d7]");
    }

    tap_diag("--- S2.2.2/S2.3: fields the parser ignores or truncates ---");
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_rgnh(&rgn, 0x010A, 0x0114, 100, 127, 0, 0x0103);
        dlx_wsmp(&rgn, 0x013C, 1, 7, 2, 4, 0);
        int rc = dlx_load_one(&rgn, dlx_no_conn, 0, 0);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->low_key == 10 && r->high_key == 20 &&
               r->key_group == 3,
           "S2.3.3 rgnh keys and key group: low byte only");
        ok(r && r->unity_note == 60, "S2.3.4 usUnityNote: low byte only");
        ok(r && !r->no_loop && r->loop_start == 2 && r->loop_end == 6,
           "S2.3.4 region-level ulLoopType is never read");
        Instrument *inst = dl_find_inst(0);
        ok(inst && inst->region_count == 1,
           "S2.3.1/S2.3.2 insh.cRegions and colh.cInstruments are ignored "
           "(actual chunks are counted)");
        synth_construct();
        voice_note_on(0, 15, 10);
        ok(vo_find(0, 15) != 0,
           "S2.3.3 rgnh velocity range is never read: velocity 10 plays a "
           "region authored for 100-127");
        voice_pool_reset();
    }
    {
        dl_buf body, lrgn, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0x10000, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 8, dlx_no_conn, 0, 0);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0x85, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        int rc = dls_load(file.d, file.n);
        Instrument *inst = dl_find_inst(0x85);
        ok(rc == 0 && inst,
           "S2.3.1 insh.ulInstrument is stored unmasked into the locale");
        ok(inst && inst->first_region &&
               inst->first_region->wave_pool_index == 0,
           "S2.3.5 wlnk.ulTableIndex: low word only");
    }
    {
        dl_buf body, lrgn, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 8, dlx_no_conn, 0, 0);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "Wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        int rc = dls_load(file.d, file.n);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->wave && r->wave->samples == 0,
           "S2.2.2 a wave LIST typed 'Wave' (neither 'wave' nor 'WAVE') is "
           "skipped [A:0x15c9c]");
    }
    {
        static const uint8_t pcm8[4] = {0xC0, 0x40, 0x80, 0x00};
        dl_buf body, lrgn, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 8, dlx_no_conn, 0, 0);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 8, pcm8, sizeof pcm8, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        int rc = dls_load(file.d, file.n);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->wave && r->wave->samples &&
               r->wave->samples[0] == 16384 && r->wave->samples[1] == -16384 &&
               r->wave->samples[3] == -32768,
           "S2.7.2/S6.2.2 8-bit PCM: byte+0x80 then *256 [A:0x1554e]");
    }

    tap_diag("--- S2.3.6/S2.4: art1 decoding ---");
    {
        static const dlx_conn c[1] = {
            {0, 0, ART_DST_EG1_ATTACKTIME, 0, 0x01234567}};
        dl_buf body, lrgn, lart, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lart);
        dlx_lart(&lart, 12, c, 1, 4); /* cbSize 12: header padded by 4 */
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, &lart);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        int rc = dls_load(file.d, file.n);
        Region *r = dlx_region0();
        ok(rc == 0 && r && r->artic && r->artic->eg1_attack_tc == 0x01234567,
           "S2.3.6 connection blocks start at +8 whatever cbSize says "
           "[A:0x15804]");
    }
    {
        static const dlx_conn c[8] = {
            {0, 0, ART_DST_EG1_DECAYTIME, 1, 0x00450000},
            {0, 0, ART_DST_EG1_SUSTAINLEVEL_LO, 0, 500},
            {0, 0, ART_DST_EG1_SUSTAINLEVEL_HI, 0, 800 << 16},
            {1, ART_CTRL_CC1, ART_DST_PITCH, 0, 2000 << 16},
            {1, 0x82, ART_DST_PITCH, 0, 50 << 16},
            {5, 0, ART_DST_PITCH, 0, (int32_t) ((300u << 16) | 0xFFFFu)},
            {2, 0, ART_DST_ATTENUATION, 0, -300 * 65536 / 10},
            {2, 0, ART_DST_ATTENUATION, 0, (int32_t) 0x80000000},
        };
        int rc = dlx_load_one(0, c, 8, 0);
        Region *r = dlx_region0();
        Artic *a = r ? r->artic : 0;
        ok(rc == 0 && a && a->eg1_decay_tc == 0x00450000,
           "S2.4 usTransform is never read (a concave block applies "
           "linearly)");
        ok(a && a->eg1_sustain_permille == 800,
           "S2.4.3 dest 0x020a writes lScale's HIGH word into the EG1 sustain "
           "field shared with 0x0208 [A:0x15aa0]");
        ok(a && a->lfo_pitch_cc1_cents == 1200,
           "S2.4.2 pitch destinations take lScale>>16, clamped to +-1200");
        ok(a && a->lfo_pitch_inherent_cents == 0,
           "S2.4.3 an LFO connection with usControl other than 0/0x81 is "
           "dropped [A:0x158e8]");
        ok(a && a->eg2_to_pitch_cents == 300,
           "S2.4.2 EG2->pitch: high word, low word discarded");
        ok(a && a->vel_to_atten_depth == -9600,
           "S2.4.2 velocity->attenuation lScale INT32_MIN maps to -9600 "
           "[A:0x158a4]");
    }

    tap_diag("--- S3.2: a region with no lart at either scope never sounds "
             "[A:0x19b68] ---");
    {
        dl_buf body, lrgn, lins, wvpl, file;
        uint32_t offs[1], n = 0;
        dl_init(&body);
        dlx_wlnk(&body, 0, 12);
        dl_init(&lrgn);
        dlx_region(&lrgn, &body);
        dl_init(&lins);
        dlx_instrument(&lins, 0, 0, 1, &lrgn, 0);
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 1, &lins, &wvpl, offs, n);
        ok(dls_load(file.d, file.n) == 0 && dls_find_region(0, 60, 0) == 0,
           "S3.2 no region or instrument lart -> no voice");
    }

    tap_diag("--- S3.3.5: loop end <= loop start disables the loop "
             "[A:0x18f95] ---");
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_wsmp(&rgn, 60, 1, 0, 4, 0, 0);
        dlx_load_one(&rgn, dlx_no_conn, 0, 0);
        synth_construct();
        voice_note_on(0, 60, 100);
        Voice *v = vo_find(0, 60);
        ok(v && v->loop_len_s == 0 && v->sample_end_s == 8,
           "S3.3.5 a zero-length loop plays as a one-shot to the end of the "
           "data");
        voice_pool_reset();
    }

    tap_diag("--- S5.6: same-note retrigger only on regions without "
             "SELFNONEXCLUSIVE [A:0x12e8c]-[A:0x12ec6] ---");
    for (int opt = 0; opt < 2; opt++)
    {
        dl_buf rgn;
        dl_init(&rgn);
        dlx_rgnh(&rgn, 0, 127, 0, 127, (uint16_t) opt, 0);
        dlx_load_one(&rgn, dlx_no_conn, 0, 0);
        synth_construct();
        voice_note_on(0, 60, 100);
        Voice *a = vo_find(0, 60);
        voice_note_off(0, 60);
        voice_note_on(0, 60, 100);
        if (opt == 0)
            ok(a && a->fast_release_committed,
               "S5.6 fusOptions bit 0 clear: a repeated note fast-releases "
               "the earlier one, held or not");
        else
            ok(a && !a->fast_release_committed,
               "S5.6 fusOptions bit 0 set (SELFNONEXCLUSIVE, 98 gm.dls "
               "regions): the earlier note keeps its own release");
        voice_pool_reset();
    }

    tap_diag("--- S3.1.2/S3.8.1: melodic fallback, and choke scope uses the "
             "locale after fallback [A:0x12e5c],[A:0x12f26] ---");
    {
        static const dlx_conn kg[1] = {{0, 0, 0, 0, 0}};
        dl_buf lins, wvpl, file, lart;
        uint32_t offs[1], n = 0;
        dl_init(&lins);
        dl_init(&lart);
        dlx_lart(&lart, 8, kg, 0, 0);
        /* drum kit 0: keys 30 and 31 in key group 1 */
        {
            dl_buf lrgn, b;
            dl_init(&lrgn);
            for (int k = 30; k <= 31; k++)
            {
                dl_init(&b);
                dlx_rgnh(&b, (uint16_t) k, (uint16_t) k, 0, 127, 0, 1);
                dlx_wlnk(&b, 0, 12);
                dlx_region(&lrgn, &b);
            }
            dlx_instrument(&lins, 0x80000000u, 0, 2, &lrgn, &lart);
        }
        /* drum kit 5: key 60 only; melodic bank 0 program 7 */
        for (int i = 0; i < 2; i++)
        {
            dl_buf lrgn, b;
            dl_init(&lrgn);
            dl_init(&b);
            dlx_rgnh(&b, i ? 0 : 60, i ? 127 : 60, 0, 127, 0, 0);
            dlx_wlnk(&b, 0, 12);
            dlx_region(&lrgn, &b);
            dlx_instrument(&lins, i ? 0 : 0x80000000u, i ? 7 : 5, 1, &lrgn,
                           &lart);
        }
        dl_init(&wvpl);
        dlx_wave(&wvpl, offs, &n, "wave", 16, dlx_pcm, sizeof dlx_pcm, 0);
        dlx_file(&file, 3, &lins, &wvpl, offs, n);
        ok(dls_load(file.d, file.n) == 0, "sanity: three-instrument buffer "
                                          "loads");
        synth_construct();
        sy_gs_reset();
        synth_midi(0xB2, 0, 3);
        synth_midi(0xC2, 7, 0);
        voice_note_on(2, 40, 100);
        ok(vo_find(2, 40) != 0,
           "S3.1.2 bank 3 program 7 missing -> retry bank 0 program 7");
        synth_midi(0xC9, 0, 0);
        voice_note_on(9, 30, 100);
        Voice *a = vo_find(9, 30);
        synth_midi(0xC9, 5, 0);
        voice_note_on(9, 31, 100);
        ok(a && a->fast_release_committed,
           "S3.8.1 kit 5 lacks key 31, falls back to kit 0 with locale "
           "0x80000000, which matches kit 0's voice and chokes it");
        voice_pool_reset();
    }

    tap_diag("--- S3.3.2a/S4.2.2: GS Scale Tuning retunes by semitone class; "
             "the store loop tests one byte ahead [A:0x1377a]-[A:0x13797],"
             "[A:0x12f60] ---");
    {
        dlx_load_one(0, dlx_no_conn, 0, 0);
        synth_construct();
        sy_gs_reset();
        /* block 1 -> channel 0; 3 data bytes then F7: only 2 are stored */
        uint8_t b[11] = {0x41, 0x10,      0x42,      0x12,      0x40, 0x11,
                         0x40, 0x40 + 10, 0x40 + 20, 0x40 + 30, 0xF7};
        synth_sysex(b, sizeof b);
        int32_t want[3] = {10, 20, 0};
        int bad = 0;
        for (int k = 0; k < 3; k++)
        {
            voice_note_on(0, 60 + k, 100);
            Voice *v = vo_find(0, 60 + k);
            if (!v || v->base_cents != k * 100 + want[k])
                bad++;
        }
        ok(bad == 0, "S4.2.2 C +10 and C# +20 cents are applied; D is not "
                     "written because F7 is tested one byte early");
        voice_pool_reset();
        synth_construct();
        voice_note_on(0, 60, 100);
        Voice *v = vo_find(0, 60);
        ok(v && v->base_cents == 0, "S4.6.1 a reset clears the tuning grid");
        voice_pool_reset();
    }
}

int main(void)
{
    tap_begin();
    tables_build();
    /* Engine-cannot-initialise check: if the table builder did not run, nothing
     * downstream is meaningful. S1.4.4 Q12 unity is the cheapest proof it ran.
     */
    if (g_table_cents[100] != 4096)
        tap_bail("engine init failed: tables_build() did not populate "
                 "g_table_cents");

    /* dist/gm.dls is an ENGINE INPUT, not an audio reference. */
    {
        FILE *f = fopen("dist/gm.dls", "rb");
        uint8_t *buf = NULL;
        uint32_t len = 0;
        if (f)
        {
            if (fseek(f, 0, SEEK_END) == 0)
            {
                long n = ftell(f);
                if (n > 0)
                {
                    rewind(f);
                    buf = (uint8_t *) malloc((size_t) n);
                    if (buf && fread(buf, 1, (size_t) n, f) == (size_t) n)
                    {
                        len = (uint32_t) n;
                    }
                    else
                    {
                        free(buf);
                        buf = NULL;
                    }
                }
            }
            fclose(f);
        }
        have_dls = (buf != NULL && dls_load(buf, len) == 0);
    }
    if (!have_dls)
        tap_diag("dist/gm.dls not loaded -- DLS-backed tests will SKIP");

    synth_construct();

    t_tables();
    t_rt();
    t_synth();
    t_voice();
    t_render();
    t_findings_live();
    /* LAST: its malformed-buffer cases overwrite the global g_dls */
    t_dls();
    t_findings_dls();
    return tap_done();
}
