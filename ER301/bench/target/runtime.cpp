// What the od objects need from the firmware that the benchmark image does not have:
// the frame pool, the profiler clock, the log, the configuration, a heap. Everything else
// (Object, Inlet, Outlet, Parameter, the stock units) is the firmware's own source.
#include <od/AudioThread.h>
#include <od/objects/Outlet.h>
#include <od/extras/LookupTables.h>
#include <od/config.h>
#include <hal/timing.h>
#include <hal/log.h>
#include <hal/heap.h>
#include <hal/rng.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <sys/stat.h>
#include "bench.h"

extern "C"
{
  ConfigData globalConfig;
  BenchCfg g_bench_cfg = {0, 48000, 64, 64};
  BenchOut g_bench_out;
  char g_bench_list[BENCH_LIST_LEN];

  // The simulator watches for this address; the arguments are read from r0 and r1.
  void __attribute__((noinline)) bench_mark(int code, int arg)
  {
    __asm__ volatile("");
    (void)code;
    (void)arg;
  }

  // ---- heap and C library plumbing
  extern char __heap_start, __heap_end;
  static char *brk_now = &__heap_start;
  void *_sbrk(ptrdiff_t n)
  {
    if (brk_now + n > &__heap_end)
    {
      errno = ENOMEM;
      return (void *)-1;
    }
    char *p = brk_now;
    brk_now += n;
    return p;
  }
  int _write(int, const char *, int n) { return n; }
  int _read(int, char *, int) { return 0; }
  int _close(int) { return -1; }
  int _lseek(int, int, int) { return 0; }
  int _fstat(int, struct stat *st)
  {
    st->st_mode = S_IFCHR;
    return 0;
  }
  int _isatty(int) { return 1; }
  int _kill(int, int) { return -1; }
  int _getpid(void) { return 1; }
  void bench_exit(void);
  void _exit(int)
  {
    g_bench_out.status = -99;
    bench_exit();
    for (;;)
    {
    }
  }
  void *__dso_handle = 0;

  // ---- hal
  static tick_t fakeTicks = 0;
  void Timing_init(void) {}
  tick_t ticks(void) { return fakeTicks; }
  float ticks2secs(tick_t n) { return (float)n * 1e-9f; }
  double ticks2secsD(tick_t n) { return (double)n * 1e-9; }
  float wallclock(void) { return 0.0f; }
  void wallclock_reset(void) {}
  unsigned long micros(void) { return 0; }

  void Log_init() {}
  int logCount() { return 0; }
  void logWrite(const char *, int) {}
  void _logRaw(const char *, ...) {}
  void _logAssert(const char *, const char *, int, int expr)
  {
    if (!expr)
      g_bench_out.status = -2;
  }
  void _logBriefNice(const char *, const char *, ...) {}
  void _logCompleteNice(const char *, const char *, const char *, int, const char *, ...) {}
  void _logRude(const char *, const char *, const char *, int, const char *, ...) { g_bench_out.status = -3; }
  void _logSkipped() {}

  void *Heap_memalign(size_t align, size_t size) { return memalign(align, size); }
  void *Heap_malloc(size_t size) { return malloc(size); }
  void *Heap_calloc(size_t n, size_t size) { return calloc(n, size); }
  void *Heap_realloc(void *p, size_t size) { return realloc(p, size); }
  void Heap_free(void *p) { free(p); }
  extern char __bigheap_start, __bigheap_end;
  uintptr_t Heap_getUnusedMemoryStart() { return (uintptr_t)&__bigheap_start; }
  uint32_t Heap_getUnusedMemorySize() { return (uint32_t)(&__bigheap_end - &__bigheap_start); }

  static uint32_t rngState = 0x12345678u;
  uint32_t Rng_read32(void)
  {
    rngState = rngState * 1664525u + 1013904223u;
    return rngState;
  }
}

// ---- the audio frame pool: 1024 cache-line aligned frames, as AudioThread::init() makes
namespace od
{
  static float *pool = 0;
  static float **freeList = 0;
  static int freeCount = 0;
  static const int kFrames = 1024;

  void AudioThread::init()
  {
    const int n = globalConfig.frameLength;
    const size_t bytes = CACHE_ALIGNED_SIZE(n * sizeof(float));
    pool = (float *)memalign(CACHELINE_SIZE_MAX, bytes * kFrames);
    freeList = (float **)malloc(sizeof(float *) * kFrames);
    for (int i = 0; i < kFrames; i++)
      freeList[i] = (float *)((char *)pool + bytes * (kFrames - 1 - i));
    freeCount = kFrames;
    Outlet::initializeGlobalOutlets();
    LookupTables::initialize();
  }
  float *AudioThread::getFrame()
  {
    if (freeCount == 0)
    {
      g_bench_out.status = -4;
      return 0;
    }
    return freeList[--freeCount];
  }
  void AudioThread::releaseFrame(float *frame) { freeList[freeCount++] = frame; }
  void AudioThread::connect(Outlet *outlet, Inlet *inlet, Object *) { inlet->connect(outlet); }
  void AudioThread::disconnect(Inlet *inlet, Object *) { inlet->disconnect(); }
  void AudioThread::disconnect(Outlet *outlet, Object *) { outlet->disconnect(); }
  void AudioThread::beginTransaction() {}
  void AudioThread::endTransaction() {}
}

// ---- C runtime start
extern "C"
{
  extern char __bss_start__, __bss_end__;
  void __libc_init_array(void);
  int main(void);
  void bench_crt(void)
  {
    memset(&__bss_start__, 0, &__bss_end__ - &__bss_start__);
    brk_now = &__heap_start;
    __libc_init_array();
    main();
  }
  void _init(void) {}
  void _fini(void) {}
}
