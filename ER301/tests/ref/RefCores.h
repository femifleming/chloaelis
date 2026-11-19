#pragma once
// The whole of each ER-301 unit except the SDK plumbing, so the same code runs in the
// offline tests on a desktop machine. Block based: n samples in, n samples out.
#include "RefDsp.h"

namespace chlref
{
    // ER-301 signals are 1.0 = 10 V, so ordinary +-5 V audio peaks at 0.5. The plug-in's
    // saturation points were voiced for +-1.0, so the units lift the signal on the way in and
    // drop it again on the way out.
    constexpr float kIn = 2.0f, kOut = 0.5f;

    struct Ramp
    {
        float cur = 0, step = 0;
        bool primed = false;
        inline void to (float target, int n)
        {
            if (! primed) { cur = target; primed = true; }
            step = (target - cur) / (float) n;
        }
        inline float next() { cur += step; return cur; }
    };

    //==========================================================================
    // A clocked chip (PT2399 or BBD) in its regeneration loop. Slew mode: time changes glide
    // (the clock moves, so the echoes bend in pitch). Fade mode: the new time is set up on a
    // second chip and crossfaded in once it has filled, so nothing changes pitch.
    template <typename Line>
    struct ChipDelay
    {
        Line ln[2];
        int act = 0, waitN = 0;
        bool transition = false, started = false;
        float sr = 48000.0f, xf = 1, xfStep = 0.001f, lineTime[2] { 0.33f, 0.33f }, fb = 0, env = 0;
        bool frozenNow = false;
        Biquad fbHP;
        Ramp regenR, dryR, wetR;

        void prepare (float s, uint32_t seed)
        {
            sr = s;
            ln[0].prepare (sr); ln[1].prepare (sr);
            ln[0].rng.setSeed (seed + 3); ln[1].rng.setSeed (seed + 7);
            fbHP.highpass (320.0f, sr, 0.6f);
            xfStep = 1.0f / (0.05f * sr);
            fb = 0; act = 0; transition = false; xf = 1; waitN = 0; started = false;
        }

        inline float runLines (float in, float timeSec, bool freeze, bool fadeMode)
        {
            if (! fadeMode)
            {
                transition = false;
                ln[act].setTime (timeSec, false);
                lineTime[act] = timeSec;
                return ln[act].process (in, freeze);
            }
            const int other = 1 - act;
            if (! transition && ! freeze && fabsf (timeSec - lineTime[act]) > 0.01f * lineTime[act])
            {
                lineTime[other] = timeSec;
                ln[other].setTime (timeSec, true);
                waitN = (int) (timeSec * sr) + 96;
                transition = true; xf = 0;
            }
            if (transition && freeze && waitN > 0) transition = false;     // don't fade into a half-filled chip

            ln[act].setTime (lineTime[act], false);
            const float ya = ln[act].process (in, freeze);
            if (! transition) return ya;
            const float yb = ln[other].process (in, freeze);
            if (waitN > 0) { --waitN; return ya; }
            xf += xfStep;
            if (xf >= 1.0f) { xf = 1.0f; act = other; transition = false; return yb; }
            return ya * fadeOut (xf) + yb * fadeIn (xf);
        }

        // freezeGate: per-sample gate (> 0.5 = frozen), or null
        void process (const float* in, const float* freezeGate, float* out, int n,
                      float timeSec, float regen, float wet, bool hpf, bool fadeMode)
        {
            timeSec = clampf (0.03f, 1.0f, timeSec);
            regen = clampf (0.0f, 1.15f, regen);
            wet = clampf (0.0f, 1.0f, wet);
            if (! started)
            {
                started = true;
                ln[0].setTime (timeSec, true); ln[1].setTime (timeSec, true);
                lineTime[0] = lineTime[1] = timeSec;
            }
            ln[0].updateCoeffs(); ln[1].updateCoeffs();
            regenR.to (regen, n);
            dryR.to (fadeOut (wet), n);
            wetR.to (fadeIn (wet) * kOut, n);
            for (int i = 0; i < n; ++i)
            {
                const bool freeze = freezeGate != nullptr && freezeGate[i] > 0.5f;
                const float dry = in[i];
                const float y = runLines (dry * kIn + fb, timeSec, freeze, fadeMode);
                const float f = hpf ? fbHP.process (y) : y;
                fb = freeze ? 0.0f : 1.05f * fastTanh (f * regenR.next());
                env += 0.004f * (fabsf (y) - env);
                frozenNow = freeze;
                out[i] = dry * dryR.next() + y * wetR.next();
            }
        }
    };

    //==========================================================================
    // The tape loop in its regeneration loop. The tape wears as it runs (ten minutes from new
    // to ruined); "age" adds wear on top by hand or by CV; a refresh puts on new tape.
    struct TapeDelay
    {
        TapeLine tp;
        float sr = 48000.0f, fb = 0, runAge = 0, lastRefresh = 0, env = 0;
        bool frozenNow = false;
        bool started = false;
        Biquad fbHP;
        Ramp regenR, dryR, wetR;
        static constexpr float kLifeSec = 600.0f;

        void prepare (float s, uint32_t seed)
        {
            sr = s;
            tp.prepare (sr, 1.75f, seed);
            fbHP.highpass (320.0f, sr, 0.6f);
            fb = 0; started = false; runAge = 0;
        }
        float age() const { return tp.age; }

        void process (const float* in, const float* freezeGate, const float* refreshGate, float* out, int n,
                      float timeSec, float regen, float wet, float ageAdd, bool ageing, bool hpf, bool fadeMode)
        {
            timeSec = clampf (0.03f, 1.0f, timeSec);
            regen = clampf (0.0f, 1.15f, regen);
            wet = clampf (0.0f, 1.0f, wet);
            const float dt = (float) n / sr;
            if (refreshGate != nullptr)
                for (int i = 0; i < n; ++i)
                {
                    if (refreshGate[i] > 0.5f && lastRefresh <= 0.5f) { runAge = 0; tp.refresh(); }
                    lastRefresh = refreshGate[i];
                }
            if (ageing) runAge = minf (1.0f, runAge + dt / kLifeSec);
            tp.age = clampf (0.0f, 1.0f, runAge + ageAdd);
            tp.wear (dt);
            tp.setTime (timeSec, ! started);
            started = true;
            regenR.to (regen, n);
            dryR.to (fadeOut (wet), n);
            wetR.to (fadeIn (wet) * kOut, n);
            for (int i = 0; i < n; ++i)
            {
                const bool freeze = freezeGate != nullptr && freezeGate[i] > 0.5f;
                const float dry = in[i];
                const float y = tp.process (dry * kIn + fb, freeze, fadeMode);
                const float f = hpf ? fbHP.process (y) : y;
                fb = freeze ? 0.0f : 1.05f * fastTanh (f * regenR.next());
                env += 0.004f * (fabsf (y) - env);
                frozenNow = freeze;
                out[i] = dry * dryR.next() + y * wetR.next();
            }
        }
    };

    //==========================================================================
    struct FilterCore
    {
        MultiModeFilter f;
        float g = 0.1f, env = 0, hzNow = 440.0f;
        bool primed = false;
        Ramp resR, morphR;

        void prepare (float sr) { f.prepare (sr); primed = false; }

        // f0: Hz per sample, voct: ER-301 pitch signal (1.0 = 10 octaves) per sample, either may be null
        void process (const float* in, const float* voct, const float* f0, float* out, int n,
                      float fixedF0, float res, float morph)
        {
            resR.to (clampf (0.0f, 1.0f, res), n);
            morphR.to (clampf (0.0f, 3.0f, morph), n);
            for (int i = 0; i < n; i += 4)
            {
                const int j = i + 3 < n ? i + 3 : n - 1;        // aim at the end of this group of four
                float hz = f0 != nullptr ? f0[j] : fixedF0;
                if (voct != nullptr) hz *= exp2f (clampf (-1.0f, 1.0f, voct[j]) * 10.0f);
                const float gT = f.gFor (hz);
                hzNow = hz;
                if (! primed) { g = gT; primed = true; }
                const int m = n - i < 4 ? n - i : 4;
                const float dg = (gT - g) / (float) m;
                for (int k = 0; k < m; ++k)
                {
                    g += dg;
                    const float r = resR.next(), mo = morphR.next();
                    const float y = f.process (in[i + k] * kIn, g, r, mo);
                    env += 0.004f * (fabsf (y) - env);
                    out[i + k] = y * kOut;
                }
            }
        }
    };

    //==========================================================================
    struct LorenzCore
    {
        Lorenz lz;
        float last[3] { 0, 0, 0 };
        float sr = 48000.0f;

        void prepare (float s) { sr = s; }

        // speed, chaos 0..1; shape 0.5..4; axis 0..2 picks what goes to out1 (out2 gets the next one)
        void process (float* out1, float* out2, int n, float speed, float chaos, float shape, float level, int axis)
        {
            speed = clampf (0.0f, 1.0f, speed);
            chaos = clampf (0.0f, 1.0f, chaos);
            const double rho = 24.5 + (double) chaos * chaos * 155.5, beta = clampf (0.5f, 4.0f, shape);
            const double rate = 0.03 * (double) exp2f (speed * 9.0f);      // attractor time units per second
            lz.step (rate * (double) n / sr, rho, beta);
            const float now[3] = { lz.outX (rho, beta), lz.outY (rho, beta), lz.outZ (rho) };
            const int a = axis < 0 ? 0 : (axis > 2 ? 2 : axis), b = (a + 1) % 3;
            const float inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1) * inv;
                if (out1 != nullptr) out1[i] = (last[a] + (now[a] - last[a]) * t) * level;
                if (out2 != nullptr) out2[i] = (last[b] + (now[b] - last[b]) * t) * level;
            }
            last[0] = now[0]; last[1] = now[1]; last[2] = now[2];
        }
    };

    //==========================================================================
    // A scale as the ER-301 hands it over: cents above 1/1, the largest being the period.
    struct ScaleTable
    {
        static constexpr int kMax = 1024;
        float deg[kMax + 1];        // within one period, ascending, deg[0] = 0
        int count = 1;
        float period = 1200.0f;

        ScaleTable() { setEqual (12); }
        void setEqual (int steps) { count = steps; period = 1200.0f; for (int i = 0; i < steps; ++i) deg[i] = 1200.0f * (float) i / (float) steps; }
        void begin() { count = 1; deg[0] = 0.0f; period = 0.0f; }
        void add (float cents)
        {
            if (! (cents > 0.5f) || cents > 12000.0f || count > kMax) return;
            int i = count++;                          // insertion sort, the lists are short
            while (i > 1 && deg[i - 1] > cents) { deg[i] = deg[i - 1]; --i; }
            deg[i] = cents;
        }
        void end()
        {
            if (count < 2) { setEqual (12); return; }
            period = deg[count - 1];                  // the largest pitch closes the scale
            --count;
        }
        // nearest degree in any period: returns the cents, and which degree it was
        float quantise (float cents, int& degree) const
        {
            float best = 0.0f, bestDist = 1.0e9f;
            degree = 0;
            const int base = (int) floorf (cents / period);
            for (int p = base - 1; p <= base + 1; ++p)
                for (int i = 0; i < count; ++i)
                {
                    const float cand = (float) p * period + deg[i];
                    const float d = fabsf (cand - cents);
                    if (d < bestDist) { bestDist = d; best = cand; degree = i; }
                }
            return best;
        }
        // cents from degree d up to the degree `steps` further on (wrapping into later periods)
        float stepsAbove (int d, int steps) const
        {
            const int idx = d + steps;
            return (float) (idx / count) * period + deg[idx % count] - deg[d];
        }
    };

    //==========================================================================
    // The string. Pitch = fundamental * 2^(V/Oct), snapped to the scale (1/1 = the fundamental).
    // Poly fades in up to seven more strings, one after another, on other degrees of the scale
    // stacked up from the note played (side 0: 2, 4, 6 .. degrees up; side 1: 1, 3, 5 ..), so the
    // two channels of a stereo unit make one chord between them.
    struct StringCore
    {
        static constexpr int kTaps = 7;
        KSLine main, taps[kTaps];
        bool tapRunning[kTaps] {};
        float tapMul[kTaps] { 1, 1, 1, 1, 1, 1, 1 };
        ScaleTable scales[2];
        volatile int activeScale = 0;
        int seenScale = -1, scaleSerial = 0, seenSerial = -1;
        float sr = 48000.0f, noteHz = 110.0f, period = 1.0f / 110.0f, lastCents = 1.0e9f, lastF0 = -1.0f, polyLast = 0.0f;
        int lastSide = -1;
        bool started = false, frozenNow = false;
        Ramp regenR, dryR, wetR;

        void prepare (float s)
        {
            sr = s;
            main.prepare (sr);
            for (int k = 0; k < kTaps; ++k) { taps[k].prepare (sr); tapRunning[k] = false; }
            started = false; lastCents = 1.0e9f;
        }

        // UI thread: fill the spare table, then swap
        void beginScale() { scales[1 - activeScale].begin(); }
        void addPitch (float cents) { scales[1 - activeScale].add (cents); }
        void endScale() { scales[1 - activeScale].end(); activeScale = 1 - activeScale; ++scaleSerial; }

        static inline float tapLevel (int k, float width) { return clampf (0.0f, 1.0f, width * (float) kTaps - (float) k); }
        // the string's sustain: most of the knob's travel is long rings; past 100% it sings on its own
        static inline float ksRegen (float r) { return r < 1.0f ? powf (maxf (0.0f, r), 0.08f) : r; }

        void retune (float f0, float voct, int side)
        {
            const ScaleTable& sc = scales[activeScale];
            const float cents = clampf (-1.0f, 1.0f, voct) * 12000.0f;
            if (cents == lastCents && f0 == lastF0 && side == lastSide && seenSerial == scaleSerial) return;
            lastCents = cents; lastF0 = f0; lastSide = side; seenSerial = scaleSerial;
            int degree = 0;
            float q = sc.quantise (cents, degree);
            float hz = f0 * exp2f (q * (1.0f / 1200.0f));
            // keep it inside what the line can play: a note outside moves a period in
            const float per = exp2f (maxf (50.0f, sc.period) * (1.0f / 1200.0f));
            for (int guard = 0; guard < 64 && hz > 4000.0f; ++guard) hz /= per;
            for (int guard = 0; guard < 64 && hz < 16.0f; ++guard) hz *= per;
            noteHz = hz;
            period = 1.0f / hz;
            for (int k = 0; k < kTaps; ++k)
            {
                float r = exp2f (sc.stepsAbove (degree, side == 0 ? 2 * (k + 1) : 2 * k + 1) * (1.0f / 1200.0f));
                while (hz * r > 5000.0f) r *= 0.5f;        // too high for the line: an octave down
                tapMul[k] = 1.0f / r;
            }
        }

        // voct: the first sample of the block is used (a snapped pitch has no use for more)
        void process (const float* in, const float* voct, const float* freezeGate, float* out, int n,
                      float f0, float sustain, float poly, float wet, int dampType, bool hpf, bool fadeMode, int side)
        {
            f0 = clampf (8.0f, 4000.0f, f0);
            poly = clampf (0.0f, 1.0f, poly);
            wet = clampf (0.0f, 1.0f, wet);
            retune (f0, voct != nullptr ? voct[0] : 0.0f, side);
            const float d = KSLine::dampFor (dampType);
            main.setPeriod (period, ! started);
            started = true;

            bool tapOn[kTaps];
            bool anyTap = false;
            for (int k = 0; k < kTaps; ++k)
            {
                tapOn[k] = tapLevel (k, polyLast) > 0.0005f || tapLevel (k, poly) > 0.0005f;
                if (tapOn[k] && ! tapRunning[k]) { taps[k].reset(); taps[k].setPeriod (period * tapMul[k], true); }
                else if (tapOn[k]) taps[k].setPeriod (period * tapMul[k], false);
                tapRunning[k] = tapOn[k];
                anyTap = anyTap || tapOn[k];
            }

            regenR.to (ksRegen (clampf (0.0f, 1.15f, sustain)), n);
            dryR.to (fadeOut (wet), n);
            wetR.to (fadeIn (wet) * kOut, n);
            const float inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const bool freeze = freezeGate != nullptr && freezeGate[i] > 0.5f;
                frozenNow = freeze;
                const float dry = in[i], x = dry * kIn;
                const float rg = regenR.next();
                float y = main.process (x, rg, hpf, freeze, fadeMode, d);
                if (anyTap)
                {
                    const float wd = polyLast + (poly - polyLast) * ((float) (i + 1) * inv);
                    float sum = y, lv = 0.0f;
                    for (int k = 0; k < kTaps; ++k)
                    {
                        if (! tapOn[k]) continue;
                        const float l = tapLevel (k, wd);
                        sum += taps[k].process (x, rg * (1.0f - 0.0015f * (float) (k + 1)), hpf, freeze, fadeMode, d) * l;
                        lv += l;
                    }
                    y = sum / (1.0f + 0.3f * lv);
                }
                out[i] = dry * dryR.next() + y * wetR.next();
            }
            polyLast = poly;
        }
    };
}
