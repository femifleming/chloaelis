// The firmware's own units, wired as their Lua files wire them (mods/core/assets), for
// comparison and for the multi-unit configurations.
#pragma once
#include "bench.h"

namespace stock
{
  using bench::Graph;
  typedef od::Object *Obj;
  struct Stereo { Obj l; const char *lp; Obj r; const char *rp; };

  // sources (one output)
  Obj sineOsc(Graph &g, float hz, float level = 0.5f);       // Oscillators/Sine.lua
  Obj sawOsc(Graph &g, float hz, float level = 0.5f);        // Oscillators/AliasingSaw.lua
  Obj triangleOsc(Graph &g, float hz, float level = 0.5f);   // Oscillators/AliasingTriangle.lua
  Obj adsr(Graph &g, Obj gateSrc, const char *gatePort);     // Envelopes/ADSR.lua

  // effects: src/port pairs in, the unit's output object and ports out
  Stereo ladderLpf(Graph &g, Stereo in, bool stereo, float hz = 440.0f, float res = 0.25f, bool voctLfo = false); // LadderFilterUnit.lua
  Stereo eq3(Graph &g, Stereo in, bool stereo);                                       // EQ3Unit.lua
  Stereo delay(Graph &g, Stereo in, bool stereo, float secs = 0.33f, float feedback = 0.4f, float wet = 0.5f); // Delay/DelayUnit.lua
  Stereo microDelay(Graph &g, Stereo in, bool stereo);                                // Delay/MicroDelay.lua
  Stereo dopplerDelay(Graph &g, Stereo in, bool stereo, bool lfo);                    // Delay/DopplerDelayUnit.lua
  Stereo grainDelay(Graph &g, Stereo in, bool stereo);                                // Delay/GrainDelayUnit.lua
  Stereo freeverb(Graph &g, Stereo in, bool stereo);                                  // FreeverbUnit.lua
  Stereo limiter(Graph &g, Stereo in, bool stereo, int type);                         // LimiterUnit.lua
  Stereo vca(Graph &g, Stereo in, bool stereo);                                       // VCA/LinearBipolar.lua
}
