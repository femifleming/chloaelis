#include "Engine.h"
#include "ChloeString.h"
#include <cstring>

namespace chloaelis
{
  namespace eng
  {
    static inline const float *at(const float *p, int off) { return p ? p + off : p; }
    // Nothing that is not a number may leave a unit (it would stay in every delay after it). The
    // last sample of the block is looked at, as bits; a unit that made one is started afresh.
    static inline bool broken(const float *l, const float *r, int n)
    {
      return !chl::finiteBits(l[n - 1]) || (r && !chl::finiteBits(r[n - 1]));
    }
    static inline void hush(float *l, float *r, int n)
    {
      memset(l, 0, sizeof(float) * (size_t)n);
      if (r)
        memset(r, 0, sizeof(float) * (size_t)n);
    }
    static inline float *at(float *p, int off) { return p ? p + off : p; }

    //==========================================================================
    struct Delay
    {
      int kind;
      DelayIO io;
      float sr;
      unsigned seed;
      chl::ChipDelay<chl::PTLine> *pt;
      chl::ChipDelay<chl::BBDLine> *bbd;
      chl::TapeDelay *tape;
      // the display's picture of the line: the loudness every sixteenth of the delay time
      float cells[kCells];
      int head, count;
      float timeSec;
    };

    Delay *delayNew(int kind, float sampleRate, unsigned seed)
    {
      Delay *d = new Delay();
      d->kind = kind;
      d->sr = sampleRate;
      d->seed = seed;
      d->pt = 0;
      d->bbd = 0;
      d->tape = 0;
      d->head = 0;
      d->count = 0;
      d->timeSec = 0.33f;
      for (int i = 0; i < kCells; i++)
        d->cells[i] = 0.0f;
      if (kind == 0)
      {
        d->pt = new chl::ChipDelay<chl::PTLine>();
        d->pt->prepare(sampleRate, seed);
      }
      else if (kind == 1)
      {
        d->bbd = new chl::ChipDelay<chl::BBDLine>();
        d->bbd->prepare(sampleRate, seed);
      }
      else
      {
        d->tape = new chl::TapeDelay();
        d->tape->prepare(sampleRate, seed);
      }
      return d;
    }

    void delayFree(Delay *d)
    {
      if (!d)
        return;
      delete d->pt;
      delete d->bbd;
      delete d->tape;
      delete d;
    }

    // after the sound: the guard against anything that is not a number, and the display's picture
    CHL_NOINLINE static void delayAfter(Delay *d)
    {
      const DelayIO &io = d->io;
      const int n = io.n;
      if (broken(io.outL, io.outR, n))
      {
        const float aged = delayRunAge(d);
        if (d->pt)
          d->pt->prepare(d->sr, d->seed);
        else if (d->bbd)
          d->bbd->prepare(d->sr, d->seed);
        else
        {
          d->tape->prepare(d->sr, d->seed);
          d->tape->runAge = aged;
        }
        hush(io.outL, io.outR, n);
      }
      const float env = d->pt ? d->pt->env : (d->bbd ? d->bbd->env : d->tape->env);
      d->timeSec = chl::clampf(0.03f, 1.0f, io.time);
      d->count += n;
      const int per = (int)(d->timeSec * d->sr) / kCells;
      if (d->count >= per)
      {
        d->count = 0;
        d->head = (d->head + kCells - 1) % kCells;
        d->cells[d->head] = env;
      }
    }

    DelayIO *delayIO(Delay *d) { return &d->io; }

    // One call per kind of line, kept out of line so that delayRun itself holds nothing on the
    // audio thread's small stack while the line runs.
    CHL_NOINLINE static void delayRunPT(Delay *d, int o, int m)
    {
      const DelayIO &io = d->io;
      d->pt->process(io.inL + o, at(io.inR, o), at(io.freeze, o), io.outL + o, at(io.outR, o), m, io.time, io.regen, io.wet, io.hpf, io.fade);
    }
    CHL_NOINLINE static void delayRunBBD(Delay *d, int o, int m)
    {
      const DelayIO &io = d->io;
      d->bbd->process(io.inL + o, at(io.inR, o), at(io.freeze, o), io.outL + o, at(io.outR, o), m, io.time, io.regen, io.wet, io.hpf, io.fade);
    }
    CHL_NOINLINE static void delayRunTape(Delay *d, int o, int m)
    {
      const DelayIO &io = d->io;
      d->tape->process(io.inL + o, at(io.inR, o), at(io.freeze, o), at(io.refresh, o), io.outL + o, at(io.outR, o), m, io.time, io.regen, io.wet, io.age, io.ageing, io.hpf, io.fade);
    }

    // (the DSP works on at most chl::kMaxBlock samples at a time)
    void delayRun(Delay *d)
    {
      const DelayIO &io = d->io;
      const int n = io.n;
      for (int o = 0; o < n; o += chl::kMaxBlock)
      {
        const int m = n - o < chl::kMaxBlock ? n - o : chl::kMaxBlock;
        if (d->pt)
          delayRunPT(d, o, m);
        else if (d->bbd)
          delayRunBBD(d, o, m);
        else
          delayRunTape(d, o, m);
      }
      delayAfter(d);
    }

    void delayView(Delay *d, DelayView *v)
    {
      for (int i = 0; i < kCells; i++)
      {
        v->cells[i] = d->cells[(d->head + i) % kCells];
        v->wear[i] = 0.0f;
      }
      v->timeSec = d->timeSec;
      v->age = 0.0f;
      if (d->pt)
      {
        v->level = d->pt->env;
        v->frozen = d->pt->frozenNow;
      }
      else if (d->bbd)
      {
        v->level = d->bbd->env;
        v->frozen = d->bbd->frozenNow;
      }
      else
      {
        chl::TapeLine &tp = d->tape->tp;
        v->level = d->tape->env;
        v->frozen = d->tape->frozenNow;
        v->age = tp.age;
        // the stretch of tape between the heads, newest first
        const float len = d->timeSec * d->sr;
        for (int i = 0; i < kCells; i++)
          v->wear[i] = tp.dmgAt(0, tp.wrapIndex(tp.w - (int)(len * ((float)i + 0.5f) / (float)kCells)));
      }
    }

    float delayRunAge(Delay *d) { return d->tape ? d->tape->runAge : 0.0f; }
    void delaySetRunAge(Delay *d, float a)
    {
      if (d->tape)
        d->tape->runAge = chl::clampf(0.0f, 1.0f, a);
    }

    //==========================================================================
    struct Tube
    {
      TubeIO io;
      chl::Tube tube;
      float sr;
      int factor;
    };

    Tube *tubeNew(float sampleRate)
    {
      chl::TubeTables::get(); // build the valve curve now, not in the audio thread
      Tube *t = new Tube();
      t->sr = sampleRate;
      t->factor = 2;
      t->tube.prepare(sampleRate, 2);
      return t;
    }
    void tubeFree(Tube *t) { delete t; }

    TubeIO *tubeIO(Tube *t) { return &t->io; }
    void tubeRun(Tube *t)
    {
      const TubeIO &io = t->io;
      const int factor = io.oversample >= 4 ? 4 : 2;
      if (factor != t->factor)
      {
        t->factor = factor;
        t->tube.prepare(t->sr, factor);
      }
      const int type = (io.type < 0 || io.type > 3) ? 0 : io.type, n = io.n;
      for (int o = 0; o < n; o += chl::kMaxBlock)
        t->tube.process(io.inL + o, at(io.inR, o), io.outL + o, at(io.outR, o), n - o < chl::kMaxBlock ? n - o : chl::kMaxBlock, io.drive, type, chl::kIn, chl::kOut);
      if (broken(io.outL, io.outR, n))
      {
        t->tube.prepare(t->sr, t->factor);
        hush(io.outL, io.outR, n);
      }
    }
    void tubeView(Tube *t, TubeView *v)
    {
      v->in = t->tube.envIn;
      v->out = t->tube.env;
      v->type = t->tube.stages[t->tube.cur].type;
    }

    //==========================================================================
    struct Filter
    {
      FilterIO io;
      chl::FilterCore f;
      float res, morph;
    };

    Filter *filterNew(float sampleRate)
    {
      Filter *f = new Filter();
      f->f.prepare(sampleRate);
      f->res = 0.0f;
      f->morph = 0.0f;
      return f;
    }
    void filterFree(Filter *f) { delete f; }
    FilterIO *filterIO(Filter *f) { return &f->io; }
    void filterRun(Filter *f)
    {
      const FilterIO &io = f->io;
      const int n = io.n;
      f->res = chl::clampf(0.0f, 1.0f, io.res);
      f->morph = chl::clampf(0.0f, 3.0f, io.morph);
      for (int o = 0; o < n; o += chl::kMaxBlock)
        f->f.process(io.inL + o, at(io.inR, o), at(io.voct, o), at(io.f0, o), io.outL + o, at(io.outR, o), n - o < chl::kMaxBlock ? n - o : chl::kMaxBlock, 440.0f, io.res, io.morph);
      if (broken(io.outL, io.outR, n))
      {
        f->f.prepare(f->f.sr);
        hush(io.outL, io.outR, n);
      }
    }
    void filterView(Filter *f, FilterView *v)
    {
      v->hz = f->f.hzNow;
      v->res = f->res;
      v->morph = f->morph;
      v->level = f->f.env;
    }

    //==========================================================================
    struct Lorenz
    {
      LorenzIO io;
      chl::LorenzCore l;
      float trail[2][48];
      int head, count, every;
    };

    Lorenz *lorenzNew(float sampleRate)
    {
      Lorenz *l = new Lorenz();
      l->l.prepare(sampleRate);
      l->head = 0;
      l->count = 0;
      l->every = (int)(sampleRate / 40.0f);
      for (int i = 0; i < 48; i++)
        l->trail[0][i] = l->trail[1][i] = 0.0f;
      return l;
    }
    void lorenzFree(Lorenz *l) { delete l; }
    LorenzIO *lorenzIO(Lorenz *l) { return &l->io; }
    void lorenzRun(Lorenz *l)
    {
      const LorenzIO &io = l->io;
      l->l.process(io.out1, io.out2, io.n, io.speed, io.chaos, io.shape, io.level, io.axis);
      l->count += io.n;
      if (l->count >= l->every)
      {
        l->count = 0;
        l->head = (l->head + 47) % 48;
        l->trail[0][l->head] = l->l.last[0];
        l->trail[1][l->head] = l->l.last[2];
      }
    }
    void lorenzView(Lorenz *l, LorenzView *v)
    {
      v->x = l->l.last[0];
      v->y = l->l.last[1];
      v->z = l->l.last[2];
      v->n = 48;
      for (int i = 0; i < 48; i++)
      {
        v->trail[0][i] = l->trail[0][(l->head + i) % 48];
        v->trail[1][i] = l->trail[1][(l->head + i) % 48];
      }
    }

    //==========================================================================
    struct Flock
    {
      FlockIO io;
      chl::FlockCore f;
      int boid;
    };

    Flock *flockNew(float sampleRate)
    {
      Flock *f = new Flock();
      f->f.prepare(sampleRate); // (makes the one shared flock if this is the first unit)
      f->boid = 0;
      return f;
    }
    void flockFree(Flock *f) { delete f; }
    FlockIO *flockIO(Flock *f) { return &f->io; }
    void flockRun(Flock *f)
    {
      const FlockIO &io = f->io;
      f->boid = io.boid < 0 ? 0 : (io.boid >= kBoids ? kBoids - 1 : io.boid);
      f->f.process(io.out1, io.out2, io.n, io.boid, io.level, io.axis);
    }
    void flockView(Flock *f, FlockView *v)
    {
      const chl::Flock &w = *chl::Flock::shared();
      for (int i = 0; i < kBoids; i++)
      {
        v->x[i] = w.x[i];
        v->y[i] = w.y[i];
      }
      v->boid = f->boid;
    }

    //==========================================================================
    struct String
    {
      StringIO io;
      chl::StringCore s;
    };

    String *stringNew(float sampleRate)
    {
      String *s = new String();
      s->s.prepare(sampleRate);
      return s;
    }
    void stringFree(String *s) { delete s; }
    void stringBeginScale(String *s) { s->s.beginScale(); }
    void stringAddPitch(String *s, float cents) { s->s.addPitch(cents); }
    void stringEndScale(String *s) { s->s.endScale(); }
    StringIO *stringIO(String *s) { return &s->io; }
    void stringRun(String *s)
    {
      const StringIO &io = s->io;
      const int n = io.n;
      for (int o = 0; o < n; o += chl::kMaxBlock)
        s->s.process(io.inL + o, at(io.inR, o), at(io.voct, o), at(io.freeze, o), io.outL + o, at(io.outR, o), n - o < chl::kMaxBlock ? n - o : chl::kMaxBlock,
                     io.f0, io.sustain, io.poly, io.wet, io.damp, io.hpf, io.fade);
      if (broken(io.outL, io.outR, n))
      {
        s->s.prepare(s->s.sr); // (the scale is kept)
        hush(io.outL, io.outR, n);
      }
    }
    void stringView(String *s, StringView *v)
    {
      v->hz = s->s.noteHz;
      v->frozen = s->s.frozenNow;
      v->level[0] = s->s.envPair[0];
      v->ratio[0] = 1.0f;
      for (int k = 0; k < chl::StringCore::kTaps; k++)
      {
        v->level[k + 1] = s->s.tapRunning[k] ? s->s.envPair[k + 1] * chl::StringCore::tapLevel(k, s->s.polyLast) : 0.0f;
        v->ratio[k + 1] = 1.0f / s->s.tapMul[0][k];
      }
    }
  }
}
