#pragma once
// The whole of each ER-301 unit except the SDK plumbing, so the same code runs in the
// offline tests on a desktop machine. Block based: n samples in, n samples out, left and right
// together (the right input and output may be null for a mono unit). n is a multiple of 4 and
// at most kMaxBlock.
#include "ChloeLines.h"

namespace chl
{
    // ER-301 signals are 1.0 = 10 V, so ordinary +-5 V audio peaks at 0.5. The plug-in's
    // saturation points were voiced for +-1.0, so the units lift the signal on the way in and
    // drop it again on the way out.
    constexpr float kIn = 2.0f, kOut = 0.5f;


    //==========================================================================
    // What the three delays share: the regeneration and the dry / wet mix as vectors that move in
    // a straight line across a block (and only if they moved), kept beside the unit's state.
    struct LoopMix
    {
        float fb[4], yPrev[4], gain[4], dGain[4], dry[4], dDry[4], wet[4], dWet[4], peak[4];
        float lastRegen = 0, lastWet = -1.0f, regenT = 0, wetAmountT = 0, lineGainT = 1;
        bool primed = false, moving = false;

        void reset() { set4 (fb, 0); set4 (yPrev, 0); set4 (peak, 0); primed = false; }
        void begin (float regen, float wetAmount, float lineGain, int n)
        {
            set4 (peak, 0);
            moving = ! primed || bitsOf (regen) != bitsOf (lastRegen) || bitsOf (wetAmount) != bitsOf (lastWet);
            if (! moving) return;
            regenT = regen; wetAmountT = wetAmount; lineGainT = lineGain;
            const float dryT = fadeOut (wetAmount), wetT = fadeIn (wetAmount) * kOut;
            if (! primed) { set22 (gain, regen, lineGain); set4 (dry, dryT); set4 (wet, wetT); lastRegen = regen; lastWet = wetAmount; primed = true; }
            const float inv = 1.0f / (float) n;
            set22 (dGain, (regen - gain[0]) * inv, 0.0f); set4 (dDry, (dryT - dry[0]) * inv); set4 (dWet, (wetT - wet[0]) * inv);
        }
        void end()
        {
            if (! moving) return;
            set22 (gain, regenT, lineGainT); set4 (dry, fadeOut (wetAmountT)); set4 (wet, fadeIn (wetAmountT) * kOut);
            lastRegen = regenT; lastWet = wetAmountT;
        }
        // what goes to the line: {the loop signal, input + feedback}
        CHL_INLINE f4 feed (const float* inL, const float* inR) const
        {
            return highLow (ld (yPrev), mla (ld (fb), ldPair (inL, inR, splat (0.0f)), splat (kIn)));
        }
        // the line's output y (lanes 2,3) mixed with the dry sound and written out
        CHL_INLINE void mix (f4 y, const float* inL, const float* inR, float* outL, float* outR)
        {
            st (yPrev, y);
            const f4 o = mla (dupLow (ldPair (inL, inR, splat (0.0f))) * ld (dry), y, ld (wet));
            stLane (outL, o, 2);
            if (outR != nullptr) stLane (outR, o, 3);
            if (moving) { st (gain, ld (gain) + ld (dGain)); st (dry, ld (dry) + ld (dDry)); st (wet, ld (wet) + ld (dWet)); }
        }
        CHL_INLINE void look() { st (peak, max4 (ld (peak), abs4 (ld (yPrev)))); }      // every 16 samples, for the display
        float level() const { return maxf (peak[2], peak[3]); }
    };

    //==========================================================================
    // A clocked chip (PT2399 or BBD) in its regeneration loop. Slew mode: time changes glide
    // (the clock moves, so the echoes bend in pitch). Fade mode: the new time is set up on a
    // second chip and crossfaded in once it has filled, so nothing changes pitch.
    template <typename Line>
    struct ChipDelay
    {
        Line ln[2];
        LoopMix mx;
        int act = 0, waitN = 0, xfN = 0, xfLen = 2400;
        bool transition = false, started = false, frozenNow = false, lastHpf = false;
        float sr = 48000.0f, lineTime[2] { 0.33f, 0.33f }, env = 0;
        float gFade[4], dFade[4];           // lanes 0,1: the old chip's gain; 2,3: the new chip's
        BiquadCoef hp;

        void prepare (float s, uint32_t seed)
        {
            sr = s;
            ln[0].prepare (sr); ln[1].prepare (sr);
            ln[0].seed (seed + 3); ln[1].seed (seed + 7);
            hp.highpass (320.0f, sr, 0.6f);
            xfLen = ((int) (0.05f * sr) / kChunk) * kChunk;
            mx.reset();
            set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
            act = 0; transition = false; waitN = 0; started = false;
        }

        // freezeGate: per-sample gate (> 0.5 = frozen), or null
        void process (const float* inL, const float* inR, const float* freezeGate, float* outL, float* outR, int n,
                      float timeSec, float regen, float wetAmount, bool hpf, bool fadeMode)
        {
            if (inR == nullptr) inR = silence();
            timeSec = clampf (0.03f, 1.0f, timeSec);
            regen = clampf (0.0f, 1.15f, regen);
            wetAmount = clampf (0.0f, 1.0f, wetAmount);
            if (! started)
            {
                started = true;
                ln[0].setTime (timeSec, true); ln[1].setTime (timeSec, true);
                lineTime[0] = lineTime[1] = timeSec;
            }
            if (! mx.primed || hpf != lastHpf)
            {
                BiquadCoef c;
                if (hpf) c = hp;
                ln[0].setLoopHP (c); ln[1].setLoopHP (c);
                lastHpf = hpf;
            }
            // the time control, once a block
            if (! fadeMode)
            {
                transition = false;
                ln[act].setTime (timeSec, false);
                lineTime[act] = timeSec;
            }
            else
            {
                const int other = 1 - act;
                if (! transition && ! frozenNow && fabsf (timeSec - lineTime[act]) > 0.01f * lineTime[act])
                {
                    lineTime[other] = timeSec;
                    ln[other].setTime (timeSec, true);
                    waitN = (((int) (timeSec * sr) + 96) / kChunk + 1) * kChunk;        // the new chip has to fill first
                    transition = true; xfN = 0;
                }
                if (transition && frozenNow && waitN > 0) transition = false;           // don't fade into a half-filled chip
                ln[act].setTime (lineTime[act], false);
            }
            ln[0].filters(); ln[1].filters();
            const bool gated = freezeGate != nullptr && ! allZero (freezeGate, n);
            mx.begin (regen, wetAmount, Line::kLineGain, n);

            for (int c = 0; c < n; c += kChunk)
            {
                ln[act].control();
                bool xfade = false;
                if (transition)
                {
                    ln[1 - act].control();
                    if (waitN <= 0)
                    {
                        const float t0 = (float) xfN / (float) xfLen, t1 = (float) (xfN + kChunk) / (float) xfLen;
                        set22 (gFade, fadeOut (t0), fadeIn (t0));
                        set22 (dFade, (fadeOut (t1) - fadeOut (t0)) * (1.0f / (float) kChunk), (fadeIn (t1) - fadeIn (t0)) * (1.0f / (float) kChunk));
                        xfade = true;
                    }
                }
                const int e = c + kChunk < n ? c + kChunk : n;
                Line& a = ln[act];
                Line& b = ln[1 - act];
                for (int i = c; i < e; ++i)
                {
                    const bool freeze = gated && gateHigh (freezeGate[i]);
                    f4 y = a.tick (mx.feed (inL + i, inR + i), ld (mx.gain), freeze);
                    if (transition)
                    {
                        const f4 yb = b.tick (mx.feed (inL + i, inR + i), ld (mx.gain), freeze);
                        if (xfade)
                        {
                            const f4 g = ld (gFade);
                            y = mla (y * dupLow (g), yb, dupHigh (g));
                            st (gFade, g + ld (dFade));
                        }
                    }
                    if (freeze) set4 (mx.fb, 0.0f); else st (mx.fb, ld (a.fbOut));
                    mx.mix (y, inL + i, inR + i, outL + i, outR != nullptr ? outR + i : nullptr);
                    frozenNow = freeze;
                }
                mx.look();
                if (transition)
                {
                    if (waitN > 0) waitN -= kChunk;
                    else { xfN += kChunk; if (xfN >= xfLen) { act = 1 - act; transition = false; } }
                }
            }
            mx.end();
            env += 0.25f * (mx.level() * 0.6f - env);
        }
    };

    //==========================================================================
    // The tape loop in its regeneration loop. The tape wears as it runs (ten minutes from new
    // to ruined); "age" adds wear on top by hand or by CV; a refresh puts on new tape.
    struct TapeDelay
    {
        TapeLine tp;
        LoopMix mx;
        float sr = 48000.0f, runAge = 0, env = 0;
        uint32_t lastRefresh = 0;
        bool frozenNow = false, started = false, lastHpf = false;
        BiquadCoef hp;
        static constexpr float kLifeSec = 600.0f;

        void prepare (float s, uint32_t seed)
        {
            sr = s;
            tp.prepare (sr, 1.75f, seed);
            hp.highpass (320.0f, sr, 0.6f);
            mx.reset();
            started = false; runAge = 0;
        }
        float age() const { return tp.age; }

        void process (const float* inL, const float* inR, const float* freezeGate, const float* refreshGate, float* outL, float* outR, int n,
                      float timeSec, float regen, float wetAmount, float ageAdd, bool ageing, bool hpf, bool fadeMode)
        {
            if (inR == nullptr) inR = silence();
            timeSec = clampf (0.03f, 1.0f, timeSec);
            regen = clampf (0.0f, 1.15f, regen);
            wetAmount = clampf (0.0f, 1.0f, wetAmount);
            if (refreshGate != nullptr && (lastRefresh != 0 || ! allZero (refreshGate, n)))
                for (int i = 0; i < n; ++i)
                {
                    const uint32_t g = gateHigh (refreshGate[i]) ? 1u : 0u;
                    if (g != 0 && lastRefresh == 0) { runAge = 0; tp.refresh(); }
                    lastRefresh = g;
                }
            if (ageing) runAge = minf (1.0f, runAge + (float) n / (sr * kLifeSec));
            tp.age = clampf (0.0f, 1.0f, runAge + ageAdd);
            tp.block (n);
            tp.setTime (timeSec, ! started);
            started = true;
            if (! mx.primed || hpf != lastHpf)
            {
                BiquadCoef c;
                if (hpf) c = hp;
                tp.setLoopHP (c);
                lastHpf = hpf;
            }
            const bool gated = freezeGate != nullptr && ! allZero (freezeGate, n);
            mx.begin (regen, wetAmount, TapeLine::kLineGain, n);

            for (int c = 0; c < n; c += kChunk)
            {
                tp.control (fadeMode, gated && gateHigh (freezeGate[c]));
                const bool freeze = tp.frozen;
                const int e = c + kChunk < n ? c + kChunk : n;
                for (int i = c; i < e; ++i)
                {
                    const f4 y = tp.tick (mx.feed (inL + i, inR + i), ld (mx.gain), i);
                    if (freeze) set4 (mx.fb, 0.0f); else st (mx.fb, ld (tp.fbOut));
                    mx.mix (y, inL + i, inR + i, outL + i, outR != nullptr ? outR + i : nullptr);
                }
                tp.controlEnd();
                mx.look();
                frozenNow = freeze;
            }
            tp.blockEnd();
            mx.end();
            env += 0.25f * (mx.level() * 0.6f - env);
        }
    };

    //==========================================================================
    // Analog-flavoured state variable filter (Zavalishin TPT topology) with a saturating
    // integrator, continuously morphing LP > BP > HP > Notch. The cutoff (Hz per sample, times
    // 2^(10 V/Oct)) is read every four samples; while neither it nor the resonance nor the mode is
    // moving, no coefficient is recomputed.
    struct FilterCore
    {
        float sr = 48000.0f, env = 0, hzNow = 440.0f, lastRes = 0, lastMorph = 0;
        float ic1[4], ic2[4];
        float coef[6][4];       // a1 a2 a3, and the weights of input, low-pass and band-pass that make the mode
        float dco[6][4];        // their change per sample while something moves
        float tgt[6][4];        // where they are going, for the next four groups of four samples
        float xs[2][kMaxBlock]; // the input after its saturating stage
        float tmpA[4], tmpB[4];
        uint32_t lastF0 = 0, lastVoct = 0;
        bool primed = false;
        int idle = 0;

        void prepare (float s)
        {
            sr = s; primed = false; idle = 0;
            set4 (ic1, 0); set4 (ic2, 0);
            memset (coef, 0, sizeof (coef)); memset (dco, 0, sizeof (dco)); memset (tgt, 0, sizeof (tgt)); memset (xs, 0, sizeof (xs));
        }

        // the input stage, for a whole block
        CHL_NOINLINE void saturate (const float* in, float* out, int n) const
        {
            const f4 a = splat (kIn * 0.6f), b = splat (1.0f / 0.6f);
            for (int i = 0; i < n; i += 4) st (out + i, tanh4 (ld (in + i) * a) * b);
        }
        // where the coefficients should be at the end of each of the next four groups of four
        // samples: tmpA holds the four cutoffs in Hz, tmpB the four pitch signals
        CHL_NOINLINE void aim (float groupIndex, float invGroups, float dRes, float dMorph)
        {
            const f4 hz = ld (tmpA) * exp2x4 (max4 (min4 (ld (tmpB), splat (1.0f)), splat (-1.0f)) * splat (10.0f));
            st (tmpA, hz);
            const f4 g = tan4 (max4 (min4 (hz, splat (sr * 0.46f)), splat (12.0f)) * splat (pi / sr));
            const f4 t = (splat (groupIndex) + make4 (1.0f, 2.0f, 3.0f, 4.0f)) * splat (invGroups);
            const f4 k = mls (splat (2.0f), splat (1.96f), mla (splat (lastRes), splat (dRes), t));
            const f4 b1 = recip2 (mla (splat (1.0f), g, g + k)), b2 = g * b1;
            st (tgt[0], b1); st (tgt[1], b2); st (tgt[2], g * b2);
            st (tmpB, k);
            aimMode (t, dMorph);
        }
        CHL_NOINLINE void aimMode (f4 t, float dMorph)
        {
            const f4 m = mla (splat (lastMorph), splat (dMorph), t), k = ld (tmpB);
            const f4 zero = splat (0.0f), one = splat (1.0f), ko = splat (kOut);
            const f4 wl = max4 (one - m, zero) + max4 (m - splat (2.0f), zero);
            const f4 wb = max4 (one - abs4 (m - one), zero);
            const f4 wh = min4 (max4 (m - one, zero), one);
            st (tgt[3], wh * ko); st (tgt[4], (wl - wh) * ko); st (tgt[5], (wb - wh) * k * ko);
        }
        // n samples with the coefficients as they stand (moving: each sample they step by dco)
        CHL_NOINLINE void run (int i0, int n, float* outL, float* outR, bool moving)
        {
            f4 s1 = ld (ic1), s2 = ld (ic2);
            for (int i = i0; i < i0 + n; ++i)
            {
                if (moving) for (int k = 0; k < 6; ++k) st (coef[k], ld (coef[k]) + ld (dco[k]));
                const f4 x = ldPair (xs[0] + i, xs[1] + i, splat (0.0f));
                const f4 v3 = x - s2, a2 = ld (coef[1]);
                const f4 v1 = mla (ld (coef[0]) * s1, a2, v3);
                const f4 v2 = mla (mla (s2, a2, s1), ld (coef[2]), v3);
                s2 = v2 + v2 - s2;
                s1 = tanh4 ((v1 + v1 - s1) * splat (1.0f / 3.0f)) * splat (3.0f);          // OTA-ish soft limit on the band integrator
                const f4 y = mla (mla (ld (coef[3]) * x, ld (coef[4]), v2), ld (coef[5]), v1);
                if (outR != nullptr) stPair (outL + i, outR + i, y); else stLane (outL + i, y, 0);
            }
            st (ic1, s1); st (ic2, s2);
        }

        // f0: Hz per sample, voct: ER-301 pitch signal (1.0 = 10 octaves) per sample, either may be null
        void process (const float* inL, const float* inR, const float* voct, const float* f0, float* outL, float* outR, int n,
                      float fixedF0, float res, float morph)
        {
            res = clampf (0.0f, 1.0f, res);
            morph = clampf (0.0f, 3.0f, morph);
            const bool quiet = allZero (inL, n) && (inR == nullptr || allZero (inR, n));
            if (quiet && idle > 8)
            {
                // nothing coming in and the filter has rung out
                memset (outL, 0, sizeof (float) * (size_t) n);
                if (outR != nullptr) memset (outR, 0, sizeof (float) * (size_t) n);
                env *= 0.7f; lastRes = res; lastMorph = morph; primed = false;
                return;
            }
            if (! quiet) idle = 0;

            saturate (inL, xs[0], n);
            if (inR != nullptr) saturate (inR, xs[1], n);
            else memset (xs[1], 0, sizeof (float) * (size_t) n);
            if (! primed) { lastRes = res; lastMorph = morph; }
            const float dRes = res - lastRes, dMorph = morph - lastMorph, invGroups = 4.0f / (float) n;
            const bool knobs = bitsOf (dRes) != 0u || bitsOf (dMorph) != 0u;

            for (int c = 0; c < n; c += kChunk)
            {
                const int groups = (n - c) < kChunk ? (n - c) / 4 : 4;
                bool same = primed && ! knobs;
                for (int g = 0; g < 4; ++g)
                {
                    const int j = c + 4 * (g < groups ? g : groups - 1) + 3;      // aim at the end of each group of four
                    tmpA[g] = f0 != nullptr ? f0[j] : fixedF0;
                    tmpB[g] = voct != nullptr ? voct[j] : 0.0f;
                    same = same && bitsOf (tmpA[g]) == lastF0 && bitsOf (tmpB[g]) == lastVoct;
                }
                if (same) { run (c, 4 * groups, outL, outR, false); continue; }

                lastF0 = bitsOf (tmpA[groups - 1]); lastVoct = bitsOf (tmpB[groups - 1]);
                aim ((float) (c / 4), invGroups, dRes, dMorph);
                hzNow = tmpA[groups - 1];
                for (int g = 0; g < groups; ++g)
                {
                    if (! primed) { for (int k = 0; k < 6; ++k) set4 (coef[k], tgt[k][g]); primed = true; }
                    for (int k = 0; k < 6; ++k) st (dco[k], (ldSplat (tgt[k] + g) - ld (coef[k])) * splat (0.25f));
                    run (c + 4 * g, 4, outL, outR, true);
                    for (int k = 0; k < 6; ++k) set4 (coef[k], tgt[k][g]);
                }
            }
            lastRes = res; lastMorph = morph;
            const float p = maxf (peakAbs (outL, n), outR != nullptr ? peakAbs (outR, n) : 0.0f);
            env += 0.25f * (p * 1.2f - env);
            if (quiet && p < 1.0e-7f) ++idle;
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
            int n = (int) (dt * 250.0) + 1;          // steps of at most 0.004
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
        // tanh to about 1e-3, without the C library
        static inline float squash (float v)
        {
            const float v2 = v * v;
            const float t = v * (945.0f + v2 * (105.0f + v2)) / (945.0f + v2 * (420.0f + 15.0f * v2));
            return t > 1.0f ? 1.0f : (t < -1.0f ? -1.0f : t);
        }
        // roughly -1..1
        float outX (double rho, double beta) const { const float s = (float) (beta * (rho - 1.0)); return squash (clampf (-8.0f, 8.0f, (float) x / (1.4f * sqrtf (s > 1.0f ? s : 1.0f)))); }
        float outY (double rho, double beta) const { const float s = (float) (beta * (rho - 1.0)); return squash (clampf (-8.0f, 8.0f, (float) y / (1.9f * sqrtf (s > 1.0f ? s : 1.0f)))); }
        float outZ (double rho) const { return (float) clampd ((z - (rho - 1.0)) / (rho > 1.0 ? rho : 1.0)); }
        static inline double clampd (double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }
    };

    // a straight line from a to b over n samples (n a multiple of 4), four at a time
    static inline void rampOut (float* out, int n, float a, float b)
    {
        const float d = (b - a) / (float) n;
        f4 v = mla (splat (a), make4 (1.0f, 2.0f, 3.0f, 4.0f), splat (d));
        const f4 d4 = splat (4.0f * d);
        for (int i = 0; i < n; i += 4) { st (out + i, v); v = v + d4; }
    }

    struct LorenzCore
    {
        Lorenz lz;
        float last[3] { 0, 0, 0 };
        float sr = 48000.0f, lastSpeed = -1.0f, rate = 0.03f;

        void prepare (float s) { sr = s; lastSpeed = -1.0f; }

        // speed, chaos 0..1; shape 0.5..4; axis 0..2 picks what goes to out1 (out2 gets the next one)
        void process (float* out1, float* out2, int n, float speed, float chaos, float shape, float level, int axis)
        {
            speed = clampf (0.0f, 1.0f, speed);
            chaos = clampf (0.0f, 1.0f, chaos);
            const double rho = 24.5 + (double) chaos * chaos * 155.5, beta = clampf (0.5f, 4.0f, shape);
            if (speed != lastSpeed) { lastSpeed = speed; rate = 0.03f * fastExp2 (speed * 9.0f); }      // attractor time units per second
            lz.step ((double) (rate * (float) n / sr), rho, beta);
            const float now[3] = { lz.outX (rho, beta), lz.outY (rho, beta), lz.outZ (rho) };
            const int a = axis < 0 ? 0 : (axis > 2 ? 2 : axis), b = (a + 1) % 3;
            if (out1 != nullptr) rampOut (out1, n, last[a] * level, now[a] * level);
            if (out2 != nullptr) rampOut (out2, n, last[b] * level, now[b] * level);
            last[0] = now[0]; last[1] = now[1]; last[2] = now[2];
        }
    };

    //==========================================================================
    // A small flock (boids): separation, alignment and cohesion in a unit square with soft
    // walls. In the plug-in each boid steers one poly tap's delay time, so the taps drift,
    // cluster, scatter and regroup together instead of wobbling independently. Here there is one
    // flock for the whole module and every Flock unit follows one boid of it: put a unit in the
    // time sub-chain of each of several delays, give each a different boid, and they move as a
    // flock.
    //
    // It is stepped every 512 samples (a boid crosses the square in several seconds) and read in
    // between by a straight line. The step is worked in vectors of four boids.
    struct Flock
    {
        static constexpr int n = 14, kPad = 16, kStep = 512;
        float x[kPad], y[kPad], vx[kPad], vy[kPad];
        float px[kPad], py[kPad];           // where they were a step ago
        float cx[kPad], cy[kPad], avx[kPad], avy[kPad], sx[kPad], sy[kPad], cnt[kPad];
        uint32_t clock = 0, stepAt = 0;     // the furthest any unit has got, and when the positions in x, y are for
        float invSr = 1.0f / 48000.0f;

        void reset (float sr, unsigned seed = 0xb01d5u)
        {
            invSr = 1.0f / sr;
            Rng r (seed);
            for (int i = 0; i < kPad; ++i)
            {
                x[i] = r.nextFloat() * 2.0f - 1.0f; y[i] = r.nextFloat() * 2.0f - 1.0f;
                float s, c; sinCos (r.nextFloat() * twoPi, s, c);
                vx[i] = 0.2f * c; vy[i] = 0.2f * s;
                if (i >= n) { x[i] = 1000.0f + 10.0f * (float) i; y[i] = 1000.0f; vx[i] = vy[i] = 0.0f; }     // the padding: out of everyone's sight
                px[i] = x[i]; py[i] = y[i];
            }
            clock = 0; stepAt = 0;
        }

        // boid j, as seen by the four boids from i on
        CHL_NOINLINE void see (int j, int i)
        {
            constexpr float view = 0.6f, sep = 0.22f;
            const f4 xj = ldSplat (x + j), yj = ldSplat (y + j);
            const f4 dx = xj - ld (x + i), dy = yj - ld (y + i);
            const f4 d2 = mla (dx * dx, dy, dy);
            const u4 seen = and4 (gt4 (splat (view * view), d2), gt4 (d2, splat (0.0f)));       // near enough, and not itself
            st (cx + i, ld (cx + i) + flt (and4 (bits (xj), seen)));
            st (cy + i, ld (cy + i) + flt (and4 (bits (yj), seen)));
            st (avx + i, ld (avx + i) + flt (and4 (bits (ldSplat (vx + j)), seen)));
            st (avy + i, ld (avy + i) + flt (and4 (bits (ldSplat (vy + j)), seen)));
            st (cnt + i, ld (cnt + i) + flt (and4 (bits (splat (1.0f)), seen)));
            const u4 close = and4 (seen, gt4 (splat (sep * sep), d2));
            const f4 k = flt (and4 (bits (recip1 (max4 (d2, splat (0.0004f)))), close));
            st (sx + i, mls (ld (sx + i), dx, k));
            st (sy + i, mls (ld (sy + i), dy, k));
        }
        // what the four boids from i on want (into sx, sy): towards the middle of those in sight and
        // their heading, away from the ones too close, and back from the walls
        CHL_NOINLINE void steer (int i)
        {
            const f4 c = ld (cnt + i);
            const u4 any = gt4 (c, splat (0.5f));
            const f4 ic = flt (and4 (bits (recip1 (max4 (c, splat (1.0f)))), any));      // 1 / count, or 0 with nobody in sight
            const f4 has = flt (and4 (bits (splat (1.0f)), any));
            const f4 xi = ld (x + i), yi = ld (y + i);
            f4 ax = mla ((ld (cx + i) * ic - xi * has) * splat (0.5f), ld (avx + i) * ic - ld (vx + i) * has, splat (0.9f));
            f4 ay = mla ((ld (cy + i) * ic - yi * has) * splat (0.5f), ld (avy + i) * ic - ld (vy + i) * has, splat (0.9f));
            ax = mla (ax, ld (sx + i), splat (0.012f)); ay = mla (ay, ld (sy + i), splat (0.012f));
            // soft walls
            ax = mls (ax, max4 (xi - splat (0.85f), splat (0.0f)) + min4 (xi + splat (0.85f), splat (0.0f)), splat (6.0f));
            ay = mls (ay, max4 (yi - splat (0.85f), splat (0.0f)) + min4 (yi + splat (0.85f), splat (0.0f)), splat (6.0f));
            st (sx + i, ax); st (sy + i, ay);
        }
        // ... and where that takes them in dt seconds, their speed kept between its limits
        CHL_NOINLINE void move (int i, float dt)
        {
            constexpr float maxV = 0.42f, minV = 0.14f;
            const f4 vdt = splat (dt);
            f4 vxi = mla (ld (vx + i), ld (sx + i), vdt), vyi = mla (ld (vy + i), ld (sy + i), vdt);
            const f4 sp2 = max4 (mla (vxi * vxi, vyi, vyi), splat (1.0e-12f));
            const f4 isp = rsqrt1 (sp2);
            const f4 lim = min4 (max4 (sp2 * isp, splat (minV)), splat (maxV)) * isp;
            vxi = vxi * lim; vyi = vyi * lim;
            st (vx + i, vxi); st (vy + i, vyi);
            st (x + i, min4 (max4 (mla (ld (x + i), vxi, vdt), splat (-1.0f)), splat (1.0f)));
            st (y + i, min4 (max4 (mla (ld (y + i), vyi, vdt), splat (-1.0f)), splat (1.0f)));
        }
        void step (float dt)
        {
            memcpy (px, x, sizeof (px)); memcpy (py, y, sizeof (py));
            memset (cx, 0, sizeof (cx)); memset (cy, 0, sizeof (cy)); memset (avx, 0, sizeof (avx)); memset (avy, 0, sizeof (avy));
            memset (sx, 0, sizeof (sx)); memset (sy, 0, sizeof (sy)); memset (cnt, 0, sizeof (cnt));
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < kPad; i += 4) see (j, i);
            for (int i = 0; i < kPad; i += 4) steer (i);
            for (int i = 0; i < kPad; i += 4) move (i, dt);
            for (int i = n; i < kPad; ++i) { x[i] = px[i]; y[i] = py[i]; vx[i] = vy[i] = 0.0f; }
        }

        // A unit that has processed up to its own time `t` (in samples) asks for the flock to be that
        // far. Whichever unit gets furthest first does the stepping; the others find it done.
        void advanceTo (uint32_t t)
        {
            if ((int32_t) (t - clock) > 0) clock = t;
            int guard = 0;
            while ((int32_t) (clock - stepAt) > 0 && guard++ < 8) { step ((float) kStep * invSr); stepAt += (uint32_t) kStep; }
            if ((int32_t) (clock - stepAt) > 0) stepAt = clock;         // (a long gap: don't try to catch up)
        }
        // boid b's position at time t, between the last two steps: 0 = x, 1 = y
        float at (int b, int axis, uint32_t t) const
        {
            float f = 1.0f - (float) (int32_t) (stepAt - t) * (1.0f / (float) kStep);
            f = clampf (0.0f, 1.0f, f);
            const float* now = axis == 0 ? x : y;
            const float* was = axis == 0 ? px : py;
            return was[b] + (now[b] - was[b]) * f;
        }
        // the one flock (made by the first Flock unit, on the UI thread)
        static Flock*& shared() { static Flock* f = nullptr; return f; }
    };

    struct FlockCore
    {
        uint32_t t = 0;
        float last[2] { 0, 0 };
        bool primed = false;

        void prepare (float sr)
        {
            Flock*& f = Flock::shared();
            if (f == nullptr) { f = new Flock(); f->reset (sr); }
            t = f->clock; primed = false;
        }
        // boid 0..13; out1 gets its x (or y with axis 1), out2 the other
        void process (float* out1, float* out2, int n, int boid, float level, int axis)
        {
            Flock& f = *Flock::shared();
            boid = boid < 0 ? 0 : (boid >= Flock::n ? Flock::n - 1 : boid);
            if ((int32_t) (f.clock - t) > 4 * n) t = f.clock;              // this unit was asleep: rejoin the others
            t += (uint32_t) n;
            f.advanceTo (t);
            const int a = axis == 1 ? 1 : 0;
            const float now[2] = { f.at (boid, a, t) * level, f.at (boid, 1 - a, t) * level };
            if (! primed) { last[0] = now[0]; last[1] = now[1]; primed = true; }
            if (out1 != nullptr) rampOut (out1, n, last[0], now[0]);
            if (out2 != nullptr) rampOut (out2, n, last[1], now[1]);
            last[0] = now[0]; last[1] = now[1];
        }
    };
}
