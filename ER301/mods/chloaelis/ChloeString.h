#pragma once
// The Scale String: Karplus-Strong strings tuned to a scale.
#include "ChloeCores.h"

namespace chl
{
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
            float q = cents / period;
            int base = (int) q;
            if ((float) base > q) --base;                              // floor
            const float r = cents - (float) base * period;             // 0 .. period
            int lo = 0, hi = count - 1;                                // the last degree at or below r
            while (lo < hi) { const int mid = (lo + hi + 1) >> 1; if (deg[mid] <= r) lo = mid; else hi = mid - 1; }
            const float below = deg[lo], above = lo + 1 < count ? deg[lo + 1] : period;
            if (r - below <= above - r) { degree = lo; return (float) base * period + below; }
            if (lo + 1 < count) { degree = lo + 1; return (float) base * period + above; }
            degree = 0; return (float) (base + 1) * period;
        }
        // cents from degree d up to the degree `steps` further on (wrapping into later periods)
        float stepsAbove (int d, int steps) const
        {
            const int idx = d + steps;
            return (float) (idx / count) * period + deg[idx % count] - deg[d];
        }
    };

    //==========================================================================
    // Karplus-Strong: a short, clean, exactly tuned line in a regeneration loop is a plucked or
    // bowed string at the pitch whose period is the delay. The string's damping is the classic
    // two-point average, blended in by the damping type (soft, bright, dark): its delay is the
    // same at every frequency, so the partials stay harmonic. The line is shortened by the loop's
    // other delays (and lengthened by the DC blocker's phase lead at the fundamental), so the
    // pitch is true. Slew glides between notes; fade crossfades to the new note with no glide.
    // Freeze holds the string.
    //
    // Pitch = fundamental * 2^(V/Oct), snapped to the scale (1/1 = the fundamental). Poly fades
    // in up to seven more strings, one after another, on other degrees of the scale stacked up
    // from the note played: on the left 2, 4, 6 .. degrees up, on the right 1, 3, 5 .., so the two
    // channels of a stereo unit make one chord between them.
    //
    // That is up to sixteen strings, and they all do the same sum, so they are worked four at a
    // time: vector q holds pairs 2q and 2q+1 (pair 0 is the main string, 1..7 the poly strings)
    // as {left, right, left, right}. Only the vectors with a sounding string in them are worked.
    struct StringCore
    {
        static constexpr int kTaps = 7, kStrings = 16, kFix = 16;
        // the governor's ceilings. RMS: a sine at -11 dBFS; and no peak past 0.5
        static constexpr float ceiling = 0.2f, peakCeiling = 0.5f;

        float* store[kStrings];
        float* buf[kStrings];
        int size = 4096, mask = 4095, w = 0;
        // per string: the line's length in fixed point (now, per-sample change, and the one being faded to)
        int32_t lineFix[kStrings], lineStep[kStrings], lineFixB[kStrings];
        float wgt[4][kStrings], wgtB[4][kStrings];              // the four interpolation weights
        float dA[kStrings], dT[kStrings], dB[kStrings];         // the period in samples: now, wanted, and the one being faded to
        float lp[kStrings], dcX[kStrings], dcY[kStrings], hz1[kStrings], hz2[kStrings];
        float msq[kStrings], msqA[kStrings], pk[kStrings], pkDecay[kStrings], fb[kStrings];
        float gov[kStrings], rg[kStrings], dRg[kStrings], lev[kStrings], dLev[kStrings];
        int32_t scratch[4];
        uint32_t near[4];
        bool hot[4] {};
        float sr = 48000.0f, dcR = 0.999f, dcLead = 0, slewA = 0.01f, damp = 0.5f;
        BiquadCoef hp;
        bool fading = false, frozenNow = false, started = false, primed = false, gliding = false;
        int xfN = 0, xfLen = 1440, glide = 0, glideChunks = 430, nVec = 1, idle = 0;
        float gFade[4], dFade[4];
        bool tapRunning[kTaps] {};
        float tapMul[2][kTaps];
        float envPair[kTaps + 1] {};
        ScaleTable scales[2];
        volatile int activeScale = 0;
        int scaleSerial = 0, seenSerial = -1;
        float noteHz = 110.0f, period = 1.0f / 110.0f, lastF0 = -1.0f, polyLast = 0.0f, lastSustain = -1.0f, regenNow = 0, lastWet = -1.0f;
        uint32_t lastVoct = 0xffffffffu;
        float dry[4], wet[4], dDry[4], dWet[4];
        // (the plan for the block in hand)
        float dryT = 1, wetT = 0, wetNow = 0, polyNow = 0;
        int pairs = 1;
        bool mixMoving = false, levMoving = false, gated = false, quiet = false, again = true, kFade = false;
        uint32_t kF0 = 0, kSustain = 0, kPoly = 0, kWet = 0;
        int kDamp = -1;

        StringCore() { for (int s = 0; s < kStrings; ++s) store[s] = buf[s] = nullptr; }
        ~StringCore() { for (int s = 0; s < kStrings; ++s) delete[] store[s]; }
        StringCore (const StringCore&) = delete;
        StringCore& operator= (const StringCore&) = delete;

        static inline float dampFor (int type) { return type == 1 ? 0.25f : (type == 2 ? 1.0f : 0.5f); }   // soft, bright, dark
        static inline float tapLevel (int k, float width) { return clampf (0.0f, 1.0f, width * (float) kTaps - (float) k); }
        // the string's sustain: most of the knob's travel is long rings; past 100% it sings on its own
        static inline float ksRegen (float r) { return r < 1.0f ? powf (maxf (0.0f, r), 0.08f) : r; }

        void prepare (float s)
        {
            sr = s;
            size = 2048;
            while ((float) size < sr / 14.0f) size *= 2;        // room for a string of about 12 Hz
            mask = size - 1;
            for (int k = 0; k < kStrings; ++k)
            {
                delete[] store[k];
                store[k] = buf[k] = new float[size + 4];
            }
            slewA = coefOver (onePoleCoef (11.0f, sr), kChunk);
            glideChunks = (int) (10.0f * sr / (twoPi * 11.0f)) / kChunk + 1;      // ten time constants of the glide
            xfLen = ((int) (0.03f * sr) / kChunk) * kChunk;
            dcR = 1.0f - twoPi * 1.5f / sr;
            dcLead = 1.5f / (twoPi * sr);       // the blocker's phase lead at a note of period P is dcLead * P * P samples
            hp.highpass (320.0f, sr, 0.6f);
            for (int k = 0; k < kStrings; ++k) { resetString (k); dA[k] = dT[k] = dB[k] = 100.0f; lineFix[k] = lineFixB[k] = 100 << kFix; lineStep[k] = 0; lev[k] = dLev[k] = 0; rg[k] = dRg[k] = 0; gov[k] = 0; }
            for (int k = 0; k < kTaps; ++k) { tapRunning[k] = false; tapMul[0][k] = tapMul[1][k] = 1.0f; }
            for (int k = 0; k < kStrings; ++k) setWeights (wgt, k, lineFix[k]);
            memcpy (wgtB, wgt, sizeof (wgt));
            w = 0; fading = false; started = false; primed = false; frozenNow = false; glide = 0; gliding = false; idle = 0;
            set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
            lastVoct = 0xffffffffu; lastF0 = -1.0f; seenSerial = -1;
        }
        void resetString (int s)
        {
            for (int i = 0; i < size + 4; ++i) store[s][i] = 0.0f;
            lp[s] = dcX[s] = dcY[s] = hz1[s] = hz2[s] = msq[s] = pk[s] = fb[s] = 0.0f;
            msqA[s] = 0.001f; pkDecay[s] = 0.998f;
        }

        // UI thread: fill the spare table, then swap
        void beginScale() { scales[1 - activeScale].begin(); }
        void addPitch (float cents) { scales[1 - activeScale].add (cents); }
        void endScale() { scales[1 - activeScale].end(); activeScale = 1 - activeScale; ++scaleSerial; }

        // how long the line must be for a period of p samples (the newest sample in the line is one
        // old, the feedback arrives a sample later and the damping filter adds its own delay: all
        // taken off). Held, the string goes round on its own, undamped.
        inline float lineLen (float p, bool held) const { return maxf (3.0f, held ? p : p - 1.0f - 0.5f * damp + dcLead * p * p); }
        static inline int32_t toFix (float len) { return (int32_t) (len * (float) (1 << kFix)); }
        // the four cubic weights for a line of this length
        void setWeights (float (*wg)[kStrings], int s, int32_t fix) const
        {
            const float f = (float) ((uint32_t) (-fix) & ((1u << kFix) - 1u)) * (1.0f / (float) (1 << kFix));
            wg[0][s] = f * (-0.5f + f * (1.0f - 0.5f * f));
            wg[1][s] = 1.0f + f * f * (-2.5f + 1.5f * f);
            wg[2][s] = f * (0.5f + f * (2.0f - 1.5f * f));
            wg[3][s] = f * f * (-0.5f + 0.5f * f);
        }
        // the same for four strings from their fractions, each sample of a glide
        static CHL_INLINE void weights4 (f4 f, f4& w0, f4& w1, f4& w2, f4& w3)
        {
            const f4 f2 = f * f;
            w0 = f * mla (splat (-0.5f), f, mls (splat (1.0f), splat (0.5f), f));
            w1 = mla (splat (1.0f), f2, mla (splat (-2.5f), splat (1.5f), f));
            w2 = f * mla (splat (0.5f), f, mls (splat (2.0f), splat (1.5f), f));
            w3 = f2 * mla (splat (-0.5f), splat (0.5f), f);
        }
        void settle (int s, bool held)
        {
            lineFix[s] = toFix (lineLen (dA[s], held));
            lineStep[s] = 0;
            setWeights (wgt, s, lineFix[s]);
        }
        void setPeriod (int s, float samples, bool jump)
        {
            dT[s] = clampf (5.0f, (float) size - 8.0f, samples);
            // the governor's level window: four periods, never under 5 ms
            msqA[s] = minf (0.25f / dT[s], 1.0f / (0.005f * sr));
            pkDecay[s] = 1.0f - 2.0f * msqA[s];
            if (jump) { dA[s] = dT[s]; settle (s, frozenNow); }
        }

        void retune (float f0, float voct)
        {
            const ScaleTable& sc = scales[activeScale];
            const float cents = clampf (-1.0f, 1.0f, voct) * 12000.0f;
            int degree = 0;
            const float q = sc.quantise (cents, degree);
            float hz = f0 * fastExp2 (q * (1.0f / 1200.0f));
            // keep it inside what the line can play: a note outside moves a period in
            const float per = fastExp2 (maxf (50.0f, sc.period) * (1.0f / 1200.0f));
            for (int guard = 0; guard < 64 && hz > 4000.0f; ++guard) hz /= per;
            for (int guard = 0; guard < 64 && hz < 16.0f; ++guard) hz *= per;
            noteHz = hz;
            period = 1.0f / hz;
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < kTaps; ++k)
                {
                    float r = fastExp2 (sc.stepsAbove (degree, side == 0 ? 2 * (k + 1) : 2 * k + 1) * (1.0f / 1200.0f));
                    while (hz * r > 5000.0f) r *= 0.5f;        // too high for the line: an octave down
                    tapMul[side][k] = 1.0f / r;
                }
        }

        // The governor. The loop's regeneration sits near 1, so a steady input would be amplified by
        // 1/(1-g) (tens of dB), and a limiter in the loop then squares the string off into a buzz.
        // Instead the loop stays linear and its regeneration is eased down as the string's level (RMS
        // over a few periods, so the gain doesn't ripple at the pitch) reaches the ceiling, and as its
        // peak does: the string rings up to the ceiling and no further, and past 100% regen it sings
        // there, clean.
        //
        // Every 16 samples, for one vector of strings: the regen moves on, and a look at whether any
        // of the four is near its ceiling. Only then is the governor worked, and then every sample
        // (a governor that lags the string by a few samples pulls its pitch flat).
        inline void govern (int q)
        {
            const int s = 4 * q;
            const f4 r = ld (rg + s) + ld (dRg + s);
            st (rg + s, r);
            st (gov + s, r * splat (0.25f));
            stu (near, or4 (gt4 (ld (msq + s), splat (0.64f * ceiling * ceiling)), gt4 (ld (pk + s), splat (0.8f * peakCeiling))));
            hot[q] = (near[0] | near[1] | near[2] | near[3]) != 0u;
        }
        // every 16 samples of a glide, for one vector: the periods move on, and the lines after them
        inline void glideVec (int q)
        {
            const int s = 4 * q;
            f4 d = ld (dA + s);
            d = mla (d, ld (dT + s) - d, splat (slewA));
            st (dA + s, d);
            const f4 len = max4 (mla (d - splat (1.0f + 0.5f * damp), d * d, splat (dcLead)), splat (3.0f));
            sti (scratch, toInt (len * splat ((float) (1 << kFix))));
            for (int j = 0; j < 4; ++j) lineStep[s + j] = (scratch[j] - lineFix[s + j]) / kChunk;
        }

        // one sample of one vector of four strings. x: {left, right, left, right} input.
        CHL_INLINE f4 tickVec (int q, f4 x, bool held, bool hpfOn)
        {
            const int s = 4 * q;
            const int32_t wf = w << kFix;
            f4 r0, r1, r2, r3, y;
            if (gliding)
            {
                uint32_t fr[4];
                const float* p[4];
                for (int j = 0; j < 4; ++j)
                {
                    const int32_t pos = wf - (lineFix[s + j] += lineStep[s + j]);
                    p[j] = buf[s + j] + (((pos >> kFix) - 1) & mask);
                    fr[j] = (uint32_t) pos & ((1u << kFix) - 1u);
                }
                r0 = ld (p[0]); r1 = ld (p[1]); r2 = ld (p[2]); r3 = ld (p[3]);
                transpose4 (r0, r1, r2, r3);
                f4 w0, w1, w2, w3;
                weights4 (CHL_FIX_TO_FLOAT (fromInts (fr[0], fr[1], fr[2], fr[3]), kFix), w0, w1, w2, w3);
                y = mla (mla (mla (r0 * w0, r1, w1), r2, w2), r3, w3);
            }
            else
            {
                r0 = ld (buf[s] + ((((wf - lineFix[s]) >> kFix) - 1) & mask));
                r1 = ld (buf[s + 1] + ((((wf - lineFix[s + 1]) >> kFix) - 1) & mask));
                r2 = ld (buf[s + 2] + ((((wf - lineFix[s + 2]) >> kFix) - 1) & mask));
                r3 = ld (buf[s + 3] + ((((wf - lineFix[s + 3]) >> kFix) - 1) & mask));
                transpose4 (r0, r1, r2, r3);
                y = mla (mla (mla (r0 * ld (wgt[0] + s), r1, ld (wgt[1] + s)), r2, ld (wgt[2] + s)), r3, ld (wgt[3] + s));
                if (fading)
                {
                    // on the way to a new note: the line is read at the new length as well
                    r0 = ld (buf[s] + ((((wf - lineFixB[s]) >> kFix) - 1) & mask));
                    r1 = ld (buf[s + 1] + ((((wf - lineFixB[s + 1]) >> kFix) - 1) & mask));
                    r2 = ld (buf[s + 2] + ((((wf - lineFixB[s + 2]) >> kFix) - 1) & mask));
                    r3 = ld (buf[s + 3] + ((((wf - lineFixB[s + 3]) >> kFix) - 1) & mask));
                    transpose4 (r0, r1, r2, r3);
                    const f4 yb = mla (mla (mla (r0 * ld (wgtB[0] + s), r1, ld (wgtB[1] + s)), r2, ld (wgtB[2] + s)), r3, ld (wgtB[3] + s));
                    const f4 g = ld (gFade);
                    y = mla (y * dupLow (g), yb, dupHigh (g));
                }
            }
            // held: the string goes round on its own, one read of the line written straight back
            const f4 in = held ? y : x + ld (fb + s);
            stLane (buf[s] + w, in, 0); stLane (buf[s + 1] + w, in, 1); stLane (buf[s + 2] + w, in, 2); stLane (buf[s + 3] + w, in, 3);
            if (w < 3)
            {
                // the copy past the end, so four taps can always be read in one go
                stLane (buf[s] + size + w, in, 0); stLane (buf[s + 1] + size + w, in, 1);
                stLane (buf[s + 2] + size + w, in, 2); stLane (buf[s + 3] + size + w, in, 3);
            }
            if (held) return y;

            const f4 o = mla (y, ld (lp + s) - y, splat (0.5f * damp));          // the two-point average, blended in
            st (lp + s, y);
            const f4 out = mla (o - ld (dcX + s), splat (dcR), ld (dcY + s));
            st (dcX + s, o); st (dcY + s, out);
            f4 f = out;
            if (hpfOn)
            {
                f = mla (ld (hz1 + s), splat (hp.b0), out);
                st (hz1 + s, mla (mls (ld (hz2 + s), splat (hp.a1), f), splat (hp.b1), out));
                st (hz2 + s, mls (splat (hp.b2) * out, splat (hp.a2), f));
            }
            f4 ms = ld (msq + s);
            ms = mla (ms, f * f - ms, ld (msqA + s));
            st (msq + s, ms);
            const f4 p = max4 (abs4 (f), ld (pk + s) * ld (pkDecay + s));
            st (pk + s, p);
            f4 g = ld (gov + s);
            if (hot[q])
            {
                const f4 one = splat (1.0f);
                const f4 a = sel4 (gt4 (ms, splat (ceiling * ceiling)), splat (ceiling) * rsqrt1 (ms), one);
                const f4 b = sel4 (gt4 (p, splat (peakCeiling)), splat (peakCeiling) * recip1 (p), one);
                g = g * min4 (a, b);
            }
            // (the limiter is only the net under the governor, well above its ceiling)
            st (fb + s, softClip4 (f * g) * splat (4.0f));
            return out;
        }

        // samples i0 up to i1, all with the strings held or all not
        CHL_NOINLINE void run (const float* inL, const float* inR, float* outL, float* outR, int i0, int i1,
                               bool held, bool hpfOn, bool levMoving, bool mixMoving)
        {
            for (int i = i0; i < i1; ++i)
            {
                const f4 x = dupLow (ldPair (inL + i, inR + i, splat (0.0f)));
                const f4 xk = x * splat (kIn);
                f4 acc = splat (0.0f);
                for (int q = 0; q < nVec; ++q)
                {
                    f4 l = ld (lev + 4 * q);
                    if (levMoving) { l = l + ld (dLev + 4 * q); st (lev + 4 * q, l); }
                    acc = mla (acc, tickVec (q, xk, held, hpfOn), l);
                }
                w = (w + 1) & mask;
                if (fading) st (gFade, ld (gFade) + ld (dFade));
                const f4 o = mla (x * ld (dry), acc + swapHalves (acc), ld (wet));
                if (outR != nullptr) stPair (outL + i, outR + i, o); else stLane (outL + i, o, 0);
                if (mixMoving) { st (dry, ld (dry) + ld (dDry)); st (wet, ld (wet) + ld (dWet)); }
            }
        }

        // Before the samples of a block: the note, which strings sound, where the controls are going.
        // False when there is nothing to do (the output has been written as silence).
        CHL_NOINLINE bool begin (const float* inL, const float* inR, const float* voct, const float* freezeGate, float* outL, float* outR, int n,
                                 float f0, float sustain, float poly, float wetAmount, int dampType, bool fadeMode)
        {
            const int chunks = (n + kChunk - 1) / kChunk;
            const float v0 = voct != nullptr ? voct[0] : 0.0f;
            // A block in which no control has moved (they are compared as bit patterns) and nothing is
            // on its way anywhere needs none of the sums below.
            const bool same = primed && started && ! again && bitsOf (v0) == lastVoct && bitsOf (f0) == kF0 && bitsOf (sustain) == kSustain
                              && bitsOf (poly) == kPoly && bitsOf (wetAmount) == kWet && dampType == kDamp && seenSerial == scaleSerial && fadeMode == kFade;
            if (same) { mixMoving = levMoving = false; }
            else
            {
                kF0 = bitsOf (f0); kSustain = bitsOf (sustain); kPoly = bitsOf (poly); kWet = bitsOf (wetAmount); kDamp = dampType; kFade = fadeMode;
                bool off = false;
                f0 = clampf (8.0f, 4000.0f, f0);
                poly = clampf (0.0f, 1.0f, poly);
                wetAmount = clampf (0.0f, 1.0f, wetAmount);
                sustain = clampf (0.0f, 1.15f, sustain);

                // the note
                bool retuned = false;
                if (bitsOf (v0) != lastVoct || f0 != lastF0 || seenSerial != scaleSerial)
                {
                    lastVoct = bitsOf (v0); lastF0 = f0; seenSerial = scaleSerial;
                    retune (f0, v0);
                    retuned = true;
                }
                const float d = dampFor (dampType);
                if (d != damp) { damp = d; retuned = true; }

                // which strings sound
                pairs = 1;
                bool tapOn[kTaps];
                for (int k = 0; k < kTaps; ++k)
                {
                    tapOn[k] = tapLevel (k, polyLast) > 0.0005f || tapLevel (k, poly) > 0.0005f;
                    if (tapOn[k]) pairs = k + 2;
                }
                if (retuned || ! started)
                {
                    setPeriod (0, period * sr, ! started); setPeriod (1, period * sr, ! started);
                    for (int k = 0; k < kTaps; ++k)
                        if (tapOn[k] && tapRunning[k])
                        { setPeriod (2 * k + 2, period * tapMul[0][k] * sr, false); setPeriod (2 * k + 3, period * tapMul[1][k] * sr, false); }
                }
                for (int k = 0; k < kTaps; ++k)
                {
                    if (tapOn[k] && ! tapRunning[k])
                    {
                        // a string that starts, starts clean and on its note
                        resetString (2 * k + 2); resetString (2 * k + 3);
                        setPeriod (2 * k + 2, period * tapMul[0][k] * sr, true); setPeriod (2 * k + 3, period * tapMul[1][k] * sr, true);
                        lineFixB[2 * k + 2] = lineFix[2 * k + 2]; lineFixB[2 * k + 3] = lineFix[2 * k + 3];
                        dB[2 * k + 2] = dA[2 * k + 2]; dB[2 * k + 3] = dA[2 * k + 3];
                        for (int t = 0; t < 4; ++t) { wgtB[t][2 * k + 2] = wgt[t][2 * k + 2]; wgtB[t][2 * k + 3] = wgt[t][2 * k + 3]; }
                    }
                    tapRunning[k] = tapOn[k];
                }
                started = true;
                nVec = (pairs + 1) / 2;
                const int live = 2 * pairs;

                // on the way to a new note: glide, or fade
                if (! frozenNow)
                {
                    off = false;
                    for (int s = 0; s < live; ++s) off = off || fabsf (dT[s] - dA[s]) > 0.02f;
                    if (off && ! fadeMode) { glide = glideChunks; fading = false; }
                    else if (off && ! fading && glide == 0)
                    {
                        for (int s = 0; s < kStrings; ++s) { dB[s] = dT[s]; lineFixB[s] = toFix (lineLen (dB[s], false)); setWeights (wgtB, s, lineFixB[s]); }
                        fading = true; xfN = 0;
                    }
                    else if (retuned && ! off) for (int s = 0; s < live; ++s) settle (s, false);     // (the damping changed)
                }

                // sustain and the mix: where they are going by the end of the block
                const bool sustainMoved = sustain != lastSustain;
                if (sustainMoved) { lastSustain = sustain; regenNow = ksRegen (sustain); }
                    float lv = 0.0f;
                for (int k = 0; k < kTaps; ++k) if (tapOn[k]) lv += tapLevel (k, poly);
                const float norm = pairs > 1 ? 1.0f / (1.0f + 0.3f * lv) : 1.0f;
                for (int p = 0; p < 8; ++p)
                {
                    const float r = regenNow * (1.0f - 0.0015f * (float) p);
                    const float l = p == 0 ? norm : (p < pairs ? tapLevel (p - 1, poly) * norm : 0.0f);
                    for (int c = 0; c < 2; ++c)
                    {
                        const int s = 2 * p + c;
                        if (! primed) { rg[s] = r; lev[s] = l; }
                        dRg[s] = (r - rg[s]) / (float) chunks;
                        dLev[s] = (l - lev[s]) / (float) n;
                    }
                }
                dryT = fadeOut (wetAmount); wetT = fadeIn (wetAmount) * kOut;
                if (! primed) { set4 (dry, dryT); set4 (wet, wetT); lastWet = wetAmount; primed = true; }
                mixMoving = wetAmount != lastWet; levMoving = poly != polyLast;
                wetNow = wetAmount; polyNow = poly;
                if (mixMoving) { const float inv = 1.0f / (float) n; set4 (dDry, (dryT - dry[0]) * inv); set4 (dWet, (wetT - wet[0]) * inv); }

                // (the block after a change comes through here once more, to find everything arrived)
                again = retuned || off || mixMoving || levMoving || glide > 0 || fading || sustainMoved;
            }

            gated = freezeGate != nullptr && ! allZero (freezeGate, n);
            // nothing coming in and every string has died away: nothing to do
            quiet = ! gated && ! frozenNow && allZero (inL, n) && allZero (inR, n);
            if (quiet && idle > 8 && glide == 0 && ! fading)
            {
                memset (outL, 0, sizeof (float) * (size_t) n);
                if (outR != nullptr) memset (outR, 0, sizeof (float) * (size_t) n);
                for (int s = 0; s < kStrings; ++s) { rg[s] += dRg[s] * (float) chunks; lev[s] += dLev[s] * (float) n; }
                for (int p = 0; p < 8; ++p) envPair[p] *= 0.7f;
                set4 (dry, dryT); set4 (wet, wetT); lastWet = wetNow; polyLast = polyNow;
                return false;
            }
            if (! quiet) idle = 0;
            return true;
        }


        // After them: the controls have arrived; the levels for the display
        CHL_NOINLINE void finish (int n)
        {
            if (mixMoving) { set4 (dry, dryT); set4 (wet, wetT); lastWet = wetNow; }
            if (levMoving) for (int s = 0; s < kStrings; ++s) if (s >= 4 * nVec) lev[s] += dLev[s] * (float) n;
            polyLast = polyNow;
            // for the display, and to know when everything has died away
            float loudest = 0.0f;
            for (int p = 0; p < pairs; ++p)
            {
                const float e = maxf (pk[2 * p], pk[2 * p + 1]);
                envPair[p] += 0.25f * (e * 0.6f - envPair[p]);
                loudest = maxf (loudest, e);
            }
            for (int p = pairs; p < 8; ++p) envPair[p] = 0.0f;
            if (quiet && loudest < 1.0e-6f) ++idle;
        }

        // voct: the first sample of the block is used (a snapped pitch has no use for more)
        void process (const float* inL, const float* inR, const float* voct, const float* freezeGate, float* outL, float* outR, int n,
                      float f0, float sustain, float poly, float wetAmount, int dampType, bool hpfOn, bool fadeMode)
        {
            if (inR == nullptr) inR = silence();
            if (! begin (inL, inR, voct, freezeGate, outL, outR, n, f0, sustain, poly, wetAmount, dampType, fadeMode)) return;
            for (int c = 0; c < n; c += kChunk)
            {
                gliding = glide > 0 && ! frozenNow;
                for (int q = 0; q < nVec; ++q) { govern (q); if (gliding) glideVec (q); }
                if (fading)
                {
                    const float t0 = (float) xfN / (float) xfLen, t1 = (float) (xfN + kChunk) / (float) xfLen;
                    set22 (gFade, fadeOut (t0), fadeIn (t0));
                    set22 (dFade, (fadeOut (t1) - fadeOut (t0)) * (1.0f / (float) kChunk), (fadeIn (t1) - fadeIn (t0)) * (1.0f / (float) kChunk));
                }
                const int e = c + kChunk < n ? c + kChunk : n;
                // in runs of samples over which the freeze gate does not change
                for (int i = c; i < e;)
                {
                    const bool held = gated && gateHigh (freezeGate[i]);
                    if (held != frozenNow)
                    {
                        // Held: the string goes round on its own at the period it had. No fade while
                        // held: a fade reads the line at two places and sums them, and writing that sum
                        // back into a line that feeds itself is a loop with a gain above one, which runs
                        // away. A fade in progress is dropped where it stands; a glide waits.
                        frozenNow = held; fading = false; gliding = false;
                        set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
                        for (int s = 0; s < kStrings; ++s) { settle (s, held); fb[s] = 0.0f; }
                        if (! held) glide = glideChunks;
                        again = true;
                    }
                    int j = i + 1;
                    if (gated) while (j < e && gateHigh (freezeGate[j]) == held) ++j;
                    else j = e;
                    run (inL, inR, outL, outR, i, j, held, hpfOn, levMoving, mixMoving);
                    i = j;
                }
                if (gliding && --glide == 0)
                {
                    for (int s = 0; s < kStrings; ++s) { dA[s] = dT[s]; settle (s, false); }
                    gliding = false;
                }
                if (fading)
                {
                    xfN += kChunk;
                    if (xfN >= xfLen)
                    {
                        // arrived: the new length is the line
                        fading = false; set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
                        for (int s = 0; s < kStrings; ++s) { dA[s] = dB[s]; lineFix[s] = lineFixB[s]; for (int t = 0; t < 4; ++t) wgt[t][s] = wgtB[t][s]; }
                    }
                }
            }
            finish (n);
        }
    };
}
