/* What the simulator (sim/a8sim.c) and the benchmark image agree on. Plain C. */
#pragma once
#include <stdint.h>

#define BENCH_MARK_BEGIN 1 /* warm-up is over, measurement starts */
#define BENCH_MARK_FRAME 2 /* one 128-sample frame is done */
#define BENCH_MARK_OBJ 3   /* arg = index of the object about to run, -1 = harness code */
#define BENCH_MARK_END 4

#define BENCH_MAX_OBJ 320
#define BENCH_NAME_LEN 48
#define BENCH_LIST_LEN 32768

typedef struct
{
  int32_t scenario;     /* index, or -1 to list the scenarios into g_bench_list */
  int32_t sampleRate;   /* 48000 or 96000 */
  int32_t warmupFrames; /* run before BENCH_MARK_BEGIN */
  int32_t frames;       /* measured */
} BenchCfg;

typedef struct
{
  int32_t status; /* 1 = ran to the end, negative = error */
  int32_t nobj;
  float peak[2]; /* of the scenario's outputs over the measured frames */
  float rms[2];
  char name[64];
  char group[32];
  char obj[BENCH_MAX_OBJ][BENCH_NAME_LEN]; /* "unit/object" */
} BenchOut;
