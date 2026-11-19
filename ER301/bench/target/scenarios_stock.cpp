// Stock ER-301 units in the same harness. Several of them carry on-hardware profiler
// readings in the firmware source (tools/calibrate.py lists them), so they double as
// calibration points for the NEON side of the model.
#include "stock_units.h"
#include <od/objects/math/ConstantOffset.h>
#include <od/objects/math/ConstantGain.h>
#include <od/objects/math/GainBias.h>
#include <od/objects/math/Multiply.h>
#include <od/objects/measurement/MinMax.h>
#include <od/objects/mixing/CrossFade.h>
#include <od/objects/mixing/StereoCrossFade.h>
#include <od/objects/mixing/StereoPanner.h>
#include <od/objects/timing/Comparator.h>
#include <od/objects/adapters/ParameterAdapter.h>
#include <core/objects/oscillators/SineOscillator.h>
#include <core/objects/oscillators/SawtoothOscillator.h>
#include <core/objects/oscillators/TriangleOscillator.h>
#include <core/objects/env/ADSR.h>
#include <core/objects/filters/StereoLadderFilter.h>
#include <core/objects/filters/Equalizer3.h>
#include <core/objects/delays/Delay.h>
#include <core/objects/delays/MicroDelay.h>
#include <core/objects/delays/DopplerDelay.h>
#include <core/objects/granular/MonoGrainDelay.h>
#include <core/objects/freeverb/Freeverb.h>
#include <core/objects/Limiter.h>
#include <core/objects/Clipper.h>
#include <core/objects/SnapToZero.h>
#include <core/objects/pitch/VoltPerOctave.h>

using namespace bench;

namespace stock
{
  static Obj range(Graph &g, const char *name, Obj of)
  {
    Obj mm = g.add(name, new od::MinMax());
    g.connect(of, "Out", mm, "In");
    return mm;
  }
  static Obj gb(Graph &g, const char *name, const char *rangeName, float bias)
  {
    Obj o = g.add(name, new od::GainBias());
    g.set(o, "Bias", bias);
    g.branch(o, "In");
    range(g, rangeName, o);
    return o;
  }

  template <typename Osc>
  static Obj oscUnit(Graph &g, const char *title, float hz, float level, bool sine)
  {
    g.unit(title);
    Obj osc = g.add("osc", new Osc());
    Obj tune = g.add("tune", new od::ConstantOffset());
    g.branch(tune, "In");
    range(g, "tuneRange", tune);
    Obj f0 = gb(g, "f0", "f0Range", hz);
    Obj vca = g.add("vca", new od::Multiply());
    Obj lvl = gb(g, "level", "levelRange", level);
    od::Comparator *sync = new od::Comparator();
    sync->setTriggerMode();
    g.add("sync", sync);
    g.branch(sync, "In");
    g.connect(sync, "Out", osc, "Sync");
    g.connect(tune, "Out", osc, "V/Oct");
    g.connect(f0, "Out", osc, "Fundamental");
    if (sine)
    {
      Obj phase = gb(g, "phase", "phaseRange", 0.0f);
      Obj fb = gb(g, "feedback", "feedbackRange", 0.0f);
      g.connect(phase, "Out", osc, "Phase");
      g.connect(fb, "Out", osc, "Feedback");
    }
    g.connect(lvl, "Out", vca, "Left");
    g.connect(osc, "Out", vca, "Right");
    return vca;
  }
  Obj sineOsc(Graph &g, float hz, float level) { return oscUnit<od::SineOscillator>(g, "Sine Osc", hz, level, true); }
  Obj sawOsc(Graph &g, float hz, float level) { return oscUnit<od::SawtoothOscillator>(g, "Aliasing Saw", hz, level, false); }
  Obj triangleOsc(Graph &g, float hz, float level) { return oscUnit<od::TriangleOscillator>(g, "Aliasing Triangle", hz, level, false); }

  Obj adsr(Graph &g, Obj gateSrc, const char *gatePort)
  {
    g.unit("ADSR");
    od::Comparator *gate = new od::Comparator();
    gate->setGateMode();
    g.add("gate", gate);
    Obj env = g.add("adsr", new od::ADSR());
    Obj a = gb(g, "attack", "attackRange", 0.05f);
    Obj d = gb(g, "decay", "decayRange", 0.05f);
    Obj s = gb(g, "sustain", "sustainRange", 1.0f);
    Obj r = gb(g, "release", "releaseRange", 0.1f);
    g.connect(gateSrc, gatePort, gate, "In");
    g.connect(gate, "Out", env, "Gate");
    g.connect(a, "Out", env, "Attack");
    g.connect(d, "Out", env, "Decay");
    g.connect(s, "Out", env, "Sustain");
    g.connect(r, "Out", env, "Release");
    return env;
  }

  Stereo ladderLpf(Graph &g, Stereo in, bool stereo, float hz, float res, bool voctLfo)
  {
    g.unit("Ladder LPF");
    Obj f = g.add("filter", new od::StereoLadderFilter());
    g.connect(in.l, in.lp, f, "Left In");
    if (stereo)
      g.connect(in.r, in.rp, f, "Right In");
    Obj tune = g.add("tune", new od::ConstantOffset());
    g.branch(tune, "In");
    range(g, "tuneRange", tune);
    Obj f0 = gb(g, "f0", "f0Range", hz);
    Obj r = g.add("res", new od::GainBias());
    g.set(r, "Bias", res);
    g.branch(r, "In");
    Obj clip = g.add("clipper", new od::Clipper(0.0f, 1.0f));
    g.connect(tune, "Out", f, "V/Oct");
    g.connect(f0, "Out", f, "Fundamental");
    g.connect(r, "Out", clip, "In");
    g.connect(clip, "Out", f, "Resonance");
    range(g, "resRange", clip);
    if (voctLfo)
      g.drive(tune, g.in, "LFO");
    return Stereo{f, "Left Out", f, stereo ? "Right Out" : "Left Out"};
  }

  Stereo eq3(Graph &g, Stereo in, bool stereo)
  {
    g.unit("EQ3");
    Obj e1 = g.add("equalizer1", new od::Equalizer3());
    g.connect(in.l, in.lp, e1, "In");
    Obj e2 = e1;
    if (stereo)
    {
      e2 = g.add("equalizer2", new od::Equalizer3());
      g.connect(in.r, in.rp, e2, "In");
      g.tie(e2, "Low Freq", e1, "Low Freq");
      g.tie(e2, "High Freq", e1, "High Freq");
    }
    Obj lo = gb(g, "lowGain", "lowGainRange", 1.0f);
    Obj mid = gb(g, "midGain", "midGainRange", 1.0f);
    Obj hi = gb(g, "highGain", "highGainRange", 1.0f);
    for (Obj e : {e1, e2})
    {
      g.connect(lo, "Out", e, "Low Gain");
      g.connect(mid, "Out", e, "Mid Gain");
      g.connect(hi, "Out", e, "High Gain");
      if (!stereo)
        break;
    }
    return Stereo{e1, "Out", e2, "Out"};
  }

  static Obj wetDry(Graph &g, bool stereo, float wet)
  {
    Obj xf = stereo ? g.add("xfade", new od::StereoCrossFade()) : g.add("xfade", new od::CrossFade());
    Obj fader = gb(g, "fader", "faderRange", wet);
    g.connect(fader, "Out", xf, "Fade");
    return xf;
  }
  static Stereo xfOut(Obj xf, bool stereo) { return stereo ? Stereo{xf, "Left Out", xf, "Right Out"} : Stereo{xf, "Out", xf, "Out"}; }

  Stereo delay(Graph &g, Stereo in, bool stereo, float secs, float feedback, float wet)
  {
    g.unit("Delay");
    od::Delay *d = new od::Delay(stereo ? 2 : 1);
    d->allocateTimeUpTo(1.0f);
    g.add("delay", d);
    Obj sl = g.add("secsL", new od::ParameterAdapter());
    g.set(sl, "Bias", secs);
    g.tie(d, "Left Delay", sl, "Out");
    if (stereo)
    {
      Obj sr = g.add("secsR", new od::ParameterAdapter());
      g.set(sr, "Bias", secs * 1.25f);
      g.tie(d, "Right Delay", sr, "Out");
    }
    Obj xf = wetDry(g, stereo, wet);
    Obj fb = gb(g, "feedback", "feedbackRange", feedback);
    Obj snap = g.add("snap", new od::SnapToZero());
    g.branch(snap, "In");
    g.connect(snap, "Out", fb, "In");
    g.connect(fb, "Out", d, "Feedback");
    g.connect(in.l, in.lp, d, "Left In");
    if (stereo)
    {
      g.connect(in.r, in.rp, d, "Right In");
      g.connect(d, "Left Out", xf, "Left A");
      g.connect(d, "Right Out", xf, "Right A");
      g.connect(in.l, in.lp, xf, "Left B");
      g.connect(in.r, in.rp, xf, "Right B");
    }
    else
    {
      g.connect(d, "Left Out", xf, "A");
      g.connect(in.l, in.lp, xf, "B");
    }
    return xfOut(xf, stereo);
  }

  Stereo microDelay(Graph &g, Stereo in, bool stereo)
  {
    g.unit("Micro Delay");
    Obj l = g.add("delayL", new od::MicroDelay(0.1f));
    g.set(l, "Delay", 0.013f);
    g.connect(in.l, in.lp, l, "In");
    Obj r = l;
    if (stereo)
    {
      r = g.add("delayR", new od::MicroDelay(0.1f));
      g.connect(in.r, in.rp, r, "In");
      g.tie(r, "Delay", l, "Delay");
    }
    return Stereo{l, "Out", r, "Out"};
  }

  Stereo dopplerDelay(Graph &g, Stereo in, bool stereo, bool lfo)
  {
    g.unit("Doppler Delay");
    od::DopplerDelay *dl = new od::DopplerDelay(2.0f);
    g.add("delayL", dl);
    Obj secsL = gb(g, "secsL", "secsLRange", 0.33f);
    g.connect(secsL, "Out", dl, "Delay");
    if (lfo)
    {
      g.set(secsL, "Gain", 0.5f);
      g.drive(secsL, g.in, "LFO");
    }
    od::DopplerDelay *dr = 0;
    if (stereo)
    {
      dr = new od::DopplerDelay(2.0f);
      g.add("delayR", dr);
      Obj secsR = gb(g, "secsR", "secsRRange", 0.41f);
      g.connect(secsR, "Out", dr, "Delay");
    }
    Obj xf = wetDry(g, stereo, 0.5f);
    Obj fb = gb(g, "feedback", "feedbackRange", 0.4f);
    Obj snap = g.add("snap", new od::SnapToZero());
    g.branch(snap, "In");
    g.connect(snap, "Out", fb, "In");
    g.connect(fb, "Out", dl, "Feedback");
    g.connect(in.l, in.lp, dl, "In");
    if (stereo)
    {
      g.connect(fb, "Out", dr, "Feedback");
      g.connect(in.r, in.rp, dr, "In");
      g.connect(dl, "Out", xf, "Left A");
      g.connect(dr, "Out", xf, "Right A");
      g.connect(in.l, in.lp, xf, "Left B");
      g.connect(in.r, in.rp, xf, "Right B");
    }
    else
    {
      g.connect(dl, "Out", xf, "A");
      g.connect(in.l, in.lp, xf, "B");
    }
    return xfOut(xf, stereo);
  }

  Stereo grainDelay(Graph &g, Stereo in, bool stereo)
  {
    g.unit("Grain Delay");
    Obj gl = g.add("grainL", new od::MonoGrainDelay(1.0f));
    Obj gr = stereo ? g.add("grainR", new od::MonoGrainDelay(1.0f)) : 0;
    Obj xf = wetDry(g, stereo, 0.5f);
    Obj mul = g.add("multiply", new od::Multiply());
    Obj pitch = g.add("pitch", new od::VoltPerOctave());
    Obj tune = g.add("tune", new od::ConstantOffset());
    g.branch(tune, "In");
    range(g, "tuneRange", tune);
    Obj speed = gb(g, "speed", "speedRange", 1.0f);
    Obj clip = g.add("speedClipper", new od::Clipper(-100.0f, 100.0f));
    Obj dL = gb(g, "delayL", "delayLRange", 0.2f);
    Obj dLc = g.add("delayLClipper", new od::Clipper(0.0f, 1.0f));
    g.connect(tune, "Out", pitch, "In");
    g.connect(pitch, "Out", mul, "Left");
    g.connect(speed, "Out", mul, "Right");
    g.connect(mul, "Out", clip, "In");
    g.connect(clip, "Out", gl, "Speed");
    g.connect(dL, "Out", dLc, "In");
    g.connect(dLc, "Out", gl, "Delay");
    g.connect(in.l, in.lp, gl, "In");
    if (stereo)
    {
      Obj dR = gb(g, "delayR", "delayRRange", 0.27f);
      Obj dRc = g.add("delayRClipper", new od::Clipper(0.0f, 1.0f));
      g.connect(dR, "Out", dRc, "In");
      g.connect(dRc, "Out", gr, "Delay");
      g.connect(clip, "Out", gr, "Speed");
      g.connect(in.r, in.rp, gr, "In");
      g.connect(gl, "Out", xf, "Left A");
      g.connect(gr, "Out", xf, "Right A");
      g.connect(in.l, in.lp, xf, "Left B");
      g.connect(in.r, in.rp, xf, "Right B");
    }
    else
    {
      g.connect(gl, "Out", xf, "A");
      g.connect(in.l, in.lp, xf, "B");
    }
    return xfOut(xf, stereo);
  }

  Stereo freeverb(Graph &g, Stereo in, bool stereo)
  {
    g.unit("Freeverb");
    Obj verb = g.add("verb", new od::Freeverb());
    Obj xf = wetDry(g, stereo, 0.5f);
    g.connect(in.l, in.lp, verb, "Left In");
    g.connect(stereo ? in.r : in.l, stereo ? in.rp : in.lp, verb, "Right In");
    if (stereo)
    {
      g.connect(verb, "Left Out", xf, "Left A");
      g.connect(verb, "Right Out", xf, "Right A");
      g.connect(in.l, in.lp, xf, "Left B");
      g.connect(in.r, in.rp, xf, "Right B");
    }
    else
    {
      g.connect(verb, "Left Out", xf, "A");
      g.connect(in.l, in.lp, xf, "B");
    }
    bench::fader(g, "size", {verb}, "Size", 0.5f);
    bench::fader(g, "damp", {verb}, "Damp", 0.5f);
    bench::fader(g, "width", {verb}, "Width", 1.0f);
    return xfOut(xf, stereo);
  }

  Stereo limiter(Graph &g, Stereo in, bool stereo, int type)
  {
    g.unit("Limiter");
    Obj out[2] = {0, 0}, first[3] = {0, 0, 0};
    for (int ch = 0; ch < (stereo ? 2 : 1); ch++)
    {
      Obj ig = g.add(ch ? "inGain2" : "inGain1", new od::ConstantGain());
      Obj og = g.add(ch ? "outGain2" : "outGain1", new od::ConstantGain());
      Obj lim = g.add(ch ? "limiter2" : "limiter1", new od::Limiter());
      g.connect(ch ? in.r : in.l, ch ? in.rp : in.lp, ig, "In");
      g.connect(ig, "Out", lim, "In");
      g.connect(lim, "Out", og, "In");
      if (ch == 0)
      {
        g.set(ig, "Gain", 1.0f);
        g.set(og, "Gain", 1.0f);
        g.option(lim, "Type", type);
        first[0] = ig;
        first[1] = og;
        first[2] = lim;
      }
      else
      {
        g.tie(ig, "Gain", first[0], "Gain");
        g.tie(og, "Gain", first[1], "Gain");
        g.tieOption(lim, first[2], "Type");
      }
      out[ch] = og;
    }
    return Stereo{out[0], "Out", stereo ? out[1] : out[0], "Out"};
  }

  Stereo vca(Graph &g, Stereo in, bool stereo)
  {
    g.unit("Linear VCA");
    Obj level = gb(g, "level", "levelRange", 0.8f);
    Obj v1 = g.add("vca1", new od::Multiply());
    g.connect(level, "Out", v1, "Left");
    g.connect(in.l, in.lp, v1, "Right");
    if (!stereo)
      return Stereo{v1, "Out", v1, "Out"};
    Obj v2 = g.add("vca2", new od::Multiply());
    g.connect(level, "Out", v2, "Left");
    g.connect(in.r, in.rp, v2, "Right");
    Obj bal = g.add("balance", new od::StereoPanner());
    Obj pan = gb(g, "pan", "panRange", 0.0f);
    g.connect(v1, "Out", bal, "Left In");
    g.connect(v2, "Out", bal, "Right In");
    g.connect(pan, "Out", bal, "Pan");
    return Stereo{bal, "Left Out", bal, "Right Out"};
  }
}

using namespace stock;
static Stereo input(Graph &g) { return Stereo{g.in, "Out1", g.in, "Out2"}; }
static void outs(Graph &g, Stereo s, bool stereo)
{
  g.out(s.l, s.lp);
  if (stereo)
    g.out(s.r, s.rp);
}

BENCH_SCENARIO("stock/sine-osc", "stock") { g.out(sineOsc(g, 440.0f), "Out"); }
BENCH_SCENARIO("stock/saw-osc", "stock") { g.out(sawOsc(g, 440.0f), "Out"); }
BENCH_SCENARIO("stock/triangle-osc", "stock") { g.out(triangleOsc(g, 440.0f), "Out"); }
BENCH_SCENARIO("stock/adsr", "stock") { g.out(adsr(g, g.in, "Gate"), "Out"); }
BENCH_SCENARIO("stock/ladder-lpf/mono", "stock") { outs(g, ladderLpf(g, input(g), false), false); }
BENCH_SCENARIO("stock/ladder-lpf/stereo", "stock") { outs(g, ladderLpf(g, input(g), true), true); }
BENCH_SCENARIO("stock/ladder-lpf/stereo-voct-lfo", "stock") { outs(g, ladderLpf(g, input(g), true, 880.0f, 0.6f, true), true); }
BENCH_SCENARIO("stock/eq3/mono", "stock") { outs(g, eq3(g, input(g), false), false); }
BENCH_SCENARIO("stock/eq3/stereo", "stock") { outs(g, eq3(g, input(g), true), true); }
BENCH_SCENARIO("stock/delay/mono", "stock") { outs(g, delay(g, input(g), false), false); }
BENCH_SCENARIO("stock/delay/stereo", "stock") { outs(g, delay(g, input(g), true), true); }
BENCH_SCENARIO("stock/micro-delay/stereo", "stock") { outs(g, microDelay(g, input(g), true), true); }
BENCH_SCENARIO("stock/doppler-delay/mono", "stock") { outs(g, dopplerDelay(g, input(g), false, false), false); }
BENCH_SCENARIO("stock/doppler-delay/stereo-lfo", "stock") { outs(g, dopplerDelay(g, input(g), true, true), true); }
BENCH_SCENARIO("stock/grain-delay/mono", "stock") { outs(g, grainDelay(g, input(g), false), false); }
BENCH_SCENARIO("stock/grain-delay/stereo", "stock") { outs(g, grainDelay(g, input(g), true), true); }
BENCH_SCENARIO("stock/freeverb/mono", "stock") { outs(g, freeverb(g, input(g), false), false); }
BENCH_SCENARIO("stock/freeverb/stereo", "stock") { outs(g, freeverb(g, input(g), true), true); }
BENCH_SCENARIO("stock/limiter/mono-cubic", "stock") { outs(g, limiter(g, input(g), false, LIMITER_CUBIC), false); }
BENCH_SCENARIO("stock/limiter/stereo-cubic", "stock") { outs(g, limiter(g, input(g), true, LIMITER_CUBIC), true); }
BENCH_SCENARIO("stock/limiter/mono-invsqrt", "stock") { outs(g, limiter(g, input(g), false, LIMITER_INVSQRT), false); }
BENCH_SCENARIO("stock/vca/mono", "stock") { outs(g, vca(g, input(g), false), false); }
BENCH_SCENARIO("stock/vca/stereo", "stock") { outs(g, vca(g, input(g), true), true); }

// Two scalar loops that the firmware source keeps (disabled) next to their NEON replacements,
// each with its reading: the same arithmetic, written out again here. They are the
// calibration points for plain scalar float code on the module.
class ScalarTanh : public od::Object
{
public:
  ScalarTanh()
  {
    addInput(mInput);
    addOutput(mOutput);
  }
  virtual ~ScalarTanh() {}
  // Lambert's continued fraction for tanh: mods/core/objects/Limiter.cpp, "regular floating point", "55k ticks"
  virtual void process()
  {
    float *in = mInput.buffer();
    float *out = mOutput.buffer();
    const int n = FRAMELENGTH;
    for (int i = 0; i < n; i++)
    {
      float x = in[i];
      float x2 = x * x;
      float a = (((x2 + 378) * x2 + 17325) * x2 + 135135) * x;
      float b = ((28 * x2 + 3150) * x2 + 62370) * x2 + 135135;
      out[i] = a / b;
    }
  }
  od::Inlet mInput{"In"};
  od::Outlet mOutput{"Out"};
};
class ScalarClipper : public od::Object
{
public:
  ScalarClipper()
  {
    addInput(mInput);
    addOutput(mOutput);
  }
  virtual ~ScalarClipper() {}
  // mods/core/objects/Clipper.cpp, the scalar version: "9600 ticks for 128 samples"
  virtual void process()
  {
    float *in = mInput.buffer();
    float *out = mOutput.buffer();
    float *end = out + globalConfig.frameLength;
    while (out < end)
    {
      if (*in > mMaximum)
        *(out++) = mMaximum;
      else if (*in < mMinimum)
        *(out++) = mMinimum;
      else
        *(out++) = *in;
      in++;
    }
  }
  od::Inlet mInput{"In"};
  od::Outlet mOutput{"Out"};
  float mMaximum = 0.3f, mMinimum = -0.3f;
};
BENCH_SCENARIO("stock/object/scalar-tanh", "stock")
{
  g.unit("object");
  Obj c = g.add("tanh", new ScalarTanh());
  g.connect(g.in, "Out1", c, "In");
  g.out(c, "Out");
}
BENCH_SCENARIO("stock/object/scalar-clipper", "stock")
{
  g.unit("object");
  Obj c = g.add("clipper", new ScalarClipper());
  g.connect(g.in, "Out1", c, "In");
  g.out(c, "Out");
}

// single objects that carry a per-object reading in the firmware source
BENCH_SCENARIO("stock/object/clipper", "stock")
{
  g.unit("object");
  Obj c = g.add("clipper", new od::Clipper(-0.3f, 0.3f));
  g.connect(g.in, "Out1", c, "In");
  g.out(c, "Out");
}
BENCH_SCENARIO("stock/object/constant-gain", "stock")
{
  g.unit("object");
  Obj c = g.add("gain", new od::ConstantGain());
  g.set(c, "Gain", 0.7f);
  g.connect(g.in, "Out1", c, "In");
  g.out(c, "Out");
}
BENCH_SCENARIO("stock/object/crossfade", "stock")
{
  g.unit("object");
  Obj c = g.add("xfade", new od::CrossFade());
  g.connect(g.in, "Out1", c, "A");
  g.connect(g.in, "Out2", c, "B");
  g.connect(g.in, "LFO", c, "Fade");
  g.out(c, "Out");
}
