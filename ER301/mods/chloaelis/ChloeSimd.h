#pragma once
// Four-lane float vectors for the Chloaelis DSP.
//
// Why everything per-sample goes through these: the ER-301's Cortex-A8 has two floating-point
// units. Ordinary scalar float code (what the compiler makes of `a * b + c`) runs on the
// VFP-lite, which is not pipelined: about ten cycles for every add or multiply, and a
// float compare whose result has to cross back to the integer core costs twenty more. NEON
// instructions run on a pipelined unit several times faster, and they carry four values at
// once. So the units keep their per-sample state in lanes (left and right channel, and where
// the maths allows it two stages of a chain side by side) and use only the operations below:
// no scalar float arithmetic, no float compare-and-branch, no divide, no library call per
// sample. NEON also flushes denormals to zero by itself.
//
// On machines without NEON (the Linux emulator on x86) the same names are plain C++.
#include <cstdint>
#include <cstring>

#if ! defined(CHL_NO_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define CHL_NEON 1
#include <arm_neon.h>
#else
#define CHL_NEON 0
#include <cmath>
#endif

// Small functions that hand vectors back through references must vanish into their caller: a
// vector that is given an address ends up in memory with an alignment promise attached to every
// access, and the units keep no such promises (see tools/check-package.py).
#define CHL_INLINE inline __attribute__ ((always_inline))
// And the other way round: a function that is called while its caller holds vectors must be a
// real call with few of them (four vector registers survive a call; a fifth would be parked on
// the stack, with a promise).
#define CHL_NOINLINE __attribute__ ((noinline))

namespace chl
{
#if CHL_NEON
    typedef float32x4_t f4;
    typedef uint32x4_t u4;
    typedef int32x4_t i4;

    static inline f4 splat (float x) { return vdupq_n_f32 (x); }
    static inline f4 ld (const float* p) { return vld1q_f32 (p); }
    static inline f4 ldSplat (const float* p) { return vld1q_dup_f32 (p); }
    static inline void st (float* p, f4 v) { vst1q_f32 (p, v); }
    static inline u4 ldu (const uint32_t* p) { return vld1q_u32 (p); }
    static inline void stu (uint32_t* p, u4 v) { vst1q_u32 (p, v); }
    static inline void sti (int32_t* p, i4 v) { vst1q_s32 (p, v); }
    static inline u4 splatu (uint32_t x) { return vdupq_n_u32 (x); }

    static inline f4 mla (f4 a, f4 b, f4 c) { return vmlaq_f32 (a, b, c); }      // a + b * c
    static inline f4 mls (f4 a, f4 b, f4 c) { return vmlsq_f32 (a, b, c); }      // a - b * c
    static inline f4 min4 (f4 a, f4 b) { return vminq_f32 (a, b); }
    static inline f4 max4 (f4 a, f4 b) { return vmaxq_f32 (a, b); }
    static inline f4 abs4 (f4 a) { return vabsq_f32 (a); }
    static inline f4 neg4 (f4 a) { return vnegq_f32 (a); }
    static inline u4 gt4 (f4 a, f4 b) { return vcgtq_f32 (a, b); }
    static inline f4 sel4 (u4 m, f4 a, f4 b) { return vbslq_f32 (m, a, b); }     // m ? a : b
    static inline u4 and4 (u4 a, u4 b) { return vandq_u32 (a, b); }
    static inline u4 or4 (u4 a, u4 b) { return vorrq_u32 (a, b); }
    static inline u4 xor4 (u4 a, u4 b) { return veorq_u32 (a, b); }
    static inline u4 andnot4 (u4 a, u4 b) { return vbicq_u32 (a, b); }           // a & ~b
    static inline u4 bits (f4 a) { return vreinterpretq_u32_f32 (a); }
    static inline f4 flt (u4 a) { return vreinterpretq_f32_u32 (a); }
    static inline i4 toInt (f4 a) { return vcvtq_s32_f32 (a); }                  // towards zero
    static inline f4 toFloat (i4 a) { return vcvtq_f32_s32 (a); }

    // 1 / d by estimate and Newton steps: one step is good to about 1.5e-5, two to 1e-9
    static inline f4 recip1 (f4 d) { f4 r = vrecpeq_f32 (d); return vmulq_f32 (r, vrecpsq_f32 (d, r)); }
    static inline f4 recip2 (f4 d) { f4 r = recip1 (d); return vmulq_f32 (r, vrecpsq_f32 (d, r)); }
    // 1 / sqrt (d), one Newton step
    static inline f4 rsqrt1 (f4 d) { f4 r = vrsqrteq_f32 (d); return vmulq_f32 (r, vrsqrtsq_f32 (vmulq_f32 (d, r), r)); }

    // the left / right pair of one sample, into lanes 0 and 1 (the other two keep `v`'s)
    static inline f4 ldPair (const float* l, const float* r, f4 v) { v = vld1q_lane_f32 (l, v, 0); return vld1q_lane_f32 (r, v, 1); }
    static inline void stPair (float* l, float* r, f4 v) { vst1q_lane_f32 (l, v, 0); vst1q_lane_f32 (r, v, 1); }
    // lanes 0,1 of a with lanes 0,1 of b behind them: {a0, a1, b0, b1}
    static inline f4 lowLow (f4 a, f4 b) { return vcombine_f32 (vget_low_f32 (a), vget_low_f32 (b)); }
    // {a2, a3, b2, b3}
    static inline f4 highHigh (f4 a, f4 b) { return vcombine_f32 (vget_high_f32 (a), vget_high_f32 (b)); }
    // {a0, a1, a0, a1}
    static inline f4 dupLow (f4 a) { return vcombine_f32 (vget_low_f32 (a), vget_low_f32 (a)); }
    // {a2, a3, a2, a3}
    static inline f4 dupHigh (f4 a) { return vcombine_f32 (vget_high_f32 (a), vget_high_f32 (a)); }
    // lane-wise 32-bit pseudo-random step (the same generator as Rng, four at once)
    static inline u4 lcg4 (u4 s) { return vmlaq_u32 (vdupq_n_u32 (1013904223u), s, vdupq_n_u32 (1664525u)); }
    // the generator's state as a float in -0.5 .. 0.5
    static inline f4 noise4 (u4 s) { return vcvtq_n_f32_s32 (vreinterpretq_s32_u32 (s), 32); }
    // an unsigned fixed-point number with `fracBits` bits after the point, as a float
#define CHL_FIX_TO_FLOAT(u, fracBits) vcvtq_n_f32_u32 ((u), (fracBits))
    // four 4-tap reads at once: rows a, b, c, d (each four taps) against four weights each, giving
    // {sum a, sum b, sum c, sum d}
    static inline f4 dot4x4 (f4 a, f4 wa, f4 b, f4 wb, f4 c, f4 wc, f4 d, f4 wd)
    {
        const f4 pa = vmulq_f32 (a, wa), pb = vmulq_f32 (b, wb), pc = vmulq_f32 (c, wc), pd = vmulq_f32 (d, wd);
        const float32x2_t sa = vpadd_f32 (vget_low_f32 (pa), vget_high_f32 (pa)), sb = vpadd_f32 (vget_low_f32 (pb), vget_high_f32 (pb));
        const float32x2_t sc = vpadd_f32 (vget_low_f32 (pc), vget_high_f32 (pc)), sd = vpadd_f32 (vget_low_f32 (pd), vget_high_f32 (pd));
        return vcombine_f32 (vpadd_f32 (sa, sb), vpadd_f32 (sc, sd));
    }
    // from four vectors each holding {x_k, x_k+1, ..} keep element 0 of each in `lo` and element 1 in `hi`
    static CHL_INLINE void gather2 (const float* p0, const float* p1, const float* p2, const float* p3, f4& lo, f4& hi)
    {
        const float32x4_t a = vcombine_f32 (vld1_f32 (p0), vld1_f32 (p1)), b = vcombine_f32 (vld1_f32 (p2), vld1_f32 (p3));
        const float32x4x2_t u = vuzpq_f32 (a, b);
        lo = u.val[0]; hi = u.val[1];
    }
    // {a2, a3, b0, b1} and {a0, a1, b2, b3} and {a2, a3, a0, a1}
    static inline f4 highLow (f4 a, f4 b) { return vcombine_f32 (vget_high_f32 (a), vget_low_f32 (b)); }
    static inline f4 lowHigh (f4 a, f4 b) { return vcombine_f32 (vget_low_f32 (a), vget_high_f32 (b)); }
    static inline f4 swapHalves (f4 a) { return vcombine_f32 (vget_high_f32 (a), vget_low_f32 (a)); }
    // two floats from memory into lanes 0,1 (lanes 2,3 zero), and back
    static inline f4 ld2 (const float* p) { return vcombine_f32 (vld1_f32 (p), vdup_n_f32 (0.0f)); }
    static inline void st2 (float* p, f4 v) { vst1_f32 (p, vget_low_f32 (v)); }
    static inline void st2hi (float* p, f4 v) { vst1_f32 (p, vget_high_f32 (v)); }
    // 2^n for whole n
    static inline f4 pow2i (i4 n) { return vreinterpretq_f32_s32 (vshlq_n_s32 (vaddq_s32 (n, vdupq_n_s32 (127)), 23)); }
    // four unsigned integers from the integer core into lanes (cheap in this direction)
    static inline u4 fromInts (uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        u4 v = vdupq_n_u32 (a);
        v = vsetq_lane_u32 (b, v, 1); v = vsetq_lane_u32 (c, v, 2); return vsetq_lane_u32 (d, v, 3);
    }
    // rows a, b, c, d (four values each) become columns
    static CHL_INLINE void transpose4 (f4& a, f4& b, f4& c, f4& d)
    {
        const float32x4x2_t ab = vtrnq_f32 (a, b), cd = vtrnq_f32 (c, d);
        a = vcombine_f32 (vget_low_f32 (ab.val[0]), vget_low_f32 (cd.val[0]));
        b = vcombine_f32 (vget_low_f32 (ab.val[1]), vget_low_f32 (cd.val[1]));
        c = vcombine_f32 (vget_high_f32 (ab.val[0]), vget_high_f32 (cd.val[0]));
        d = vcombine_f32 (vget_high_f32 (ab.val[1]), vget_high_f32 (cd.val[1]));
    }
    // The PT2399's memory: one 16-bit word per bit position, the low byte the left channel's bit
    // and the high byte the right's, each stored as 0x00 or 0xff. Read: the word as masks in
    // lanes {l, r, l, r}. Write: lanes 2 and 3 of a mask vector.
    static inline u4 ramLoad (const uint16_t* p)
    {
        const int8x8_t b = vreinterpret_s8_u16 (vld1_dup_u16 (p));
        return vreinterpretq_u32_s32 (vmovl_s16 (vget_low_s16 (vmovl_s8 (b))));
    }
    static inline void ramStore (uint16_t* p, u4 m)
    {
        const uint16x4_t h = vmovn_u32 (m);
        vst1_lane_u16 (p, vreinterpret_u16_u8 (vmovn_u16 (vcombine_u16 (h, h))), 1);
    }
    static inline i4 splati (int32_t x) { return vdupq_n_s32 (x); }
    static inline u4 selu (u4 m, u4 a, u4 b) { return vbslq_u32 (m, a, b); }
    static inline u4 lowHighU (u4 a, u4 b) { return vcombine_u32 (vget_low_u32 (a), vget_high_u32 (b)); }     // {a0, a1, b2, b3}
    static inline u4 gtZero (f4 a) { return vcgtq_f32 (a, vdupq_n_f32 (0.0f)); }
    // a mask (all ones or none) as a sign bit
    static inline u4 signOf (u4 m) { return vshlq_n_u32 (m, 31); }
    // four floats from four places
    static inline f4 ld4s (const float* a, const float* b, const float* c, const float* d)
    {
        f4 v = vld1q_dup_f32 (a);
        v = vld1q_lane_f32 (b, v, 1); v = vld1q_lane_f32 (c, v, 2); return vld1q_lane_f32 (d, v, 3);
    }
    // {a0, b0, a1, b1} and {a2, b2, a3, b3}
    static CHL_INLINE void zip4 (f4 a, f4 b, f4& lo, f4& hi) { const float32x4x2_t z = vzipq_f32 (a, b); lo = z.val[0]; hi = z.val[1]; }
    static inline void stLane (float* p, f4 v, int lane)
    {
        switch (lane) { case 0: vst1q_lane_f32 (p, v, 0); break; case 1: vst1q_lane_f32 (p, v, 1); break;
                        case 2: vst1q_lane_f32 (p, v, 2); break; default: vst1q_lane_f32 (p, v, 3); break; }
    }
    static inline float lane0 (f4 a) { return vgetq_lane_f32 (a, 0); }
    // {lo, lo, hi, hi} and {a, b, c, d} from plain numbers (no table in memory)
    static CHL_INLINE f4 pair4 (float lo, float hi) { return vcombine_f32 (vdup_n_f32 (lo), vdup_n_f32 (hi)); }
    static CHL_INLINE f4 make4 (float a, float b, float c, float d)
    {
        f4 v = vdupq_n_f32 (a);
        v = vsetq_lane_f32 (b, v, 1); v = vsetq_lane_f32 (c, v, 2); return vsetq_lane_f32 (d, v, 3);
    }
    static CHL_INLINE u4 pairu (uint32_t lo, uint32_t hi) { return vcombine_u32 (vdup_n_u32 (lo), vdup_n_u32 (hi)); }
    // the larger of two lanes, as a number (it leaves the vector unit: for once-a-block use)
    static CHL_INLINE float maxLanes01 (f4 v) { const float32x2_t d = vget_low_f32 (v); return vget_lane_f32 (vpmax_f32 (d, d), 0); }
    static CHL_INLINE float maxLanes23 (f4 v) { const float32x2_t d = vget_high_f32 (v); return vget_lane_f32 (vpmax_f32 (d, d), 0); }
    static inline void prefetch (const void* p) { __builtin_prefetch (p); }
#else
    //==========================================================================
    struct f4 { float v[4]; };
    struct u4 { uint32_t v[4]; };
    struct i4 { int32_t v[4]; };
#define CHL_EACH for (int k = 0; k < 4; ++k)
    static inline f4 operator+ (f4 a, f4 b) { f4 r; CHL_EACH r.v[k] = a.v[k] + b.v[k]; return r; }
    static inline f4 operator- (f4 a, f4 b) { f4 r; CHL_EACH r.v[k] = a.v[k] - b.v[k]; return r; }
    static inline f4 operator* (f4 a, f4 b) { f4 r; CHL_EACH r.v[k] = a.v[k] * b.v[k]; return r; }
    static inline f4 splat (float x) { f4 r; CHL_EACH r.v[k] = x; return r; }
    static inline f4 ld (const float* p) { f4 r; CHL_EACH r.v[k] = p[k]; return r; }
    static inline f4 ldSplat (const float* p) { return splat (*p); }
    static inline void st (float* p, f4 a) { CHL_EACH p[k] = a.v[k]; }
    static inline u4 ldu (const uint32_t* p) { u4 r; CHL_EACH r.v[k] = p[k]; return r; }
    static inline void stu (uint32_t* p, u4 a) { CHL_EACH p[k] = a.v[k]; }
    static inline void sti (int32_t* p, i4 a) { CHL_EACH p[k] = a.v[k]; }
    static inline u4 splatu (uint32_t x) { u4 r; CHL_EACH r.v[k] = x; return r; }
    static inline f4 mla (f4 a, f4 b, f4 c) { f4 r; CHL_EACH r.v[k] = a.v[k] + b.v[k] * c.v[k]; return r; }
    static inline f4 mls (f4 a, f4 b, f4 c) { f4 r; CHL_EACH r.v[k] = a.v[k] - b.v[k] * c.v[k]; return r; }
    static inline f4 min4 (f4 a, f4 b) { f4 r; CHL_EACH r.v[k] = a.v[k] < b.v[k] ? a.v[k] : b.v[k]; return r; }
    static inline f4 max4 (f4 a, f4 b) { f4 r; CHL_EACH r.v[k] = a.v[k] > b.v[k] ? a.v[k] : b.v[k]; return r; }
    static inline f4 abs4 (f4 a) { f4 r; CHL_EACH r.v[k] = std::fabs (a.v[k]); return r; }
    static inline f4 neg4 (f4 a) { f4 r; CHL_EACH r.v[k] = -a.v[k]; return r; }
    static inline u4 gt4 (f4 a, f4 b) { u4 r; CHL_EACH r.v[k] = a.v[k] > b.v[k] ? 0xffffffffu : 0u; return r; }
    static inline u4 and4 (u4 a, u4 b) { u4 r; CHL_EACH r.v[k] = a.v[k] & b.v[k]; return r; }
    static inline u4 or4 (u4 a, u4 b) { u4 r; CHL_EACH r.v[k] = a.v[k] | b.v[k]; return r; }
    static inline u4 xor4 (u4 a, u4 b) { u4 r; CHL_EACH r.v[k] = a.v[k] ^ b.v[k]; return r; }
    static inline u4 andnot4 (u4 a, u4 b) { u4 r; CHL_EACH r.v[k] = a.v[k] & ~b.v[k]; return r; }
    static inline u4 bits (f4 a) { u4 r; std::memcpy (&r, &a, sizeof (r)); return r; }
    static inline f4 flt (u4 a) { f4 r; std::memcpy (&r, &a, sizeof (r)); return r; }
    static inline f4 sel4 (u4 m, f4 a, f4 b) { const u4 x = bits (a), y = bits (b); u4 r; CHL_EACH r.v[k] = (x.v[k] & m.v[k]) | (y.v[k] & ~m.v[k]); return flt (r); }
    static inline i4 toInt (f4 a) { i4 r; CHL_EACH r.v[k] = (int32_t) a.v[k]; return r; }
    static inline f4 toFloat (i4 a) { f4 r; CHL_EACH r.v[k] = (float) a.v[k]; return r; }
    static inline f4 recip1 (f4 d) { f4 r; CHL_EACH r.v[k] = 1.0f / d.v[k]; return r; }
    static inline f4 recip2 (f4 d) { return recip1 (d); }
    static inline f4 rsqrt1 (f4 d) { f4 r; CHL_EACH r.v[k] = 1.0f / std::sqrt (d.v[k]); return r; }
    static inline f4 ldPair (const float* l, const float* r, f4 v) { v.v[0] = *l; v.v[1] = *r; return v; }
    static inline void stPair (float* l, float* r, f4 v) { *l = v.v[0]; *r = v.v[1]; }
    static inline f4 lowLow (f4 a, f4 b) { f4 r = { { a.v[0], a.v[1], b.v[0], b.v[1] } }; return r; }
    static inline f4 highHigh (f4 a, f4 b) { f4 r = { { a.v[2], a.v[3], b.v[2], b.v[3] } }; return r; }
    static inline f4 dupLow (f4 a) { return lowLow (a, a); }
    static inline f4 dupHigh (f4 a) { return highHigh (a, a); }
    static inline u4 lcg4 (u4 s) { u4 r; CHL_EACH r.v[k] = s.v[k] * 1664525u + 1013904223u; return r; }
    static inline f4 noise4 (u4 s) { f4 r; CHL_EACH r.v[k] = (float) (int32_t) s.v[k] * (1.0f / 4294967296.0f); return r; }
    static inline f4 fixToFloat (u4 u, int fracBits) { f4 r; CHL_EACH r.v[k] = (float) u.v[k] / (float) (1u << fracBits); return r; }
#define CHL_FIX_TO_FLOAT(u, fracBits) fixToFloat ((u), (fracBits))
    static inline f4 dot4x4 (f4 a, f4 wa, f4 b, f4 wb, f4 c, f4 wc, f4 d, f4 wd)
    {
        f4 r = { { 0, 0, 0, 0 } };
        CHL_EACH { r.v[0] += a.v[k] * wa.v[k]; r.v[1] += b.v[k] * wb.v[k]; r.v[2] += c.v[k] * wc.v[k]; r.v[3] += d.v[k] * wd.v[k]; }
        return r;
    }
    static CHL_INLINE void gather2 (const float* p0, const float* p1, const float* p2, const float* p3, f4& lo, f4& hi)
    {
        lo.v[0] = p0[0]; lo.v[1] = p1[0]; lo.v[2] = p2[0]; lo.v[3] = p3[0];
        hi.v[0] = p0[1]; hi.v[1] = p1[1]; hi.v[2] = p2[1]; hi.v[3] = p3[1];
    }
    static inline f4 highLow (f4 a, f4 b) { f4 r = { { a.v[2], a.v[3], b.v[0], b.v[1] } }; return r; }
    static inline f4 lowHigh (f4 a, f4 b) { f4 r = { { a.v[0], a.v[1], b.v[2], b.v[3] } }; return r; }
    static inline f4 swapHalves (f4 a) { return highLow (a, a); }
    static inline f4 ld2 (const float* p) { f4 r = { { p[0], p[1], 0.0f, 0.0f } }; return r; }
    static inline void st2 (float* p, f4 v) { p[0] = v.v[0]; p[1] = v.v[1]; }
    static inline void st2hi (float* p, f4 v) { p[0] = v.v[2]; p[1] = v.v[3]; }
    static inline f4 pow2i (i4 n) { f4 r; for (int k = 0; k < 4; ++k) r.v[k] = std::ldexp (1.0f, n.v[k]); return r; }
    static inline u4 fromInts (uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u4 r = { { a, b, c, d } }; return r; }
    static CHL_INLINE void transpose4 (f4& a, f4& b, f4& c, f4& d)
    {
        const f4 r0 = { { a.v[0], b.v[0], c.v[0], d.v[0] } }, r1 = { { a.v[1], b.v[1], c.v[1], d.v[1] } };
        const f4 r2 = { { a.v[2], b.v[2], c.v[2], d.v[2] } }, r3 = { { a.v[3], b.v[3], c.v[3], d.v[3] } };
        a = r0; b = r1; c = r2; d = r3;
    }
    static inline u4 ramLoad (const uint16_t* p)
    {
        const uint32_t l = (*p & 0x00ffu) ? 0xffffffffu : 0u, r = (*p & 0xff00u) ? 0xffffffffu : 0u;
        u4 m = { { l, r, l, r } }; return m;
    }
    static inline void ramStore (uint16_t* p, u4 m) { *p = (uint16_t) ((m.v[2] ? 0x00ffu : 0u) | (m.v[3] ? 0xff00u : 0u)); }
    static inline i4 splati (int32_t x) { i4 r = { { x, x, x, x } }; return r; }
    static inline u4 selu (u4 m, u4 a, u4 b) { u4 r; for (int k = 0; k < 4; ++k) r.v[k] = (a.v[k] & m.v[k]) | (b.v[k] & ~m.v[k]); return r; }
    static inline u4 lowHighU (u4 a, u4 b) { u4 r = { { a.v[0], a.v[1], b.v[2], b.v[3] } }; return r; }
    static inline u4 gtZero (f4 a) { u4 r; for (int k = 0; k < 4; ++k) r.v[k] = a.v[k] > 0.0f ? 0xffffffffu : 0u; return r; }
    static inline u4 signOf (u4 m) { u4 r; for (int k = 0; k < 4; ++k) r.v[k] = m.v[k] << 31; return r; }
    static inline f4 ld4s (const float* a, const float* b, const float* c, const float* d) { f4 r = { { *a, *b, *c, *d } }; return r; }
    static CHL_INLINE void zip4 (f4 a, f4 b, f4& lo, f4& hi)
    {
        const f4 l = { { a.v[0], b.v[0], a.v[1], b.v[1] } }, h = { { a.v[2], b.v[2], a.v[3], b.v[3] } };
        lo = l; hi = h;
    }
    static inline void stLane (float* p, f4 v, int lane) { *p = v.v[lane]; }
    static inline float lane0 (f4 a) { return a.v[0]; }
    static inline f4 pair4 (float lo, float hi) { f4 r = { { lo, lo, hi, hi } }; return r; }
    static inline f4 make4 (float a, float b, float c, float d) { f4 r = { { a, b, c, d } }; return r; }
    static inline u4 pairu (uint32_t lo, uint32_t hi) { u4 r = { { lo, lo, hi, hi } }; return r; }
    static inline float maxLanes01 (f4 v) { return v.v[0] > v.v[1] ? v.v[0] : v.v[1]; }
    static inline float maxLanes23 (f4 v) { return v.v[2] > v.v[3] ? v.v[2] : v.v[3]; }
    static inline void prefetch (const void*) {}
#undef CHL_EACH
#endif

    //==========================================================================
    // tanh, the plug-in's Pade form x (27 + x^2) / (27 + 9 x^2) held to +-1 beyond +-3, with the
    // divide done as a reciprocal estimate and one Newton step (good to about 1.5e-5)
    static inline f4 tanh4 (f4 x)
    {
        x = max4 (min4 (x, splat (3.0f)), splat (-3.0f));
        const f4 x2 = x * x;
        const f4 num = x * (x2 + splat (27.0f));
        const f4 den = mla (splat (27.0f), x2, splat (9.0f));
        return num * recip1 (den);
    }
    // a cheaper limiter for places where the signal is meant to stay small and this is only the
    // net under it: x - 4/27 x^3 held to +-1 beyond +-1.5 (unity slope at zero, like tanh)
    static inline f4 softClip4 (f4 x)
    {
        x = max4 (min4 (x, splat (1.5f)), splat (-1.5f));
        return mls (x, x * x, x * splat (4.0f / 27.0f));
    }
    // sin (2 pi p) for p in 0..1, the parabola the scalar code uses (within 0.1%)
    static inline f4 sin01x4 (f4 p)
    {
        const f4 x = sel4 (gt4 (splat (0.5f), p), p, p - splat (1.0f));
        const f4 y = splat (8.0f) * x * mls (splat (1.0f), splat (2.0f), abs4 (x));
        return y * mla (splat (0.775f), splat (0.225f), abs4 (y));
    }
    // true when a float buffer of n (a multiple of 4) samples is all zeros: an unpatched input
    static inline bool allZero (const float* p, int n)
    {
        typedef uint32_t __attribute__ ((may_alias)) word;
        uint32_t acc = 0;
        const word* u = reinterpret_cast<const word*> (p);
        for (int i = 0; i < n; i += 4) acc |= (u[i] | u[i + 1] | u[i + 2] | u[i + 3]);
        return (acc & 0x7fffffffu) == 0;
    }
    // the largest magnitude in a float buffer of n (a multiple of 4) samples, for the displays
    static CHL_NOINLINE float peakAbs (const float* p, int n)
    {
        f4 m = splat (0.0f);
        for (int i = 0; i < n; i += 4) m = max4 (m, abs4 (ld (p + i)));
        m = max4 (m, swapHalves (m));
        return maxLanes01 (m);
    }
    // the bit pattern of a float, for "has this changed" tests without touching the FPU
    static inline uint32_t bitsOf (float x) { uint32_t u; std::memcpy (&u, &x, sizeof (u)); return u; }
    static inline bool finiteBits (float x) { return (bitsOf (x) & 0x7f800000u) != 0x7f800000u; }
}
