#pragma once
// The Chloaelis look on the ER-301's grey-scale screen: glass cells with a nucleus that glows
// with the sound, joined by tethers into a small living network, always gently moving.
//
// All inline, no .cpp (see Units.h). No C-library trig: sine comes from a parabola.
#include <od/graphics/Graphic.h>
#include <od/graphics/FrameBuffer.h>
#include "Units.h"

namespace chloaelis
{
#ifndef SWIGLUA
  namespace gfx
  {
    static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
    // sin (2 pi p), any p
    static inline float psin(float p)
    {
      p -= (float)(int)p;
      if (p < 0.0f)
        p += 1.0f;
      const float x = p < 0.5f ? p : p - 1.0f;
      const float ax = x < 0.0f ? -x : x;
      const float y = 8.0f * x * (1.0f - 2.0f * ax);
      return y * (0.775f + 0.225f * (y < 0.0f ? -y : y));
    }
    static inline float pcos(float p) { return psin(p + 0.25f); }
    // loudness (0..about 0.7) to 0..1 on a curve that shows quiet sound too
    static inline float loud(float env)
    {
      float x = clamp01(env * 2.2f);
      return x * (2.0f - x);
    }
    static inline int grey(float v)
    {
      int g = (int)(v * 15.0f + 0.5f);
      return g < 0 ? 0 : (g > 15 ? 15 : g);
    }
    // rough log2 without the C library
    static inline float log2f_(float x)
    {
      union
      {
        float f;
        uint32_t i;
      } u;
      u.f = x;
      const float e = (float)((int)((u.i >> 23) & 255) - 127);
      u.i = (u.i & 0x007fffffu) | 0x3f800000u;
      return e + (u.f - 1.0f) * (1.0f + 0.3465f * (2.0f - u.f)); // mantissa 1..2, small curve correction
    }

    // A cell: a membrane, a faint inner wall, a nucleus lit by `glow`, a glint on the glass.
    static inline void cell(od::FrameBuffer &fb, int x, int y, int r, float glow, int membrane = 9)
    {
      if (r < 1)
      {
        fb.pixel(grey(0.3f + 0.7f * glow), x, y);
        return;
      }
      if (r >= 6)
        fb.circle(GRAY3, x, y, r - 2);
      const int nr = r >= 5 ? (r * 2) / 5 + (int)(glow * (float)r * 0.25f) : (r >= 2 ? r - 1 : 0);
      if (nr > 0)
        fb.fillCircle(grey(0.2f + 0.8f * glow), x, y, nr);
      else
        fb.pixel(grey(0.25f + 0.75f * glow), x, y);
      fb.circle(membrane + (int)(glow * (float)(15 - membrane)), x, y, r);
      if (r >= 4)
        fb.pixel(WHITE, x - r / 2, y + r / 2);
    }
    static inline void tether(od::FrameBuffer &fb, int x0, int y0, int x1, int y1, int g = GRAY4)
    {
      fb.line(g, x0, y0, x1, y1);
    }
  }
#endif

  //============================================================================
  // The delays. The sound travels along a strand of sixteen cells, newest at the left:
  // PT2399 as hard square bits, the bucket brigade as round buckets on a sagging line, the
  // tape as cells round a loop, the worn stretches going hollow and dim.
  class DelayGraphic : public od::Graphic
  {
  public:
    DelayGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpDelay(0), mT(0)
    {
    }

    virtual ~DelayGraphic()
    {
      if (mpDelay)
        mpDelay->release();
    }

    void follow(Delay *p)
    {
      if (mpDelay)
        mpDelay->release();
      mpDelay = p;
      if (mpDelay)
        mpDelay->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpDelay)
        return;
      eng::DelayView v;
      mpDelay->view(&v);
      mT++;
      const int kind = mpDelay->getKind();
      const float t = (float)mT * 0.004f;
      const int L = mWorldLeft, B = mWorldBottom, W = mWidth, H = mHeight;
      const int cy = B + H / 2;
      const int n = eng::kCells;
      int px = 0, py = 0;

      if (kind == 2)
      {
        // the loop of tape: an ellipse, the record head at the left
        const float rx = (float)W * 0.5f - 9.0f, ry = (float)H * 0.5f - 9.0f;
        const int cx = L + W / 2;
        const float spin = t * (0.33f / v.timeSec) * 0.35f;
        for (int i = 0; i <= n; i++)
        {
          const int k = i % n;
          const float a = 0.5f - (float)k / (float)n - spin * 0.0f;
          const float wob = 1.0f + 0.04f * gfx::psin(t * 0.7f + (float)k * 0.21f) * (1.0f + 3.0f * v.age);
          const int x = cx + (int)(rx * wob * gfx::pcos(a));
          const int y = cy + (int)(ry * wob * gfx::psin(a));
          if (i > 0)
            gfx::tether(fb, px, py, x, y, v.wear[k] > 0.3f ? GRAY2 : GRAY5);
          px = x;
          py = y;
        }
        for (int k = 0; k < n; k++)
        {
          const float a = 0.5f - (float)k / (float)n;
          const float wob = 1.0f + 0.04f * gfx::psin(t * 0.7f + (float)k * 0.21f) * (1.0f + 3.0f * v.age);
          const int x = cx + (int)(rx * wob * gfx::pcos(a));
          const int y = cy + (int)(ry * wob * gfx::psin(a));
          const float lv = gfx::loud(v.cells[k]);
          const float worn = gfx::clamp01(v.wear[k] * 1.5f);
          const int r = 2 + (int)(lv * 4.0f);
          if (worn > 0.5f)
            fb.circle(GRAY3 + (int)(lv * 4.0f), x, y, r); // oxide gone: an empty husk
          else
            gfx::cell(fb, x, y, r, lv * (1.0f - worn), 7);
        }
        // the heads in the middle, and the tape's age as a ring that empties
        gfx::cell(fb, cx, cy, 7, gfx::loud(v.level), v.frozen ? 15 : 9);
        const int flakes = (int)(v.age * 8.0f + 0.5f);
        for (int k = 0; k < flakes; k++)
          fb.pixel(GRAY6, cx - 9 + ((k * 37 + mT / 9) % 19), cy - 12 - ((k * 11 + mT / 5) % 9));
      }
      else
      {
        const int x0 = L + 9, x1 = L + W - 4;
        for (int i = 0; i < n; i++)
        {
          const float u = (float)i / (float)(n - 1);
          const int x = x0 + 6 + (int)(u * (float)(x1 - x0 - 6));
          int y;
          if (kind == 0)
            y = cy + ((i & 1) ? 7 : -7) + (int)(2.0f * gfx::psin(t + u)); // bits, clocked: a square wave of cells
          else
            y = cy - (int)(13.0f * gfx::psin(u * 0.5f)) + 6 + (int)(3.0f * gfx::psin(t * 0.8f + u * 1.3f)); // a sagging chain
          if (i > 0)
            gfx::tether(fb, px, py, x, y, GRAY4);
          else
            gfx::tether(fb, x0, cy, x, y, GRAY6);
          px = x;
          py = y;
        }
        for (int i = 0; i < n; i++)
        {
          const float u = (float)i / (float)(n - 1);
          const int x = x0 + 6 + (int)(u * (float)(x1 - x0 - 6));
          const float lv = gfx::loud(v.cells[i]);
          if (kind == 0)
          {
            const int y = cy + ((i & 1) ? 7 : -7) + (int)(2.0f * gfx::psin(t + u));
            const int r = 1 + (int)(lv * 2.5f);
            fb.fill(gfx::grey(0.15f + 0.85f * lv), x - r, y - r, x + r, y + r);
            fb.box(v.frozen ? WHITE : GRAY9, x - r - 1, y - r - 1, x + r + 1, y + r + 1);
          }
          else
          {
            const int y = cy - (int)(13.0f * gfx::psin(u * 0.5f)) + 6 + (int)(3.0f * gfx::psin(t * 0.8f + u * 1.3f));
            gfx::cell(fb, x, y, 2 + (int)(lv * 3.0f), lv, v.frozen ? 14 : 8);
          }
        }
        // the mother cell the sound enters by
        gfx::cell(fb, x0, cy, 7, gfx::loud(v.level), v.frozen ? 15 : 10);
      }
    }
#endif

  private:
    Delay *mpDelay;
    int mT;
  };

  //============================================================================
  // The tube: one glass cell, its nucleus glowing with what comes out, cilia standing up with
  // what goes in. The small cells round it count the valve type.
  class TubeGraphic : public od::Graphic
  {
  public:
    TubeGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpTube(0), mT(0)
    {
    }

    virtual ~TubeGraphic()
    {
      if (mpTube)
        mpTube->release();
    }

    void follow(Tube *p)
    {
      if (mpTube)
        mpTube->release();
      mpTube = p;
      if (mpTube)
        mpTube->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpTube)
        return;
      eng::TubeView v;
      mpTube->view(&v);
      mT++;
      const float t = (float)mT * 0.004f;
      const int cx = mWorldLeft + mWidth / 2, cy = mWorldBottom + mHeight / 2;
      const float in = gfx::loud(v.in * 0.5f), out = gfx::loud(v.out);
      const int r = 17 + (int)(2.0f * gfx::psin(t * 0.6f));
      // cilia, no beads on the ends
      for (int k = 0; k < 24; k++)
      {
        const float a = (float)k / 24.0f + 0.01f * gfx::psin(t * 1.3f + (float)k * 0.37f);
        const float len = 2.0f + 8.0f * in * (0.6f + 0.4f * gfx::psin(t * 3.1f + (float)k * 0.19f));
        const float c = gfx::pcos(a), s = gfx::psin(a);
        fb.line(GRAY5 + (int)(in * 6.0f), cx + (int)(c * (float)(r + 1)), cy + (int)(s * (float)(r + 1)),
                cx + (int)(c * ((float)r + 1.0f + len)), cy + (int)(s * ((float)r + 1.0f + len)));
      }
      fb.fillCircle(BLACK, cx, cy, r);
      fb.circle(GRAY4, cx, cy, r - 3);
      fb.fillCircle(gfx::grey(0.15f + 0.85f * out), cx, cy, 5 + (int)(out * 6.0f));
      fb.circle(GRAY10 + (int)(out * 5.0f), cx, cy, r);
      fb.pixel(WHITE, cx - r / 2, cy + r / 2);
      fb.pixel(GRAY10, cx - r / 2 + 1, cy + r / 2 + 1);
      // satellites: one per valve type, the chosen one alive
      for (int k = 0; k < 4; k++)
      {
        const float a = 0.625f + (float)k * 0.085f + 0.005f * gfx::psin(t + (float)k);
        const int x = cx + (int)(gfx::pcos(a) * (float)(mWidth / 2 - 5));
        const int y = cy + (int)(gfx::psin(a) * 27.0f);
        gfx::tether(fb, x, y, cx + (int)(gfx::pcos(a) * (float)r), cy + (int)(gfx::psin(a) * (float)r), k == v.type ? GRAY7 : GRAY2);
        gfx::cell(fb, x, y, 3, k == v.type ? 0.4f + 0.6f * out : 0.0f, k == v.type ? 13 : 5);
      }
    }
#endif

  private:
    Tube *mpTube;
    int mT;
  };

  //============================================================================
  // The filter: its response drawn as a membrane across the screen, a cell riding it at the
  // cutoff, swelling with resonance and glowing with the sound.
  class FilterGraphic : public od::Graphic
  {
  public:
    FilterGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpFilter(0), mT(0)
    {
    }

    virtual ~FilterGraphic()
    {
      if (mpFilter)
        mpFilter->release();
    }

    void follow(Filter *p)
    {
      if (mpFilter)
        mpFilter->release();
      mpFilter = p;
      if (mpFilter)
        mpFilter->attach();
    }

#ifndef SWIGLUA
    // |H|^2 of the state variable filter at w = f / fc, for the morph position
    static float mag2(float w, float k, float morph)
    {
      const float w2 = w * w;
      const float den = (1.0f - w2) * (1.0f - w2) + k * k * w2;
      const float lp = 1.0f, bp = k * k * w2, hp = w2 * w2, no = (1.0f - w2) * (1.0f - w2);
      const float o[5] = {lp, bp, hp, no, no};
      const int s = morph >= 3.0f ? 3 : (int)morph;
      const float f = morph - (float)s;
      return (o[s] + (o[s + 1] - o[s]) * f) / (den > 1.0e-9f ? den : 1.0e-9f);
    }

    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpFilter)
        return;
      eng::FilterView v;
      mpFilter->view(&v);
      mT++;
      const float t = (float)mT * 0.004f;
      const int L = mWorldLeft, B = mWorldBottom, W = mWidth, H = mHeight;
      const float k = 2.0f - 1.96f * v.res;
      const float lv = gfx::loud(v.level);
      // 20 Hz .. 20 kHz across the width: ten octaves
      const float hz = v.hz < 20.0f ? 20.0f : (v.hz > 20000.0f ? 20000.0f : v.hz);
      const float fcOct = gfx::log2f_(hz / 20.0f);
      int px = 0, py = 0;
      const int base = B + 20;
      for (int x = 0; x < W; x += 2)
      {
        const float oct = 9.97f * (float)x / (float)(W - 1);
        // 2^(oct - fcOct) without pow: split into whole and part
        float d = oct - fcOct;
        float w = 1.0f;
        while (d >= 1.0f)
        {
          w *= 2.0f;
          d -= 1.0f;
        }
        while (d < 0.0f)
        {
          w *= 0.5f;
          d += 1.0f;
        }
        w *= 1.0f + d * (0.6931f + d * (0.2402f + d * 0.0666f));
        const float db = 3.0103f * gfx::log2f_(mag2(w, k, v.morph) + 1.0e-6f); // 10 log10
        float yv = (float)base + db * 0.55f + 1.5f * lv * gfx::psin(t * 4.0f + (float)x * 0.05f);
        int y = (int)yv;
        if (y < B + 1)
          y = B + 1;
        if (y > B + H - 2)
          y = B + H - 2;
        if (x > 0)
        {
          fb.line(GRAY11, L + px, py, L + x, y);
          fb.line(GRAY3, L + px, py - 2, L + x, y - 2);
        }
        px = x;
        py = y;
      }
      // the cell at the cutoff
      const int cx = L + (int)(fcOct / 9.97f * (float)(W - 1));
      const int r = 4 + (int)(v.res * 6.0f);
      int cyc = base + (int)(3.0103f * gfx::log2f_(mag2(1.0f, k, v.morph) + 1.0e-6f) * 0.55f);
      if (cyc > B + H - r - 2)
        cyc = B + H - r - 2;
      if (cyc < B + r + 1)
        cyc = B + r + 1;
      fb.fillCircle(BLACK, cx, cyc, r);
      gfx::cell(fb, cx, cyc, r, lv, 10);
      // four small cells along the floor: where the morph sits between LP BP HP notch
      for (int m = 0; m < 4; m++)
      {
        float near = 1.0f - (v.morph > (float)m ? v.morph - (float)m : (float)m - v.morph);
        near = gfx::clamp01(near);
        const int x = L + W / 2 - 21 + m * 14;
        gfx::cell(fb, x, B + 5, 2 + (int)(near * 2.0f), near, 5);
        if (m > 0)
          gfx::tether(fb, x - 10, B + 5, x - 4, B + 5, GRAY3);
      }
    }
#endif

  private:
    Filter *mpFilter;
    int mT;
  };

  //============================================================================
  // The attractor: a cell flying the butterfly, its wake fading behind it, the two lobes it
  // circles marked by faint cells of their own.
  class LorenzGraphic : public od::Graphic
  {
  public:
    LorenzGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpLorenz(0)
    {
    }

    virtual ~LorenzGraphic()
    {
      if (mpLorenz)
        mpLorenz->release();
    }

    void follow(Lorenz *p)
    {
      if (mpLorenz)
        mpLorenz->release();
      mpLorenz = p;
      if (mpLorenz)
        mpLorenz->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpLorenz)
        return;
      eng::LorenzView v;
      mpLorenz->view(&v);
      const int cx = mWorldLeft + mWidth / 2, cy = mWorldBottom + mHeight / 2;
      const float sx = (float)(mWidth / 2 - 6), sy = (float)(mHeight / 2 - 6);
      // the two lobes
      gfx::cell(fb, cx - (int)(sx * 0.55f), cy, 6, v.x < 0.0f ? 0.5f : 0.0f, 4);
      gfx::cell(fb, cx + (int)(sx * 0.55f), cy, 6, v.x > 0.0f ? 0.5f : 0.0f, 4);
      gfx::tether(fb, cx - (int)(sx * 0.55f) + 6, cy, cx + (int)(sx * 0.55f) - 6, cy, GRAY2);
      int px = 0, py = 0;
      for (int i = v.n - 1; i >= 0; i--)
      {
        const int x = cx + (int)(v.trail[0][i] * sx), y = cy + (int)(v.trail[1][i] * sy);
        if (i < v.n - 1)
          fb.line(1 + ((v.n - 1 - i) * 11) / v.n, px, py, x, y);
        px = x;
        py = y;
      }
      const int hx = cx + (int)(v.x * sx), hy = cy + (int)(v.z * sy);
      fb.line(GRAY12, px, py, hx, hy);
      fb.fillCircle(BLACK, hx, hy, 4);
      gfx::cell(fb, hx, hy, 4, gfx::clamp01(0.5f + 0.5f * v.y), 12);
    }
#endif

  private:
    Lorenz *mpLorenz;
  };

  //============================================================================
  // The flock: fourteen small cells in their square, tethered to the neighbours they can see.
  // The one this unit follows is the bright one.
  class FlockGraphic : public od::Graphic
  {
  public:
    FlockGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpFlock(0)
    {
    }

    virtual ~FlockGraphic()
    {
      if (mpFlock)
        mpFlock->release();
    }

    void follow(Flock *p)
    {
      if (mpFlock)
        mpFlock->release();
      mpFlock = p;
      if (mpFlock)
        mpFlock->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpFlock)
        return;
      eng::FlockView v;
      mpFlock->view(&v);
      const int cx = mWorldLeft + mWidth / 2, cy = mWorldBottom + mHeight / 2;
      const float sx = (float)(mWidth / 2 - 5), sy = (float)(mHeight / 2 - 5);
      int px[eng::kBoids], py[eng::kBoids];
      for (int i = 0; i < eng::kBoids; i++)
      {
        px[i] = cx + (int)(v.x[i] * sx);
        py[i] = cy + (int)(v.y[i] * sy);
      }
      // tethers between boids that are close
      for (int i = 0; i < eng::kBoids; i++)
        for (int j = i + 1; j < eng::kBoids; j++)
        {
          const float dx = v.x[i] - v.x[j], dy = v.y[i] - v.y[j], d2 = dx * dx + dy * dy;
          if (d2 < 0.16f)
            gfx::tether(fb, px[i], py[i], px[j], py[j], d2 < 0.05f ? GRAY5 : GRAY2);
        }
      for (int i = 0; i < eng::kBoids; i++)
        if (i != v.boid)
          gfx::cell(fb, px[i], py[i], 2, 0.25f, 6);
      fb.fillCircle(BLACK, px[v.boid], py[v.boid], 4);
      gfx::cell(fb, px[v.boid], py[v.boid], 4, 1.0f, 12);
    }
#endif

  private:
    Flock *mpFlock;
  };

  //============================================================================
  // The strings: stretched between two cells, each one shaking as hard as it sounds. The main
  // string is in the middle, the poly strings fan out round it by pitch.
  class StringGraphic : public od::Graphic
  {
  public:
    StringGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpString(0), mT(0)
    {
    }

    virtual ~StringGraphic()
    {
      if (mpString)
        mpString->release();
    }

    void follow(String *p)
    {
      if (mpString)
        mpString->release();
      mpString = p;
      if (mpString)
        mpString->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
      if (!mpString)
        return;
      eng::StringView v;
      mpString->view(&v);
      mT++;
      const float t = (float)mT * 0.02f;
      const int L = mWorldLeft, B = mWorldBottom, W = mWidth, H = mHeight;
      const int xa = L + 8, xb = L + W - 9, cy = B + H / 2;
      float total = 0.0f;
      static const int slot[8] = {0, 1, -1, 2, -2, 3, -3, 4};
      for (int s = 7; s >= 0; s--)
      {
        const float lv = gfx::loud(v.level[s] * 1.6f);
        total += lv;
        if (s > 0 && v.level[s] <= 0.0f)
          continue;
        const int y0 = cy + slot[s] * 6;
        // higher strings show more loops of the standing wave
        float loops = v.ratio[s];
        loops = loops < 1.0f ? 1.0f : (loops > 4.0f ? 4.0f : loops);
        const float amp = (s == 0 ? 7.0f : 4.0f) * lv * gfx::psin(t * (0.9f + 0.23f * (float)s));
        int px = xa, py = cy;
        for (int i = 1; i <= 16; i++)
        {
          const float u = (float)i / 16.0f;
          const int x = xa + (int)(u * (float)(xb - xa));
          // pulled together at both ends
          const float fan = 4.0f * u * (1.0f - u);
          const int y = cy + (int)((float)(y0 - cy) * fan + amp * gfx::psin(0.5f * u * (float)(int)(loops + 0.5f)));
          fb.line(s == 0 ? GRAY8 + (int)(lv * 7.0f) : GRAY4 + (int)(lv * 8.0f), px, py, x, y);
          px = x;
          py = y;
        }
      }
      total = gfx::clamp01(total * 0.4f);
      gfx::cell(fb, xa, cy, 6, total, v.frozen ? 15 : 9);
      gfx::cell(fb, xb, cy, 6, total, v.frozen ? 15 : 9);
    }
#endif

  private:
    String *mpString;
    int mT;
  };

} // namespace chloaelis
