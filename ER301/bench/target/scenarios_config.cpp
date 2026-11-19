// Several units in one frame, the way a patch loads the module: every object of every unit
// runs back to back, so the caches and the branch predictor are shared as they would be.
// Built only from the stock:: and chloe:: unit builders, so this file does not change when a
// unit's inside does.
#include "chloe_units.h"

using namespace bench;
using stock::Stereo;

static Stereo mono(od::Object *o, const char *port) { return Stereo{o, port, o, port}; }
static Stereo src(Graph &g, const char *l, const char *r) { return Stereo{g.in, l, g.in, r}; }
static void out1(Graph &g, Stereo s) { g.out(s.l, s.lp); }

// Four mono channels, a mix of stock and Chloaelis units.
//   1: Sine Osc > Ladder LPF > Tube > PT2399 Delay
//   2: Aliasing Saw > Cell Filter (V/Oct from an LFO) > Tape Loop
//   3: input > Scale String (half the poly strings) > stock Delay
//   4: input > Freeverb > Limiter; a Lorenz and an ADSR > VCA alongside as modulation
BENCH_SCENARIO("config/four-channels-mixed", "config")
{
  Stereo a = mono(stock::sineOsc(g, 110.0f), "Out");
  a = stock::ladderLpf(g, a, false, 880.0f, 0.4f);
  a = chloe::tube(g, a, false);
  a = chloe::delay(g, a, false, chloe::kPT2399);
  out1(g, a);

  Stereo b = mono(stock::sawOsc(g, 220.0f), "Out");
  b = chloe::filter(g, b, false, 880.0f, 0.5f, 0.5f, true);
  b = chloe::delay(g, b, false, chloe::kTape);
  out1(g, b);

  Stereo c = src(g, "Out3", "Out3");
  c = chloe::string(g, c, false, 110.0f, 0.9f, 0.5f);
  c = stock::delay(g, c, false);
  out1(g, c);

  Stereo d = src(g, "Out4", "Out4");
  d = stock::freeverb(g, d, false);
  d = stock::limiter(g, d, false, 2);
  out1(g, d);

  chloe::lorenz(g, true);
  Stereo e = mono(stock::adsr(g, g.in, "Gate"), "Out");
  stock::vca(g, e, false);
}

// The same patch with stock units where the Chloaelis ones were (for scale).
BENCH_SCENARIO("config/four-channels-stock-only", "config")
{
  Stereo a = mono(stock::sineOsc(g, 110.0f), "Out");
  a = stock::ladderLpf(g, a, false, 880.0f, 0.4f);
  a = stock::limiter(g, a, false, 1);
  a = stock::delay(g, a, false);
  out1(g, a);

  Stereo b = mono(stock::sawOsc(g, 220.0f), "Out");
  b = stock::ladderLpf(g, b, false, 880.0f, 0.5f, true);
  b = stock::dopplerDelay(g, b, false, false);
  out1(g, b);

  Stereo c = src(g, "Out3", "Out3");
  c = stock::grainDelay(g, c, false);
  c = stock::microDelay(g, c, false);
  out1(g, c);

  Stereo d = src(g, "Out4", "Out4");
  d = stock::freeverb(g, d, false);
  d = stock::limiter(g, d, false, 2);
  out1(g, d);

  stock::triangleOsc(g, 0.7f);
  Stereo e = mono(stock::adsr(g, g.in, "Gate"), "Out");
  stock::vca(g, e, false);
}

// Two stereo chains.
//   1+2: input > Cell Filter > Tube > Tape Loop > Freeverb
//   3+4: Sine Osc > Scale String (half the poly strings) > PT2399 Delay > Limiter; a Lorenz
BENCH_SCENARIO("config/two-stereo-chains", "config")
{
  Stereo a = src(g, "Out1", "Out2");
  a = chloe::filter(g, a, true, 1760.0f, 0.3f, 0.0f, true);
  a = chloe::tube(g, a, true);
  a = chloe::delay(g, a, true, chloe::kTape);
  a = stock::freeverb(g, a, true);
  g.out(a.l, a.lp);
  g.out(a.r, a.rp);

  Stereo b = mono(stock::sineOsc(g, 220.0f), "Out");
  b = chloe::string(g, b, true, 110.0f, 0.9f, 0.5f);
  b = chloe::delay(g, b, true, chloe::kPT2399);
  b = stock::limiter(g, b, true, 2);
  chloe::lorenz(g, true);
}

// Every Chloaelis unit at once, stereo, at its defaults.
BENCH_SCENARIO("config/all-chloaelis-stereo", "config")
{
  Stereo a = src(g, "Out1", "Out2");
  a = chloe::filter(g, a, true);
  a = chloe::tube(g, a, true);
  a = chloe::delay(g, a, true, chloe::kPT2399);
  a = chloe::delay(g, a, true, chloe::kBBD);
  a = chloe::delay(g, a, true, chloe::kTape);
  g.out(a.l, a.lp);
  g.out(a.r, a.rp);
  Stereo b = src(g, "Out3", "Out4");
  chloe::string(g, b, true);
  chloe::lorenz(g, true);
}

// The expensive corners together: PT2399 at 30 ms, the tube at 4x, every poly string.
BENCH_SCENARIO("config/chloaelis-heavy-corners-stereo", "config")
{
  Stereo a = src(g, "Out1", "Out2");
  chloe::DelayOpts fast = chloe::kPT2399;
  fast.time = 0.03f;
  a = chloe::tube(g, a, true, 0.5f, 3, true);
  a = chloe::delay(g, a, true, fast);
  g.out(a.l, a.lp);
  g.out(a.r, a.rp);
  Stereo b = src(g, "Out3", "Out4");
  chloe::string(g, b, true, 110.0f, 1.0f, 1.0f);
}

// Stock-only stereo reference: a typical effects chain on each pair.
BENCH_SCENARIO("config/two-stereo-chains-stock-only", "config")
{
  Stereo a = src(g, "Out1", "Out2");
  a = stock::ladderLpf(g, a, true, 1760.0f, 0.3f, true);
  a = stock::eq3(g, a, true);
  a = stock::delay(g, a, true);
  a = stock::freeverb(g, a, true);
  g.out(a.l, a.lp);
  g.out(a.r, a.rp);
  Stereo b = mono(stock::sineOsc(g, 220.0f), "Out");
  b = stock::grainDelay(g, b, true);
  b = stock::dopplerDelay(g, b, true, true);
  b = stock::limiter(g, b, true, 2);
  stock::triangleOsc(g, 0.7f);
}
