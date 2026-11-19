// Offline checks of the Chloaelis ER-301 DSP, run on the desktop (no ER-301 SDK needed):
//   c++ -std=c++11 -O2 -I mods/chloaelis -I tests/ref tests/test.cpp -o build/chloe_test && build/chloe_test
//
// The units are checked for what they should do (echo times, filter responses, pitches, levels,
// freezes, fades) and, where the first port of the plug-in's DSP did the same job, against that
// port (tests/ref: plain scalar code, one channel at a time), which stands in for the plug-in.
#include "ChloeString.h"
#include "RefCores.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace chl;

static int failures = 0;
static void check (bool ok, const char* what, double got, double lo, double hi)
{
    printf ("  %-4s %-62s %10.4f   (want %g .. %g)\n", ok ? "ok" : "FAIL", what, got, lo, hi);
    if (! ok) ++failures;
}
static void range (const char* what, double got, double lo, double hi) { check (got >= lo && got <= hi && got == got, what, got, lo, hi); }

static int N = 128;             // the ER-301 frame
static float SR = 48000.0f;

typedef std::vector<float> Buf;

static double rms (const Buf& v, size_t from = 0)
{
    double s = 0; for (size_t i = from; i < v.size(); ++i) s += (double) v[i] * v[i];
    return sqrt (s / (double) (v.size() - from));
}
static double peak (const Buf& v, size_t from = 0) { double p = 0; for (size_t i = from; i < v.size(); ++i) { const float x = v[i]; if (! (x == x) || fabsf (x) > 1e30f) return 1e30; p = fmax (p, fabs (x)); } return p; }
// level of one frequency (plain correlation)
static double toneLevel (const Buf& v, double hz, double sr, size_t from = 0)
{
    double re = 0, im = 0; const size_t n = v.size() - from;
    for (size_t i = 0; i < n; ++i) { const double ph = 2.0 * M_PI * hz * (double) i / sr; re += v[from + i] * cos (ph); im += v[from + i] * sin (ph); }
    return 2.0 * sqrt (re * re + im * im) / (double) n;
}
static Buf sine (double hz, double amp, int n, double sr = SR)
{
    Buf v ((size_t) n); for (int i = 0; i < n; ++i) v[(size_t) i] = (float) (amp * sin (2.0 * M_PI * hz * i / sr)); return v;
}
static Buf noise (double amp, int n, unsigned seed)
{
    Rng r (seed); Buf v ((size_t) n); for (auto& x : v) x = (r.nextFloat() - 0.5f) * 2.0f * (float) amp; return v;
}
// the strongest frequency near `around`, by scanning the spectrum
static double pitchOf (const Buf& v, size_t from, double around)
{
    double best = 0, bestHz = around;
    for (double hz = around * 0.97; hz < around * 1.03; hz += around * 0.0001)
    {
        const double m = toneLevel (v, hz, SR, from);
        if (m > best) { best = m; bestHz = hz; }
    }
    return bestHz;
}
static double db (double ratio) { return 20.0 * log10 (ratio + 1e-30); }

// block by block: f (first sample of the block, n)
template <typename F> static void blocks (size_t total, F&& f) { for (size_t i = 0; i + (size_t) N <= total; i += (size_t) N) f (i, N); }
// a mono run: the right channel left unconnected
template <typename F> static Buf runMono (const Buf& in, F&& f)
{
    Buf out (in.size());
    blocks (in.size(), [&] (size_t i, int n) { f (&in[i], (const float*) nullptr, &out[i], (float*) nullptr, n, i); });
    return out;
}
struct Stereo { Buf l, r; };
template <typename F> static Stereo runStereo (const Buf& inL, const Buf& inR, F&& f)
{
    Stereo o { Buf (inL.size()), Buf (inL.size()) };
    blocks (inL.size(), [&] (size_t i, int n) { f (&inL[i], &inR[i], &o.l[i], &o.r[i], n, i); });
    return o;
}
static double maxDiff (const Buf& a, const Buf& b) { double d = 0; for (size_t i = 0; i < a.size(); ++i) d = fmax (d, fabs ((double) a[i] - b[i])); return d; }

//==============================================================================
static void testMaths()
{
    printf ("maths\n");
    double worst = 0;
    for (int i = 0; i < 1000; ++i) { const float p = (float) i / 1000.0f; worst = fmax (worst, fabs (sin01 (p) - sin (2.0 * M_PI * p))); }
    range ("worst error of sin01", worst, 0.0, 0.002);
    range ("fadeOut(0.5)^2 + fadeIn(0.5)^2", fadeOut (0.5f) * fadeOut (0.5f) + fadeIn (0.5f) * fadeIn (0.5f), 0.99, 1.01);
    worst = 0;
    for (float x = -20.0f; x < 20.0f; x += 0.0137f) worst = fmax (worst, fabs (fastExp2 (x) / exp2 ((double) x) - 1.0));
    range ("fastExp2: worst relative error", worst, 0.0, 5e-6);
    worst = 0; double worstTan = 0, worstTanh = 0, worstFine = 0, worstSin = 0, worstClip = 0;
    for (float x = -12.0f; x < 12.0f; x += 0.0171f)
    {
        float t[4];
        st (t, exp2x4 (splat (x))); worst = fmax (worst, fabs (t[0] / exp2 ((double) x) - 1.0));
        st (t, tanh4 (splat (x))); worstTanh = fmax (worstTanh, fabs (t[2] - fastTanh (x)));
        st (t, softClip4 (splat (x))); worstClip = fmax (worstClip, fabs (t[3]));
        if (x >= 0.0f) { st (t, tanhFine4 (splat (x))); worstFine = fmax (worstFine, fabs (t[1] - tanh ((double) x))); }
        if (x > 0.001f && x < 1.445f) { st (t, tan4 (splat (x))); worstTan = fmax (worstTan, fabs (t[3] / tan ((double) x) - 1.0)); }
        if (x >= 0.0f && x < 1.0f) { st (t, sin01x4 (splat (x))); worstSin = fmax (worstSin, fabs (t[0] - sin01 (x))); }
    }
    range ("exp2x4: worst relative error", worst, 0.0, 5e-6);
    range ("tan4: worst relative error up to 0.46 pi", worstTan, 0.0, 2e-5);
    range ("tanh4 against the scalar form", worstTanh, 0.0, 5e-5);
    range ("tanhFine4 against tanh", worstFine, 0.0, 2e-3);
    range ("sin01x4 against sin01", worstSin, 0.0, 1e-5);
    range ("softClip4 never passes 1", worstClip, 0.99, 1.0001);
    {
        float a[4] = { 0.5f, 2.0f, 37.0f, 1.0e-3f }, t[4];
        st (t, recip1 (ld (a))); double e1 = 0; for (int k = 0; k < 4; ++k) e1 = fmax (e1, fabs (t[k] * a[k] - 1.0));
        st (t, recip2 (ld (a))); double e2 = 0; for (int k = 0; k < 4; ++k) e2 = fmax (e2, fabs (t[k] * a[k] - 1.0));
        st (t, rsqrt1 (ld (a))); double e3 = 0; for (int k = 0; k < 4; ++k) e3 = fmax (e3, fabs (t[k] * sqrt ((double) a[k]) - 1.0));
        range ("reciprocal, one Newton step: worst error", e1, 0.0, 3e-5);
        range ("reciprocal, two steps: worst error", e2, 0.0, 1e-6);
        range ("reciprocal square root: worst error", e3, 0.0, 1e-4);
    }
    {
        // the PT2399's memory word: lanes 2,3 in, lanes {l, r, l, r} out
        uint16_t word = 0; uint32_t m[4] = { 1u, 2u, 0xffffffffu, 0u }, back[4];
        ramStore (&word, ldu (m)); stu (back, ramLoad (&word));
        range ("ram word round trip", (back[0] == 0xffffffffu && back[1] == 0u && back[2] == 0xffffffffu && back[3] == 0u) ? 1 : 0, 1, 1);
    }
}

static void testHalfBand()
{
    printf ("oversampler\n");
    float t[4];
    for (double hz : { 1000.0, 10000.0, 18000.0 })
    {
        HalfBand4 up, dn;
        auto in = sine (hz, 0.5, 48000);
        Buf out (in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            const f4 a = up.up (splat (in[i]));                 // {first, first, second, second}
            st (t, dn.down (dupLow (a), a));
            out[i] = t[0];
        }
        char name[64]; snprintf (name, sizeof name, "up then down, gain at %.0f Hz", hz);
        range (name, toneLevel (out, hz, SR, 4800) / 0.5, 0.97, 1.03);
    }
    {   // the image of a 5 kHz tone after upsampling sits at 96k - 5k = 91k... at the 96k rate that is 43 kHz
        HalfBand4 up;
        auto in = sine (5000.0, 0.5, 48000);
        Buf hi (in.size() * 2);
        for (size_t i = 0; i < in.size(); ++i) { st (t, up.up (splat (in[i]))); hi[2 * i] = t[0]; hi[2 * i + 1] = t[2]; }
        range ("upsampled tone level", toneLevel (hi, 5000.0, 96000.0, 9600) / 0.5, 0.97, 1.03);
        range ("upsampling image (dB)", db (toneLevel (hi, 43000.0, 96000.0, 9600) / 0.5), -200.0, -55.0);
    }
    {   // a 30 kHz tone at the high rate must not fold to 18 kHz
        HalfBand4 dn;
        auto hi = sine (30000.0, 0.5, 96000, 96000.0);
        Buf lo (hi.size() / 2);
        for (size_t i = 0; i < lo.size(); ++i) { st (t, dn.down (splat (hi[2 * i]), splat (hi[2 * i + 1]))); lo[i] = t[1]; }
        range ("downsampling alias (dB)", db (toneLevel (lo, 18000.0, SR, 4800) / 0.5), -200.0, -55.0);
    }
}

static void testTube()
{
    printf ("tube\n");
    const TubeTables& tb = TubeTables::get();
    double worst = 0;
    for (float v = -8.0f; v < 30.0f; v += 0.00137f)
    {
        const float vv = clampf (TubeTables::kLo, TubeTables::kHi, v);
        worst = fmax (worst, fabs (tb.atScalar (v) - TubeTables::plateExact (TubeTables::conduct (vv))));
    }
    range ("plate table worst error", worst, 0.0, 3e-5);
    {
        int32_t idx[4]; float t[4]; double w4 = 0;
        for (float v = -7.0f; v < 27.0f; v += 0.0113f) { st (t, tb.at (splat (v), idx)); w4 = fmax (w4, fabs (t[2] - tb.atScalar (v))); }
        range ("four-lane table read against the scalar read", w4, 0.0, 2e-5);
    }

    static const char* names[4] = { "12AX7", "EL84", "6L6", "starved" };
    for (int os : { 2, 4 })
        for (int type = 0; type < 4; ++type)
            for (float drive : { 0.0f, 0.3f, 1.0f })
            {
                Tube t; t.stages[0].type = t.stages[1].type = type; t.prepare (SR, os);
                chlref::Tube r; r.stages[0].type = r.stages[1].type = type; r.prepare (SR, os);
                auto in = sine (220.0, 0.25, 48000);      // +-2.5 V on the ER-301
                auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { t.process (l, rr, ol, orr, n, drive, type, kIn, kOut); });
                Buf ref (in.size());
                blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], &ref[i], n, drive, type, chlref::kIn, chlref::kOut); });
                char name[96]; snprintf (name, sizeof name, "%s %dx drive %.1f: output rms", names[type], os, drive);
                range (name, rms (out, 24000), 0.02, 0.6);
                snprintf (name, sizeof name, "%s %dx drive %.1f: level against the first port (dB)", names[type], os, drive);
                range (name, db (rms (out, 24000) / rms (ref, 24000)), -0.5, 0.5);
                if (drive >= 0.3f)
                {
                    const double h2 = toneLevel (out, 440.0, SR, 24000), h3 = toneLevel (out, 660.0, SR, 24000);
                    const double r2 = toneLevel (ref, 440.0, SR, 24000), r3 = toneLevel (ref, 660.0, SR, 24000);
                    snprintf (name, sizeof name, "%s %dx drive %.1f: 2nd + 3rd harmonic against it (dB)", names[type], os, drive);
                    range (name, db ((h2 + h3) / (r2 + r3)), -1.0, 1.0);
                }
                if (drive == 1.0f)
                {
                    snprintf (name, sizeof name, "%s %dx drive 1.0: 3rd+2nd harmonic vs fundamental", names[type], os);
                    range (name, (toneLevel (out, 440.0, SR, 24000) + toneLevel (out, 660.0, SR, 24000)) / toneLevel (out, 220.0, SR, 24000), 0.03, 3.0);
                }
            }
    {   // type change while running: no click larger than the signal, nothing non-finite
        Tube t; t.prepare (SR, 2);
        auto in = sine (220.0, 0.25, 48000);
        int block = 0;
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t) { t.process (l, r, ol, orr, n, 0.5f, (block++ / 60) % 4, kIn, kOut); });
        range ("switching types while running: peak", peak (out), 0.01, 1.5);
        double jump = 0; for (size_t i = 1; i < out.size(); ++i) jump = fmax (jump, fabs (out[i] - out[i - 1]));
        range ("switching types while running: largest step", jump, 0.0, 0.25);
    }
    {   // the drive swept by CV every block: smooth, bounded
        Tube t; t.prepare (SR, 2);
        auto in = sine (220.0, 0.25, 48000);
        int block = 0;
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t) { t.process (l, r, ol, orr, n, 0.5f + 0.5f * (float) sin (block++ * 0.05), 0, kIn, kOut); });
        range ("drive swept: peak", peak (out), 0.05, 1.0);
    }
    {   // the two channels do not hear each other, and a mono unit is the left of a stereo one
        Tube a, b, c; a.prepare (SR, 2); b.prepare (SR, 2); c.prepare (SR, 2);
        auto l = sine (220.0, 0.25, 24000), r = sine (333.0, 0.4, 24000); Buf z (l.size(), 0.0f);
        auto s1 = runStereo (l, r, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { a.process (il, ir, ol, orr, n, 0.6f, 1, kIn, kOut); });
        auto s2 = runStereo (l, z, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { b.process (il, ir, ol, orr, n, 0.6f, 1, kIn, kOut); });
        auto m = runMono (l, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { c.process (il, ir, ol, orr, n, 0.6f, 1, kIn, kOut); });
        range ("stereo: left is untouched by the right input", maxDiff (s1.l, s2.l), 0.0, 1e-7);
        range ("mono = the left of stereo", maxDiff (s1.l, m), 0.0, 1e-7);
        range ("stereo: the right channel works (rms)", rms (s1.r, 12000), 0.05, 0.6);
    }
    {   // silence in: the output reaches exactly nothing and the valve rests; sound wakes it
        Tube t; t.prepare (SR, 2);
        Buf in (48000 * 5, 0.0f);
        for (int i = 0; i < 4800; ++i) in[(size_t) i] = 0.25f * (float) sin (2.0 * M_PI * 220.0 * i / SR);
        for (int i = 192000; i < 196800; ++i) in[(size_t) i] = 0.25f * (float) sin (2.0 * M_PI * 220.0 * i / SR);
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t) { t.process (l, r, ol, orr, n, 0.5f, 0, kIn, kOut); });
        double tail = 0; for (size_t i = 168000; i < 192000; ++i) tail = fmax (tail, fabs (out[i]));
        range ("after 3 s of silence the output is exactly zero", tail, 0.0, 0.0);
        double woke = 0; for (size_t i = 192000; i < 196800; ++i) woke = fmax (woke, fabs (out[i]));
        range ("and sound wakes it again (peak)", woke, 0.05, 1.0);
    }
}

// one of the three delays behind one call
struct AnyDelay
{
    ChipDelay<PTLine>* pt = nullptr; ChipDelay<BBDLine>* bb = nullptr; TapeDelay* tp = nullptr;
    AnyDelay (int kind, unsigned seed = 1)
    {
        if (kind == 0) { pt = new ChipDelay<PTLine>(); pt->prepare (SR, seed); }
        else if (kind == 1) { bb = new ChipDelay<BBDLine>(); bb->prepare (SR, seed); }
        else { tp = new TapeDelay(); tp->prepare (SR, seed); }
    }
    ~AnyDelay() { delete pt; delete bb; delete tp; }
    void process (const float* il, const float* ir, const float* fz, const float* rf, float* ol, float* orr, int n,
                  float t, float regen, float wet, bool hpf, bool fade, float age = 0.0f, bool ageing = false)
    {
        if (pt) pt->process (il, ir, fz, ol, orr, n, t, regen, wet, hpf, fade);
        else if (bb) bb->process (il, ir, fz, ol, orr, n, t, regen, wet, hpf, fade);
        else tp->process (il, ir, fz, rf, ol, orr, n, t, regen, wet, age, ageing, hpf, fade);
    }
};
// the same for the first port (mono)
struct RefDelay
{
    chlref::ChipDelay<chlref::PT2399Line>* pt = nullptr; chlref::ChipDelay<chlref::BBDLine>* bb = nullptr; chlref::TapeDelay* tp = nullptr;
    RefDelay (int kind)
    {
        if (kind == 0) { pt = new chlref::ChipDelay<chlref::PT2399Line>(); pt->prepare (SR, 1); }
        else if (kind == 1) { bb = new chlref::ChipDelay<chlref::BBDLine>(); bb->prepare (SR, 1); }
        else { tp = new chlref::TapeDelay(); tp->prepare (SR, 1); }
    }
    ~RefDelay() { delete pt; delete bb; delete tp; }
    void process (const float* in, const float* fz, float* out, int n, float t, float regen, float wet, bool hpf, bool fade)
    {
        if (pt) pt->process (in, fz, out, n, t, regen, wet, hpf, fade);
        else if (bb) bb->process (in, fz, out, n, t, regen, wet, hpf, fade);
        else tp->process (in, fz, nullptr, out, n, t, regen, wet, 0.0f, false, hpf, fade);
    }
};
static const char* delayNames[3] = { "PT2399", "BBD", "Tape" };

// where the loudest 10 ms after the burst is, and how loud
static void findEcho (const Buf& out, double& atMs, double& level)
{
    double best = 0; size_t at = 0;
    for (size_t i = 600; i + 480 < out.size(); i += 16)
    {
        double s = 0; for (size_t k = 0; k < 480; ++k) s += (double) out[i + k] * out[i + k];
        if (s > best) { best = s; at = i; }
    }
    atMs = 1000.0 * (double) at / SR; level = sqrt (best / 480.0);
}

static void testDelays()
{
    printf ("delays\n");
    char what[128];
    for (float t : { 0.05f, 0.33f, 0.9f })
        for (int kind = 0; kind < 3; ++kind)
        {
            // a short burst, then listen for when the first echo arrives
            Buf in (96000, 0.0f);
            for (int i = 0; i < 480; ++i) in[(size_t) i] = 0.3f * (float) sin (2.0 * M_PI * 500.0 * i / SR) * (float) sin (M_PI * i / 480.0);
            AnyDelay d (kind); RefDelay r (kind);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, t, 0.0f, 1.0f, false, false); });
            Buf ref (in.size());
            blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], nullptr, &ref[i], n, t, 0.0f, 1.0f, false, false); });
            double at, lv, rat, rlv;
            findEcho (out, at, lv); findEcho (ref, rat, rlv);
            snprintf (what, sizeof what, "%s: echo of a %.0f ms delay arrives at (ms)", delayNames[kind], t * 1000.0f);
            range (what, at, t * 1000.0 * 0.93 - 4.0, t * 1000.0 * 1.07 + 6.0);
            snprintf (what, sizeof what, "%s %.0f ms: ... against the first port (ms)", delayNames[kind], t * 1000.0f);
            range (what, at - rat, -1.5, 1.5);
            snprintf (what, sizeof what, "%s %.0f ms: echo level against the burst", delayNames[kind], t * 1000.0f);
            range (what, lv / (0.3 * 0.5), 0.2, 2.0);
            snprintf (what, sizeof what, "%s %.0f ms: echo level against the first port (dB)", delayNames[kind], t * 1000.0f);
            range (what, db (lv / rlv), -1.0, 1.0);
            snprintf (what, sizeof what, "%s %.0f ms: peak", delayNames[kind], t * 1000.0f);
            range (what, peak (out), 0.0, 1.0);
        }
    // what a steady tone and noise come back as, against the first port
    for (int kind = 0; kind < 3; ++kind)
    {
        for (double hz : { 200.0, 1000.0, 3000.0 })
        {
            auto in = sine (hz, 0.25, 96000);
            AnyDelay d (kind); RefDelay r (kind);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, 0.33f, 0.0f, 1.0f, false, false); });
            Buf ref (in.size());
            blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], nullptr, &ref[i], n, 0.33f, 0.0f, 1.0f, false, false); });
            snprintf (what, sizeof what, "%s: a %.0f Hz tone through it, against the first port (dB)", delayNames[kind], hz);
            // (the tape's wow bends the tone, each tape its own way, so there the whole level is compared)
            if (kind == 2) range (what, db (rms (out, 48000) / rms (ref, 48000)), -0.6, 0.6);
            else range (what, db (toneLevel (out, hz, SR, 48000) / toneLevel (ref, hz, SR, 48000)), -0.6, 0.6);
        }
        {
            Buf in (96000, 0.0f);
            AnyDelay d (kind); RefDelay r (kind);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, 0.33f, 0.0f, 1.0f, false, false); });
            Buf ref (in.size());
            blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], nullptr, &ref[i], n, 0.33f, 0.0f, 1.0f, false, false); });
            snprintf (what, sizeof what, "%s: its noise floor with nothing going in, against the first port (dB)", delayNames[kind]);
            range (what, db (rms (out, 48000) / rms (ref, 48000)), -3.0, 3.0);
        }
    }
    // regeneration right up: it must self-oscillate without running away
    {
        auto in = sine (300.0, 0.25, 48000 * 6);
        for (size_t i = 24000; i < in.size(); ++i) in[i] = 0.0f;
        for (int kind = 0; kind < 3; ++kind)
        {
            AnyDelay d (kind);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, 0.2f, 1.15f, 1.0f, kind == 1, false, 0.0f, true); });
            snprintf (what, sizeof what, "%s full regen: peak", delayNames[kind]);
            range (what, peak (out), 0.01, 1.0);
            snprintf (what, sizeof what, "%s full regen: still sounding after 5 s", delayNames[kind]);
            range (what, rms (out, 48000 * 5), 0.005, 1.0);
        }
    }
    // freeze holds the loop with the input gone
    {
        auto in = sine (300.0, 0.25, 48000 * 4);
        for (size_t i = 48000; i < in.size(); ++i) in[i] = 0.0f;
        Buf gate (in.size(), 0.0f);
        for (size_t i = 40000; i < gate.size(); ++i) gate[i] = 1.0f;
        for (int kind = 0; kind < 3; ++kind)
        {
            AnyDelay d (kind);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { d.process (l, rr, &gate[i], nullptr, ol, orr, n, 0.3f, 0.3f, 1.0f, false, false); });
            snprintf (what, sizeof what, "%s frozen: level 3 s later", delayNames[kind]);
            range (what, rms (out, 48000 * 3 + 24000), kind == 1 ? 0.01 : 0.02, 1.0);
        }
    }
    // fade mode: change the time and make sure it gets there without blowing up
    for (int kind = 0; kind < 3; ++kind)
    {
        auto in = sine (300.0, 0.25, 48000 * 4);
        int block = 0;
        AnyDelay d (kind);
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, (block++ < 400) ? 0.2f : 0.5f, 0.5f, 1.0f, false, true); });
        snprintf (what, sizeof what, "%s fade mode time change: peak", delayNames[kind]);
        range (what, peak (out), 0.01, 1.0);
        double jump = 0; for (size_t i = 1; i < out.size(); ++i) jump = fmax (jump, fabs (out[i] - out[i - 1]));
        snprintf (what, sizeof what, "%s fade mode time change: largest step", delayNames[kind]);
        range (what, jump, 0.0, 0.12);
        if (kind == 0) range ("PT2399 fade mode: second chip took over (line time)", d.pt->lineTime[d.pt->act], 0.49, 0.51);
        if (kind == 2) range ("Tape fade mode: the head arrived (ms)", d.tp->tp.dA / SR * 1000.0, 499.0, 501.0);
    }
    // slew mode: the time glides, the echoes bend, nothing blows up
    for (int kind = 0; kind < 3; ++kind)
    {
        auto in = sine (300.0, 0.25, 48000 * 3);
        int block = 0;
        AnyDelay d (kind);
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, 0.2f + 0.15f * (float) sin (block++ * 0.02), 0.5f, 1.0f, false, false); });
        snprintf (what, sizeof what, "%s time swept by CV: peak", delayNames[kind]);
        range (what, peak (out), 0.01, 1.0);
    }
    // the two channels: independent, and each has its own noise
    for (int kind = 0; kind < 3; ++kind)
    {
        auto l = sine (300.0, 0.25, 48000), r = sine (470.0, 0.25, 48000); Buf z (l.size(), 0.0f);
        AnyDelay a (kind), b (kind), c (kind);
        auto s1 = runStereo (l, r, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { a.process (il, ir, nullptr, nullptr, ol, orr, n, 0.1f, 0.5f, 1.0f, false, false); });
        auto s2 = runStereo (l, z, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { b.process (il, ir, nullptr, nullptr, ol, orr, n, 0.1f, 0.5f, 1.0f, false, false); });
        auto m = runMono (l, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t) { c.process (il, ir, nullptr, nullptr, ol, orr, n, 0.1f, 0.5f, 1.0f, false, false); });
        snprintf (what, sizeof what, "%s stereo: left is untouched by the right input", delayNames[kind]);
        range (what, maxDiff (s1.l, s2.l), 0.0, 1e-7);
        snprintf (what, sizeof what, "%s: mono = the left of stereo", delayNames[kind]);
        range (what, maxDiff (s1.l, m), 0.0, 1e-7);
        snprintf (what, sizeof what, "%s stereo: the right echoes its own input (470 Hz, dB against left's 300 Hz)", delayNames[kind]);
        range (what, db (toneLevel (s1.r, 470.0, SR, 24000) / toneLevel (s1.l, 300.0, SR, 24000)), -2.0, 2.0);
        snprintf (what, sizeof what, "%s stereo: nothing of the left's tone in the right (dB)", delayNames[kind]);
        range (what, db (toneLevel (s1.r, 300.0, SR, 24000) / toneLevel (s1.l, 300.0, SR, 24000)), -200.0, -40.0);
    }
    // an old tape is duller and quieter than a new one
    {
        auto in = sine (4000.0, 0.25, 48000 * 3);
        double lv[2];
        for (int k = 0; k < 2; ++k)
        {
            TapeDelay d; d.prepare (SR, 5);
            auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t) { d.process (l, rr, nullptr, nullptr, ol, orr, n, 0.2f, 0.0f, 1.0f, k == 0 ? 0.0f : 1.0f, false, false, false); });
            lv[k] = toneLevel (out, 4000.0, SR, 48000);
        }
        range ("Tape: worn / new level at 4 kHz", lv[1] / lv[0], 0.02, 0.8);
        TapeDelay d; d.prepare (SR, 5);
        Buf rf (in.size(), 0.0f); for (size_t i = 48000 * 2; i < 48000 * 2 + 200; ++i) rf[i] = 1.0f;
        runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { d.process (l, rr, nullptr, &rf[i], ol, orr, n, 0.2f, 0.0f, 1.0f, 0.0f, true, false, false); });
        range ("Tape: age 1 s after a refresh", d.age(), 0.0, 0.003);
    }
    {   // a worn tape, frozen for a long time, with flakes and crackle: bounded, and it decays
        TapeDelay d; d.prepare (SR, 9);
        auto in = sine (300.0, 0.25, 48000 * 8);
        Buf gate (in.size(), 0.0f); for (size_t i = 48000; i < gate.size(); ++i) gate[i] = 1.0f;
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { d.process (l, rr, &gate[i], nullptr, ol, orr, n, 0.4f, 0.5f, 1.0f, 0.9f, true, true, false); });
        range ("worn tape held 7 s: peak", peak (out), 0.001, 1.0);
        range ("worn tape held: it is wearing away (last second against the second, dB)", db (rms (out, 48000 * 7) / (rms (Buf (out.begin() + 48000, out.begin() + 96000)) + 1e-9)), -200.0, -0.1);
    }
}

static void testFilter()
{
    printf ("filter\n");
    char what[128];
    struct Case { float morph; double hz; double lo, hi; const char* name; };
    const Case cases[] = {
        { 0.0f, 100.0, 0.85, 1.15, "LP at 1 kHz passes 100 Hz" },
        { 0.0f, 8000.0, 0.0, 0.05, "LP at 1 kHz stops 8 kHz" },
        { 2.0f, 100.0, 0.0, 0.05, "HP at 1 kHz stops 100 Hz" },
        { 2.0f, 8000.0, 0.85, 1.15, "HP at 1 kHz passes 8 kHz" },
        { 1.0f, 1000.0, 0.7, 1.3, "BP at 1 kHz passes 1 kHz" },
        { 3.0f, 1000.0, 0.0, 0.12, "notch at 1 kHz stops 1 kHz" },
        { 0.5f, 700.0, 0.3, 1.5, "half way LP > BP at 700 Hz" },
        { 2.5f, 3000.0, 0.5, 1.5, "half way HP > notch at 3 kHz" },
    };
    for (const auto& c : cases)
    {
        FilterCore f; f.prepare (SR);
        chlref::FilterCore r; r.prepare (SR);
        auto in = sine (c.hz, 0.1, 48000);
        Buf f0 (in.size(), 1000.0f), vo (in.size(), 0.0f), ref (in.size());
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { f.process (l, rr, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.15f, c.morph); });
        blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], &vo[i], &f0[i], &ref[i], n, 440.0f, 0.15f, c.morph); });
        range (c.name, toneLevel (out, c.hz, SR, 24000) / 0.1, c.lo, c.hi);
        snprintf (what, sizeof what, "   ... the whole output against the first port (worst difference)");
        range (what, maxDiff (Buf (out.begin() + 4800, out.end()), Buf (ref.begin() + 4800, ref.end())), 0.0, 2e-4);
    }
    {   // one octave of V/Oct (0.1 on the ER-301) doubles the cutoff: a 2 kHz tone through BP
        FilterCore f; f.prepare (SR);
        auto in = sine (2000.0, 0.1, 48000);
        Buf f0 (in.size(), 1000.0f), vo (in.size(), 0.1f);
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { f.process (l, rr, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.6f, 1.0f); });
        range ("BP at 1 kHz + 1 octave passes 2 kHz", toneLevel (out, 2000.0, SR, 24000) / 0.1, 0.7, 1.3);
    }
    {   // full resonance, swept at audio rate: it may ring but must stay bounded, and track the first port
        FilterCore f; f.prepare (SR);
        chlref::FilterCore r; r.prepare (SR);
        Buf in (96000), f0 (96000), vo (96000, 0.0f), ref (96000);
        Rng rn (5);
        for (size_t i = 0; i < in.size(); ++i) { in[i] = (rn.nextFloat() - 0.5f) * 0.5f; f0[i] = 30.0f * powf (500.0f, (float) i / 96000.0f); }
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { f.process (l, rr, &vo[i], &f0[i], ol, orr, n, 440.0f, 1.0f, 0.0f); });
        blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], &vo[i], &f0[i], &ref[i], n, 440.0f, 1.0f, 0.0f); });
        range ("full resonance sweep: peak", peak (out), 0.01, 3.0);
        range ("full resonance sweep: level against the first port (dB)", db (rms (out) / rms (ref)), -0.5, 0.5);
    }
    {   // resonance and mode swept by CV every block
        FilterCore f; f.prepare (SR);
        auto in = noise (0.3, 96000, 9);
        Buf f0 (in.size(), 800.0f), vo (in.size(), 0.0f);
        int block = 0;
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i)
                            { f.process (l, rr, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.5f + 0.5f * (float) sin (block * 0.03), 1.5f + 1.5f * (float) sin (block * 0.021)); ++block; });
        range ("resonance and mode swept: peak", peak (out), 0.01, 3.0);
    }
    {   // stereo
        FilterCore a, b, c; a.prepare (SR); b.prepare (SR); c.prepare (SR);
        auto l = noise (0.3, 24000, 3), r = noise (0.3, 24000, 4); Buf z (l.size(), 0.0f);
        Buf f0 (l.size(), 600.0f), vo (l.size(), 0.0f);
        auto s1 = runStereo (l, r, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t i) { a.process (il, ir, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.7f, 1.0f); });
        auto s2 = runStereo (l, z, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t i) { b.process (il, ir, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.7f, 1.0f); });
        auto s3 = runStereo (r, l, [&] (const float* il, const float* ir, float* ol, float* orr, int n, size_t i) { c.process (il, ir, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.7f, 1.0f); });
        range ("stereo: left is untouched by the right input", maxDiff (s1.l, s2.l), 0.0, 1e-7);
        range ("stereo: the right is the same filter as the left", maxDiff (s1.r, s3.l), 0.0, 1e-7);
    }
    {   // rings out to exactly nothing, then rests
        FilterCore f; f.prepare (SR);
        Buf in (48000, 0.0f); for (int i = 0; i < 2000; ++i) in[(size_t) i] = 0.2f;
        Buf f0 (in.size(), 500.0f), vo (in.size(), 0.0f);
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { f.process (l, rr, &vo[i], &f0[i], ol, orr, n, 440.0f, 0.9f, 0.0f); });
        double tail = 0; for (size_t i = 43200; i < 48000; ++i) tail = fmax (tail, fabs (out[i]));
        range ("after silence the output is exactly zero", tail, 0.0, 0.0);
    }
}

static void testLorenz()
{
    printf ("lorenz and flock\n");
    LorenzCore l; l.prepare (SR);
    Buf x (48000 * 20), y (x.size());
    for (size_t i = 0; i + N <= x.size(); i += N) l.process (&x[i], &y[i], N, 0.7f, 0.3f, 8.0f / 3.0f, 0.5f, 0);
    range ("x peak at level 0.5", peak (x), 0.2, 0.5001);
    range ("y peak at level 0.5", peak (y), 0.2, 0.5001);
    int crossings = 0; for (size_t i = 1; i < x.size(); ++i) if ((x[i] > 0) != (x[i - 1] > 0)) ++crossings;
    range ("x changes lobe in 20 s (zero crossings)", crossings, 4, 100000);
    double step = 0; for (size_t i = 1; i < x.size(); ++i) step = fmax (step, fabs (x[i] - x[i - 1]));
    range ("largest sample-to-sample step", step, 0.0, 0.01);

    // the flock: two units follow two boids of the one flock
    FlockCore a, b, c; a.prepare (SR); b.prepare (SR); c.prepare (SR);
    Buf ax (48000 * 30), ay (ax.size()), bx (ax.size()), by (ax.size()), cx (ax.size());
    for (size_t i = 0; i + N <= ax.size(); i += N)
    {
        a.process (&ax[i], &ay[i], N, 0, 1.0f, 0);
        b.process (&bx[i], &by[i], N, 5, 1.0f, 0);
        c.process (&cx[i], nullptr, N, 0, 1.0f, 0);         // a third unit on the first one's boid
    }
    range ("flock: a boid stays in its square (peak)", fmax (peak (ax), peak (ay)), 0.2, 1.0001);
    double fstep = 0; for (size_t i = 1; i < ax.size(); ++i) fstep = fmax (fstep, fabs (ax[i] - ax[i - 1]));
    range ("flock: largest sample-to-sample step", fstep, 0.0, 0.0005);
    double travelled = 0; for (size_t i = 4800; i < ax.size(); i += 4800) travelled += fabs (ax[i] - ax[i - 4800]) + fabs (ay[i] - ay[i - 4800]);
    range ("flock: the boid keeps moving (distance in 30 s)", travelled, 2.0, 30.0);
    range ("flock: two units on one boid agree", maxDiff (ax, cx), 0.0, 0.01);
    range ("flock: two boids are in different places (worst gap)", maxDiff (ax, bx), 0.05, 2.0);
    const Flock& f = *Flock::shared();
    double fast = 0, slow = 1e9;
    for (int i = 0; i < Flock::n; ++i) { const double sp = sqrt (f.vx[i] * f.vx[i] + f.vy[i] * f.vy[i]); fast = fmax (fast, sp); slow = fmin (slow, sp); }
    range ("flock: fastest boid", fast, 0.14, 0.4201);
    range ("flock: slowest boid", slow, 0.1399, 0.42);
    // the same flock stepped the plug-in's way (plain loops), from the same start: the same motion
    {
        Flock v; v.reset (SR);
        float rx[14], ry[14], rvx[14], rvy[14];
        for (int i = 0; i < 14; ++i) { rx[i] = v.x[i]; ry[i] = v.y[i]; rvx[i] = v.vx[i]; rvy[i] = v.vy[i]; }
        const float dt = 512.0f / SR;
        double worst = 0;
        for (int s = 0; s < 200; ++s)
        {
            v.step (dt);
            const float view = 0.6f, sep = 0.22f, maxV = 0.42f, minV = 0.14f;
            float axx[14], ayy[14];
            for (int i = 0; i < 14; ++i)
            {
                float cxx = 0, cyy = 0, avx = 0, avy = 0, sx = 0, sy = 0; int cnt = 0;
                for (int j = 0; j < 14; ++j)
                {
                    if (j == i) continue;
                    const float dx = rx[j] - rx[i], dy = ry[j] - ry[i], d2 = dx * dx + dy * dy;
                    if (d2 > view * view) continue;
                    cxx += rx[j]; cyy += ry[j]; avx += rvx[j]; avy += rvy[j]; ++cnt;
                    if (d2 < sep * sep) { const float k = 1.0f / fmaxf (0.0004f, d2); sx -= dx * k; sy -= dy * k; }
                }
                axx[i] = ayy[i] = 0.0f;
                if (cnt > 0) { const float ic = 1.0f / (float) cnt; axx[i] += (cxx * ic - rx[i]) * 0.5f + (avx * ic - rvx[i]) * 0.9f; ayy[i] += (cyy * ic - ry[i]) * 0.5f + (avy * ic - rvy[i]) * 0.9f; }
                axx[i] += sx * 0.012f; ayy[i] += sy * 0.012f;
                if (rx[i] > 0.85f) axx[i] -= (rx[i] - 0.85f) * 6.0f;
                if (rx[i] < -0.85f) axx[i] -= (rx[i] + 0.85f) * 6.0f;
                if (ry[i] > 0.85f) ayy[i] -= (ry[i] - 0.85f) * 6.0f;
                if (ry[i] < -0.85f) ayy[i] -= (ry[i] + 0.85f) * 6.0f;
            }
            for (int i = 0; i < 14; ++i)
            {
                rvx[i] += axx[i] * dt; rvy[i] += ayy[i] * dt;
                const float sp = sqrtf (rvx[i] * rvx[i] + rvy[i] * rvy[i]);
                const float lim = fminf (fmaxf (sp, minV), maxV);
                if (sp > 1.0e-6f) { rvx[i] *= lim / sp; rvy[i] *= lim / sp; }
                rx[i] = fminf (fmaxf (rx[i] + rvx[i] * dt, -1.0f), 1.0f);
                ry[i] = fminf (fmaxf (ry[i] + rvy[i] * dt, -1.0f), 1.0f);
            }
            for (int i = 0; i < 14; ++i) worst = fmax (worst, fmax (fabs (rx[i] - v.x[i]), fabs (ry[i] - v.y[i])));
        }
        range ("flock: 200 steps against the plug-in's loops (worst distance apart)", worst, 0.0, 0.02);
    }
}

static void testScale()
{
    printf ("scale\n");
    ScaleTable s;
    // the ER-301's "Major": 200 400 500 700 900 1100 1200
    s.begin(); for (float c : { 200.f, 400.f, 500.f, 700.f, 900.f, 1100.f, 1200.f }) s.add (c); s.end();
    range ("major: degrees", s.count, 7, 7);
    range ("major: period", s.period, 1200, 1200);
    int d;
    range ("major: 290 cents snaps to", s.quantise (290.0f, d), 200, 200);
    range ("major: 1150 cents snaps to", s.quantise (1150.0f, d), 1100, 1200);
    range ("major: -80 cents snaps to", s.quantise (-80.0f, d), -100, -100);
    range ("major: 2 steps above the 3rd degree (cents)", s.stepsAbove (2, 2), 300, 300);
    range ("major: 2 steps above the 7th degree wraps", s.stepsAbove (6, 2), 300, 300);
    // every pitch across ten octaves snaps to the same place as the first port's exhaustive search
    {
        chlref::ScaleTable r; r.begin(); for (float c : { 200.f, 400.f, 500.f, 700.f, 900.f, 1100.f, 1200.f }) r.add (c); r.end();
        int bad = 0;
        for (float c = -6000.0f; c < 6000.0f; c += 0.37f)
        {
            int d1, d2; const float a = s.quantise (c, d1), b = r.quantise (c, d2);
            if (fabsf (a - b) > 0.01f && fabsf (fabsf (a - c) - fabsf (b - c)) > 0.01f) ++bad;
        }
        range ("major: pitches that snap differently from the exhaustive search", bad, 0, 0);
    }
    // Bohlen-Pierce: period is the tritave, given out of order
    s.begin(); for (float c : { 1901.955f, 146.3f, 292.6f, 438.9f }) s.add (c); s.end();
    range ("unsorted input, tritave period", s.period, 1901.9, 1902.0);
    range ("unsorted input, degrees", s.count, 4, 4);
    s.begin(); s.end();
    range ("empty scale falls back to 12 equal", s.count, 12, 12);
}

static Buf pluck (int seconds, float amp = 0.4f)
{
    Buf in ((size_t) (48000 * seconds), 0.0f);
    Rng r (3); for (int i = 0; i < 400; ++i) in[(size_t) i] = (r.nextFloat() - 0.5f) * amp;
    return in;
}

static void testString()
{
    printf ("string\n");
    char what[128];
    struct Case { float voct; int damp; double hz; const char* name; };
    const Case cases[] = {
        { 0.0f, 0, 110.0, "1/1 = fundamental 110 Hz" },
        { 0.1f, 0, 220.0, "one octave up (0.1) = 220 Hz" },
        { 0.1f * 7.2f / 12.0f, 1, 110.0 * pow (2.0, 7.0 / 12.0), "7.2 semitones snaps to a fifth (12 equal)" },
        { 0.2f, 2, 440.0, "two octaves up, dark damping = 440 Hz" },
        { -0.1f, 0, 55.0, "one octave down = 55 Hz" },
        { 0.4f, 1, 1760.0, "four octaves up = 1760 Hz" },
    };
    for (const auto& c : cases)
    {
        StringCore s; s.prepare (SR);
        auto in = pluck (2); Buf vo (in.size(), c.voct);
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], nullptr, ol, orr, n, 110.0f, 0.9f, 0.0f, 1.0f, c.damp, false, false); });
        range (c.name, pitchOf (out, 12000, c.hz), c.hz * 0.9994, c.hz * 1.0006);     // within a cent
        if (c.hz < 1000.0) range ("   ... and it rings (rms from 0.5 s)", rms (out, 24000), 0.0005, 0.6);
    }
    {   // poly up: seven more strings, bounded, and louder than one
        StringCore s; s.prepare (SR);
        s.beginScale(); for (float c : { 200.f, 400.f, 500.f, 700.f, 900.f, 1100.f, 1200.f }) s.addPitch (c); s.endScale();
        auto in = pluck (3); Buf vo (in.size(), 0.0f);
        auto st = runStereo (in, in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], nullptr, ol, orr, n, 110.0f, 1.0f, 1.0f, 1.0f, 0, false, false); });
        range ("poly chord, sustain 1.0: peak", fmax (peak (st.l), peak (st.r)), 0.01, 1.0);
        // left, first poly string: two scale degrees up, a major third in the major scale = 400 cents
        range ("poly: left's first string is a major third (ratio)", 1.0 / s.tapMul[0][0], pow (2.0, 400.0 / 1200.0) * 0.999, pow (2.0, 400.0 / 1200.0) * 1.001);
        range ("poly: right's first string is a second (ratio)", 1.0 / s.tapMul[1][0], pow (2.0, 200.0 / 1200.0) * 0.999, pow (2.0, 200.0 / 1200.0) * 1.001);
        range ("poly: the third is in the left (138.6 Hz)", toneLevel (st.l, 110.0 * pow (2.0, 4.0 / 12.0), SR, 48000), 0.0003, 1.0);
        range ("poly: the second is in the right (123.5 Hz)", toneLevel (st.r, 110.0 * pow (2.0, 2.0 / 12.0), SR, 48000), 0.0003, 1.0);
        range ("poly: and the second is not in the left (dB below the third)",
               db (toneLevel (st.l, 110.0 * pow (2.0, 2.0 / 12.0), SR, 48000) / toneLevel (st.l, 110.0 * pow (2.0, 4.0 / 12.0), SR, 48000)), -200.0, -20.0);
        // each poly string is in tune: the left's third one is six degrees up = 1100 cents
        const double hz = 110.0 * pow (2.0, 11.0 / 12.0);
        range ("poly: left's third string, six degrees up, is in tune (Hz)", pitchOf (st.l, 48000, hz), hz * 0.9994, hz * 1.0006);
    }
    {   // sustain past 100%: sings on its own, bounded, in tune
        StringCore s; s.prepare (SR);
        auto in = pluck (4); Buf vo (in.size(), 0.0f);
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], nullptr, ol, orr, n, 110.0f, 1.15f, 0.0f, 1.0f, 0, true, false); });
        range ("sustain 1.15: peak", peak (out), 0.05, 1.0);
        range ("sustain 1.15: still sounding after 3 s", rms (out, 48000 * 3), 0.02, 1.0);
    }
    // The governor (plug-in 1.2.0): a steady input at the string's own pitch, sustain at the top,
    // must not be amplified into clipping. The string settles at its ceiling.
    for (float sustain : { 0.95f, 1.0f, 1.15f })
        for (float poly : { 0.0f, 1.0f })
        {
            StringCore s; s.prepare (SR);
            auto in = sine (110.0, 0.05, 48000 * 4); Buf vo (in.size(), 0.0f);      // +-0.5 V
            auto st = runStereo (in, in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], nullptr, ol, orr, n, 110.0f, sustain, poly, 1.0f, 0, false, false); });
            snprintf (what, sizeof what, "steady 110 Hz in, sustain %.2f, poly %.0f: peak", sustain, poly);
            range (what, fmax (peak (st.l), peak (st.r)), 0.01, 0.5);
            // (the governor can take the regeneration away, not the input: the level settles at about
            // the input plus regen x ceiling)
            snprintf (what, sizeof what, "   ... level of the main string against its ceiling (rms)");
            range (what, sqrt (s.msq[0]), 0.01, 0.0707 + 1.15 * StringCore::ceiling + 0.02);
        }
    // Plug-in 1.2.1: a string held (frozen) while a fade between two notes is in progress must not
    // run away. The note steps every 50 ms in fade mode and the freeze comes and goes on top of it.
    {
        StringCore s; s.prepare (SR);
        chlref::StringCore r; r.prepare (SR);
        auto in = sine (146.8, 0.4, 48000 * 12);
        Buf vo (in.size()), gate (in.size()), ref (in.size());
        Rng rn (11);
        float note = 0.0f; bool held = false;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (i % 2400 == 0) note = (float) rn.nextInt (36) / 120.0f - 0.1f;
            if (i % 1700 == 0) held = rn.nextFloat() < 0.5f;
            vo[i] = note; gate[i] = held ? 1.0f : 0.0f;
        }
        auto st = runStereo (in, in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { s.process (l, rr, &vo[i], &gate[i], ol, orr, n, 110.0f, 1.15f, 1.0f, 1.0f, 1, false, true); });
        blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], &vo[i], &gate[i], &ref[i], n, 110.0f, 1.15f, 1.0f, 1.0f, 1, false, true, 0); });
        // (a hot +-4 V input into sixteen strings: loud, but it must not run away)
        range ("held while fading between notes, 12 s: peak", fmax (peak (st.l), peak (st.r)), 0.01, 2.0);
        printf ("       (the first port, which had the fault, peaks at %.3g in the same run)\n", peak (ref));
        range ("   ... and still sounding at the end (rms)", rms (st.l, 48000 * 11), 0.001, 1.0);
    }
    {   // glide: the note slides, in tune at both ends, no steps
        StringCore s; s.prepare (SR);
        chlref::StringCore r; r.prepare (SR);
        auto in = pluck (3);
        Buf vo (in.size(), 0.0f), ref (in.size()); for (size_t i = 48000; i < vo.size(); ++i) vo[i] = 0.1f;
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { s.process (l, rr, &vo[i], nullptr, ol, orr, n, 110.0f, 1.1f, 0.0f, 1.0f, 0, false, false); });
        blocks (in.size(), [&] (size_t i, int n) { r.process (&in[i], &vo[i], nullptr, &ref[i], n, 110.0f, 1.1f, 0.0f, 1.0f, 0, false, false, 0); });
        double jump = 0, refJump = 0;       // (from half a second in: the pluck itself is a burst of noise)
        for (size_t i = 24000; i < out.size(); ++i) { jump = fmax (jump, fabs (out[i] - out[i - 1])); refJump = fmax (refJump, fabs (ref[i] - ref[i - 1])); }
        range ("glide to the octave: largest step, against the first port's", jump / refJump, 0.0, 1.5);
        // (singing on its own past 100% regen, the governor's gain moves a little with the wave: the
        // plug-in is within two cents there too)
        range ("glide: arrives on 220 Hz", pitchOf (out, 48000 * 2, 220.0), 220.0 * 0.9988, 220.0 * 1.0012);
        printf ("       (the first port arrives on %.3f Hz; half way through the glide: this %.2f Hz, the first port %.2f Hz)\n", pitchOf (ref, 48000 * 2, 220.0),
                pitchOf (Buf (out.begin(), out.begin() + 48000 + 2400), 48000 + 1200, 180.0), pitchOf (Buf (ref.begin(), ref.begin() + 48000 + 2400), 48000 + 1200, 180.0));
    }
    {   // freeze holds the string with the input gone, at its pitch
        StringCore s; s.prepare (SR);
        auto in = sine (110.0, 0.3, 48000 * 4);
        for (size_t i = 48000; i < in.size(); ++i) in[i] = 0.0f;
        Buf vo (in.size(), 0.0f), gate (in.size(), 0.0f); for (size_t i = 40000; i < gate.size(); ++i) gate[i] = 1.0f;
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], &gate[i], ol, orr, n, 110.0f, 0.5f, 0.0f, 1.0f, 0, false, false); });
        range ("frozen string: level 3 s later", rms (out, 48000 * 3 + 24000), 0.01, 1.0);
        range ("frozen string: held for 3 s without growing (last / first second held, dB)", db (rms (out, 48000 * 3) / rms (Buf (out.begin() + 48000, out.begin() + 96000))), -3.0, 0.5);
    }
    {   // a scale with a thousand notes, the pitch swept every block: this is where a slow search would hurt
        StringCore s; s.prepare (SR);
        s.beginScale(); for (int i = 1; i <= 1000; ++i) s.addPitch (1.2f * (float) i); s.endScale();
        auto in = pluck (2); Buf vo (in.size());
        for (size_t i = 0; i < vo.size(); ++i) vo[i] = 0.2f * (float) sin ((double) i * 0.0003);
        auto out = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int n, size_t i) { s.process (l, r, &vo[i], nullptr, ol, orr, n, 220.0f, 1.0f, 0.5f, 1.0f, 0, false, true); });
        range ("1000-note scale, pitch swept: peak", peak (out), 0.0, 1.0);
    }
    {   // dies away to exactly nothing, rests, and a new pluck wakes it
        StringCore s; s.prepare (SR);
        Buf in (48000 * 6, 0.0f);
        Rng r (3); for (int i = 0; i < 400; ++i) in[(size_t) i] = in[(size_t) (48000 * 5 + i)] = (r.nextFloat() - 0.5f) * 0.4f;
        Buf vo (in.size(), 0.0f);
        auto out = runMono (in, [&] (const float* l, const float* rr, float* ol, float* orr, int n, size_t i) { s.process (l, rr, &vo[i], nullptr, ol, orr, n, 440.0f, 0.3f, 0.0f, 1.0f, 2, false, false); });
        double tail = 0; for (size_t i = 48000 * 4; i < 48000 * 5; ++i) tail = fmax (tail, fabs (out[i]));
        range ("after it has died away the output is exactly zero", tail, 0.0, 0.0);
        range ("and a new pluck sounds (rms)", rms (out, 48000 * 5), 0.0005, 1.0);
    }
}

// everything at other frame sizes and at 96 kHz: bounded, finite
static void testFrames()
{
    printf ("other frame lengths and 96 kHz\n");
    char what[128];
    const int savedN = N; const float savedSR = SR;
    for (float sr : { 48000.0f, 96000.0f })
        for (int n : { 32, 128, 256 })
        {
            N = n; SR = sr;
            auto in = sine (220.0, 0.25, (int) sr * 2);
            Buf vo (in.size(), 0.05f), f0 (in.size(), 900.0f), gate (in.size(), 0.0f);
            double worst = 0, quietest = 1e9;
            { Tube t; t.prepare (SR, 4); auto o = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int nn, size_t) { t.process (l, r, ol, orr, nn, 0.7f, 2, kIn, kOut); }); worst = fmax (worst, peak (o)); quietest = fmin (quietest, rms (o)); }
            { FilterCore f; f.prepare (SR); auto o = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int nn, size_t i) { f.process (l, r, &vo[i], &f0[i], ol, orr, nn, 440.0f, 0.5f, 0.3f); }); worst = fmax (worst, peak (o)); quietest = fmin (quietest, rms (o)); }
            for (int kind = 0; kind < 3; ++kind)
            { AnyDelay d (kind); auto o = runMono (in, [&] (const float* l, const float* r, float* ol, float* orr, int nn, size_t i) { d.process (l, r, &gate[i], &gate[i], ol, orr, nn, 0.11f, 0.8f, 0.7f, true, false, 0.3f, true); }); worst = fmax (worst, peak (o)); quietest = fmin (quietest, rms (o)); }
            { StringCore s; s.prepare (SR); Buf pl (in.size(), 0.0f); Rng rn (3); for (int i = 0; i < 400; ++i) pl[(size_t) i] = (rn.nextFloat() - 0.5f) * 0.4f;
              auto o = runMono (pl, [&] (const float* l, const float* r, float* ol, float* orr, int nn, size_t i) { s.process (l, r, &vo[i], &gate[i], ol, orr, nn, 110.0f, 1.0f, 0.0f, 1.0f, 0, false, false); }); worst = fmax (worst, peak (o));
              snprintf (what, sizeof what, "%.0f Hz, frames of %d: the string is on its note (Hz)", sr, n);
              N = savedN; const float keep = SR; SR = savedSR;      // (pitchOf measures at the rate in SR)
              SR = keep; double best = 0, bestHz = 0; for (double hz = 150.0; hz < 165.0; hz += 0.02) { const double m = toneLevel (o, hz, sr, (size_t) sr); if (m > best) { best = m; bestHz = hz; } }
              N = n;
              range (what, bestHz, 155.56 * 0.999, 155.56 * 1.001); }
            snprintf (what, sizeof what, "%.0f Hz, frames of %d: worst peak of any unit", sr, n);
            range (what, worst, 0.01, 1.0);
            snprintf (what, sizeof what, "%.0f Hz, frames of %d: quietest unit (rms)", sr, n);
            range (what, quietest, 0.005, 1.0);
        }
    N = savedN; SR = savedSR;
}

int main()
{
    testMaths();
    testHalfBand();
    testTube();
    testDelays();
    testFilter();
    testLorenz();
    testScale();
    testString();
    testFrames();
    printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
