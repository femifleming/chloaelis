// The Chloaelis units as the configurations (scenarios_config.cpp) see them: feed a signal
// in, get the unit's outputs back. Implemented by the Chloaelis scenario table
// (scenarios_chloaelis.cpp, or the 1.0.0 one), which is the only file that knows the
// package's classes, ports and parameters.
#pragma once
#include "stock_units.h"

namespace chloe
{
  using bench::Graph;
  using stock::Stereo;

  struct DelayOpts
  {
    int kind;          // 0 PT2399, 1 BBD, 2 tape
    float time, regen, wet, age;
    bool fade;         // time changes crossfade instead of gliding
    bool hpf;          // high-pass in the regeneration loop
    bool freezeGate;   // the freeze sub-chain is driven by the source's gate
    bool moveTime;     // a new time every 0.35 s (a sequenced time CV)
  };
  static const DelayOpts kPT2399 = {0, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false};
  static const DelayOpts kBBD = {1, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false};
  static const DelayOpts kTape = {2, 0.33f, 0.4f, 0.35f, 0.0f, false, false, false, false};

  // `title` names the unit in the report (give two instances different titles)
  Stereo delay(Graph &g, Stereo in, bool stereo, const DelayOpts &o, const char *title = 0);
  Stereo tube(Graph &g, Stereo in, bool stereo, float drive = 0.3f, int valve = 1, bool fourTimes = false, bool cycleValves = false, const char *title = 0);
  Stereo filter(Graph &g, Stereo in, bool stereo, float hz = 1760.0f, float res = 0.15f, float morph = 0.0f, bool voctLfo = false, const char *title = 0);
  Stereo string(Graph &g, Stereo in, bool stereo, float f0 = 110.0f, float sustain = 0.6f, float poly = 0.0f, bool fade = false,
                bool freezeGate = false, bool voctLfo = false, const char *title = 0);
  Stereo lorenz(Graph &g, bool stereo, float speed = 0.35f, float chaos = 0.3f, const char *title = 0);
  // not in 1.0.0 (only the current table defines it)
  Stereo flock(Graph &g, bool stereo, float boid = 1.0f, const char *title = 0);
}
