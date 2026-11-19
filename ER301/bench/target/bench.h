// The harness: a few objects wired the way an ER-301 unit wires them, run one frame at a
// time the way od::Unit::process() does (updateParameters, then process, in graph order).
#pragma once
#include <od/objects/Object.h>
#include <od/config.h>
#include <vector>
#include "bench_abi.h"

extern "C" void bench_mark(int code, int arg);

namespace bench
{
  // What a chain's input would carry: programme material on four channels, plus a few
  // control signals for the sub-chains (an LFO, a gate, a trigger).
  class Source : public od::Object
  {
  public:
    Source();
    virtual ~Source();
    virtual void process();
    od::Outlet mOut1{"Out1"}, mOut2{"Out2"}, mOut3{"Out3"}, mOut4{"Out4"};
    od::Outlet mLfo{"LFO"};     // slow sine, +-0.1 (= +-1 octave on a V/Oct input)
    od::Outlet mGate{"Gate"};   // high for 0.4 s out of every 0.8 s
    od::Outlet mTrig{"Trig"};   // one sample high every second
    od::Outlet mSilence{"Silence"};
    int mPos = 0, mFrame = 0;
    int mMuteAfter = 0;         // programme outputs go silent after this many frames (0 = never)
  };

  // An empty sub-chain: what stands behind every fader and gate of a unit. The firmware runs
  // each one as an od::UnitChain whose output od::Repeater copies the chain's input (silence
  // when nothing is patched) to its outlet every frame, so the control it feeds sees a full
  // buffer, not a constant. (The chain's mutex and the scheduler's bookkeeping are SYS/BIOS
  // and are not part of the image.)
  class Branch : public od::Object
  {
  public:
    Branch();
    virtual ~Branch();
    virtual void process();
    od::Inlet mIn{"In"};
    od::Outlet mOut{"Out"};
  };

  struct Graph
  {
    int sampleRate = 48000;
    Source *in = 0;
    std::vector<od::Object *> objects;  // processing order after compile()
    std::vector<const char *> names;
    std::vector<od::Outlet *> outs;     // what the scenario sends to its "chain output"
    void (*onFrame)(Graph &, int frame) = 0;
    // "the music stops": the programme plays for playSecs, then silence for settleSecs before
    // the warm-up and the measurement (what is left in the unit decays: denormals show here)
    void silenceAfter(float playSecs, float settleSecs);
    int settleFrames = 0;
    void *user = 0;
    const char *unitName = "";

    // every object added after this is reported as "title/name"; a title used before gets
    // a number ("Delay #2")
    void unit(const char *title);
    std::vector<const char *> unitTitles;
    od::Object *add(const char *name, od::Object *o);
    // give inlet `inlet` of `to` its sub-chain; returns the chain (connect a source to its "In")
    od::Object *branch(od::Object *to, const char *inlet);
    // patch a source into the sub-chain that feeds `control`
    void drive(od::Object *control, od::Object *src, const char *outlet);
    std::vector<od::Object *> branchOf, branchChain;
    void connect(od::Object *from, const char *outlet, od::Object *to, const char *inlet);
    void tie(od::Object *follower, const char *param, od::Object *leader, const char *leaderParam);
    void tieOption(od::Object *follower, od::Object *leader, const char *option);
    void set(od::Object *o, const char *param, float value);
    void option(od::Object *o, const char *name, int choice);
    void out(od::Object *o, const char *outlet);
    od::Object *find(const char *name);   // by "unit/object" or plain object name
    void compile();
    void frame(int index);
  };

  typedef void (*BuildFn)(Graph &);
  struct Scenario
  {
    const char *name, *group;
    BuildFn build;
    Scenario *next;
    Scenario(const char *n, const char *g, BuildFn b);
  };

  // a ParameterAdapter with its bias set, tied to `param` of every op: what a fader is
  od::Object *fader(Graph &g, const char *name, std::vector<od::Object *> ops, const char *param, float value);
  // a Comparator in gate (or trigger) mode feeding `inlet` of every op: what a gate control is
  od::Object *gate(Graph &g, const char *name, std::vector<od::Object *> ops, const char *inlet, bool trigger);
  // a GainBias (audio-rate control) with its MinMax, as the stock units have: returns the GainBias
  od::Object *gainBias(Graph &g, const char *name, float bias);
}

#define BENCH_CAT2(a, b) a##b
#define BENCH_CAT(a, b) BENCH_CAT2(a, b)
#define BENCH_SCENARIO(name, group)                                     \
  static void BENCH_CAT(scn_, __LINE__)(bench::Graph &g);               \
  static bench::Scenario BENCH_CAT(reg_, __LINE__)(name, group, BENCH_CAT(scn_, __LINE__)); \
  static void BENCH_CAT(scn_, __LINE__)(bench::Graph &g)
