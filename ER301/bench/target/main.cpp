// Benchmark image entry: build the scenario the simulator asked for, warm it up, run it.
#include "bench.h"
#include <od/AudioThread.h>
#include <od/objects/adapters/ParameterAdapter.h>
#include <od/objects/timing/Comparator.h>
#include <od/objects/math/GainBias.h>
#include <od/objects/measurement/MinMax.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern "C"
{
  extern BenchCfg g_bench_cfg;
  extern BenchOut g_bench_out;
  extern char g_bench_list[BENCH_LIST_LEN];
}

namespace bench
{
  static Scenario *head = 0;
  Scenario::Scenario(const char *n, const char *g, BuildFn b) : name(n), group(g), build(b), next(0)
  {
    // keep registration order within a file
    Scenario **p = &head;
    while (*p)
      p = &(*p)->next;
    *p = this;
  }

  //---------------------------------------------------------------------------
  // Programme material, made once: a loop of plucked partials and noise bursts, peaking
  // near 0.5 (= +-5 V), different on each channel.
  static const int kLoop = 48000;
  static float *table[4] = {0, 0, 0, 0};
  static uint32_t lcg = 22222u;
  static inline float rnd()
  {
    lcg = lcg * 1664525u + 1013904223u;
    return (float)(lcg >> 8) * (1.0f / 8388608.0f) - 1.0f;
  }
  static void makeTables(int sr)
  {
    static const float notes[4][5] = {{110, 220, 331, 554, 1109}, {147, 293, 441, 660, 1320}, {82.4f, 165, 247, 415, 831}, {196, 392, 588, 784, 1568}};
    for (int ch = 0; ch < 4; ch++)
    {
      table[ch] = new float[kLoop];
      // five recursive oscillators (no libm in the loop), each re-plucked on its own beat
      float y1[5], y2[5], k[5], env[5];
      for (int p = 0; p < 5; p++)
      {
        const float w = 6.2831853f * notes[ch][p] / (float)sr;
        k[p] = 2.0f * cosf(w);
        y1[p] = sinf(-w);
        y2[p] = sinf(-2.0f * w);
        env[p] = 0.0f;
      }
      float burst = 0.0f, lp = 0.0f, peak = 1e-9f;
      for (int i = 0; i < kLoop; i++)
      {
        float s = 0.0f;
        for (int p = 0; p < 5; p++)
        {
          const float y = k[p] * y1[p] - y2[p];
          y2[p] = y1[p];
          y1[p] = y;
          if (i % (sr / 4 * (p + 1) / 2 + 977 * ch + 1) == 0)
            env[p] = 1.0f / (float)(p + 1);
          env[p] *= 1.0f - 3.0f / (float)sr;
          s += y * env[p];
        }
        if (i % (sr / 8 + 131 * ch) == 0)
          burst = 0.6f;
        burst *= 1.0f - 60.0f / (float)sr;
        const float nz = rnd();
        lp += 0.3f * (nz - lp);
        s += (nz - lp) * burst;
        table[ch][i] = s;
        if (fabsf(s) > peak)
          peak = fabsf(s);
      }
      for (int i = 0; i < kLoop; i++)
        table[ch][i] *= 0.5f / peak;
    }
  }

  Source::Source()
  {
    addOutput(mOut1);
    addOutput(mOut2);
    addOutput(mOut3);
    addOutput(mOut4);
    addOutput(mLfo);
    addOutput(mGate);
    addOutput(mTrig);
    addOutput(mSilence);
  }
  Source::~Source() {}
  void Source::process()
  {
    const int n = globalConfig.frameLength, sr = globalConfig.sampleRate;
    od::Outlet *o[4] = {&mOut1, &mOut2, &mOut3, &mOut4};
    for (int ch = 0; ch < 4; ch++)
    {
      float *b = o[ch]->buffer();
      if (mMuteAfter && mFrame >= mMuteAfter)
        memset(b, 0, sizeof(float) * n);
      else
        for (int i = 0; i < n; i++)
          b[i] = table[ch][(mPos + i) % kLoop];
    }
    float *lfo = mLfo.buffer(), *gt = mGate.buffer(), *tr = mTrig.buffer(), *z = mSilence.buffer();
    for (int i = 0; i < n; i++)
    {
      const int t = mFrame * n + i;
      // triangle-ish parabola LFO at 0.7 Hz
      float ph = (float)(t % (int)(sr / 0.7f)) / (sr / 0.7f);
      float x = ph < 0.5f ? ph : ph - 1.0f;
      lfo[i] = 0.1f * 8.0f * x * (1.0f - 2.0f * fabsf(x));
      gt[i] = (t % (sr * 4 / 5)) < (sr * 2 / 5) ? 1.0f : 0.0f;
      tr[i] = (t % sr) == 100 ? 1.0f : 0.0f;
      z[i] = 0.0f;
    }
    mPos = (mPos + n) % kLoop;
    mFrame++;
  }

  Branch::Branch()
  {
    addInput(mIn);
    addOutput(mOut);
  }
  Branch::~Branch() {}
  void Branch::process()
  {
    // od::Repeater::copyInputToOutput()
    memcpy(mOut.buffer(), mIn.buffer(), sizeof(float) * FRAMELENGTH);
  }

  //---------------------------------------------------------------------------
  void Graph::silenceAfter(float playSecs, float settleSecs)
  {
    const float fps = (float)sampleRate / 128.0f;
    in->mMuteAfter = (int)(playSecs * fps) + 1;
    settleFrames = in->mMuteAfter + (int)(settleSecs * fps);
  }

  void Graph::unit(const char *title)
  {
    int seen = 0;
    for (const char *t : unitTitles)
      if (strcmp(t, title) == 0)
        seen++;
    unitTitles.push_back(title);
    if (seen == 0)
    {
      unitName = title;
      return;
    }
    char *numbered = new char[BENCH_NAME_LEN];
    snprintf(numbered, BENCH_NAME_LEN, "%s #%d", title, seen + 1);
    unitName = numbered;
  }

  od::Object *Graph::add(const char *name, od::Object *o)
  {
    o->attach();
    char *full = new char[BENCH_NAME_LEN];
    snprintf(full, BENCH_NAME_LEN, "%s/%s", unitName, name);
    o->setName(full);
    objects.push_back(o);
    names.push_back(full);
    return o;
  }
  static void fail(int code);
  od::Object *Graph::branch(od::Object *to, const char *inlet)
  {
    const char *slash = strrchr(to->name().c_str(), '/');
    char nm[BENCH_NAME_LEN];
    snprintf(nm, sizeof(nm), "%s~", slash ? slash + 1 : to->name().c_str());
    od::Object *b = add(nm, new Branch());
    if (!od::connect(*b, "Out", *to, inlet))
      fail(-17);
    branchOf.push_back(to);
    branchChain.push_back(b);
    return b;
  }
  void Graph::drive(od::Object *control, od::Object *src, const char *outlet)
  {
    for (size_t i = 0; i < branchOf.size(); i++)
      if (branchOf[i] == control)
      {
        connect(src, outlet, branchChain[i], "In");
        return;
      }
    fail(-18);
  }
  static void fail(int code)
  {
    g_bench_out.status = code;
    bench_mark(BENCH_MARK_END, code);
    for (;;)
    {
    }
  }
  void Graph::connect(od::Object *from, const char *outlet, od::Object *to, const char *inlet)
  {
    if (!od::connect(*from, outlet, *to, inlet))
      fail(-10);
  }
  void Graph::tie(od::Object *follower, const char *param, od::Object *leader, const char *leaderParam)
  {
    od::Parameter *a = follower->getParameter(param), *b = leader->getParameter(leaderParam);
    if (!a || !b)
      fail(-11);
    a->tie(*b);
  }
  void Graph::tieOption(od::Object *follower, od::Object *leader, const char *option)
  {
    od::Option *a = follower->getOption(option), *b = leader->getOption(option);
    if (!a || !b)
      fail(-12);
    a->tie(*b);
  }
  void Graph::set(od::Object *o, const char *param, float value)
  {
    if (!o->hardSet(param, value))
      fail(-13);
  }
  void Graph::option(od::Object *o, const char *name, int choice)
  {
    if (!o->setOptionValue(name, choice))
      fail(-14);
  }
  void Graph::out(od::Object *o, const char *outlet)
  {
    od::Outlet *p = o->getOutput(outlet);
    if (!p)
      fail(-15);
    outs.push_back(p);
  }
  od::Object *Graph::find(const char *name)
  {
    for (size_t i = 0; i < objects.size(); i++)
    {
      if (strcmp(names[i], name) == 0)
        return objects[i];
      const char *slash = strchr(names[i], '/');
      if (slash && strcmp(slash + 1, name) == 0)
        return objects[i];
    }
    fail(-16);
    return 0;
  }

  // Order the objects so each runs after whatever feeds it (what od::GraphCompiler does).
  void Graph::compile()
  {
    const size_t n = objects.size();
    std::vector<od::Object *> order;
    std::vector<const char *> orderNames;
    std::vector<bool> done(n, false);
    auto ownerOf = [&](od::Outlet *p) -> int {
      for (size_t i = 0; i < n; i++)
        for (int k = 0; k < objects[i]->getOutputCount(); k++)
          if (objects[i]->getOutput(k) == p)
            return (int)i;
      return -1; // the source, or a constant
    };
    while (order.size() < n)
    {
      bool placed = false;
      for (size_t i = 0; i < n; i++)
      {
        if (done[i])
          continue;
        bool ready = true;
        for (int k = 0; k < objects[i]->getInputCount() && ready; k++)
        {
          od::Outlet *src = objects[i]->getInput(k)->mInwardConnection;
          if (src)
          {
            const int j = ownerOf(src);
            if (j >= 0 && j != (int)i && !done[j])
              ready = false;
          }
        }
        if (ready)
        {
          done[i] = true;
          order.push_back(objects[i]);
          orderNames.push_back(names[i]);
          placed = true;
        }
      }
      if (!placed) // a feedback loop: break it at the first object left
        for (size_t i = 0; i < n; i++)
          if (!done[i])
          {
            done[i] = true;
            order.push_back(objects[i]);
            orderNames.push_back(names[i]);
            break;
          }
    }
    objects = order;
    names = orderNames;
  }

  void Graph::frame(int index)
  {
    if (onFrame)
      onFrame(*this, index);
    in->process();
    const int n = (int)objects.size();
    for (int i = 0; i < n; i++)
    {
      od::Object *o = objects[i];
      bench_mark(BENCH_MARK_OBJ, i);
      o->updateParameters();
      o->process();
    }
    bench_mark(BENCH_MARK_OBJ, -1);
  }

  od::Object *fader(Graph &g, const char *name, std::vector<od::Object *> ops, const char *param, float value)
  {
    od::Object *a = g.add(name, new od::ParameterAdapter());
    g.set(a, "Bias", value);
    g.branch(a, "In");
    for (od::Object *op : ops)
      g.tie(op, param, a, "Out");
    return a;
  }
  od::Object *gate(Graph &g, const char *name, std::vector<od::Object *> ops, const char *inlet, bool trigger)
  {
    od::Comparator *c = new od::Comparator();
    if (trigger)
      c->setTriggerOnRiseMode();
    else
      c->setGateMode();
    g.add(name, c);
    g.branch(c, "In");
    for (od::Object *op : ops)
      g.connect(c, "Out", op, inlet);
    return c;
  }
  od::Object *gainBias(Graph &g, const char *name, float bias)
  {
    od::Object *gb = g.add(name, new od::GainBias());
    g.set(gb, "Bias", bias);
    g.branch(gb, "In");
    char range[BENCH_NAME_LEN];
    snprintf(range, sizeof(range), "%sRange", name);
    char *keep = new char[BENCH_NAME_LEN];
    strcpy(keep, range);
    od::Object *mm = g.add(keep, new od::MinMax());
    g.connect(gb, "Out", mm, "In");
    return gb;
  }
}

int main(void)
{
  using namespace bench;
  memset(&g_bench_out, 0, sizeof(g_bench_out));
  if (g_bench_cfg.scenario < 0)
  {
    int at = 0;
    for (Scenario *s = head; s; s = s->next)
      at += snprintf(g_bench_list + at, BENCH_LIST_LEN - at, "%s\t%s\n", s->name, s->group);
    g_bench_out.status = 1;
    return 0;
  }
  Scenario *s = head;
  for (int i = 0; s && i < g_bench_cfg.scenario; i++)
    s = s->next;
  if (!s)
  {
    g_bench_out.status = -1;
    return 0;
  }

  globalConfig.frameLength = 128;
  globalConfig.sampleRate = g_bench_cfg.sampleRate;
  globalConfig.frameRate = globalConfig.sampleRate / globalConfig.frameLength;
  globalConfig.samplePeriod = 1.0f / (float)globalConfig.sampleRate;
  globalConfig.framePeriod = (float)globalConfig.frameLength / (float)globalConfig.sampleRate;
  od::AudioThread::init();
  makeTables(globalConfig.sampleRate);

  Graph g;
  g.sampleRate = globalConfig.sampleRate;
  g.in = new Source();
  g.in->attach();
  s->build(g);
  g.compile();

  strncpy(g_bench_out.name, s->name, sizeof(g_bench_out.name) - 1);
  strncpy(g_bench_out.group, s->group, sizeof(g_bench_out.group) - 1);
  g_bench_out.nobj = (int)g.objects.size();
  if (g_bench_out.nobj > BENCH_MAX_OBJ)
  {
    g_bench_out.status = -5;
    return 0;
  }
  for (int i = 0; i < g_bench_out.nobj; i++)
    strncpy(g_bench_out.obj[i], g.names[i], BENCH_NAME_LEN - 1);

  bench_mark(BENCH_MARK_OBJ, -1);
  int f = 0;
  for (; f < g.settleFrames; f++)
    g.frame(f);
  for (int k = 0; k < g_bench_cfg.warmupFrames; k++, f++)
    g.frame(f);
  bench_mark(BENCH_MARK_BEGIN, 0);
  double sum[2] = {0, 0};
  float peak[2] = {0, 0};
  for (int k = 0; k < g_bench_cfg.frames; k++, f++)
  {
    g.frame(f);
    for (size_t c = 0; c < g.outs.size() && c < 2; c++)
    {
      const float *b = g.outs[c]->buffer();
      for (int i = 0; i < 128; i++)
      {
        const float a = fabsf(b[i]);
        if (!(a <= peak[c]))
          peak[c] = a; // a NaN lands here too
        sum[c] += (double)b[i] * b[i];
      }
    }
    bench_mark(BENCH_MARK_FRAME, k);
  }
  for (int c = 0; c < 2; c++)
  {
    g_bench_out.peak[c] = peak[c];
    g_bench_out.rms[c] = (float)sqrt(sum[c] / (128.0 * (g_bench_cfg.frames > 0 ? g_bench_cfg.frames : 1)));
  }
  if (g_bench_out.status == 0)
    g_bench_out.status = 1;
  bench_mark(BENCH_MARK_END, 0);
  return 0;
}
