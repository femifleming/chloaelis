#pragma once
// Chloaelis DSP for the ER-301: the plug-in's engines (Source/DSP.h) rebuilt for the Cortex-A8.
// No JUCE, no allocation after construction, nothing large on the stack (the ER-301 audio task
// has a 2 KB stack for the whole chain).
//
// How it differs from the plug-in, all for the sake of the processor (see ChloeSimd.h):
//  - every engine carries the left and the right channel together, one in each lane of a NEON
//    vector, so a stereo unit costs what a mono one does
//  - stages of a chain that do the same sum (two filters in a row, the two triodes, the chip's
//    coder and decoder) sit side by side in the other two lanes, a sample apart
//  - nothing per sample uses scalar floating point, a compare-and-branch on a float, a divide or
//    a library call: selections are bit masks, divides are reciprocal estimates, positions in
//    delay lines are fixed-point integers
//  - everything that moves slowly is worked out every 16 samples or once a block: clocks and
//    tape speed, wow and flutter, wear, filter coefficients, the valves' drive, the governor of
//    the strings. A control that is not moving costs nothing
//  - the triode's curve (with its grid conduction folded in) is read from a table
//  - the oversampler is a polyphase all-pass half-band (2x or 4x), both branches and both
//    channels in one vector
#include "ChloeSimd.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace chl
{
    constexpr float pi = 3.14159265358979f;
    constexpr float twoPi = 6.28318530717959f;
    constexpr int kChunk = 16;          // samples between control-rate updates
    constexpr int kMaxBlock = 256;      // longer blocks are processed in pieces of this size

    static inline float clampf (float lo, float hi, float x) { return x < lo ? lo : (x > hi ? hi : x); }
    static inline float maxf (float a, float b) { return a > b ? a : b; }
    static inline float minf (float a, float b) { return a < b ? a : b; }

    inline float fastTanh (float x)
    {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // sin (2 pi p) for p in 0..1, within 0.1%
    inline float sin01 (float p)
    {
        const float x = p < 0.5f ? p : p - 1.0f;                 // -0.5 .. 0.5
        const float y = 8.0f * x * (1.0f - 2.0f * fabsf (x));    // parabola, peaks +-1
        return y * (0.775f + 0.225f * fabsf (y));
    }
    // equal-power crossfade gains for t in 0..1
    inline float fadeOut (float t) { return sin01 (0.25f + 0.25f * t); }
    inline float fadeIn (float t) { return sin01 (0.25f * t); }

    // Sine and cosine together for any angle of ordinary size, to about 1e-6. Used instead of
    // the C library, whose trig is reported to miscompute when called from an ER-301 package.
    inline void sinCos (float a, float& s, float& c)
    {
        const float q = a * (1.0f / twoPi);
        float r = q - (float) (int) (q + (q >= 0.0f ? 0.5f : -0.5f));      // -0.5 .. 0.5 turns
        float sign = 1.0f;
        if (r > 0.25f) { r = 0.5f - r; sign = -1.0f; }
        else if (r < -0.25f) { r = -0.5f - r; sign = -1.0f; }
        const float x = r * twoPi, x2 = x * x;                              // -pi/2 .. pi/2
        s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f - x2 * (1.0f / 39916800.0f))))));
        c = sign * (1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f + x2 * (-1.0f / 3628800.0f + x2 * (1.0f / 479001600.0f)))))));
    }
    inline float tanOf (float a) { float s, c; sinCos (a, s, c); return s / c; }

    inline float onePoleCoef (float fc, float sr) { return 1.0f - expf (-twoPi * fc / sr); }
    // the coefficient that does in `steps` samples what `a` does in one
    inline float coefOver (float a, int steps) { float keep = 1.0f; for (int i = 0; i < steps; ++i) keep *= 1.0f - a; return 1.0f - keep; }

    // 2^x without the C library, to about 3e-6 (0.005 cents)
    inline float fastExp2 (float x)
    {
        x = clampf (-100.0f, 100.0f, x);
        const int n = (int) (x + 256.5f);                        // the nearest whole number, + 256
        const float f = x - (float) (n - 256);                   // -0.5 .. 0.5
        const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
        const uint32_t u = (uint32_t) (n - 256 + 127) << 23;
        float s; memcpy (&s, &u, sizeof (s));
        return p * s;
    }
    // the same for four values between about -30 and 30
    static inline f4 exp2x4 (f4 x)
    {
        const i4 n = toInt (x + splat (64.5f));
        const f4 f = x - (toFloat (n) - splat (64.0f));
        f4 p = mla (splat (0.00961813f), f, splat (0.00133336f));
        p = mla (splat (0.05550411f), f, p);
        p = mla (splat (0.24022651f), f, p);
        p = mla (splat (0.69314718f), f, p);
        p = mla (splat (1.0f), f, p);
        return p * pow2i (toInt (toFloat (n) - splat (64.0f)));
    }
    // tan (x) for four values in 0 .. 1.5
    static inline f4 tan4 (f4 x)
    {
        const f4 x2 = x * x;
        f4 s = mls (splat (1.0f / 362880.0f), x2, splat (1.0f / 39916800.0f));
        s = mla (splat (-1.0f / 5040.0f), x2, s);
        s = mla (splat (1.0f / 120.0f), x2, s);
        s = mla (splat (-1.0f / 6.0f), x2, s);
        s = x * mla (splat (1.0f), x2, s);
        f4 c = mla (splat (-1.0f / 3628800.0f), x2, splat (1.0f / 479001600.0f));
        c = mla (splat (1.0f / 40320.0f), x2, c);
        c = mla (splat (-1.0f / 720.0f), x2, c);
        c = mla (splat (1.0f / 24.0f), x2, c);
        c = mla (splat (-0.5f), x2, c);
        c = mla (splat (1.0f), x2, c);
        return s * recip2 (c);
    }

    struct Rng
    {
        uint32_t s;
        explicit Rng (uint32_t seed = 1) : s (seed * 2654435761u + 12345u) {}
        void setSeed (uint32_t seed) { s = seed * 2654435761u + 12345u; }
        inline uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
        inline float nextFloat() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }      // 0..1
        inline int nextInt (int n) { return n <= 1 ? 0 : (int) ((next() >> 8) % (uint32_t) n); }
    };

    // a block of nothing, for an input that is not connected
    static inline const float* silence()
    {
        static const float zeros[kMaxBlock] = {};
        return zeros;
    }

    // a float array of four, all lanes the same / two pairs
    static inline void set4 (float* p, float v) { p[0] = p[1] = p[2] = p[3] = v; }
    static inline void set22 (float* p, float lo, float hi) { p[0] = p[1] = lo; p[2] = p[3] = hi; }

    //==========================================================================
    struct BiquadCoef
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

        void lowpass (float fc, float sr, float q = 0.7071f)
        {
            fc = clampf (10.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float al = sn / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f - c) * 0.5f * n; b1 = (1.0f - c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        void peak (float fc, float sr, float q, float db)
        {
            fc = clampf (10.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float A = fastExp2 (db * (0.16609640474f * 0.5f)), al = sn / (2.0f * q);
            const float n = 1.0f / (1.0f + al / A);
            b0 = (1.0f + al * A) * n; b1 = -2.0f * c * n; b2 = (1.0f - al * A) * n; a1 = b1; a2 = (1.0f - al / A) * n;
        }
        void highpass (float fc, float sr, float q = 0.7071f)
        {
            fc = clampf (5.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float al = sn / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f + c) * 0.5f * n; b1 = -(1.0f + c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        void through() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    };

    // Four second-order sections side by side: usually lanes 0,1 are one filter for left and
    // right, and lanes 2,3 the next filter of the chain (fed a sample later).
    struct Biquad4
    {
        float b0[4], b1[4], b2[4], a1[4], a2[4], z1[4], z2[4];

        Biquad4() { for (int k = 0; k < 4; ++k) { b0[k] = 1; b1[k] = b2[k] = a1[k] = a2[k] = z1[k] = z2[k] = 0; } }
        void setPair (int first, const BiquadCoef& c)
        {
            for (int k = first; k < first + 2; ++k) { b0[k] = c.b0; b1[k] = c.b1; b2[k] = c.b2; a1[k] = c.a1; a2[k] = c.a2; }
        }
        void reset() { set4 (z1, 0); set4 (z2, 0); }
        inline f4 tick (f4 x)
        {
            const f4 y = mla (ld (z1), ld (b0), x);
            st (z1, mla (mls (ld (z2), ld (a1), y), ld (b1), x));
            st (z2, mls (ld (b2) * x, ld (a2), y));
            return y;
        }
    };

    // y = x - x1 + r y1, four at once
    struct DCBlock4
    {
        float x1[4], y1[4], r = 0.999f;
        DCBlock4() { reset(); }
        void prepare (float sr, float fc = 8.0f) { r = 1.0f - twoPi * fc / sr; reset(); }
        void reset() { set4 (x1, 0); set4 (y1, 0); }
        inline f4 tick (f4 x)
        {
            const f4 y = mla (x - ld (x1), splat (r), ld (y1));
            st (x1, x); st (y1, y);
            return y;
        }
    };

    //==========================================================================
    // The triode's curve, tabulated once. Plate current follows a softplus^1.5 law (cut-off on
    // the negative swing, smooth compression on the positive); above 0 V the grid conducts,
    // which clamps the positive swing: that clamp is folded into the table, so one read gives
    // the plate current for any grid voltage.
    struct TubeTables
    {
        static constexpr int kN = 4096;
        static constexpr float kLo = -6.0f, kHi = 26.0f;        // grid volts covered
        float plate[kN + 2];
        float scale;

        // (logf and expf rather than log1pf: only functions other ER-301 packages already rely on)
        static inline float softplus (float x) { return x > 12.0f ? x : (x < -9.0f ? expf (x) : logf (1.0f + expf (x))); }
        static inline float plateExact (float vg) { const float s = softplus (4.0f * (vg + 1.0f)) * 0.25f; return s * sqrtf (s); }
        static inline float conduct (float v) { return v > 0.0f ? v / (1.0f + 0.85f * v) : v; }

        TubeTables()
        {
            scale = (float) kN / (kHi - kLo);
            for (int i = 0; i < kN + 2; ++i) plate[i] = plateExact (conduct (kLo + (float) i / scale));
        }
        // built on first use (from a unit's constructor, on the UI thread); a plain pointer, so
        // no static-initialisation guard is needed from the firmware
        static const TubeTables& get() { static TubeTables* t = nullptr; if (t == nullptr) t = new TubeTables(); return *t; }

        // plate current for four grid voltages. The four positions leave the vector unit through
        // memory once, then the eight table values come straight back in.
        CHL_INLINE f4 at (f4 v, int32_t* idx) const
        {
            f4 p = (v - splat (kLo)) * splat (scale);
            p = max4 (min4 (p, splat ((float) kN - 0.01f)), splat (0.0f));
            const i4 i = toInt (p);
            const f4 frac = p - toFloat (i);
            sti (idx, i);
            f4 lo, hi;
            gather2 (plate + idx[0], plate + idx[1], plate + idx[2], plate + idx[3], lo, hi);
            return mla (lo, hi - lo, frac);
        }
        inline float atScalar (float v) const
        {
            const float p = clampf (0.0f, (float) kN - 0.01f, (v - kLo) * scale);
            const int i = (int) p;
            return plate[i] + (plate[i + 1] - plate[i]) * (p - (float) i);
        }
    };

    // tanh for u >= 0 to about 1e-4 (the pentodes' knee): a higher-order Pade form than tanh4
    static inline f4 tanhFine4 (f4 u)
    {
        u = min4 (u, splat (6.0f));
        const f4 u2 = u * u;
        const f4 num = u * mla (splat (945.0f), u2, u2 + splat (105.0f));
        const f4 den = mla (splat (945.0f), u2, mla (splat (420.0f), u2, splat (15.0f)));
        return min4 (num * recip1 (den), splat (1.0f));
    }

    // Tube types: 0 = 12AX7 (two triode stages, warm, even harmonics, blocking distortion),
    // 1 = EL84 (a 12AX7 into a push-pull pentode pair: symmetric, crunchy odd harmonics),
    // 2 = 6L6 (a push-pull beam pair with power-supply sag: it blooms and compresses),
    // 3 = starved (a 12AX7 at a low plate voltage, biased near cut-off: gated, sputtery fuzz).
    //
    // Everything that depends only on the drive knob, as the vectors the stage multiplies by.
    struct TubeCoefs
    {
        float gv[4];        // input gain: {first stage, first stage, into the second stage x2}
        float av[4];        // the Miller low-pass ahead of the first stage (lanes 2,3: straight through)
        float shv[4];       // bias shift of a starved stage
        float idv[4];       // plate current at rest, taken off
        float c2[4];        // drive into the pentodes
        float mk[4];        // make-up gain
    };

    struct TubeStage
    {
        float osr = 96000.0f;
        int type = 0;
        // lanes: 0,1 the first triode (left, right), 2,3 the second
        float pre[4], bias[4], pX[4], pY[4], o1[4], dX[4], dY[4], sag[4], prevA[4];
        float att = 0.01f, rel = 0.0005f, ip0 = 0, invSlope = 1, postA = 0.5f, rInter = 0.999f, rOut = 0.999f;
        float sagAtt = 0.01f, sagRel = 0.001f;
        float pb0[4], pb1[4], pa1[4];       // the section after the triodes: a DC blocker (0,1) and a low-pass (2,3)
        int32_t idx[4];

        void prepare (float oversampledRate)
        {
            osr = oversampledRate;
            ip0 = TubeTables::plateExact (0.0f);
            invSlope = 0.002f / (TubeTables::plateExact (0.001f) - TubeTables::plateExact (-0.001f));
            att = onePoleCoef (1.0f / (twoPi * 0.002f), osr);
            rel = onePoleCoef (1.0f / (twoPi * 0.120f), osr);
            rInter = 1.0f - twoPi * 12.0f / osr;       // the coupling cap
            rOut = 1.0f - twoPi * 7.0f / osr;
            postA = onePoleCoef (15000.0f, osr);
            sagAtt = onePoleCoef (1.0f / (twoPi * 0.012f), osr);
            sagRel = onePoleCoef (1.0f / (twoPi * 0.25f), osr);
            set22 (pb0, 1.0f, postA); set22 (pb1, -1.0f, 0.0f); set22 (pa1, rInter, 1.0f - postA);
            reset();
        }
        void reset()
        {
            set4 (pre, 0); set4 (bias, 0); set4 (pX, 0); set4 (pY, 0); set4 (o1, 0); set4 (dX, 0); set4 (dY, 0); set4 (sag, 0); set4 (prevA, 0);
        }

        // once per block, when the drive (0..1) or the type has changed
        void coefs (float drive, TubeCoefs& c) const
        {
            const TubeTables& tb = TubeTables::get();
            drive = clampf (0.0f, 1.0f, drive);
            const float gain = fastExp2 (drive * 7.0f);                                  // 0..42 dB first-stage gain
            const float millerA = onePoleFast (18000.0f - 10500.0f * drive);             // Miller capacitance gets worse with gain
            float c2 = 1, makeup = 1, shift = 0, idle1 = ip0, idle2 = ip0, in1 = 0.5f;
            if (type == 0)
            {
                c2 = 0.35f + 0.9f * drive;
                makeup = 1.0f / (0.5f * c2 * fastExp2 (drive * 7.0f * 0.36f));
            }
            else if (type == 3)
            {
                shift = 0.55f - 0.25f * drive;
                c2 = 0.6f + 0.9f * drive;
                in1 = 0.5f * 1.6f;
                idle1 = tb.atScalar (-shift);
                idle2 = tb.atScalar (-shift * 0.6f);
                makeup = 2.2f / (fastExp2 (drive * 7.0f * 0.42f) * c2)
                         * fastExp2 ((1.2f + 2.0f * drive * drive) * 0.16609640474f);   // level-matched to the 12AX7
            }
            else
            {
                c2 = 0.5f + 1.6f * drive;
                const float matchDb = type == 1 ? 4.0f * drive + 6.0f * drive * drive : 9.0f * drive - 2.5f * drive * drive;
                makeup = fastExp2 (matchDb * 0.16609640474f) / ((type == 1 ? 0.53f : 0.27f) * c2 * fastExp2 (drive * 7.0f * 0.4f));
            }
            const bool pentode = type == 1 || type == 2;
            set22 (c.gv, gain * in1, pentode ? 0.0f : c2);
            set22 (c.av, millerA, 1.0f);
            set22 (c.shv, shift, shift * 0.6f);
            set22 (c.idv, idle1, idle2);
            set4 (c.c2, c2);
            set4 (c.mk, makeup);
        }
        // 1 - exp (-2 pi fc / osr) by its series (the argument is under 1.2)
        inline float onePoleFast (float fc) const
        {
            const float x = twoPi * fc / osr;
            return x < 0.5f ? x * (1.0f - x * (0.5f - x * (1.0f / 6.0f - x * (1.0f / 24.0f)))) : 1.0f - expf (-x);
        }

        // the first-stage triode (and, for the two-triode types, the second one beside it)
        CHL_INLINE f4 triodes (const TubeTables& tb, f4 s, const TubeCoefs& c)
        {
            f4 p = ld (pre);
            p = mla (p, s - p, ld (c.av));
            st (pre, p);
            f4 b = ld (bias);
            const f4 vg = p - b;
            const f4 pl = tb.at (vg - ld (c.shv), idx);
            const f4 charge = max4 (vg, splat (0.0f)) * splat (0.35f);      // the grid charges the coupling cap,
            b = mla (b, charge - b, sel4 (gt4 (charge, b), splat (att), splat (rel)));   // which shifts the bias (blocking, sag)
            st (bias, b);
            const f4 tr = (pl - ld (c.idv)) * splat (invSlope);
            const f4 pa = mla (mla (tr * ld (pb0), ld (pX), ld (pb1)), ld (pY), ld (pa1));
            st (pX, tr); st (pY, pa);
            return pa;
        }

        // one oversampled sample: x in lanes 0,1 (left, right); the result is in lanes 2,3
        CHL_NOINLINE f4 tick (const TubeTables& tb, f4 x, const TubeCoefs& c)
        {
            if (type == 0 || type == 3)
            {
                // the second triode works on what the first made one sample ago
                const f4 pa = triodes (tb, lowLow (x, ld (prevA)) * ld (c.gv), c);
                st (prevA, pa);
                const f4 d = mla (pa - ld (dX), splat (rOut), ld (dY));
                st (dX, pa); st (dY, d);
                return d * ld (c.mk);
            }
            const f4 pa = triodes (tb, x * ld (c.gv), c);                   // lanes 0,1: after the coupling cap
            f4 v = dupLow (pa) * ld (c.c2);                                 // (both halves carry left, right from here)
            f4 rail = splat (1.0f), sg = ld (sag);
            if (type == 2)                                                   // 6L6: the rail droops with the current drawn
            {
                rail = recip1 (mla (splat (1.0f), splat (2.2f), sg));
                v = v * rail * splat (0.8f);
            }
            const f4 knee = splat (type == 1 ? 0.9f : 0.55f);
            const f4 a = max4 (lowLow (v, neg4 (v)) + splat (0.7f), splat (0.0f));
            const f4 t = tanhFine4 (a * a * knee);                           // {pentode (v), pentode (-v)}
            f4 pp = (swapHalves (t) - t) * splat (0.5f);                     // lanes 2,3: the push-pull pair
            if (type == 2)
            {
                pp = pp * mla (splat (0.75f), splat (0.25f), rail);
                const f4 draw = abs4 (pp);
                sg = mla (sg, draw - sg, sel4 (gt4 (draw, sg), splat (sagAtt), splat (sagRel)));
                st (sag, sg);
            }
            f4 o = ld (o1);
            o = mla (o, pp - o, splat (postA));
            st (o1, o);
            const f4 d = mla (o - ld (dX), splat (rOut), ld (dY));
            st (dX, o); st (dY, d);
            return d * ld (c.mk);
        }
    };

    //==========================================================================
    // Polyphase all-pass half-band filter (two branches of four first-order all-pass sections in
    // z^-2): one 2x up or 2x down stage. Lanes 0,1 are branch A (left, right), lanes 2,3 branch B.
    struct HalfBand4
    {
        float s[5][4];
        float c[4][4];      // (kept beside the state, not in a table of the program: see ChloeSimd.h)
        HalfBand4()
        {
            set22 (c[0], 0.07711507983241622f, 0.2659685265210946f); set22 (c[1], 0.4820706250610472f, 0.6651041532634957f);
            set22 (c[2], 0.7968204713315797f, 0.8841015085506159f); set22 (c[3], 0.9412514277740471f, 0.9820054141886075f);
            reset();
        }
        void reset() { memset (s, 0, sizeof (s)); }

        CHL_NOINLINE f4 run (f4 x)
        {
            const f4 s1 = ld (s[1]), s2 = ld (s[2]), s3 = ld (s[3]), s4 = ld (s[4]);
            const f4 y0 = mla (ld (s[0]), ld (c[0]), x - s1);
            const f4 y1 = mla (s1, ld (c[1]), y0 - s2);
            const f4 y2 = mla (s2, ld (c[2]), y1 - s3);
            const f4 y3 = mla (s3, ld (c[3]), y2 - s4);
            st (s[0], x); st (s[1], y0); st (s[2], y1); st (s[3], y2); st (s[4], y3);
            return y3;
        }
        // Up: run ({x, x}) gives the two output samples, first in lanes 0,1 and second in lanes 2,3.
        // Down: run ({second, first}) gives the two branches, whose mean is the output sample.
        inline f4 up (f4 x) { return run (dupLow (x)); }
        inline f4 down (f4 first, f4 second)        // each in lanes 2,3 of its vector; result in all lanes
        {
            const f4 y = run (highHigh (second, first));
            return (y + swapHalves (y)) * splat (0.5f);
        }
    };

    // The oversampled tube. Changing the type copies the running stage's state into a second
    // stage set to the new type, then crossfades to it (40 ms), so a switch never drops out or
    // clicks.
    struct Tube
    {
        HalfBand4 up1, up2, dn1, dn2;
        TubeStage stages[2];
        TubeCoefs co[2], dco[2], tgt[2];
        const TubeTables* tb = nullptr;
        int cur = 0, factor = 2, xfPos = 0, xfLen = 1;
        bool xfading = false, primed = false;
        float osr = 96000.0f, lastDrive = -1.0f, env = 0, envIn = 0;
        float gains[2] { 1.0f, 1.0f };
        int idle = 0;

        void prepare (float sr, int oversample)
        {
            tb = &TubeTables::get();
            factor = oversample >= 4 ? 4 : 2;
            osr = sr * (float) factor;
            const int t0 = stages[cur].type;
            for (int i = 0; i < 2; ++i) { stages[i].prepare (osr); stages[i].type = t0; }
            xfLen = 2 * (int) (0.02f * osr);
            up1.reset(); up2.reset(); dn1.reset(); dn2.reset();
            xfading = false; primed = false; lastDrive = -1.0f; idle = 0;
        }

        // two oversampled samples through the valve(s): in {first l, r, second l, r}, and out the same
        CHL_NOINLINE f4 valves (f4 a)
        {
            TubeStage& s = stages[cur];
            const TubeCoefs& c = co[cur];
            const f4 y0 = s.tick (*tb, a, c);
            f4 y = highHigh (y0, s.tick (*tb, swapHalves (a), c));
            if (xfading)
            {
                TubeStage& o = stages[1 - cur];
                const TubeCoefs& oc = co[1 - cur];
                const f4 z0 = o.tick (*tb, a, oc);
                const f4 z = highHigh (z0, o.tick (*tb, swapHalves (a), oc));
                // a smooth s-curve from the old valve to the new (10 ms unheard first)
                f4 t = toFloat (splati (xfPos)) + pair4 (0.0f, 1.0f);
                t = min4 (max4 (t * splat (1.0f / (float) xfLen), splat (0.0f)), splat (1.0f));
                y = mla (y, z - y, t * t * mls (splat (3.0f), splat (2.0f), t));
                xfPos += 2;
                if (xfPos >= xfLen) { cur = 1 - cur; xfading = false; }
            }
            return y;
        }
        static inline void addCoefs (TubeCoefs& c, const TubeCoefs& d)
        {
            st (c.gv, ld (c.gv) + ld (d.gv)); st (c.av, ld (c.av) + ld (d.av)); st (c.shv, ld (c.shv) + ld (d.shv));
            st (c.idv, ld (c.idv) + ld (d.idv)); st (c.c2, ld (c.c2) + ld (d.c2)); st (c.mk, ld (c.mk) + ld (d.mk));
        }
        static inline void diffCoefs (TubeCoefs& d, const TubeCoefs& to, const TubeCoefs& from, float inv)
        {
            const f4 k = splat (inv);
            st (d.gv, (ld (to.gv) - ld (from.gv)) * k); st (d.av, (ld (to.av) - ld (from.av)) * k); st (d.shv, (ld (to.shv) - ld (from.shv)) * k);
            st (d.idv, (ld (to.idv) - ld (from.idv)) * k); st (d.c2, (ld (to.c2) - ld (from.c2)) * k); st (d.mk, (ld (to.mk) - ld (from.mk)) * k);
        }

        // the valve is changed: the new stage starts as a copy of the running one (same first triode,
        // coupling cap and bias state), then switches type: nothing has to settle from cold
        CHL_NOINLINE void change (int wantType)
        {
            TubeStage& from = stages[cur];
            TubeStage& to = stages[1 - cur];
            const bool wasPentode = from.type == 1 || from.type == 2, isPentode = wantType == 1 || wantType == 2;
            to = from;
            to.type = wantType;
            set4 (to.sag, 0.0f);
            if (isPentode && ! wasPentode) memcpy (to.o1, from.pY, sizeof (to.o1));          // the output low-pass carries on
            if (! isPentode && wasPentode) { to.pY[2] = from.o1[2]; to.pY[3] = from.o1[3]; }
            xfading = true; xfPos = -xfLen / 4;
            primed = false;
        }
        // the drive-dependent numbers: worked out when the drive moves, then ramped across the block
        CHL_NOINLINE void aim (float drive, int n)
        {
            stages[0].coefs (drive, tgt[0]); stages[1].coefs (drive, tgt[1]);
            if (! primed) { co[0] = tgt[0]; co[1] = tgt[1]; primed = true; }
            diffCoefs (dco[0], tgt[0], co[0], 1.0f / (float) n);
            diffCoefs (dco[1], tgt[1], co[1], 1.0f / (float) n);
            lastDrive = drive;
        }
        // (the levels are for the display, and to know when the valve has gone quiet)
        CHL_NOINLINE void look (const float* inL, const float* inR, const float* outL, const float* outR, int n, float inGain, float outGain)
        {
            const float pk = maxf (peakAbs (outL, n), outR != nullptr ? peakAbs (outR, n) : 0.0f) / outGain;
            const float pkIn = maxf (peakAbs (inL, n), peakAbs (inR, n)) * inGain;
            env += 0.2f * (pk * 0.64f - env);
            envIn += 0.2f * (pkIn * 0.64f - envIn);
            if (pk < 1.0e-7f && pkIn == 0.0f) ++idle;
        }

        // inR / outR may be null (mono). n is a multiple of 4.
        CHL_NOINLINE void process (const float* inL, const float* inR, float* outL, float* outR, int n,
                                   float drive, int wantType, float inGain, float outGain)
        {
            if (inR == nullptr) inR = silence();

            // nothing coming in and nothing left ringing: the valve is silent
            if (! xfading && wantType == stages[cur].type && allZero (inL, n) && allZero (inR, n))
            {
                if (idle > 8)
                {
                    memset (outL, 0, sizeof (float) * (size_t) n);
                    if (outR != nullptr) memset (outR, 0, sizeof (float) * (size_t) n);
                    env *= 0.7f; envIn = 0.0f;
                    return;
                }
            }
            else idle = 0;

            if (! xfading && wantType != stages[cur].type) change (wantType);
            const bool moving = ! primed || drive != lastDrive;
            if (moving) aim (drive, n);
            gains[0] = inGain; gains[1] = 0.5f * outGain;
            run (inL, inR, outL, outR, n, moving);
            look (inL, inR, outL, outR, n, inGain, outGain);
        }

        CHL_NOINLINE void run (const float* inL, const float* inR, float* outL, float* outR, int n, bool moving)
        {
            for (int i = 0; i < n; ++i)
            {
                if (moving) { addCoefs (co[0], dco[0]); addCoefs (co[1], dco[1]); }
                const f4 a = up1.run (dupLow (ldPair (inL + i, inR + i, splat (0.0f)) * ldSplat (gains)));   // {first l, r, second l, r}
                f4 y;
                if (factor == 4)
                {
                    const f4 b = up2.run (dupLow (a)), c = up2.run (dupHigh (a));
                    const f4 d0 = dn2.run (swapHalves (valves (b)));
                    const f4 d1 = dn2.run (swapHalves (valves (c)));
                    // each pair comes down to one sample: the mean of the two branches
                    y = dn1.run (lowLow (d1 + swapHalves (d1), d0 + swapHalves (d0)) * splat (0.5f));
                }
                else y = dn1.run (swapHalves (valves (a)));
                y = (y + swapHalves (y)) * ldSplat (gains + 1);
                if (outR != nullptr) stPair (outL + i, outR + i, y); else stLane (outL + i, y, 0);
            }
        }
    };
}
