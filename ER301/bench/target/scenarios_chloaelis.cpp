// The Chloaelis units as the package wires them (mods/chloaelis/assets/*.lua): edit this
// table when a unit, a port or a parameter changes. It has two jobs: the single-unit
// scenarios (the BENCH_SCENARIO lines) and the chloe:: builders of chloe_units.h, which the
// multi-unit configurations (scenarios_config.cpp) call. The 1.0.0 wiring it started from
// is kept in scenarios_chloaelis_1.0.0.cpp for the "before" numbers.
//
// Compiled with the flags the SWIG wrapper gets in the package (-Os), because that is where
// the Units.h classes are compiled on the module.
#include "chloe_units.h"
#include "Units.h"
#include <od/objects/math/ConstantOffset.h>
#include <od/objects/math/GainBias.h>
#include <od/objects/measurement/MinMax.h>
#include <core/objects/pitch/ScaleQuantizer.h>

using namespace bench;
using stock::Stereo;
typedef std::vector<od::Object *> Ops;

static Stereo input(Graph &g) { return Stereo{g.in, "Out1", g.in, "Out2"}; }
static void outs(Graph &g, Stereo s, bool stereo)
{
  g.out(s.l, s.lp);
  if (stereo)
    g.out(s.r, s.rp);
}
// A unit is one stereo object with "Left In" / "Right In" / "Left Out" / "Right Out" (the
// package now: Kit.lua connects the right side only on a stereo chain), or one mono object
// per channel with "In" / "Out" (1.0.0). This places either, going by the ports the object
// has, so the rest of the table is the same for both.
static Stereo unitOut;
static Ops place(Graph &g, Stereo in, bool stereo, od::Object *(*make)(int ch))
{
  Ops ops;
  od::Object *first = g.add("op1", make(0));
  ops.push_back(first);
  if (first->getInput("Left In"))
  {
    g.connect(in.l, in.lp, first, "Left In");
    if (stereo)
      g.connect(in.r, in.rp, first, "Right In");
    unitOut = Stereo{first, "Left Out", first, stereo ? "Right Out" : "Left Out"};
    return ops;
  }
  g.connect(in.l, in.lp, first, "In");
  if (stereo)
  {
    od::Object *second = g.add("op2", make(1));
    g.connect(in.r, in.rp, second, "In");
    ops.push_back(second);
  }
  unitOut = Stereo{ops[0], "Out", ops.back(), "Out"};
  return ops;
}
static Stereo outOf(const Ops &) { return unitOut; }

//------------------------------------------------------------------------------
// DelayBase.lua
static const char *delayTitle[3] = {"PT2399 Delay", "BBD Delay", "Tape Loop"};
static od::Object *delayTime = 0;
static void moveTime(Graph &g, int frame)
{
  static const float times[6] = {0.33f, 0.21f, 0.48f, 0.12f, 0.75f, 0.06f};
  const int every = (int)(0.35f * g.sampleRate / 128.0f);
  if (frame % every == 0)
    g.set(delayTime, "Bias", times[(frame / every) % 6]);
}
static od::Object *makePT(int) { return new chloaelis::Delay(0); }
static od::Object *makeBBD(int) { return new chloaelis::Delay(1); }
static od::Object *makeTape(int) { return new chloaelis::Delay(2); }

Stereo chloe::delay(Graph &g, Stereo in, bool stereo, const DelayOpts &o, const char *title)
{
  g.unit(title ? title : delayTitle[o.kind]);
  Ops ops = place(g, in, stereo, o.kind == 0 ? makePT : (o.kind == 1 ? makeBBD : makeTape));
  od::Object *time = fader(g, "time", ops, "Time", o.time);
  fader(g, "regen", ops, "Regen", o.regen);
  fader(g, "wet", ops, "Wet", o.wet);
  od::Object *fz = gate(g, "freeze", ops, "Freeze", false);
  if (o.freezeGate)
    g.drive(fz, g.in, "Gate");
  for (size_t i = 1; i < ops.size(); i++)
  {
    g.tieOption(ops[i], ops[0], "Time Mode");
    g.tieOption(ops[i], ops[0], "Feedback HPF");
  }
  if (o.kind == 2)
  {
    fader(g, "age", ops, "Age", o.age);
    gate(g, "refresh", ops, "Refresh", true);
    for (size_t i = 1; i < ops.size(); i++)
      g.tieOption(ops[i], ops[0], "Ageing");
  }
  g.option(ops[0], "Time Mode", o.fade ? 2 : 1);
  g.option(ops[0], "Feedback HPF", o.hpf ? 1 : 2);
  if (o.moveTime)
  {
    delayTime = time;
    g.onFrame = moveTime;
  }
  return outOf(ops);
}

#define DELAY(name, stereo, ...)                               \
  BENCH_SCENARIO(name, "chloaelis")                            \
  {                                                            \
    chloe::DelayOpts o = {__VA_ARGS__};                        \
    outs(g, chloe::delay(g, input(g), stereo, o), stereo);     \
  }
//                                          kind time   regen wet    age   fade   hpf    freeze move
DELAY("chloe/pt2399/mono/default", false, 0, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/pt2399/stereo/default", true, 0, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/pt2399/mono/30ms", false, 0, 0.03f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/pt2399/mono/1s", false, 0, 1.0f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/pt2399/mono/regen-hpf", false, 0, 0.33f, 1.0f, 0.5f, 0.0f, false, true, false, false)
DELAY("chloe/pt2399/mono/slew-moving", false, 0, 0.33f, 0.6f, 0.5f, 0.0f, false, false, false, true)
DELAY("chloe/pt2399/mono/fade-moving", false, 0, 0.33f, 0.6f, 0.5f, 0.0f, true, false, false, true)
DELAY("chloe/pt2399/mono/freeze-gated", false, 0, 0.33f, 0.6f, 0.5f, 0.0f, false, false, true, false)
DELAY("chloe/bbd/mono/default", false, 1, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/bbd/stereo/default", true, 1, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/bbd/mono/30ms", false, 1, 0.03f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/bbd/mono/1s", false, 1, 1.0f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/bbd/mono/fade-moving", false, 1, 0.33f, 0.6f, 0.5f, 0.0f, true, false, false, true)
DELAY("chloe/bbd/mono/freeze-gated", false, 1, 0.33f, 0.6f, 0.5f, 0.0f, false, false, true, false)
DELAY("chloe/tape/mono/default", false, 2, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/tape/stereo/default", true, 2, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false)
DELAY("chloe/tape/mono/aged", false, 2, 0.33f, 0.6f, 0.5f, 0.8f, false, false, false, false)
DELAY("chloe/tape/mono/1s-hpf", false, 2, 1.0f, 0.9f, 0.5f, 0.3f, false, true, false, false)
DELAY("chloe/tape/mono/slew-moving", false, 2, 0.33f, 0.6f, 0.5f, 0.2f, false, false, false, true)
DELAY("chloe/tape/mono/fade-moving", false, 2, 0.33f, 0.6f, 0.5f, 0.2f, true, false, false, true)
DELAY("chloe/tape/mono/freeze-gated", false, 2, 0.33f, 0.6f, 0.5f, 0.5f, false, false, true, false)

//------------------------------------------------------------------------------
// Tube.lua
static od::Object *valveFader = 0;
static void cycleValve(Graph &g, int frame)
{
  const int every = (int)(0.25f * g.sampleRate / 128.0f);
  if (frame % every == 0)
    g.set(valveFader, "Bias", (float)(1 + (frame / every) % 4));
}
static od::Object *makeTube(int) { return new chloaelis::Tube(); }
Stereo chloe::tube(Graph &g, Stereo in, bool stereo, float drive, int valve, bool fourTimes, bool cycle, const char *title)
{
  g.unit(title ? title : "Tube");
  Ops ops = place(g, in, stereo, makeTube);
  fader(g, "drive", ops, "Drive", drive);
  od::Object *v = fader(g, "valve", ops, "Type", (float)valve);
  for (size_t i = 1; i < ops.size(); i++)
    g.tieOption(ops[i], ops[0], "Oversampling");
  g.option(ops[0], "Oversampling", fourTimes ? 2 : 1);
  if (cycle)
  {
    valveFader = v;
    g.onFrame = cycleValve;
  }
  return outOf(ops);
}
#define TUBE(name, stereo, drive, valve, x4, cycle) \
  BENCH_SCENARIO(name, "chloaelis") { outs(g, chloe::tube(g, input(g), stereo, drive, valve, x4, cycle), stereo); }
TUBE("chloe/tube/mono/12ax7-2x", false, 0.3f, 1, false, false)
TUBE("chloe/tube/stereo/12ax7-2x", true, 0.3f, 1, false, false)
TUBE("chloe/tube/mono/el84-2x", false, 0.3f, 2, false, false)
TUBE("chloe/tube/mono/6l6-2x", false, 0.3f, 3, false, false)
TUBE("chloe/tube/mono/starved-2x", false, 0.3f, 4, false, false)
TUBE("chloe/tube/mono/12ax7-2x-drive0.9", false, 0.9f, 1, false, false)
TUBE("chloe/tube/mono/12ax7-4x", false, 0.3f, 1, true, false)
TUBE("chloe/tube/stereo/12ax7-4x", true, 0.3f, 1, true, false)
TUBE("chloe/tube/mono/6l6-4x", false, 0.3f, 3, true, false)
TUBE("chloe/tube/mono/valve-cycling-2x", false, 0.5f, 1, false, true)

//------------------------------------------------------------------------------
// Filter.lua
static od::Object *makeFilter(int) { return new chloaelis::Filter(); }
Stereo chloe::filter(Graph &g, Stereo in, bool stereo, float hz, float res, float morph, bool voctLfo, const char *title)
{
  g.unit(title ? title : "Cell Filter");
  Ops ops = place(g, in, stereo, makeFilter);
  od::Object *tune = g.add("tune", new od::ConstantOffset());
  g.branch(tune, "In");
  od::Object *tuneRange = g.add("tuneRange", new od::MinMax());
  od::Object *f0 = g.add("f0", new od::GainBias());
  od::Object *f0Range = g.add("f0Range", new od::MinMax());
  g.set(f0, "Bias", hz);
  g.branch(f0, "In");
  g.connect(tune, "Out", tuneRange, "In");
  g.connect(f0, "Out", f0Range, "In");
  for (od::Object *op : ops)
  {
    g.connect(tune, "Out", op, "V/Oct");
    g.connect(f0, "Out", op, "Fundamental");
  }
  if (voctLfo)
    g.drive(tune, g.in, "LFO");
  fader(g, "res", ops, "Resonance", res);
  fader(g, "morph", ops, "Morph", morph);
  return outOf(ops);
}
#define FILTER(name, stereo, hz, res, morph, mod) \
  BENCH_SCENARIO(name, "chloaelis") { outs(g, chloe::filter(g, input(g), stereo, hz, res, morph, mod), stereo); }
FILTER("chloe/filter/mono/default", false, 1760.0f, 0.15f, 0.0f, false)
FILTER("chloe/filter/stereo/default", true, 1760.0f, 0.15f, 0.0f, false)
FILTER("chloe/filter/mono/voct-lfo", false, 880.0f, 0.6f, 1.5f, true)
FILTER("chloe/filter/stereo/voct-lfo", true, 880.0f, 0.6f, 1.5f, true)
FILTER("chloe/filter/mono/resonant-notch", false, 440.0f, 0.95f, 3.0f, false)

//------------------------------------------------------------------------------
// Lorenz.lua (one object whatever the channel count)
Stereo chloe::lorenz(Graph &g, bool stereo, float speed, float chaos, const char *title)
{
  g.unit(title ? title : "Lorenz");
  od::Object *op = g.add("op1", new chloaelis::Lorenz());
  Ops ops = {op};
  fader(g, "speed", ops, "Speed", speed);
  fader(g, "chaos", ops, "Chaos", chaos);
  fader(g, "shape", ops, "Shape", 8.0f / 3.0f);
  fader(g, "level", ops, "Level", 0.5f);
  if (op->getOutput("Left Out"))
    return Stereo{op, "Left Out", op, stereo ? "Right Out" : "Left Out"};
  return Stereo{op, "Out1", op, stereo ? "Out2" : "Out1"};
}
BENCH_SCENARIO("chloe/lorenz/stereo/default", "chloaelis") { outs(g, chloe::lorenz(g, true), true); }
BENCH_SCENARIO("chloe/lorenz/mono/default", "chloaelis") { outs(g, chloe::lorenz(g, false), false); }
BENCH_SCENARIO("chloe/lorenz/stereo/max-speed", "chloaelis") { outs(g, chloe::lorenz(g, true, 1.0f, 0.9f), true); }

//------------------------------------------------------------------------------
// Flock.lua (one object: boids, control rate)
Stereo chloe::flock(Graph &g, bool stereo, float boid, const char *title)
{
  g.unit(title ? title : "Flock");
  od::Object *op = g.add("op1", new chloaelis::Flock());
  Ops ops = {op};
  fader(g, "boid", ops, "Boid", boid);
  fader(g, "level", ops, "Level", 0.5f);
  return Stereo{op, "Left Out", op, stereo ? "Right Out" : "Left Out"};
}
BENCH_SCENARIO("chloe/flock/stereo/default", "chloaelis") { outs(g, chloe::flock(g, true), true); }
BENCH_SCENARIO("chloe/flock/mono/default", "chloaelis") { outs(g, chloe::flock(g, false), false); }

//------------------------------------------------------------------------------
// String.lua (12 equal steps is the scale the object starts with)
static od::Object *makeString(int) { return new chloaelis::String(); }
Stereo chloe::string(Graph &g, Stereo in, bool stereo, float f0, float sustain, float poly, bool fade, bool freezeGate, bool voctLfo, const char *title)
{
  g.unit(title ? title : "Scale String");
  Ops ops = place(g, in, stereo, makeString);
  if (ops.size() > 1)
    g.option(ops[1], "Side", 2);   // 1.0.0: the second mono object plays the other half of the chord
  od::Object *tune = g.add("tune", new od::ConstantOffset());
  g.branch(tune, "In");
  od::Object *tuneRange = g.add("tuneRange", new od::MinMax());
  g.connect(tune, "Out", tuneRange, "In");
  for (od::Object *op : ops)
    g.connect(tune, "Out", op, "V/Oct");
  if (voctLfo)
    g.drive(tune, g.in, "LFO");
  fader(g, "f0", ops, "Fundamental", f0);
  fader(g, "sustain", ops, "Sustain", sustain);
  fader(g, "poly", ops, "Poly", poly);
  fader(g, "wet", ops, "Wet", 1.0f);
  od::Object *fz = gate(g, "freeze", ops, "Freeze", false);
  if (freezeGate)
    g.drive(fz, g.in, "Gate");
  for (size_t i = 1; i < ops.size(); i++)
  {
    g.tieOption(ops[i], ops[0], "Damping");
    g.tieOption(ops[i], ops[0], "Time Mode");
    g.tieOption(ops[i], ops[0], "Feedback HPF");
  }
  g.option(ops[0], "Time Mode", fade ? 2 : 1);
  // (the ScaleQuantizer that draws the pitch circle is no longer an object of the unit, so it is
  // not processed: String.lua keeps it on the side)
  return outOf(ops);
}
#define STRING(name, stereo, f0, sus, poly, fade, fz, mod) \
  BENCH_SCENARIO(name, "chloaelis") { outs(g, chloe::string(g, input(g), stereo, f0, sus, poly, fade, fz, mod), stereo); }
STRING("chloe/string/mono/default", false, 110.0f, 0.6f, 0.0f, false, false, false)
STRING("chloe/string/stereo/default", true, 110.0f, 0.6f, 0.0f, false, false, false)
STRING("chloe/string/mono/poly-0.5", false, 110.0f, 0.9f, 0.5f, false, false, false)
STRING("chloe/string/mono/poly-1.0", false, 110.0f, 0.9f, 1.0f, false, false, false)
STRING("chloe/string/stereo/poly-1.0", true, 110.0f, 0.9f, 1.0f, false, false, false)
STRING("chloe/string/mono/poly-1.0-voct-lfo-glide", false, 220.0f, 1.0f, 1.0f, false, false, true)
STRING("chloe/string/mono/poly-1.0-voct-lfo-fade", false, 220.0f, 1.0f, 1.0f, true, false, true)
STRING("chloe/string/mono/freeze-gated", false, 110.0f, 0.9f, 0.3f, false, true, false)

//------------------------------------------------------------------------------
// The music stops: 0.2 s of programme, then silence for 3 s before the measurement, so
// whatever each unit still holds has decayed. With FPSCR = 0 a filter state that is left to
// decay passes through subnormal numbers, which the VFP coprocessor handles slowly (the
// report counts them); NEON arithmetic flushes them to zero.
#define SILENCE(name, expr)             \
  BENCH_SCENARIO(name, "chloaelis")     \
  {                                     \
    outs(g, expr, false);               \
    g.silenceAfter(0.2f, 3.0f);         \
  }
SILENCE("chloe/pt2399/mono/after-silence", chloe::delay(g, input(g), false, chloe::kPT2399))
SILENCE("chloe/bbd/mono/after-silence", chloe::delay(g, input(g), false, chloe::kBBD))
SILENCE("chloe/tape/mono/after-silence", chloe::delay(g, input(g), false, chloe::kTape))
SILENCE("chloe/tube/mono/after-silence", chloe::tube(g, input(g), false))
SILENCE("chloe/filter/mono/after-silence", chloe::filter(g, input(g), false))
SILENCE("chloe/string/mono/after-silence", chloe::string(g, input(g), false, 110.0f, 0.6f, 0.3f))
