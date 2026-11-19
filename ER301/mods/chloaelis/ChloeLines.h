#pragma once
// The three delay engines as bare "lines" (input -> coloured, delayed output), stereo: the left
// and right channels ride in a pair of vector lanes and share one clock, like two chips on one
// oscillator or two tracks of one tape.
//
// How a line is called, once per sample:
//
//     y = line.tick (v, gain, freeze)
//
//   v      lanes 0,1: the unit's loop signal (the line's last output), on its way to the
//                     regeneration: the line high-passes it (if asked), scales it by the regen and
//                     saturates it, because those are the same sums as its own filters and
//                     saturators and so cost nothing extra in the spare lanes
//          lanes 2,3: the sound to write into the line (input + feedback), left and right
//   gain   lanes 0,1: the regen; lanes 2,3: the line's own input scale (kLineGain)
//   y      lanes 2,3: the line's output, left and right
//   line.fbOut, lanes 0,1: the feedback to add to the next input
//
// tick is a real call (never inlined), and the units hold no vector across it: see ChloeSimd.h.
#include "ChloeDsp.h"

namespace chl
{
    // "is this gate high" for a sample of a gate signal, without the FPU: a float above 0.5
    static inline bool gateHigh (float g) { return (int32_t) bitsOf (g) > 0x3f000000; }

    //==========================================================================
    // PT2399. The chip is a continuously variable slope delta modulator writing 1 bit per
    // conversion into a fixed 44 kbit RAM. The delay time is set by the VCO clock: longer
    // delays mean a slower bit rate, so more quantisation noise, slope overload grit,
    // narrower bandwidth and aliasing (the output is just the integrated bit stream sampled
    // and held). Turning time while it runs re-clocks the RAM, so the echoes pitch-bend.
    // Freeze stops writing the RAM, so the loop recirculates forever, bit for bit.
    //
    // Lanes: the decoder (left, right) in 0,1 and the coder in 2,3: they do the same sum on
    // different bits, so one pass of the bit loop serves both, for both channels.
    struct PTLine
    {
        static constexpr int kBits = 44000;
        static constexpr float kClockPerBit = 15.55f;   // ~684k VCO cycles for the full RAM at 342 ms
        static constexpr int kPhaseBits = 20, kMaxBits = 96;
        static constexpr float kLineGain = 0.9f;        // into the chip's input opamp

        uint16_t* mem = nullptr;
        int pos = 0;
        uint32_t phase = 0, inc = 0;
        float sr = 48000.0f, bitRate = 133333.0f, targetRate = 133333.0f, clockA = 0.01f, lastBw = -1.0f, lastWhineRate = -1.0f;
        float est[4], step[4];
        uint32_t m1[4], m2[4];
        float leak = 0.999f, beta = 0.99f, ombMin = 0.0f, ombMax = 0.0f;
        float prevIn[4], inNow[4], held[4], lp1[4], fbOut[4];
        Biquad4 qa;         // lanes 0,1: the loop's high-pass (set by the unit); lanes 2,3: the input filter
        Biquad4 qb;         // lanes 0,1: the first output filter; lanes 2,3: the second, a sample behind
        DCBlock4 dc;
        uint32_t rng[4];
        uint32_t whinePhase = 0, whineInc = 0;
        float invN[kMaxBits + 1];
        float sine[256];

        PTLine() { mem = new uint16_t[kBits]; }
        ~PTLine() { delete[] mem; }
        PTLine (const PTLine&) = delete;
        PTLine& operator= (const PTLine&) = delete;

        void prepare (float s)
        {
            sr = s;
            clockA = coefOver (onePoleCoef (12.0f, sr), kChunk);     // the VCO's RC smoothing on the time pot
            dc.prepare (sr, 10.0f);
            invN[0] = 0.0f;
            for (int i = 1; i <= kMaxBits; ++i) invN[i] = 1.0f / (float) i;
            for (int i = 0; i < 256; ++i) { float sn, cs; sinCos (twoPi * (float) i / 256.0f, sn, cs); sine[i] = sn; }
            reset();
        }
        void seed (uint32_t a) { for (int k = 0; k < 4; ++k) rng[k] = (a + 31u * (uint32_t) k) * 2654435761u + 12345u; }
        void reset()
        {
            for (int i = 0; i < kBits; ++i) mem[i] = (i & 1) ? 0xffffu : 0u;      // idle pattern = silence
            pos = 0; phase = 0; whinePhase = 0;
            set4 (est, 0); set4 (step, 0); set4 (prevIn, 0); set4 (inNow, 0); set4 (held, 0); set4 (lp1, 0); set4 (fbOut, 0);
            for (int k = 0; k < 4; ++k) m1[k] = m2[k] = 0;
            qa.reset(); qb.reset(); dc.reset();
            bitRate = targetRate = kBits / 0.33f;
            lastBw = -1.0f;
            perBit(); filters();
        }
        // jump = re-clock instantly (used by the fade mode, which waits for the RAM to refill)
        void setTime (float sec, bool jump)
        {
            targetRate = (float) kBits / clampf (0.02f, 1.6f, sec);
            if (jump) { bitRate = targetRate; perBit(); filters(); }
        }
        void setLoopHP (const BiquadCoef& c) { qa.setPair (0, c); }

        // the chip's RC integrator / syllabic filter time constants are fixed in time, so the
        // per-bit coefficients scale with the clock
        void perBit()
        {
            inc = (uint32_t) (bitRate / sr * (float) (1 << kPhaseBits));
            const float x = twoPi * 25.0f / bitRate;                       // exp (-x) by its series: x is tiny
            leak = 1.0f - x * (1.0f - x * (0.5f - x * (1.0f / 6.0f)));
            const float z = 1.0f / (0.0035f * bitRate);
            beta = 1.0f - z * (1.0f - z * (0.5f - z * (1.0f / 6.0f)));
            // step size ~ 1/sqrt(clock): long delays overload sooner (grit), and re-clocking a
            // loop changes its level the way the chip does, without +30 dB bursts
            const float isq = 1.0f / sqrtf (bitRate);
            ombMin = (1.0f - beta) * 2.6f * isq;
            ombMax = (1.0f - beta) * 145.0f * isq;
        }
        // every 16 samples: the clock glides to where the time control put it
        inline void control()
        {
            if (bitsOf (bitRate) == bitsOf (targetRate)) return;
            bitRate += (targetRate - bitRate) * clockA;
            if (fabsf (targetRate - bitRate) < 1.0e-4f * targetRate) bitRate = targetRate;
            perBit();
        }
        // once a block: the anti-alias filters follow the clock, and so does the clock's whine
        void filters()
        {
            const float bw = clampf (2400.0f, 11500.0f, bitRate * 0.048f);
            if (bw != lastBw)
            {
                lastBw = bw;
                BiquadCoef c;
                c.lowpass (bw, sr, 0.62f); qa.setPair (2, c);
                c.lowpass (bw, sr, 0.54f); qb.setPair (0, c);
                c.lowpass (bw * 1.25f, sr, 1.3f); qb.setPair (2, c);
            }
            if (bitRate != lastWhineRate)
            {
                lastWhineRate = bitRate;
                const float turns = bitRate * kClockPerBit / sr;           // the clock, folded into the audio band
                float f = turns - (float) (int) turns;
                if (f > 0.5f) f = 1.0f - f;
                whineInc = (uint32_t) (f * 65536.0f) << 16;
            }
        }

        // n conversions: the coder turns the input into bits, the oldest bits in the RAM come out
        // through the decoder
        CHL_NOINLINE void convert (int n, bool freeze)
        {
            // One conversion. The bit's history (the last bit and whether the one before differed) is
            // read from A1, PD and left in B1, QD; the estimates are kept negated, which saves a constant.
            // (Not unrolled: two conversions per pass need more vector registers than the audio
            // thread's stack budget allows.)
#define CHL_PT_CONVERSION(A1, PD, B1, QD)                                                                  \
            {                                                                                              \
                xin = xin + dIn;                                                                           \
                const f4 nl = ne * lk, sb = sp * bt;                                                       \
                /* the oldest bit in the RAM (lanes 0,1), which the coder's (is the input above its */     \
                /* estimate? lanes 2,3) then replaces */                                                   \
                B1 = lowHighU (ramLoad (mp), gtZero (xin + ne));                                           \
                if (__builtin_expect (! freeze, 1)) ramStore (mp, B1);                                     \
                if (++mp == end) mp = mem;                                                                 \
                /* CVSD, run-of-3 coincidence: the step grows while three bits in a row agree */           \
                QD = xor4 (B1, A1);                                                                        \
                sp = sb + sel4 (or4 (QD, PD), oMin, oMax);                                                 \
                ne = nl + flt (xor4 (bits (sp), signOf (B1)));      /* up by the step for a 1, down for a 0 */ \
                acc = acc - ne;                                                                            \
            }
            f4 xin = ld (prevIn);
            const f4 dIn = (ld (inNow) - xin) * ldSplat (invN + n);
            const f4 lk = splat (leak), bt = splat (beta), oMin = splat (ombMin), oMax = splat (ombMax);
            f4 ne = ld (est), sp = ld (step), acc = splat (0.0f);
            u4 a1 = ldu (m1), pd = ldu (m2), b1, qd;
            uint16_t* mp = mem + pos;
            uint16_t* const end = mem + kBits;
            for (int b = 0; b < n; ++b)
            {
                CHL_PT_CONVERSION (a1, pd, b1, qd)
                a1 = b1; pd = qd;
            }
#undef CHL_PT_CONVERSION
            pos = (int) (mp - mem);
            st (est, ne); st (step, sp); stu (m1, a1); stu (m2, pd);
            st (held, acc * ldSplat (invN + n));   // sample & hold of the bit-rate output (aliases)
        }

        CHL_NOINLINE f4 tick (f4 v, f4 gain, bool freeze)
        {
            if (freeze) v = lowLow (v, splat (0.0f));
            // the input filter, then the chip's input opamp rails (and, beside them, the loop)
            const f4 t1 = tanh4 (qa.tick (v) * gain) * pair4 (1.05f, 1.1f);
            st (fbOut, t1);

            st (inNow, t1);                         // (kept beside the state: nothing is held across the bit loop)
            phase += inc;
            int n = (int) (phase >> kPhaseBits);
            phase &= (1u << kPhaseBits) - 1u;
            if (n > 0) convert (n > kMaxBits ? kMaxBits : n, freeze);
            st (prevIn, ld (inNow));
            const f4 hv = ld (held);

            const f4 y2 = qb.tick (lowLow (hv, ld (lp1)));
            st (lp1, y2);
            f4 y = tanh4 (dc.tick (y2) * splat (1.0f / 1.3f)) * splat (1.3f);       // output opamp rails

            // noise floor + a faint clock whine folded into the audio band
            const u4 s = lcg4 (ldu (rng));
            stu (rng, s);
            y = mla (y, noise4 (s), splat (0.00012f));
            whinePhase += whineInc;
            return mla (y, ldSplat (sine + (whinePhase >> 24)), splat (0.00018f));
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
        static constexpr int kPhaseBits = 24;
        static constexpr float kLineGain = 1.0f / 1.25f;
        static constexpr float ref = 0.22f;

        float* bucket = nullptr;                    // left and right side by side
        int idx = 0;
        uint32_t phase = 0, inc = 0;
        float invInc = 0.0f;
        float sr = 48000.0f, clk = 12000.0f, targetClk = 12000.0f, clkA = 0.001f;
        float prevAA[4], held[4], cti[4], compEnv[4], expEnv[4], aaY[4], rcY[4], par[4], fbOut[4], loopIn[4];
        float cAtt = 0.01f, cRel = 0.001f, eAtt = 0.01f, eRel = 0.001f;
        Biquad4 qa;         // lanes 0,1: the second anti-alias section, a sample behind; lanes 2,3: the first
        Biquad4 qb;         // lanes 0,1: the first reconstruction section; lanes 2,3: the second
        Biquad4 qc;         // lanes 0,1: the loop's high-pass
        bool loopHP = false;
        DCBlock4 dc;
        uint32_t rng[4];

        BBDLine() { bucket = new float[2 * kStages + 4]; }
        ~BBDLine() { delete[] bucket; }
        BBDLine (const BBDLine&) = delete;
        BBDLine& operator= (const BBDLine&) = delete;

        void prepare (float s)
        {
            sr = s;
            clkA = coefOver (onePoleCoef (9.0f, sr), kChunk);
            BiquadCoef c;
            c.lowpass (6800.0f, sr, 0.54f); qa.setPair (2, c);
            c.lowpass (6800.0f, sr, 1.31f); qa.setPair (0, c);
            c.lowpass (6200.0f, sr, 0.54f); qb.setPair (0, c);
            c.lowpass (6200.0f, sr, 1.31f); qb.setPair (2, c);
            cAtt = onePoleCoef (1.0f / (twoPi * 0.004f), sr); cRel = onePoleCoef (1.0f / (twoPi * 0.045f), sr);
            eAtt = onePoleCoef (1.0f / (twoPi * 0.006f), sr); eRel = onePoleCoef (1.0f / (twoPi * 0.060f), sr);
            dc.prepare (sr, 9.0f);
            reset();
        }
        void seed (uint32_t a) { for (int k = 0; k < 4; ++k) rng[k] = (a + 31u * (uint32_t) k) * 2654435761u + 12345u; }
        void reset()
        {
            for (int i = 0; i < 2 * kStages + 4; ++i) bucket[i] = 0.0f;
            idx = 0; phase = 0;
            set4 (prevAA, 0); set4 (held, 0); set4 (cti, 0); set4 (compEnv, 0.05f); set4 (expEnv, 0.05f); set4 (aaY, 0); set4 (rcY, 0);
            set4 (par, 0.0012f); set4 (fbOut, 0);
            qa.reset(); qb.reset(); qc.reset(); dc.reset();
            clk = targetClk = (float) kStages / 0.33f;
            perClock();
        }
        void setTime (float sec, bool jump)
        {
            targetClk = (float) kStages / clampf (0.02f, 1.6f, sec);
            if (jump) { clk = targetClk; perClock(); }
        }
        void setLoopHP (const BiquadCoef& c) { qc.setPair (0, c); loopHP = ! (c.b1 == 0.0f && c.a1 == 0.0f); }
        void perClock()
        {
            inc = (uint32_t) (clk / sr * (float) (1 << kPhaseBits));
            if (inc < 1u) inc = 1u;
            invInc = 1.0f / (float) inc;
        }
        inline void control()
        {
            if (bitsOf (clk) == bitsOf (targetClk)) return;
            clk += (targetClk - clk) * clkA;
            if (fabsf (targetClk - clk) < 1.0e-4f * targetClk) clk = targetClk;
            perClock();
        }
        void filters() {}

        // the clock edges that fall in this sample: each moves every bucket on by one
        CHL_NOINLINE void clock (bool freeze)
        {
            const f4 ya = ld (aaY), pa = ld (prevAA);
            f4 ct = ld (cti), pr = ld (par);
            u4 s = ldu (rng);
            do
            {
                phase -= (1u << kPhaseBits);
                // where inside this sample the clock edge landed
                const f4 fr = max4 (mls (splat (1.0f), toFloat (splati ((int32_t) phase)), splat (invInc)), splat (0.0f));
                const f4 in = mla (pa, ya - pa, fr);
                float* const bp = bucket + 2 * idx;
                const f4 out = ld (bp);
                f4 nv;
                if (freeze) nv = out * splat (0.99985f);
                else
                {
                    // charge wells fill up softly, a little asymmetrically
                    s = lcg4 (s);
                    nv = mla ((tanh4 (mla (splat (0.06f), in, splat (1.15f))) - splat (0.0599f)) * splat (0.85f), noise4 (s), splat (0.0009f));
                }
                st2 (bp, nv);
                idx = (idx + 1) & (kStages - 1);
                ct = mla (ct, out - ct, splat (0.86f));             // charge transfer inefficiency
                pr = neg4 (pr);
            } while (phase >= (1u << kPhaseBits));
            st (cti, ct); st (held, ct); st (par, pr); stu (rng, s);
        }

        CHL_NOINLINE f4 tick (f4 v, f4 gain, bool freeze)
        {
            // compressor (2:1)
            const f4 ax = abs4 (v);
            f4 ce = ld (compEnv);
            ce = mla (ce, ax - ce, sel4 (gt4 (ax, ce), splat (cAtt), splat (cRel)));
            st (compEnv, ce);
            const f4 g = min4 (splat (10.0f), rsqrt1 (max4 (ce, splat (0.0005f)) * splat (1.0f / ref)));
            const f4 ya = qa.tick (highHigh (ld (aaY), v * g));
            st (aaY, ya);                                   // lanes 0,1 of ya: the filtered input, ready for the buckets

            st (loopIn, lowHigh ((loopHP ? qc.tick (v) : v) * gain, gain));     // lanes 0,1: the loop, scaled; 2,3: the output scale
            phase += inc;
            if (phase >= (1u << kPhaseBits)) clock (freeze);
            st (prevAA, ld (aaY));

            // S&H output + a whisper of clock feedthrough, then the reconstruction filter
            const f4 yr = qb.tick (lowLow (ld (held) + ld (par), ld (rcY)));
            st (rcY, yr);
            const u4 s2 = lcg4 (ldu (rng));
            stu (rng, s2);
            f4 y = mla (yr, noise4 (s2), splat (0.0004f));              // output noise, expanded with the signal

            // expander (1:2)
            const f4 ay = abs4 (y);
            f4 ee = ld (expEnv);
            ee = mla (ee, ay - ee, sel4 (gt4 (ay, ee), splat (eAtt), splat (eRel)));
            st (expEnv, ee);
            y = dc.tick (y * min4 (splat (6.0f), max4 (ee, splat (0.0002f)) * splat (1.0f / ref)));

            const f4 li = ld (loopIn);
            const f4 t = tanh4 (lowHigh (li, y * li)) * pair4 (1.05f, 1.25f);
            st (fbOut, t);
            return t;
        }
    };

    //==========================================================================
    // Tape. Record head (pre-emphasis, soft magnetic saturation) > a loop of oxide that
    // wears > playback head (gap loss, head bump, de-emphasis) with wow and flutter, hiss.
    // The tape ages while it runs: flakes of oxide shed (dropouts that come round again
    // every pass), the gap loss creeps down, the warble widens, the hiss rises, and with
    // freeze the held loop slowly disintegrates pass after pass. A refresh puts on new tape.
    //
    // Left and right are two tracks of the one tape: the same speed and the same heads, but each
    // track has its own flakes, its own flutter and its own hiss.
    struct TapeLine
    {
        static constexpr int kBlock = 512, kFix = 12;
        static constexpr float kLineGain = 1.15f;
        float* store[2] { nullptr, nullptr };       // what was allocated
        float* buf[2] { nullptr, nullptr };         // the tracks (one sample of the far end before, three of the start after)
        float* dmg[2] { nullptr, nullptr };
        int size = 1, w = 0, nb = 1;
        float sr = 48000.0f, age = 0, wearNow = 0;
        // the head's distance from the record head in samples, and the same in fixed point as the samples use it
        float dA = 0, dS = 0, dB = 0, dTarget = 0, slewA = 0.001f;
        int32_t dFix = 0, dStep = 0, dBFix = 0, dAFix = 0;
        bool fading = false, frozen = false;
        int xfN = 0, xfLen = 3360;
        float gFade[4], dFade[4];                   // lanes 0,1: the old head's gain; 2,3: the new head's
        int loopStart = 0, loopLen = 1, loopPos = 0;
        float invSplice = 0.0f;
        // wow: a slow random wander; flutter: the capstan and pinch roller (lanes 0,1: left, right)
        float wowT[4], wow1[4], wow2[4], flutPh[4], flutPh2[4], flutInc[4], flutInc2[4], flutAmp[4];
        int wowTimer[2] { 0, 0 };
        int32_t wobFix[2] { 0, 0 }, wobStep[2] { 0, 0 };
        float wowDepth = 100, flutDepth = 1, wowA = 0.0001f;
        float pre[4], gRec[4], dGRec[4];
        float gap1[4], gap2[4], gapA[4], dGapA[4], drop[4], dDrop[4], deemph[4], decayA[4], decayB[4], drNow[4];
        float preA = 0.2f, hissA = 0.5f, hissHA = 0.1f;
        float hissLP[4], hissHP[4], crackle[4], fbOut[4];
        float hissGain = 0, hissTarget = 1, hissUp = 0.00002f, hissDown = 0.00002f, hissAmp = 0, hissShape = 0;
        float wowK = 0, flutK = 0, recK = 1, gapK = 1;       // (what the wear makes of the constants, once a block)
        int crackAt[2] { -1, -1 };
        float crackVal[2] { 0, 0 };
        bool crackOn = false;
        Biquad4 qb;         // lanes 0,1: the loop's high-pass; lanes 2,3: the head bump
        DCBlock4 dc;
        uint32_t rng[4];
        int32_t scratch[4];
        Rng ctl[2];

        ~TapeLine() { release(); }
        TapeLine() {}
        TapeLine (const TapeLine&) = delete;
        TapeLine& operator= (const TapeLine&) = delete;
        void release() { for (int c = 0; c < 2; ++c) { delete[] store[c]; delete[] dmg[c]; store[c] = dmg[c] = buf[c] = nullptr; } }

        void prepare (float s, float maxSec, uint32_t seed)
        {
            sr = s;
            release();
            size = (int) (maxSec * sr) + 16;
            nb = size / kBlock + 1;
            for (int c = 0; c < 2; ++c)
            {
                ctl[c].setSeed (seed + 101u * (uint32_t) c);
                store[c] = new float[size + 4];
                buf[c] = store[c] + 1;
                dmg[c] = new float[nb];
                for (int i = 0; i < nb; ++i) dmg[c][i] = 0.0f;
            }
            for (int k = 0; k < 4; ++k) rng[k] = (seed + 57u * (uint32_t) k) * 2654435761u + 12345u;
            slewA = coefOver (onePoleCoef (1.6f, sr), kChunk);          // the capstan servo's inertia
            xfLen = ((int) (0.07f * sr) / kChunk) * kChunk;
            wowA = coefOver (onePoleCoef (0.55f, sr), kChunk);
            for (int k = 0; k < 4; ++k)
            {
                Rng& r = ctl[k & 1];
                flutInc[k] = (6.1f + r.nextFloat() * 0.8f) / sr * (float) kChunk;
                flutInc2[k] = (11.3f + r.nextFloat() * 1.5f) / sr * (float) kChunk;
            }
            wowDepth = 0.0021f * sr;                                // ~0.5% pitch wander when new
            flutDepth = 0.000028f * sr;
            preA = onePoleCoef (2400.0f, sr);
            hissA = onePoleCoef (11000.0f, sr);
            hissHA = onePoleCoef (900.0f, sr);
            hissUp = 1.0f / (1.6f * sr);
            hissDown = 1.0f / (1.4f * sr);
            BiquadCoef c;
            c.peak (95.0f, sr, 0.9f, 2.5f); qb.setPair (2, c);
            dc.prepare (sr, 8.0f);
            reset();
        }
        void reset()
        {
            for (int c = 0; c < 2; ++c) for (int i = 0; i < size + 4; ++i) store[c][i] = 0.0f;
            w = 0; dA = dS = dB = dTarget = 0.33f * sr; fading = frozen = false; xfN = 0;
            dFix = dBFix = dAFix = (int32_t) (dA * (float) (1 << kFix)); dStep = 0;
            set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
            set4 (wowT, 0); set4 (wow1, 0); set4 (wow2, 0); set4 (flutPh, 0); set4 (flutPh2, 0); set4 (flutAmp, 1.0f);
            wobFix[0] = wobFix[1] = wobStep[0] = wobStep[1] = 0; wowTimer[0] = wowTimer[1] = 0;
            set4 (pre, 0); set4 (gRec, 1.0f); set4 (dGRec, 0);
            set4 (gap1, 0); set4 (gap2, 0); set4 (gapA, 0.6f); set4 (dGapA, 0); set4 (drop, 1.0f); set4 (dDrop, 0); set4 (deemph, 0);
            set4 (decayA, 0); set4 (decayB, 1.0f); set4 (drNow, 0);
            set4 (hissLP, 0); set4 (hissHP, 0); set4 (crackle, 0); set4 (fbOut, 0);
            hissGain = 0; crackOn = false; crackAt[0] = crackAt[1] = -1;
            qb.reset(); dc.reset();
        }
        void setTime (float sec, bool jump)
        {
            dTarget = clampf (64.0f, (float) size - wowDepth * 4.0f - 32.0f, sec * sr);
            if (jump) { dA = dS = dB = dTarget; dFix = dBFix = dAFix = (int32_t) (dA * (float) (1 << kFix)); dStep = 0; fading = false; }
        }
        void setLoopHP (const BiquadCoef& c) { qb.setPair (0, c); }
        void refresh() { for (int c = 0; c < 2; ++c) for (int i = 0; i < nb; ++i) dmg[c][i] = 0.0f; }

        void addFlake (int ch)
        {
            Rng& r = ctl[ch];
            const int b = r.nextInt (nb);
            const int len = 1 + r.nextInt (1 + (int) (5.0f * age));
            const float s = (0.15f + 0.75f * r.nextFloat()) * sqrtf (age);
            for (int k = 0; k < len; ++k)
            {
                const float wnd = sin01 (0.5f * ((float) k + 0.5f) / (float) len);
                float& d = dmg[ch][(b + k) % nb];
                d = minf (1.0f, maxf (d, s * wnd) + 0.08f * s * wnd);
            }
        }
        // once a block: oxide sheds as the tape runs, the hiss settles, a worn splice may crackle
        void block (int n)
        {
            const float dt = (float) n / sr;
            wearNow = sqrtf (maxf (0.0f, age));
            const float rate = 0.01f + 0.12f * age * age;            // flakes per second: ~25 over a tape's life
            for (int c = 0; c < 2; ++c)
                if (age > 0.001f && ctl[c].nextFloat() < rate * dt) addFlake (c);
            hissGain += clampf (-hissDown * (float) n, hissUp * (float) n, hissTarget - hissGain);
            hissShape = hissGain * hissGain * (3.0f - 2.0f * hissGain);
            hissAmp = (0.0018f + 0.008f * wearNow) * hissShape * 2.0f;
            wowK = wowDepth * (1.0f + 1.6f * wearNow) * (float) (1 << kFix);
            flutK = flutDepth * (1.0f + 2.0f * wearNow) * (float) (1 << kFix);
            recK = 1.0f - 0.18f * wearNow;
            gapK = twoPi * 15000.0f / sr * (1.0f - 0.8f * wearNow);
            // crumbling: the odd crackle of a worn splice
            crackAt[0] = crackAt[1] = -1;
            if (wearNow > 0.25f)
                for (int c = 0; c < 2; ++c)
                    if (ctl[c].nextFloat() < 0.00003f * wearNow * wearNow * (1.0f + 8.0f * drNow[c]) * (float) n)
                    {
                        crackAt[c] = ctl[c].nextInt (n);
                        crackVal[c] = (ctl[c].nextFloat() * 2.0f - 1.0f) * 0.02f * wearNow;
                    }
        }
        inline int wrapIndex (int i) const { if (i < 0) i += size; else if (i >= size) i -= size; return i; }
        inline void mirror (int i)
        {
            // keep the copies beyond the ends in step, so four taps can always be read in one go
            if (i == size - 1) { buf[0][-1] = buf[0][i]; buf[1][-1] = buf[1][i]; }
            else if (i < 3) { buf[0][size + i] = buf[0][i]; buf[1][size + i] = buf[1][i]; }
        }
        float dmgAt (int ch, int posn) const
        {
            const int i = posn >> 9, j = i + 1 < nb ? i + 1 : 0;
            return dmg[ch][i] + (dmg[ch][j] - dmg[ch][i]) * ((float) (posn & (kBlock - 1)) * (1.0f / (float) kBlock));
        }
        void enterFreeze()
        {
            frozen = true; fading = false; set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
            loopLen = dFix >> kFix; if (loopLen < 256) loopLen = 256;
            loopStart = wrapIndex (w - 1 - loopLen); loopPos = 0;
            const int x = loopLen / 4 < 600 ? loopLen / 4 : 600;
            invSplice = 1.0f / (float) x;
        }

        // every 16 samples: the tape's speed, its wobble and what the oxide under the heads is like
        void control (bool fadeMode, bool freezeNow)
        {
            const float one = (float) (1 << kFix);
            if (freezeNow && ! frozen) enterFreeze();
            else if (! freezeNow && frozen) frozen = false;
            if (! frozen)
            {
                if (! fadeMode)
                {
                    fading = false; set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0);
                    if (bitsOf (dA) != bitsOf (dTarget))
                    {
                        dS += (dTarget - dS) * slewA;                          // tape speed glides (pitch bends)
                        dA += (dS - dA) * slewA;
                        if (fabsf (dTarget - dA) < 1.0e-3f && fabsf (dTarget - dS) < 1.0e-3f) dA = dS = dTarget;
                        dAFix = (int32_t) (dA * one);
                    }
                }
                else
                {
                    if (! fading && fabsf (dTarget - dA) > 0.01f * dA + 4.0f)
                    {
                        dB = dTarget; dBFix = (int32_t) (dB * one); fading = true; xfN = 0;
                    }
                    if (fading)
                    {
                        const float t0 = (float) xfN / (float) xfLen, t1 = (float) (xfN + kChunk) / (float) xfLen;
                        set22 (gFade, fadeOut (t0), fadeIn (t0));
                        set22 (dFade, (fadeOut (t1) - fadeOut (t0)) * (1.0f / (float) kChunk), (fadeIn (t1) - fadeIn (t0)) * (1.0f / (float) kChunk));
                    }
                    dS = dA;
                }
                dStep = (dAFix - dFix) / kChunk;
            }
            else dStep = 0;

            // wow and flutter
            for (int c = 0; c < 2; ++c)
                if (--wowTimer[c] <= 0)
                {
                    wowT[c] = ctl[c].nextFloat() * 2.0f - 1.0f;
                    wowTimer[c] = (int) (sr * (0.5f + 1.8f * ctl[c].nextFloat())) / kChunk;
                }
            const f4 wa = splat (wowA);
            f4 w1 = ld (wow1), w2 = ld (wow2);
            w1 = mla (w1, ld (wowT) - w1, wa);
            w2 = mla (w2, w1 - w2, wa);
            st (wow1, w1); st (wow2, w2);
            f4 p1 = ld (flutPh) + ld (flutInc), p2 = ld (flutPh2) + ld (flutInc2);
            p1 = sel4 (gt4 (splat (1.0f), p1), p1, p1 - splat (1.0f));
            p2 = sel4 (gt4 (splat (1.0f), p2), p2, p2 - splat (1.0f));
            st (flutPh, p1); st (flutPh2, p2);
            const u4 s = lcg4 (ldu (rng));
            stu (rng, s);
            f4 fa = ld (flutAmp);
            fa = mla (fa, mla (splat (1.0f), noise4 (s), splat (0.25f)) - fa, splat (0.0004f * (float) kChunk));
            st (flutAmp, fa);
            const f4 wob = splat (wowK) * w2 + splat (flutK) * fa * mla (sin01x4 (p1), sin01x4 (p2), splat (0.35f));
            int32_t* const wt = scratch;
            sti (wt, toInt (wob));
            wobStep[0] = (wt[0] - wobFix[0]) / kChunk;
            wobStep[1] = (wt[1] - wobFix[1]) / kChunk;

            // the oxide: under the playback head (lanes 0,1) and under the record head (lanes 2,3)
            int rp[2];
            for (int c = 0; c < 2; ++c)
                rp[c] = frozen ? wrapIndex (loopStart + loopPos) : wrapIndex (w - 1 - ((dFix + wobFix[c]) >> kFix));
            const int i0 = rp[0] >> 9, i1 = rp[1] >> 9, iw = w >> 9;            // (all below nb: a position is below size)
            const int j0 = i0 + 1 < nb ? i0 + 1 : 0, j1 = i1 + 1 < nb ? i1 + 1 : 0, jw = iw + 1 < nb ? iw + 1 : 0;
            const f4 lo = ld4s (dmg[0] + i0, dmg[1] + i1, dmg[0] + iw, dmg[1] + iw);
            const f4 hi = ld4s (dmg[0] + j0, dmg[1] + j1, dmg[0] + jw, dmg[1] + jw);
            const f4 fr = CHL_FIX_TO_FLOAT (fromInts ((uint32_t) (rp[0] & (kBlock - 1)), (uint32_t) (rp[1] & (kBlock - 1)),
                                                      (uint32_t) (w & (kBlock - 1)), (uint32_t) (w & (kBlock - 1))), 9);
            const f4 d = mla (lo, hi - lo, fr);
            const f4 k16 = splat (1.0f / (float) kChunk);
            // record: what the oxide still takes
            const f4 gr = mls (splat (1.0f), d, splat (0.8f)) * splat (recK);
            st (dGRec, (gr - ld (gRec)) * k16);
            // playback: the gap loss creeps down with wear and where oxide shed, and the level drops out there
            const f4 dr = dupLow (d);
            st (drNow, dr);
            const f4 wc = splat (gapK) * mls (splat (1.0f), dr, splat (0.55f));
            const f4 a = wc * recip2 (wc + splat (1.0f));
            st (dGapA, (a - ld (gapA)) * k16);
            st (dDrop, (mls (splat (1.0f), dr, splat (0.4f)) - ld (drop)) * k16);
            // the held loop disintegrates a little each pass, worst where the oxide is gone
            if (frozen)
            {
                const int ib = rp[0] >> 9;
                const f4 dd = ldPair (dmg[0] + ib, dmg[1] + ib, splat (0.0f));
                st (decayA, mla (splat (0.03f * wearNow), dd, splat (0.3f)));
                st (decayB, mls (splat (1.0f - 0.0009f * wearNow), dd, splat (0.012f)));
            }
        }
        // after each 16 samples
        void controlEnd()
        {
            if (fading)
            {
                xfN += kChunk;
                if (xfN >= xfLen) { dA = dS = dB; dFix = dAFix = dBFix; dStep = 0; fading = false; set22 (gFade, 1.0f, 0.0f); set4 (dFade, 0); }
            }
        }

        // the four cubic weights for two read positions: f = {fl, fr, fl, fr} (0..1)
        static CHL_INLINE void cubic (f4 f, f4& w01, f4& w23)
        {
            w01 = mla (pair4 (0.0f, 1.0f), f, mla (pair4 (-0.5f, 0.0f), f, mla (pair4 (1.0f, -2.5f), f, pair4 (-0.5f, 1.5f))));
            w23 = f * mla (pair4 (0.5f, 0.0f), f, mla (pair4 (2.0f, -0.5f), f, pair4 (-1.5f, 0.5f)));
        }
        // both tracks at their own fixed-point positions: {l, r, l, r}
        CHL_INLINE f4 read (int32_t pl, int32_t pr) const
        {
            const int32_t span = size << kFix;
            if (pl < 0) pl += span; else if (pl >= span) pl -= span;
            if (pr < 0) pr += span; else if (pr >= span) pr -= span;
            const uint32_t fl = (uint32_t) pl & ((1u << kFix) - 1u), fr = (uint32_t) pr & ((1u << kFix) - 1u);
            f4 w01, w23, z01, z23;
            cubic (CHL_FIX_TO_FLOAT (fromInts (fl, fr, fl, fr), kFix), w01, w23);
            zip4 (ld (buf[0] + (pl >> kFix) - 1), ld (buf[1] + (pr >> kFix) - 1), z01, z23);
            const f4 p = mla (z01 * w01, z23, w23);
            return p + swapHalves (p);
        }

        // i: the sample's place in the block (for the crackle)
        CHL_NOINLINE f4 tick (f4 v, f4 gain, int i)
        {
            const int32_t minD = 8 << kFix;
            f4 out;
            if (! frozen)
            {
                // playback head(s)
                const int32_t base = w << kFix;          // (this sample is written after the read: the newest on the tape is w - 1)
                const int32_t wl = wobFix[0], wr = wobFix[1];
                int32_t dl = dFix + wl, dr = dFix + wr;
                if (dl < minD) dl = minD;
                if (dr < minD) dr = minD;
                out = read (base - dl, base - dr);
                if (fading)
                {
                    int32_t el = dBFix + wl, er = dBFix + wr;
                    if (el < minD) el = minD;
                    if (er < minD) er = minD;
                    const f4 g = ld (gFade);
                    out = mla (out * dupLow (g), read (base - el, base - er), dupHigh (g));
                    st (gFade, g + ld (dFade));
                }
                dFix += dStep;
            }
            else
            {
                if (++loopPos >= loopLen) loopPos -= loopLen;
                const int32_t base = (loopStart + loopPos) << kFix;
                const int32_t pl = base - (wobFix[0] >> 1), pr = base - (wobFix[1] >> 1);
                out = read (pl, pr);
                const int x = loopLen / 4 < 600 ? loopLen / 4 : 600;      // hide the splice
                if (loopPos > loopLen - x)
                {
                    const f4 t = toFloat (splati (loopPos - (loopLen - x))) * splat (invSplice);
                    const int32_t back = loopLen << kFix;
                    out = mla (out, read (pl - back, pr - back) - out, t);
                }
                const int ip = wrapIndex (loopStart + loopPos), pp = ip > 0 ? ip - 1 : size - 1;
                f4 s = ldPair (buf[0] + ip, buf[1] + ip, splat (0.0f));
                s = mla (s, ldPair (buf[0] + pp, buf[1] + pp, splat (0.0f)) - s, ld (decayA)) * ld (decayB);
                stPair (buf[0] + ip, buf[1] + ip, s);
                mirror (ip);
            }
            wobFix[0] += wobStep[0]; wobFix[1] += wobStep[1];

            // playback colouring
            const f4 a = ld (gapA) + ld (dGapA);
            st (gapA, a);
            f4 g1 = ld (gap1), g2 = ld (gap2);
            g1 = mla (g1, out - g1, a);
            g2 = mla (g2, g1 - g2, a);
            st (gap1, g1); st (gap2, g2);
            const f4 yb = qb.tick (lowHigh (v, g2));            // lanes 0,1: the loop, high-passed; lanes 2,3: after the head bump
            f4 de = ld (deemph);
            de = mla (de, yb - de, splat (preA));
            st (deemph, de);
            const f4 dp = ld (drop) + ld (dDrop);
            st (drop, dp);
            f4 y = mls (yb, yb - de, splat (0.375f)) * dp;      // de-emphasis, and the dropout where oxide shed

            // hiss (recorded with everything else, so it builds with the regen)
            const u4 s = lcg4 (ldu (rng));
            stu (rng, s);
            f4 hl = ld (hissLP), hh = ld (hissHP);
            hl = mla (hl, noise4 (s) - hl, splat (hissA));
            hh = mla (hh, hl - hh, splat (hissHA));
            st (hissLP, hl); st (hissHP, hh);
            y = mla (y, hl - hh, splat (hissAmp));
            if (crackOn || i == crackAt[0] || i == crackAt[1])
            {
                if (i == crackAt[0]) { crackle[0] = crackle[2] = crackVal[0]; crackOn = true; }
                if (i == crackAt[1]) { crackle[1] = crackle[3] = crackVal[1]; crackOn = true; }
                const f4 ck = ld (crackle) * splat (0.92f);
                st (crackle, ck);
                y = mla (y, ck, splat (hissShape));
            }
            y = dc.tick (y);

            // record head: pre-emphasis, then the magnetic saturation (a little asymmetric); the loop's
            // own saturation is done beside it
            f4 pe = ld (pre);
            pe = mla (pe, v - pe, splat (preA));
            st (pre, pe);
            const f4 e = mla (v, v - pe, splat (0.6f));
            const f4 t = mla (pair4 (0.0f, -0.04f), tanh4 (mla (pair4 (0.0f, 0.04f), lowHigh (yb, e), gain)), pair4 (1.05f, 1.0f));
            st (fbOut, t);
            const f4 gr = ld (gRec) + ld (dGRec);
            st (gRec, gr);
            if (! frozen)
            {
                const f4 m = t * gr;
                stLane (buf[0] + w, m, 2); stLane (buf[1] + w, m, 3);
                mirror (w);
                if (++w >= size) w = 0;
            }
            return y;
        }
        // once a block: has the crackle died away
        void blockEnd() { crackOn = (bitsOf (crackle[0]) & 0x7fffffffu) > 0x33000000u || (bitsOf (crackle[1]) & 0x7fffffffu) > 0x33000000u; }
    };
}
