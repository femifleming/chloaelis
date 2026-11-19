#pragma once
// Chloaelis DSP for the ER-301: the plug-in's engines (Source/DSP.h) in plain C++.
// No JUCE, no allocation after construction, single precision, and nothing large on the
// stack (the ER-301 audio task has a 2 KB stack for the whole chain).
//
// Changes from the plug-in, all for the Cortex-A8:
//  - the triode and pentode curves are read from tables instead of exp / log / tanh per sample
//  - tube coefficients are worked out once per block and ramped
//  - LFO sines (tape flutter, clock whine, crossfades) use a parabolic sine
//  - the oversampler is a polyphase all-pass half-band (2x or 4x)
#include <cmath>
#include <cstdint>
#include <cstring>

namespace chlref
{
    constexpr float pi = 3.14159265358979f;
    constexpr float twoPi = 6.28318530717959f;

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

    struct Rng
    {
        uint32_t s;
        explicit Rng (uint32_t seed = 1) : s (seed * 2654435761u + 12345u) {}
        void setSeed (uint32_t seed) { s = seed * 2654435761u + 12345u; }
        inline uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
        inline float nextFloat() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }      // 0..1
        inline int nextInt (int n) { return n <= 1 ? 0 : (int) ((next() >> 8) % (uint32_t) n); }
    };

    //==========================================================================
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        void lowpass (float fc, float sr, float q = 0.7071f)
        {
            fc = clampf (10.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float al = sn / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f - c) * 0.5f * n; b1 = (1.0f - c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        void peak (float fc, float sr, float q, float db)
        {
            fc = clampf (10.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float A = powf (10.0f, db / 40.0f), al = sn / (2.0f * q);
            const float n = 1.0f / (1.0f + al / A);
            b0 = (1.0f + al * A) * n; b1 = -2.0f * c * n; b2 = (1.0f - al * A) * n; a1 = b1; a2 = (1.0f - al / A) * n;
        }
        void highpass (float fc, float sr, float q = 0.7071f)
        {
            fc = clampf (5.0f, sr * 0.45f, fc);
            float sn, c; sinCos (twoPi * fc / sr, sn, c); const float al = sn / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f + c) * 0.5f * n; b1 = -(1.0f + c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        inline float process (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0; }
    };

    struct DCBlock
    {
        float r = 0.999f, x1 = 0, y1 = 0;
        void prepare (float sr, float fc = 8.0f) { r = 1.0f - twoPi * fc / sr; reset(); }
        inline float process (float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
        void reset() { x1 = y1 = 0; }
    };

    //==========================================================================
    // Analog-flavoured state variable filter (Zavalishin TPT topology) with a saturating
    // integrator, continuously morphing LP > BP > HP > Notch.
    struct MultiModeFilter
    {
        float sr = 48000.0f, ic1 = 0, ic2 = 0;

        void prepare (float s) { sr = s; reset(); }
        void reset() { ic1 = ic2 = 0; }

        // the tan() is the expensive part, so the unit works it out every few samples
        inline float gFor (float fc) const { return tanOf (pi * clampf (12.0f, sr * 0.46f, fc) / sr); }

        inline float process (float x, float g, float res, float morph)
        {
            const float k = 2.0f - 1.96f * clampf (0.0f, 1.0f, res);
            x = fastTanh (x * 0.6f) * (1.0f / 0.6f);              // input stage
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            ic1 = 3.0f * fastTanh (ic1 * (1.0f / 3.0f));          // OTA-ish soft limit on the band integrator

            const float lp = v2, bp = v1 * k, hp = x - k * v1 - v2, notch = lp + hp;
            const float outs[5] = { lp, bp, hp, notch, notch };
            const float m = clampf (0.0f, 3.0f, morph);
            const int s = (int) m;
            const float t = m - (float) s;
            return outs[s] + (outs[s + 1] - outs[s]) * t;
        }
    };

    //==========================================================================
    // Tube curves, tabulated once.
    struct TubeTables
    {
        static constexpr int kN = 2048;
        static constexpr float kPlateLo = -6.0f, kPlateHi = 1.25f;    // grid volts covered
        static constexpr float kTanhHi = 6.0f;
        float plate[kN + 2];
        float tanhPos[kN + 2];
        float plateScale, tanhScale;

        // (logf and expf rather than log1pf: only functions other ER-301 packages already rely on)
        static inline float softplus (float x) { return x > 12.0f ? x : (x < -9.0f ? expf (x) : logf (1.0f + expf (x))); }
        static inline float plateExact (float vg) { const float s = softplus (4.0f * (vg + 1.0f)) * 0.25f; return s * sqrtf (s); }

        TubeTables()
        {
            plateScale = (float) kN / (kPlateHi - kPlateLo);
            tanhScale = (float) kN / kTanhHi;
            for (int i = 0; i < kN + 2; ++i)
            {
                plate[i] = plateExact (kPlateLo + (float) i / plateScale);
                tanhPos[i] = tanhf ((float) i / tanhScale);
            }
        }
        // plate current for a grid voltage
        inline float plateAt (float vg) const
        {
            float p = (vg - kPlateLo) * plateScale;
            if (p <= 0.0f) return 0.0f;
            if (p >= (float) kN) p = (float) kN;
            const int i = (int) p;
            return plate[i] + (plate[i + 1] - plate[i]) * (p - (float) i);
        }
        // tanh (u) for u >= 0
        inline float tanhAt (float u) const
        {
            float p = u * tanhScale;
            if (p >= (float) kN) return 1.0f;
            const int i = (int) p;
            return tanhPos[i] + (tanhPos[i + 1] - tanhPos[i]) * (p - (float) i);
        }
        // built on first use (from a unit's constructor, on the UI thread); a plain pointer, so
        // no static-initialisation guard is needed from the firmware
        static const TubeTables& get() { static TubeTables* t = nullptr; if (t == nullptr) t = new TubeTables(); return *t; }
    };

    // One 12AX7 common-cathode stage. Plate current follows a softplus^1.5 law (cut-off on
    // the negative swing, smooth compression on the positive), the grid conducts above 0 V
    // and charges the coupling cap, which shifts the bias (blocking distortion / sag).
    struct TriodeStage
    {
        float bias = 0, att = 0.01f, rel = 0.0005f, ip0 = 0, invSlope = 1;

        void prepare (float osr)
        {
            ip0 = TubeTables::plateExact (0.0f);
            invSlope = 0.002f / (TubeTables::plateExact (0.001f) - TubeTables::plateExact (-0.001f));
            att = onePoleCoef (1.0f / (twoPi * 0.002f), osr);
            rel = onePoleCoef (1.0f / (twoPi * 0.120f), osr);
            bias = 0;
        }
        // idle = the plate current with no signal (ip0, or plate (-shift) for a starved stage)
        inline float process (const TubeTables& tb, float vg, float shift, float idle)
        {
            vg -= bias;
            float g = vg - shift;
            if (g > 0.0f) g = g / (1.0f + 0.85f * g);           // grid conduction clamps positive swing
            const float charge = maxf (0.0f, vg) * 0.35f;
            bias += (charge - bias) * (charge > bias ? att : rel);
            return (tb.plateAt (g) - idle) * invSlope;
        }
    };

    // Tube types: 0 = 12AX7 (two triode stages, warm, even harmonics, blocking distortion),
    // 1 = EL84 (a 12AX7 into a push-pull pentode pair: symmetric, crunchy odd harmonics),
    // 2 = 6L6 (a push-pull beam pair with power-supply sag: it blooms and compresses),
    // 3 = starved (a 12AX7 at a low plate voltage, biased near cut-off: gated, sputtery fuzz).
    struct TubeCoefs
    {
        float gain = 1, millerA = 0.3f, c2 = 1, makeup = 1, shift = 0, idle1 = 0, idle2 = 0;
    };

    struct TubeStage
    {
        float osr = 192000.0f, miller = 0, out1 = 0, postA = 0.5f;
        float sag = 0, sagAtt = 0.01f, sagRel = 0.001f;
        int type = 0;
        TriodeStage v1, v2;
        DCBlock inter, dc;

        void prepare (float oversampledRate)
        {
            osr = oversampledRate;
            v1.prepare (osr); v2.prepare (osr);
            inter.prepare (osr, 12.0f); dc.prepare (osr, 7.0f);
            postA = onePoleCoef (15000.0f, osr);
            sagAtt = onePoleCoef (1.0f / (twoPi * 0.012f), osr);
            sagRel = onePoleCoef (1.0f / (twoPi * 0.25f), osr);
            miller = out1 = sag = 0;
        }

        // everything that depends only on the drive knob (0..1), once per block
        TubeCoefs coefs (float drive) const
        {
            TubeCoefs c;
            drive = clampf (0.0f, 1.0f, drive);
            c.gain = exp2f (drive * 7.0f);                                    // 0..42 dB first-stage gain
            c.millerA = onePoleCoef (18000.0f - 10500.0f * drive, osr);       // Miller capacitance gets worse with gain
            c.idle1 = c.idle2 = v1.ip0;
            if (type == 0)
            {
                c.c2 = 0.35f + 0.9f * drive;
                c.makeup = 1.0f / (0.5f * c.c2 * powf (c.gain, 0.36f));
            }
            else if (type == 3)
            {
                c.shift = 0.55f - 0.25f * drive;
                c.c2 = 0.6f + 0.9f * drive;
                c.idle1 = TubeTables::plateExact (-c.shift);
                c.idle2 = TubeTables::plateExact (-c.shift * 0.6f);
                c.makeup = 2.2f / (powf (c.gain, 0.42f) * c.c2)
                           * exp2f ((1.2f + 2.0f * drive * drive) * 0.16609640474f);   // level-matched to the 12AX7
            }
            else
            {
                c.c2 = 0.5f + 1.6f * drive;
                const float matchDb = type == 1 ? 4.0f * drive + 6.0f * drive * drive : 9.0f * drive - 2.5f * drive * drive;
                c.makeup = exp2f (matchDb * 0.16609640474f) / ((type == 1 ? 0.53f : 0.27f) * c.c2 * powf (c.gain, 0.4f));
            }
            return c;
        }

        static inline float pentode (const TubeTables& tb, float v, float knee)
        {
            const float a = maxf (0.0f, v + 0.7f);
            return tb.tanhAt (a * a * knee);
        }

        inline float process (const TubeTables& tb, float x, const TubeCoefs& c)
        {
            float s = x * c.gain * 0.5f;
            miller += c.millerA * (s - miller);

            if (type == 0)
            {
                float y = v1.process (tb, miller, 0.0f, c.idle1);
                y = inter.process (y);                             // coupling cap
                y = v2.process (tb, y * c.c2, 0.0f, c.idle2);      // second stage, phase kept so even harmonics stack
                out1 += postA * (y - out1);
                return dc.process (out1) * c.makeup;
            }
            if (type == 3)   // starved
            {
                float y = v1.process (tb, miller * 1.6f, c.shift, c.idle1);
                y = inter.process (y);
                y = v2.process (tb, y * c.c2, c.shift * 0.6f, c.idle2);
                out1 += postA * (y - out1);
                return dc.process (out1) * c.makeup;
            }
            float y = v1.process (tb, miller, 0.0f, c.idle1);
            y = inter.process (y);
            float v = y * c.c2, p;
            if (type == 1)   // EL84 pair
                p = 0.5f * (pentode (tb, v, 0.9f) - pentode (tb, -v, 0.9f));
            else             // 6L6 pair with sag: the rail droops with the current drawn
            {
                const float rail = 1.0f / (1.0f + 2.2f * sag);
                v *= 0.8f * rail;
                p = 0.5f * (pentode (tb, v, 0.55f) - pentode (tb, -v, 0.55f)) * (0.75f + 0.25f * rail);
                const float draw = fabsf (p);
                sag += (draw - sag) * (draw > sag ? sagAtt : sagRel);
            }
            out1 += postA * (p - out1);
            return dc.process (out1) * c.makeup;
        }
    };

    //==========================================================================
    // Polyphase all-pass half-band filter (two branches of four first-order all-pass
    // sections in z^-2): one 2x up or 2x down stage.
    struct HalfBand
    {
        float xa[4] {}, ya[4] {}, xb[4] {}, yb[4] {};
        void reset() { memset (xa, 0, sizeof (xa)); memset (ya, 0, sizeof (ya)); memset (xb, 0, sizeof (xb)); memset (yb, 0, sizeof (yb)); }

        inline float pathA (float x)
        {
            static const float c[4] = { 0.07711507983241622f, 0.4820706250610472f, 0.7968204713315797f, 0.9412514277740471f };
            for (int i = 0; i < 4; ++i) { const float y = xa[i] + c[i] * (x - ya[i]); xa[i] = x; ya[i] = y; x = y; }
            return x;
        }
        inline float pathB (float x)
        {
            static const float c[4] = { 0.2659685265210946f, 0.6651041532634957f, 0.8841015085506159f, 0.9820054141886075f };
            for (int i = 0; i < 4; ++i) { const float y = xb[i] + c[i] * (x - yb[i]); xb[i] = x; yb[i] = y; x = y; }
            return x;
        }
        // one input sample -> two output samples
        inline void up (float x, float& o0, float& o1) { o0 = pathA (x); o1 = pathB (x); }
        // two input samples (in time order) -> one output sample
        inline float down (float i0, float i1) { return 0.5f * (pathA (i1) + pathB (i0)); }
    };

    // The oversampled tube. Changing the type copies the running stage's state into a second
    // stage set to the new type, then crossfades to it (40 ms), so a switch never drops out or
    // clicks.
    struct Tube
    {
        HalfBand up1, up2, dn1, dn2;
        TubeStage stages[2];
        TubeCoefs last[2];
        int cur = 0, factor = 2;
        bool xfading = false, primed = false;
        float xf = 0, xfStep = 0.0001f, osr = 96000.0f, lastDrive = -1.0f, env = 0, envIn = 0;

        void prepare (float sr, int oversample)
        {
            factor = oversample >= 4 ? 4 : 2;
            osr = sr * (float) factor;
            const int t0 = stages[cur].type;
            for (int i = 0; i < 2; ++i) { stages[i].prepare (osr); stages[i].type = t0; }
            xfStep = 1.0f / (0.04f * osr);
            up1.reset(); up2.reset(); dn1.reset(); dn2.reset();
            xfading = false; primed = false; lastDrive = -1.0f;
        }

        inline float tick (const TubeTables& tb, float x, const TubeCoefs& ca, const TubeCoefs& cb)
        {
            float y = stages[cur].process (tb, x, ca);
            if (xfading)
            {
                const float yb = stages[1 - cur].process (tb, x, cb);
                const float c = clampf (0.0f, 1.0f, xf);
                y += (yb - y) * (c * c * (3.0f - 2.0f * c));          // smooth s-curve
                xf += xfStep;
                if (xf >= 1.0f) { cur = 1 - cur; xfading = false; }
            }
            return y;
        }

        void process (const float* in, float* out, int n, float drive, int wantType, float inGain, float outGain)
        {
            const TubeTables& tb = TubeTables::get();
            if (! xfading && wantType != stages[cur].type)
            {
                // the new stage starts as a copy of the running one (same first triode, coupling
                // cap and bias state), then switches type: nothing has to settle from cold
                stages[1 - cur] = stages[cur];
                stages[1 - cur].type = wantType;
                stages[1 - cur].sag = 0.0f;
                xfading = true; xf = -0.25f;     // 10 ms unheard, then the crossfade
                primed = false;
            }
            // ramp the drive-dependent numbers across the block
            TubeCoefs tgt[2] = { stages[0].coefs (drive), stages[1].coefs (drive) };
            if (! primed) { last[0] = tgt[0]; last[1] = tgt[1]; primed = true; }
            TubeCoefs c[2] = { last[0], last[1] };
            const float inv = 1.0f / (float) n;
            TubeCoefs d[2];
            for (int k = 0; k < 2; ++k)
            {
                d[k].gain = (tgt[k].gain - c[k].gain) * inv;       d[k].millerA = (tgt[k].millerA - c[k].millerA) * inv;
                d[k].c2 = (tgt[k].c2 - c[k].c2) * inv;             d[k].makeup = (tgt[k].makeup - c[k].makeup) * inv;
                d[k].shift = (tgt[k].shift - c[k].shift) * inv;    d[k].idle1 = (tgt[k].idle1 - c[k].idle1) * inv;
                d[k].idle2 = (tgt[k].idle2 - c[k].idle2) * inv;
            }
            for (int i = 0; i < n; ++i)
            {
                for (int k = 0; k < 2; ++k)
                {
                    c[k].gain += d[k].gain; c[k].millerA += d[k].millerA; c[k].c2 += d[k].c2; c[k].makeup += d[k].makeup;
                    c[k].shift += d[k].shift; c[k].idle1 += d[k].idle1; c[k].idle2 += d[k].idle2;
                }
                const float x = in[i] * inGain;
                float a0, a1;
                up1.up (x, a0, a1);
                float y;
                if (factor == 4)
                {
                    float b0, b1, b2, b3;
                    up2.up (a0, b0, b1);
                    up2.up (a1, b2, b3);
                    b0 = tick (tb, b0, c[cur], c[1 - cur]); b1 = tick (tb, b1, c[cur], c[1 - cur]);
                    b2 = tick (tb, b2, c[cur], c[1 - cur]); b3 = tick (tb, b3, c[cur], c[1 - cur]);
                    a0 = dn2.down (b0, b1);
                    a1 = dn2.down (b2, b3);
                }
                else
                {
                    a0 = tick (tb, a0, c[cur], c[1 - cur]);
                    a1 = tick (tb, a1, c[cur], c[1 - cur]);
                }
                y = dn1.down (a0, a1);
                out[i] = y * outGain;
                env += 0.002f * (fabsf (y) - env);
                envIn += 0.002f * (fabsf (x) - envIn);
            }
            last[0] = tgt[0]; last[1] = tgt[1];
        }
    };

    //==========================================================================
    // The delay engines below are bare "lines" (input -> coloured, delayed output). The
    // regeneration loop, its HPF and the slew / fade logic live in the units, the way the
    // feedback lives in the op-amps around the chip on the real module.
    //==========================================================================

    // PT2399. The chip is a continuously variable slope delta modulator writing 1 bit per
    // conversion into a fixed 44 kbit RAM. The delay time is set by the VCO clock: longer
    // delays mean a slower bit rate, so more quantisation noise, slope overload grit,
    // narrower bandwidth and aliasing (the output is just the integrated bit stream sampled
    // and held). Turning time while it runs re-clocks the RAM, so the echoes pitch-bend.
    // Freeze stops writing the RAM, so the loop recirculates forever, bit for bit.
    struct PT2399Line
    {
        static constexpr int kBits = 44000;
        static constexpr float kClockPerBit = 15.55f;   // ~684k VCO cycles for the full RAM at 342 ms

        uint8_t* mem = nullptr;
        int pos = 0;
        float sr = 48000.0f, bitRate = 130000.0f, targetRate = 130000.0f, bitPhase = 0, prevIn = 0, held = 0;
        float encEst = 0, encStep = 0, decEst = 0, decStep = 0;
        unsigned encHist = 0, decHist = 0;
        float leak = 0.999f, beta = 0.99f, dMin = 0.001f, dMax = 0.1f;
        float whinePhase = 0, whineInc = 0, clockA = 0.01f;
        Biquad inLP, outLP1, outLP2;
        DCBlock dc;
        Rng rng { 0x2399 };

        PT2399Line() { mem = new uint8_t[kBits]; }
        ~PT2399Line() { delete[] mem; }
        PT2399Line (const PT2399Line&) = delete;
        PT2399Line& operator= (const PT2399Line&) = delete;

        void prepare (float s)
        {
            sr = s;
            clockA = onePoleCoef (12.0f, sr);       // the VCO's RC smoothing on the time pot
            dc.prepare (sr, 10.0f);
            reset();
        }
        void reset()
        {
            for (int i = 0; i < kBits; ++i) mem[i] = (uint8_t) (i & 1);   // idle pattern = silence
            pos = 0; bitPhase = 0; prevIn = held = 0;
            encEst = decEst = 0; encStep = decStep = 0; encHist = decHist = 0;
            whinePhase = 0;
            inLP.reset(); outLP1.reset(); outLP2.reset(); dc.reset();
            bitRate = targetRate = kBits / 0.33f;
            updateCoeffs();
        }
        // jump = re-clock instantly (used by the fade mode, which waits for the RAM to refill)
        void setTime (float sec, bool jump)
        {
            targetRate = (float) kBits / clampf (0.02f, 1.6f, sec);
            if (jump) { bitRate = targetRate; updateCoeffs(); }
        }
        // called once per control block
        void updateCoeffs()
        {
            // the chip's RC integrator / syllabic filter time constants are fixed in time,
            // so per-bit coefficients scale with the clock
            leak = expf (-twoPi * 25.0f / bitRate);
            beta = expf (-1.0f / (0.0035f * bitRate));
            // step size ~ 1/sqrt(clock): long delays overload sooner (grit), and re-clocking a
            // loop changes its level the way the chip does, without +30 dB bursts
            const float isq = 1.0f / sqrtf (bitRate);
            dMin = 2.6f * isq;
            dMax = 145.0f * isq;
            const float bw = clampf (2400.0f, 11500.0f, bitRate * 0.048f);
            inLP.lowpass (bw, sr, 0.62f);
            outLP1.lowpass (bw, sr, 0.54f);
            outLP2.lowpass (bw * 1.25f, sr, 1.3f);
            const float clk = bitRate * kClockPerBit;
            float f = fmodf (clk, sr);
            if (f > sr * 0.5f) f = sr - f;
            whineInc = f / sr;
        }

        inline float process (float x, bool freeze)
        {
            bitRate += (targetRate - bitRate) * clockA;

            float in = freeze ? 0.0f : x;
            in = inLP.process (in);
            in = 1.1f * fastTanh (in * 0.9f);          // the chip's input opamp rails

            bitPhase += bitRate / sr;
            const int n = (int) bitPhase;
            bitPhase -= (float) n;

            if (n > 0)
            {
                float acc = 0;
                const float invN = 1.0f / (float) n;
                const float oneMinusBeta = 1.0f - beta;
                const float dIn = (in - prevIn) * invN;
                float xin = prevIn;
                for (int k = 1; k <= n; ++k)
                {
                    xin += dIn;

                    // encoder (CVSD, run-of-3 coincidence)
                    const unsigned bit = xin > encEst ? 1u : 0u;
                    encHist = ((encHist << 1) | bit) & 7u;
                    encStep = beta * encStep + oneMinusBeta * ((encHist == 0u || encHist == 7u) ? dMax : dMin);
                    encEst = encEst * leak + (bit ? encStep : -encStep);

                    // RAM: read the oldest bit, then (unless frozen) overwrite it
                    const unsigned outBit = mem[pos];
                    if (! freeze) mem[pos] = (uint8_t) bit;
                    if (++pos >= kBits) pos = 0;

                    // decoder
                    decHist = ((decHist << 1) | outBit) & 7u;
                    decStep = beta * decStep + oneMinusBeta * ((decHist == 0u || decHist == 7u) ? dMax : dMin);
                    decEst = decEst * leak + (outBit ? decStep : -decStep);
                    acc += decEst;
                }
                held = acc * invN;                     // sample & hold of the bit-rate output (aliases)
            }
            prevIn = in;

            float y = outLP2.process (outLP1.process (held));
            y = 1.3f * fastTanh (dc.process (y) * (1.0f / 1.3f));       // output opamp rails

            // noise floor + a faint clock whine folded into the audio band
            whinePhase += whineInc; if (whinePhase >= 1.0f) whinePhase -= 1.0f;
            y += (rng.nextFloat() - 0.5f) * 0.00012f + sin01 (whinePhase) * 0.00018f;
            return y;
        }
    };

    //==========================================================================
    // Bucket brigade (think MN3005 pairs, as in the old analog delays and the Serge
    // BBD module): NE570-style 2:1 compressor > fixed anti-alias filter > a chain of
    // charge buckets clocked at kStages / time > sample-and-hold output with clock
    // feedthrough > fixed reconstruction filter > 1:2 expander. The filters are fixed while
    // the clock moves, so long delays alias and go crunchy and dark; buckets clip softly and
    // leak a little each transfer; the compander breathes and pumps the noise floor.
    struct BBDLine
    {
        static constexpr int kStages = 4096;        // buckets per output (two 4096-stage chips, one output tap)
        float* bucket = nullptr;
        int idx = 0;
        float sr = 48000.0f, clk = 12000.0f, targetClk = 12000.0f, clkA = 0.001f, tickPhase = 0;
        float prevAA = 0, held = 0, cti = 0, parity = 1.0f;
        float compEnv = 0.05f, expEnv = 0.05f, cAtt = 0.01f, cRel = 0.001f, eAtt = 0.01f, eRel = 0.001f;
        Biquad aa1, aa2, rc1, rc2;
        DCBlock dc;
        Rng rng { 0xbbd };
        static constexpr float ref = 0.22f;

        BBDLine() { bucket = new float[kStages]; }
        ~BBDLine() { delete[] bucket; }
        BBDLine (const BBDLine&) = delete;
        BBDLine& operator= (const BBDLine&) = delete;

        void prepare (float s)
        {
            sr = s;
            clkA = onePoleCoef (9.0f, sr);
            aa1.lowpass (6800.0f, sr, 0.54f); aa2.lowpass (6800.0f, sr, 1.31f);
            rc1.lowpass (6200.0f, sr, 0.54f); rc2.lowpass (6200.0f, sr, 1.31f);
            cAtt = onePoleCoef (1.0f / (twoPi * 0.004f), sr); cRel = onePoleCoef (1.0f / (twoPi * 0.045f), sr);
            eAtt = onePoleCoef (1.0f / (twoPi * 0.006f), sr); eRel = onePoleCoef (1.0f / (twoPi * 0.060f), sr);
            dc.prepare (sr, 9.0f);
            reset();
        }
        void reset()
        {
            for (int i = 0; i < kStages; ++i) bucket[i] = 0.0f;
            idx = 0; tickPhase = 0; prevAA = held = cti = 0; compEnv = expEnv = 0.05f;
            aa1.reset(); aa2.reset(); rc1.reset(); rc2.reset(); dc.reset();
            clk = targetClk = (float) kStages / 0.33f;
        }
        void setTime (float sec, bool jump)
        {
            targetClk = (float) kStages / clampf (0.02f, 1.6f, sec);
            if (jump) clk = targetClk;
        }
        void updateCoeffs() {}

        static inline float bucketSat (float v)
        {
            // charge wells fill up softly, a little asymmetrically
            return 0.85f * (fastTanh (v * 1.15f + 0.06f) - 0.0599f);
        }

        inline float process (float x, bool freeze)
        {
            clk += (targetClk - clk) * clkA;

            // compressor (2:1)
            const float ax = fabsf (x);
            compEnv += (ax - compEnv) * (ax > compEnv ? cAtt : cRel);
            const float g = minf (10.0f, 1.0f / sqrtf (maxf (compEnv, 0.0005f) / ref));
            const float aa = aa2.process (aa1.process (x * g));

            const float inc = clk / sr;
            tickPhase += inc;
            while (tickPhase >= 1.0f)
            {
                tickPhase -= 1.0f;
                // where inside this sample the clock edge landed
                const float frac = clampf (0.0f, 1.0f, 1.0f - tickPhase / inc);
                const float v = prevAA + (aa - prevAA) * frac;
                const float out = bucket[idx];
                bucket[idx] = freeze ? out * 0.99985f : bucketSat (v) + (rng.nextFloat() - 0.5f) * 0.0009f;
                if (++idx >= kStages) idx = 0;
                cti += 0.86f * (out - cti);                        // charge transfer inefficiency
                held = cti;
                parity = -parity;
            }
            prevAA = aa;

            // S&H output + a whisper of clock feedthrough, then the reconstruction filter
            float y = rc2.process (rc1.process (held + parity * 0.0012f));
            y += (rng.nextFloat() - 0.5f) * 0.0004f;               // output noise, expanded with the signal

            // expander (1:2)
            const float ay = fabsf (y);
            expEnv += (ay - expEnv) * (ay > expEnv ? eAtt : eRel);
            y *= minf (6.0f, maxf (expEnv, 0.0002f) / ref);
            y = dc.process (y);
            return 1.25f * fastTanh (y * (1.0f / 1.25f));
        }
    };

    //==========================================================================
    // Tape. Record head (pre-emphasis, soft magnetic saturation) > a loop of oxide that
    // wears > playback head (gap loss, head bump, de-emphasis) with wow and flutter, hiss.
    // The tape ages while it runs: flakes of oxide shed (dropouts that come round again
    // every pass), the gap loss creeps down, the warble widens, the hiss rises, and with
    // freeze the held loop slowly disintegrates pass after pass. A refresh puts on new tape.
    struct TapeLine
    {
        static constexpr int kBlock = 512;
        float* buf = nullptr;
        float* dmg = nullptr;
        int size = 1, w = 0, nb = 1;
        float sr = 48000.0f, age = 0, wearNow = 0;
        float dA = 0, dS = 0, dB = 0, dTarget = 0, slewA = 0.001f, xf = 1, xfStep = 0.001f;
        bool fading = false, frozen = false;
        float loopStart = 0, loopLen = 1, loopPos = 0;
        float wowT = 0, wow1 = 0, wow2 = 0, wowA = 0.0001f, wowTimer = 0, flutPh = 0, flutPh2 = 0, flutInc = 0, flutInc2 = 0, flutAmp = 1;
        float wowDepth = 100, flutDepth = 1;
        float pre = 0, preA = 0.2f, gap1 = 0, gap2 = 0, deemph = 0, hissLP = 0, hissHP = 0, hissA = 0.5f, hissHA = 0.1f, crackle = 0;
        float hissGain = 0, hissTarget = 1, hissUp = 0.00002f, hissDown = 0.00002f;
        Biquad bump;
        DCBlock dc;
        Rng rng;

        ~TapeLine() { delete[] buf; delete[] dmg; }
        TapeLine() {}
        TapeLine (const TapeLine&) = delete;
        TapeLine& operator= (const TapeLine&) = delete;

        void prepare (float s, float maxSec, uint32_t seed)
        {
            sr = s;
            rng.setSeed (seed);
            delete[] buf; delete[] dmg;
            size = (int) (maxSec * sr) + 16;
            buf = new float[size];
            nb = size / kBlock + 1;
            dmg = new float[nb];
            for (int i = 0; i < nb; ++i) dmg[i] = 0.0f;
            slewA = onePoleCoef (1.6f, sr);                         // the capstan servo's inertia
            xfStep = 1.0f / (0.07f * sr);
            wowA = onePoleCoef (0.55f, sr);
            flutInc = (6.1f + rng.nextFloat() * 0.8f) / sr;
            flutInc2 = (11.3f + rng.nextFloat() * 1.5f) / sr;
            wowDepth = 0.0021f * sr;                                // ~0.5% pitch wander when new
            flutDepth = 0.000028f * sr;
            preA = onePoleCoef (2400.0f, sr);
            hissA = onePoleCoef (11000.0f, sr);
            hissHA = onePoleCoef (900.0f, sr);
            hissUp = 1.0f / (1.6f * sr);
            hissDown = 1.0f / (1.4f * sr);
            bump.peak (95.0f, sr, 0.9f, 2.5f);
            dc.prepare (sr, 8.0f);
            reset();
        }
        void reset()
        {
            for (int i = 0; i < size; ++i) buf[i] = 0.0f;
            w = 0; dA = dS = dB = dTarget = 0.33f * sr; xf = 1; fading = frozen = false;
            pre = gap1 = gap2 = deemph = hissLP = hissHP = crackle = 0;
            hissGain = 0;
            bump.reset(); dc.reset();
        }
        void setTime (float sec, bool jump)
        {
            dTarget = clampf (64.0f, (float) size - wowDepth * 4.0f - 32.0f, sec * sr);
            if (jump) dA = dS = dB = dTarget;
        }
        void refresh() { for (int i = 0; i < nb; ++i) dmg[i] = 0.0f; }

        void addFlake()
        {
            const int b = rng.nextInt (nb);
            const int len = 1 + rng.nextInt (1 + (int) (5.0f * age));
            const float s = (0.15f + 0.75f * rng.nextFloat()) * sqrtf (age);
            for (int k = 0; k < len; ++k)
            {
                const float wnd = sin01 (0.5f * ((float) k + 0.5f) / (float) len);
                float& d = dmg[(b + k) % nb];
                d = minf (1.0f, maxf (d, s * wnd) + 0.08f * s * wnd);
            }
        }
        // once per control block: oxide sheds as the tape runs
        void wear (float dt)
        {
            wearNow = sqrtf (maxf (0.0f, age));
            const float rate = 0.01f + 0.12f * age * age;            // flakes per second: ~25 over a tape's life
            if (age > 0.001f && rng.nextFloat() < rate * dt) addFlake();
        }

        inline float dmgAt (float pos) const
        {
            const float p = pos * (1.0f / (float) kBlock);
            int i = (int) p;
            const float f = p - (float) i;
            i = ((i % nb) + nb) % nb;
            const int j = i + 1 < nb ? i + 1 : 0;
            return dmg[i] + (dmg[j] - dmg[i]) * f;
        }
        inline float wrapPos (float p) const
        {
            while (p < 0.0f) p += (float) size;
            while (p >= (float) size) p -= (float) size;
            return p;
        }
        inline float at (int k) const { if (k < 0) k += size; else if (k >= size) k -= size; return buf[k]; }
        inline float readAt (float p) const
        {
            p = wrapPos (p);
            const int i = (int) p;
            const float f = p - (float) i;
            const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }

        inline float process (float in, bool freeze, bool fadeMode)
        {
            const float wear = wearNow;

            // wow: a slow random wander; flutter: the capstan and pinch roller
            if (--wowTimer <= 0.0f) { wowT = rng.nextFloat() * 2.0f - 1.0f; wowTimer = sr * (0.5f + 1.8f * rng.nextFloat()); }
            wow1 += (wowT - wow1) * wowA;
            wow2 += (wow1 - wow2) * wowA;
            flutPh += flutInc;   if (flutPh >= 1.0f) flutPh -= 1.0f;
            flutPh2 += flutInc2; if (flutPh2 >= 1.0f) flutPh2 -= 1.0f;
            flutAmp += ((0.5f + rng.nextFloat()) - flutAmp) * 0.0004f;
            float wob = wowDepth * (1.0f + 1.6f * wear) * wow2
                      + flutDepth * (1.0f + 2.0f * wear) * flutAmp * (sin01 (flutPh) + 0.35f * sin01 (flutPh2));

            if (freeze && ! frozen) { frozen = true; loopLen = maxf (256.0f, dA); loopStart = wrapPos ((float) w - 1.0f - dA); loopPos = 0; }
            else if (! freeze && frozen) frozen = false;

            float out, readPos;
            if (! frozen)
            {
                // record head
                pre += (in - pre) * preA;
                const float e = in + 0.6f * (in - pre);                       // pre-emphasis
                const float m = fastTanh (e * 1.15f + 0.04f) - 0.04f;          // magnetic saturation (a little asymmetric)
                buf[w] = m * (1.0f - 0.8f * dmgAt ((float) w)) * (1.0f - 0.18f * wear);
                if (++w >= size) w = 0;

                // playback head(s)
                const float d0 = dA;
                if (! fadeMode)
                {
                    fading = false;
                    dS += (dTarget - dS) * slewA;                              // tape speed glides (pitch bends)
                    dA += (dS - dA) * slewA;
                    out = readAt ((float) w - 1.0f - maxf (8.0f, dA + wob));
                }
                else
                {
                    if (! fading && fabsf (dTarget - dA) > 0.01f * dA + 4.0f) { dB = dTarget; fading = true; xf = 0; }
                    const float ya = readAt ((float) w - 1.0f - maxf (8.0f, dA + wob));
                    if (fading)
                    {
                        const float yb = readAt ((float) w - 1.0f - maxf (8.0f, dB + wob));
                        xf += xfStep;
                        if (xf >= 1.0f) { xf = 1.0f; dA = dB; fading = false; out = yb; }
                        else out = ya * fadeOut (xf) + yb * fadeIn (xf);
                    }
                    else out = ya;
                    dS = dA;
                }
                readPos = (float) w - 1.0f - d0;
            }
            else
            {
                loopPos += 1.0f;
                if (loopPos >= loopLen) loopPos -= loopLen;
                const float p = loopStart + loopPos - wob * 0.5f;
                out = readAt (p);
                const float X = minf (600.0f, loopLen * 0.25f);       // hide the splice
                if (loopPos > loopLen - X)
                {
                    const float t = (loopPos - (loopLen - X)) / X;
                    out = out * (1.0f - t) + readAt (p - loopLen) * t;
                }
                // the held loop disintegrates a little each pass, worst where the oxide is gone
                const int ip = (int) wrapPos (loopStart + loopPos);
                const int pp = ip > 0 ? ip - 1 : size - 1;
                const float d = dmg[(ip / kBlock) % nb];
                float& s = buf[ip];
                s += (buf[pp] - s) * (0.03f * wear + 0.3f * d);
                s *= 1.0f - (0.0009f * wear + 0.012f * d);
                readPos = p;
            }

            // playback colouring
            const float dr = dmgAt (wrapPos (readPos));
            const float fc = 15000.0f * (1.0f - 0.8f * wear) * (1.0f - 0.55f * dr);
            const float wc = twoPi * fc / sr, a = wc / (1.0f + wc);
            gap1 += (out - gap1) * a;
            gap2 += (gap1 - gap2) * a;
            float y = bump.process (gap2);
            deemph += (y - deemph) * preA;
            y -= 0.375f * (y - deemph);                                   // de-emphasis
            y *= 1.0f - 0.4f * dr;                                        // dropout where oxide shed

            // hiss (recorded with everything else, so it builds with the regen)
            const float nz = rng.nextFloat() * 2.0f - 1.0f;
            hissLP += (nz - hissLP) * hissA;
            hissHP += (hissLP - hissHP) * hissHA;
            hissGain += clampf (-hissDown, hissUp, hissTarget - hissGain);
            const float hg = hissGain * hissGain * (3.0f - 2.0f * hissGain);
            y += (hissLP - hissHP) * (0.0018f + 0.008f * wear) * hg;

            // crumbling: the odd crackle of a worn splice
            if (wear > 0.25f && rng.nextFloat() < 0.00003f * wear * wear * (1.0f + 8.0f * dr))
                crackle = (rng.nextFloat() * 2.0f - 1.0f) * 0.02f * wear;
            crackle *= 0.92f;
            y += crackle * hg;
            return dc.process (y);
        }
    };

    //==========================================================================
    // Karplus-Strong: a short, clean, exactly tuned line. Inside a regeneration loop it
    // becomes a plucked / bowed string at the pitch whose period is the delay. The string's damping
    // is the classic two-point average, blended in by the damping type (soft, bright, dark): its
    // delay is the same at every frequency, so the partials stay harmonic. The line
    // is shortened by the loop's other delays (and lengthened by the DC blocker's phase lead at the
    // fundamental), so the pitch is true. Slew glides between notes; fade crossfades to the new note
    // with no glide. Freeze holds the string.
    struct KSLine
    {
        float* buf = nullptr;
        int size = 1, w = 0;
        float sr = 48000.0f, dA = 100, dB = 100, dTarget = 100, slewA = 0.001f, xf = 1, xfStep = 0.001f, lp = 0;
        float dcLead = 0.0f, fb = 0, env = 0;
        bool fading = false;
        DCBlock dc;
        Biquad fbHP;

        KSLine() {}
        ~KSLine() { delete[] buf; }
        KSLine (const KSLine&) = delete;
        KSLine& operator= (const KSLine&) = delete;

        static inline float dampFor (int type) { return type == 1 ? 0.25f : (type == 2 ? 1.0f : 0.5f); }   // soft, bright, dark

        void prepare (float s)
        {
            sr = s;
            delete[] buf;
            size = (int) (0.09f * sr) + 16;
            buf = new float[size];
            slewA = onePoleCoef (11.0f, sr);
            xfStep = 1.0f / (0.03f * sr);
            dc.prepare (sr, 1.5f);
            dcLead = 1.5f / (twoPi * sr);       // the blocker's phase lead at a note of period P is dcLead * P * P samples
            fbHP.highpass (320.0f, sr, 0.6f);
            reset();
        }
        void reset() { for (int i = 0; i < size; ++i) buf[i] = 0.0f; w = 0; lp = 0; xf = 1; fb = 0; fading = false; dc.reset(); fbHP.reset(); }
        void setPeriod (float sec, bool jump)
        {
            dTarget = clampf (5.0f, (float) size - 8.0f, sec * sr);
            if (jump) { dA = dB = dTarget; fading = false; xf = 1; }
        }
        inline float at (int k) const { if (k < 0) k += size; else if (k >= size) k -= size; return buf[k]; }
        inline float readAt (float d) const
        {
            float p = (float) w - d;
            if (p < 0.0f) p += (float) size;
            const int i = (int) p;
            const float f = p - (float) i;
            const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }
        inline float lineFor (float period, float d, bool freeze) const
        {
            return maxf (3.0f, freeze ? period : period - 1.0f - 0.5f * d + dcLead * period * period);
        }
        // in = input + the loop's feedback; out = the string. (The newest sample in the line is one old,
        // the feedback arrives a sample later and the damping filter adds its own delay: all taken off.)
        inline float line (float in, bool freeze, bool fadeMode, float d)
        {
            if (! fadeMode) { fading = false; xf = 1; dA += (dTarget - dA) * slewA; }
            else if (! fading && fabsf (dTarget - dA) > 0.02f) { dB = dTarget; fading = true; xf = 0; }
            float y = readAt (lineFor (dA, d, freeze));
            if (fading)
            {
                const float yb = readAt (lineFor (dB, d, freeze));
                xf += xfStep;
                if (xf >= 1.0f) { xf = 1.0f; dA = dB; fading = false; y = yb; }
                else y = y * fadeOut (xf) + yb * fadeIn (xf);
            }
            buf[w] = freeze ? y : in;      // held: the string goes round on its own, undamped
            if (++w >= size) w = 0;
            if (freeze) return y;
            const float o = y + (lp - y) * (0.5f * d);
            lp = y;
            return dc.process (o);
        }
        // the string inside its regeneration loop
        inline float process (float x, float regen, bool hpf, bool freeze, bool fadeMode, float d)
        {
            const float y = line (x + fb, freeze, fadeMode, d);
            const float f = hpf ? fbHP.process (y) : y;
            fb = freeze ? 0.0f : fastTanh (f) * regen;
            env += 0.004f * (fabsf (y) - env);
            return y;
        }
    };

    //==========================================================================
    // Lorenz attractor, integrated at control rate. speed = how fast it travels, chaos = rho
    // (just above 24.7 it barely holds a lobe, higher gets wilder, with periodic windows up
    // high), shape = beta.
    struct Lorenz
    {
        double x = 0.1, y = 0.0, z = 20.0;
        Rng rng { 0x10e7 };

        void step (double dt, double rho, double beta)
        {
            const double sigma = 10.0;
            int n = (int) ceil (dt / 0.004);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            const double h = dt / n;
            for (int i = 0; i < n; ++i)
            {
                double k1x, k1y, k1z, k2x, k2y, k2z, k3x, k3y, k3z, k4x, k4y, k4z;
                deriv (x, y, z, rho, beta, sigma, k1x, k1y, k1z);
                deriv (x + h * 0.5 * k1x, y + h * 0.5 * k1y, z + h * 0.5 * k1z, rho, beta, sigma, k2x, k2y, k2z);
                deriv (x + h * 0.5 * k2x, y + h * 0.5 * k2y, z + h * 0.5 * k2z, rho, beta, sigma, k3x, k3y, k3z);
                deriv (x + h * k3x, y + h * k3y, z + h * k3z, rho, beta, sigma, k4x, k4y, k4z);
                x += h / 6.0 * (k1x + 2 * k2x + 2 * k3x + k4x);
                y += h / 6.0 * (k1y + 2 * k2y + 2 * k3y + k4y);
                z += h / 6.0 * (k1z + 2 * k2z + 2 * k3z + k4z);
            }
            // a whisper of noise so it never settles for good
            x += ((double) rng.nextFloat() - 0.5) * 0.002;
            if (! (x == x) || ! (y == y) || ! (z == z) || fabs (x) > 1e4 || fabs (y) > 1e4 || fabs (z) > 1e4) { x = 0.1; y = 0; z = 20; }
        }
        static inline void deriv (double a, double b, double c, double rho, double beta, double sigma, double& da, double& db, double& dc)
        {
            da = sigma * (b - a); db = a * (rho - c) - b; dc = a * b - beta * c;
        }
        // roughly -1..1
        float outX (double rho, double beta) const { const float s = (float) (beta * (rho - 1.0)); return tanhf ((float) x / (1.4f * sqrtf (s > 1.0f ? s : 1.0f))); }
        float outY (double rho, double beta) const { const float s = (float) (beta * (rho - 1.0)); return tanhf ((float) y / (1.9f * sqrtf (s > 1.0f ? s : 1.0f))); }
        float outZ (double rho) const { return (float) clampd ((z - (rho - 1.0)) / (rho > 1.0 ? rho : 1.0)); }
        static inline double clampd (double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }
    };
}
