/* a8sim: run the benchmark image in the Unicorn engine and estimate, instruction by
 * instruction, how many Cortex-A8 cycles it would take.
 *
 * Unicorn executes the code (so the instruction stream, the branches and the memory
 * addresses are the real ones for this binary); the timing comes from the model below,
 * built on the Cortex-A8 Technical Reference Manual r3p2 (ARM DDI 0344K), chapter 16
 * "Instruction Cycle Timing". It is a hand model of a statically scheduled in-order
 * core, not a cycle-accurate one: see README.md for what it does and does not cover.
 *
 * The image is pre-decoded by decode.py into bench.tab (class and register use of every
 * instruction); the class numbers here must match that file.
 */
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../target/bench_abi.h"

/* ---- instruction classes (decode.py) */
enum
{
  INT_ALU = 0, INT_MUL, INT_LOAD, INT_STORE, INT_LDM, INT_STM, BRANCH, OTHER,
  VFP_ADD = 10, VFP_MUL, VFP_MAC, VFP_DIV, VFP_SQRT, VFP_CPY, VFP_ABSNEG, VFP_CMP, VFP_F2I, VFP_I2F, VFP_CVT,
  VFP_LOAD, VFP_STORE, VFP_LDM, VFP_STM, XFER_TO_NEON, XFER_TO_ARM,
  N_FADD = 30, N_FMUL, N_FMLA, N_INT, N_IMUL, N_SHIFT, N_PERM, N_LOAD, N_STORE,
  NCLASS = 40
};
static const char *className[NCLASS] = {
    "int_alu", "int_mul", "int_load", "int_store", "int_ldm", "int_stm", "branch", "other", 0, 0,
    "vfp_add", "vfp_mul", "vfp_mac", "vfp_div", "vfp_sqrt", "vfp_cpy", "vfp_absneg", "vfp_cmp", "vfp_f2i", "vfp_i2f", "vfp_cvt",
    "vfp_load", "vfp_store", "vfp_ldm", "vfp_stm", "xfer_to_neon", "xfer_to_arm", 0, 0, 0,
    "neon_fadd", "neon_fmul", "neon_fmla", "neon_int", "neon_imul", "neon_shift", "neon_perm", "neon_load", "neon_store", 0};
enum
{
  F_COND = 1, F_SETFLAGS = 2, F_DOUBLE = 4, F_QUAD = 8, F_CALL = 16, F_RET = 32, F_INDIRECT = 64, F_CONDBR = 128,
  F_FPSCR_W = 256, F_FORBID = 512, F_SPMOD = 1024, F_USEFLAGS = 2048
};
/* ---- where the cycles go (report columns) */
enum { K_INT = 0, K_VFPLITE, K_NFP, K_NEON, K_XFER, K_MISPRED, K_ICACHE, K_DCACHE, K_N };
static const char *kName[K_N] = {"integer", "vfp_lite", "vfp_runfast", "neon", "neon_to_arm", "mispredict", "icache_miss", "dcache_miss"};

typedef struct
{
  uint8_t cls, nreg;
  uint16_t flags;
  uint32_t src, dst, adr; /* core registers, bit 16 = the flags */
  uint64_t vsrc, vdst;    /* the 64 single-precision halves of d0..d31 */
  int32_t sym;
} Insn;

/* ---- the model's numbers. TRM = ARM DDI 0344K. */
typedef struct
{
  double mispredict;      /* TRM 16.4.1: 13 cycles */
  double l1_miss;         /* TRM table 16-16: 8-cycle replay when the L2 has the line */
  double l2_miss;         /* TRM table 16-16: 18 + the external memory; that part is an assumption */
  double l2_miss_lo, l2_miss_hi;
  double neon_to_arm;     /* TRM 16.5.2: "a minimum of 20 cycles" */
  /* TRM table 16-25, VFP coprocessor (not RunFast): lo / hi; the model uses the middle */
  double fadd[2], fmul_s[2], fmul_d[2], fmac_s[2], fmac_d[2], fdiv_s[2], fdiv_d[2], fsqrt_s[2], fsqrt_d[2], fcmp[2];
  double funa, fcvt_ds, fcvt_sd, fi2f, ff2i;
  /* TRM 16.7.2: in RunFast mode single-precision add/sub/mul/mac/abs/neg/cmp/int-convert run
     in the NEON floating-point pipeline; "each VFP instruction takes 7 cycles", example 16-8
     shows FMACS at 11 */
  double nfp_lat, nfp_mac_lat;
  /* Not from the TRM: cycles added to every instruction the VFP coprocessor executes, fitted to
     readings from real modules (tools/calibrate.py; README.md "Calibration"). */
  double vfp_extra, vfp_extra_lo, vfp_extra_hi;
} Model;
static Model M = {
    13, 8, 18 + 60, 18 + 22, 18 + 130, 20,
    {9, 10}, {10, 12}, {11, 17}, {18, 21}, {19, 26}, {20, 37}, {29, 65}, {19, 33}, {29, 60}, {4, 7},
    4, 5, 7, 9, 8,
    7, 11,
    11, 8, 14};

/* ---- image */
static Insn *tab;
static uint32_t textStart, textEnd, aMark, aExit, aCfg, aOut, aList, aStart, markEnd;
static char **symName;
static int nsym;
static inline Insn *insnAt(uint32_t a) { return (a >= textStart && a < textEnd) ? &tab[(a - textStart) >> 2] : 0; }

/* ---- caches: L1 I and D 32 KB 4-way, L2 256 KB 8-way, 64-byte lines (AM335x) */
typedef struct
{
  int sets, ways;
  uint32_t *tag;
  uint32_t *stamp;
  uint32_t tick;
} Cache;
static void cacheInit(Cache *c, int bytes, int ways)
{
  c->ways = ways;
  c->sets = bytes / 64 / ways;
  c->tag = malloc(sizeof(uint32_t) * c->sets * ways);
  c->stamp = calloc(c->sets * ways, sizeof(uint32_t));
  memset(c->tag, 0xff, sizeof(uint32_t) * c->sets * ways);
  c->tick = 1;
}
/* returns 1 on a hit; on a miss the line replaces the least recently used way if fill */
static inline int cacheTouch(Cache *c, uint32_t line, int fill)
{
  const int set = (int)(line % (uint32_t)c->sets) * c->ways;
  int victim = 0;
  uint32_t oldest = 0xffffffffu;
  for (int w = 0; w < c->ways; w++)
  {
    if (c->tag[set + w] == line)
    {
      c->stamp[set + w] = ++c->tick;
      return 1;
    }
    if (c->stamp[set + w] < oldest)
    {
      oldest = c->stamp[set + w];
      victim = w;
    }
  }
  if (fill)
  {
    c->tag[set + victim] = line;
    c->stamp[set + victim] = ++c->tick;
  }
  return 0;
}
static Cache l1i, l1d, l2;

/* ---- branch prediction: 512-entry 2-way BTB, 4096 2-bit counters indexed by the global
   history and the PC, 8-entry return stack (TRM chapter 5) */
static uint32_t btbTag[256][2], btbTarget[256][2];
static uint8_t btbLru[256];
static uint8_t ghb[4096];
static uint32_t ghist;
static uint32_t rstack[8];
static int rsp;

static int btbLookup(uint32_t pc, uint32_t *target)
{
  const uint32_t set = (pc >> 2) & 255, tag = pc >> 10;
  for (int w = 0; w < 2; w++)
    if (btbTag[set][w] == tag)
    {
      *target = btbTarget[set][w];
      btbLru[set] = (uint8_t)(1 - w);
      return 1;
    }
  return 0;
}
static void btbStore(uint32_t pc, uint32_t target)
{
  const uint32_t set = (pc >> 2) & 255, tag = pc >> 10;
  for (int w = 0; w < 2; w++)
    if (btbTag[set][w] == tag)
    {
      btbTarget[set][w] = target;
      return;
    }
  const int w = btbLru[set];
  btbTag[set][w] = tag;
  btbTarget[set][w] = target;
  btbLru[set] = (uint8_t)(1 - w);
}

/* ---- pipeline state */
static double armT;             /* issue cycle of the newest core instruction */
static int slots, lsUsed, brUsed;
static uint32_t cycDst;
static double coreReady[17];
static uint8_t coreFromXfer[17];
static double vReady[64];
static double fpFlagsReady, xferBlock;
static double nLastStart, nLastDur, nDpFree, nLsFree, nMlaBlock, nTail;
static int nLastKind;
static double nq[16];
static int nqAt;
static uint32_t fpscr;
static uint32_t lastILine = 0xffffffffu;

/* ---- accounting */
static int measuring, curObj = -1, nobj;
static double frontier;
static double kCyc[K_N], bandLo, bandHi;
static double objCyc[BENCH_MAX_OBJ], objLo[BENCH_MAX_OBJ], objHi[BENCH_MAX_OBJ];
static uint64_t objIns[BENCH_MAX_OBJ];
static uint32_t objStack[BENCH_MAX_OBJ], spEntry, spCheck;
static double harnessCyc, totalCyc;
static uint64_t totalIns, opCount[NCLASS], nfpOps, vfpLiteOps;
/* how scalar VFP instructions follow one another (for the calibration) */
static uint64_t cVfpAfterVfpDep, cVfpAfterVfpIndep, cVfpAfterLoadDep, cVfpAfterOther, cLsAfterVfp, cStoreOfVfpResult;
static int prevNeonKind = -1; /* 0 vfp-lite op, 1 vfp/neon load, 2 store, 3 other */
static uint64_t prevNeonDst;
static double *symCyc;
static uint64_t *symIns;
static double *hotCyc;
static uint32_t *hotCnt;
static uint64_t cL1dMiss, cL2dMiss, cL1iMiss, cL2iMiss, cMispred, cLoads, cDenormal;
#define COUNT(x) do { if (measuring && curObj >= 0) (x)++; } while (0)
static double frameStartCyc, frameMax, frameSum;
static int frames;
#define MAXVIOL 32
static struct { int obj, sym; } viol[MAXVIOL];
static int nviol;
static int wantHot;

/* ---- the instruction being executed (finished when the next one starts) */
static uint32_t pendAddr;
static int pendValid, pendFpscr, pendSp;
static struct { uint32_t addr; uint8_t write; } pendMem[24];
static int npendMem;
static uc_engine *UC;

static inline double dmax(double a, double b) { return a > b ? a : b; }
static inline void newCycle(double c)
{
  armT = c;
  slots = 0;
  lsUsed = 0;
  brUsed = 0;
  cycDst = 0;
}

static double memPenalty(uint32_t addr, int write, int *l2missed)
{
  const uint32_t line = addr >> 6;
  if (write)
  {
    /* a store that misses the L1 does not stall and is not allocated in the L1 (the write
       buffer takes it to the L2): TRM table 16-16 for the stall; the no-allocate is from
       memory, not from the TRM */
    if (!cacheTouch(&l1d, line, 0))
      cacheTouch(&l2, line, 1);
    return 0.0;
  }
  COUNT(cLoads);
  if (cacheTouch(&l1d, line, 1))
    return 0.0;
  COUNT(cL1dMiss);
  if (cacheTouch(&l2, line, 1))
    return M.l1_miss;
  COUNT(cL2dMiss);
  *l2missed = 1;
  return M.l2_miss;
}

static void finish(uint32_t nextPc)
{
  const uint32_t addr = pendAddr;
  Insn *in = insnAt(addr);
  pendValid = 0;
  if (!in)
    return;
  const int cls = in->cls;
  double pen[K_N] = {0};
  double lo = 0, hi = 0;
  const double f0 = frontier;

  /* instruction fetch */
  if ((addr >> 6) != lastILine)
  {
    lastILine = addr >> 6;
    if (!cacheTouch(&l1i, lastILine, 1))
    {
      double p = M.l1_miss;
      COUNT(cL1iMiss);
      if (!cacheTouch(&l2, lastILine, 1))
      {
        p = M.l2_miss;
        COUNT(cL2iMiss);
        lo += M.l2_miss - M.l2_miss_lo;
        hi += M.l2_miss_hi - M.l2_miss;
      }
      newCycle(armT + (slots ? 1 : 0) + p);
      pen[K_ICACHE] += p;
    }
  }

  /* when can it issue in the core pipeline? */
  double need = armT, needX = armT; /* needX: what waits on a NEON-to-ARM transfer */
#define NEED(r, extra) do { const double t_ = coreReady[r] + (extra); if (coreFromXfer[r]) needX = dmax(needX, t_); else need = dmax(need, t_); } while (0)
  for (uint32_t m = in->src & 0xffff; m; m &= m - 1)
    NEED(__builtin_ctz(m), 0.0);
  for (uint32_t m = in->adr; m; m &= m - 1)
    NEED(__builtin_ctz(m), 1.0); /* address operands are needed a stage early */
  if (in->src & 0x10000)
    NEED(16, (in->flags & F_CONDBR) ? -1.0 : 0.0); /* a branch pairs with its compare */
  /* TRM 16.5.2: issue stalls at the first instruction touching the core register file
     until pending NEON-to-ARM transfers are complete */
  if ((in->src | in->dst | in->adr) & 0x1ffff)
    needX = dmax(needX, xferBlock);
  if (needX > need)
  {
    pen[K_XFER] += needX - need;
    need = needX;
  }
  if (cls >= 10 && nq[nqAt] > need)
    need = nq[nqAt]; /* the 16-entry NEON instruction queue is full */
  double c = ceil(need - 1e-9);
  if (c > armT)
    newCycle(c);

  const int ls = (cls == INT_LOAD || cls == INT_STORE || cls == INT_LDM || cls == INT_STM ||
                  (cls >= VFP_LOAD && cls <= VFP_STM) || cls == N_LOAD || cls == N_STORE);
  const int br = (cls == BRANCH);
  int k = 1;
  if (cls == INT_LDM || cls == INT_STM || (br && in->nreg))
    k = (in->nreg + 1) / 2 + ((br && in->nreg) ? 1 : 0);
  else if (cls == INT_MUL)
    k = 2;
  else if (br && (in->flags & F_INDIRECT) && in->adr)
    k = 2; /* ldr pc */
  if (k < 1)
    k = 1;
  /* TRM table 16-15: one load/store, one branch per cycle; multiply and multi-cycle
     instructions start in pipeline 0; two instructions writing the same register do not pair */
  if (slots >= 2 || (ls && lsUsed) || (br && brUsed) || ((k > 1) && slots > 0) || (in->dst & cycDst & 0xffff))
    newCycle(armT + 1);
  c = armT;
  slots++;
  lsUsed |= ls;
  brUsed |= br;
  cycDst |= in->dst;
  if (k > 1)
  {
    armT = c + k - 1;
    slots = 1;
  }

  /* data cache */
  double dpen = 0;
  for (int i = 0; i < npendMem; i++)
  {
    int l2m = 0;
    const double p = memPenalty(pendMem[i].addr, pendMem[i].write, &l2m);
    if (p > 0)
    {
      dpen += p;
      if (l2m)
      {
        lo += M.l2_miss - M.l2_miss_lo;
        hi += M.l2_miss_hi - M.l2_miss;
      }
    }
  }
  npendMem = 0;
  if (dpen > 0)
  {
    armT += dpen; /* the pipeline replays the load */
    pen[K_DCACHE] += dpen;
  }
  const double cdone = armT; /* core-side issue including any replay */

  int ownClass = K_INT;
  if (cls < 10)
  {
    double ready = cdone + 1;
    if (cls == INT_LOAD)
      ready = cdone + 2;
    else if (cls == INT_MUL)
      ready = cdone + 4;
    else if (cls == INT_LDM)
      ready = cdone + 2;
    for (uint32_t m = in->dst & 0x7fff; m; m &= m - 1)
    {
      coreReady[__builtin_ctz(m)] = ready;
      coreFromXfer[__builtin_ctz(m)] = 0;
    }
    if (in->dst & 0x10000)
    {
      coreReady[16] = cdone + 1;
      coreFromXfer[16] = 0;
    }
    if (br)
    {
      const int taken = nextPc != addr + 4;
      int miss = 0;
      uint32_t t = 0;
      if (in->flags & F_RET)
      {
        rsp = (rsp + 7) & 7;
        if (taken && rstack[rsp] != nextPc)
          miss = 1;
      }
      else if (in->flags & F_CONDBR)
      {
        const uint32_t idx = ((ghist << 2) ^ (addr >> 2)) & 4095;
        const int predTaken = (ghb[idx] >= 2) && btbLookup(addr, &t);
        if (predTaken != taken || (taken && t != nextPc))
          miss = 1;
        if (taken)
        {
          if (ghb[idx] < 3)
            ghb[idx]++;
          btbStore(addr, nextPc);
        }
        else if (ghb[idx] > 0)
          ghb[idx]--;
        ghist = ((ghist << 1) | (uint32_t)taken) & 1023;
      }
      else if (taken)
      {
        /* unconditional or computed: right if the BTB has this target */
        if (!btbLookup(addr, &t) || t != nextPc)
          miss = 1;
        btbStore(addr, nextPc);
      }
      if ((in->flags & F_CALL) && taken)
      {
        rstack[rsp] = addr + 4;
        rsp = (rsp + 1) & 7;
      }
      if (miss)
      {
        COUNT(cMispred);
        newCycle(armT + M.mispredict);
        pen[K_MISPRED] += M.mispredict;
      }
    }
  }
  else
  {
    /* ---- the NEON / VFP side */
    const int dbl = in->flags & F_DOUBLE, quad = (in->flags & F_QUAD) ? 1 : 0;
    const int runfast = (fpscr & 0x03000000u) == 0x03000000u && (fpscr & 0x00379f00u) == 0;
    int kind = 0; /* 0 data processing, 1 load/store/permute/transfer */
    double dur = 1, lat = 3, dlo = 0, dhi = 0;
    int isFp = 0, vfplite = 0, nfp = 0;
#define VFPL(rng) do { dur = lat = 0.5 * ((rng)[0] + (rng)[1]); dlo = dur - (rng)[0]; dhi = (rng)[1] - dur; vfplite = 1; } while (0)
    switch (cls)
    {
    case VFP_ADD:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else VFPL(M.fadd);
      break;
    case VFP_MUL:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else if (dbl) VFPL(M.fmul_d); else VFPL(M.fmul_s);
      break;
    case VFP_MAC:
      if (runfast && !dbl) { nfp = 1; dur = 2; lat = M.nfp_mac_lat; } else if (dbl) VFPL(M.fmac_d); else VFPL(M.fmac_s);
      break;
    case VFP_DIV:
      if (dbl) VFPL(M.fdiv_d); else VFPL(M.fdiv_s);
      break;
    case VFP_SQRT:
      if (dbl) VFPL(M.fsqrt_d); else VFPL(M.fsqrt_s);
      break;
    case VFP_CPY:
      dur = lat = M.funa; vfplite = 1;
      break;
    case VFP_ABSNEG:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else { dur = lat = M.funa; vfplite = 1; }
      break;
    case VFP_CMP:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else VFPL(M.fcmp);
      break;
    case VFP_F2I:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else { dur = lat = M.ff2i; vfplite = 1; }
      break;
    case VFP_I2F:
      if (runfast && !dbl) { nfp = 1; dur = 1; lat = M.nfp_lat; } else { dur = lat = M.fi2f; vfplite = 1; }
      break;
    case VFP_CVT:
      dur = lat = dbl ? M.fcvt_sd : M.fcvt_ds; vfplite = 1;
      break;
    case VFP_LOAD: kind = 1; dur = dbl ? 2 : 1; lat = dur + 1; break;   /* TRM 16.6.7 */
    case VFP_STORE: kind = 1; dur = dbl ? 2 : 1; lat = 0; break;
    case VFP_LDM: kind = 1; dur = in->nreg / 2 + (in->nreg & 1) + 1; lat = dur + 1; break;
    case VFP_STM: kind = 1; dur = in->nreg / 2 + (in->nreg & 1) + 1; lat = 0; break;
    case XFER_TO_NEON: kind = 1; dur = 1; lat = 2; break;
    case XFER_TO_ARM: kind = 1; dur = 1; lat = 0; break;
    case N_FADD: case N_FMUL: isFp = 1; dur = 1 + quad; lat = 4 + quad; break;    /* TRM table 16-21: result N5, needed N2 */
    case N_FMLA: isFp = 2; dur = 1 + quad; lat = 8 + quad; break;                 /* result N9 */
    case N_INT: dur = 1; lat = 3; break;
    case N_IMUL: dur = 1 + quad; lat = 6 + quad; break;
    case N_SHIFT: dur = 1; lat = 4; break;
    case N_PERM: kind = 1; dur = 1 + quad; lat = 2 + quad; break;                 /* TRM table 16-22 */
    case N_LOAD: kind = 1; dur = (in->nreg + 1) / 2 + 1; lat = dur + 1; break;    /* TRM table 16-23, no alignment hint */
    case N_STORE: kind = 1; dur = (in->nreg + 1) / 2 + 1; lat = 0; break;
    default: break;
    }
    if (vfplite && (cls == VFP_ADD || cls == VFP_MUL || cls == VFP_MAC || cls == VFP_DIV || cls == VFP_SQRT || cls == VFP_CVT) && in->vdst)
    {
      /* The instruction has executed: look at what it produced. A subnormal result costs the
         VFP coprocessor 6-7 cycles to format, and 5-6 more when it is next used as an operand
         (TRM 16.7.1); with flush-to-zero set it would have been a zero. */
      const int half = __builtin_ctzll(in->vdst);
      int sub = 0;
      if (dbl && cls != VFP_CVT)
      {
        uint64_t v = 0;
        uc_reg_read(UC, UC_ARM_REG_D0 + half / 2, &v);
        sub = ((v >> 52) & 0x7ff) == 0 && (v << 12) != 0;
      }
      else if (!(in->vdst & (in->vdst - 1)))
      {
        uint32_t v = 0;
        uc_reg_read(UC, UC_ARM_REG_S0 + half, &v);
        sub = ((v >> 23) & 0xff) == 0 && (v << 9) != 0;
      }
      if (sub)
      {
        COUNT(cDenormal);
        dur += 12;
        lat += 12;
        dlo += 1;
        dhi += 1;
      }
    }
    if (vfplite)
    {
      dur += M.vfp_extra;
      lat += M.vfp_extra;
      dlo += M.vfp_extra - M.vfp_extra_lo;
      dhi += M.vfp_extra_hi - M.vfp_extra;
    }
    double s = cdone;
    for (uint64_t m = in->vsrc; m; m &= m - 1)
      s = dmax(s, vReady[__builtin_ctzll(m)]);
    if (cls == XFER_TO_ARM && !(in->vsrc))
      s = dmax(s, fpFlagsReady); /* vmrs */
    /* in order; a load/store/permute can share a cycle with a data-processing instruction
       (TRM 16.5.3), on the first or the last cycle of a multi-cycle one */
    s = dmax(s, nLastStart + ((kind != nLastKind) ? dmax(nLastDur - 1.0, 0.0) : nLastDur));
    s = dmax(s, kind ? nLsFree : nDpFree);
    if (isFp == 1)
      s = dmax(s, nMlaBlock); /* TRM table 16-21 note: VADD.F / VMUL.F after VMLA.F waits 4 cycles */
    if (kind)
      nLsFree = s + dur;
    else
      nDpFree = s + dur;
    if (isFp == 2)
      nMlaBlock = s + 4 + dur;
    for (uint64_t m = in->vdst; m; m &= m - 1)
      vReady[__builtin_ctzll(m)] = s + lat;
    if (cls == VFP_CMP)
      fpFlagsReady = s + lat;
    if (s + lat > nTail)
      nTail = s + lat;
    nLastStart = s;
    nLastDur = dur;
    nLastKind = kind;
    nq[nqAt] = s;
    nqAt = (nqAt + 1) & 15;
    if (cls == XFER_TO_ARM)
    {
      const double done = s + M.neon_to_arm;
      if (done > xferBlock)
        xferBlock = done;
      for (uint32_t m = in->dst & 0x7fff; m; m &= m - 1)
      {
        coreReady[__builtin_ctz(m)] = done;
        coreFromXfer[__builtin_ctz(m)] = 1;
      }
      if (in->dst & 0x10000)
      {
        coreReady[16] = done;
        coreFromXfer[16] = 1;
      }
    }
    {
      const int isLoad = (cls == VFP_LOAD || cls == VFP_LDM || cls == N_LOAD), isStore = (cls == VFP_STORE || cls == VFP_STM || cls == N_STORE);
      if (vfplite)
      {
        if (prevNeonKind == 0)
        {
          if (in->vsrc & prevNeonDst) COUNT(cVfpAfterVfpDep); else COUNT(cVfpAfterVfpIndep);
        }
        else if (prevNeonKind == 1 && (in->vsrc & prevNeonDst))
          COUNT(cVfpAfterLoadDep);
        else
          COUNT(cVfpAfterOther);
      }
      else if ((isLoad || isStore) && prevNeonKind == 0)
      {
        COUNT(cLsAfterVfp);
        if (isStore && (in->vsrc & prevNeonDst))
          COUNT(cStoreOfVfpResult);
      }
      prevNeonKind = vfplite ? 0 : (isLoad ? 1 : (isStore ? 2 : 3));
      prevNeonDst = in->vdst;
    }
    ownClass = vfplite ? K_VFPLITE : (nfp ? K_NFP : K_NEON);
    if (vfplite)
    {
      lo += dlo;
      hi += dhi;
      COUNT(vfpLiteOps);
    }
    if (nfp)
      COUNT(nfpOps);
  }

  /* ---- book the time this instruction added */
  frontier = dmax(dmax(armT, nDpFree), dmax(nLsFree, f0));
  if (!measuring)
    return;
  double d = frontier - f0;
  if (curObj < 0)
  {
    harnessCyc += d;
    return;
  }
  totalIns++;
  opCount[cls]++;
  totalCyc += d;
  objCyc[curObj] += d;
  objIns[curObj]++;
  objLo[curObj] += lo;
  objHi[curObj] += hi;
  bandLo += lo;
  bandHi += hi;
  if (in->sym >= 0)
  {
    symCyc[in->sym] += d;
    symIns[in->sym]++;
  }
  if (wantHot)
  {
    hotCyc[(addr - textStart) >> 2] += d;
    hotCnt[(addr - textStart) >> 2]++;
  }
  /* penalties first, what is left belongs to the instruction's own class */
  for (int q = K_XFER; q < K_N && d > 0; q++)
  {
    const double p = pen[q] < d ? pen[q] : d;
    kCyc[q] += p;
    d -= p;
  }
  kCyc[ownClass] += d;
}

static void onMark(uint32_t code, int32_t arg)
{
  uint32_t sp;
  switch (code)
  {
  case BENCH_MARK_BEGIN:
    measuring = 1;
    frameStartCyc = totalCyc;
    break;
  case BENCH_MARK_FRAME:
  {
    const double f = totalCyc - frameStartCyc;
    frameStartCyc = totalCyc;
    if (f > frameMax)
      frameMax = f;
    frameSum += f;
    frames++;
    break;
  }
  case BENCH_MARK_OBJ:
    uc_reg_read(UC, UC_ARM_REG_SP, &sp);
    if (curObj >= 0 && spEntry > spCheck && spEntry - spCheck > objStack[curObj])
      objStack[curObj] = spEntry - spCheck;
    curObj = arg < BENCH_MAX_OBJ ? arg : -1;
    spEntry = spCheck = sp;
    break;
  case BENCH_MARK_END:
    measuring = 0;
    uc_emu_stop(UC);
    break;
  }
}

static void hookCode(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
  const uint32_t addr = (uint32_t)address;
  (void)size;
  (void)user;
  if (pendValid)
  {
    if (pendFpscr)
      uc_reg_read(uc, UC_ARM_REG_FPSCR, &fpscr);
    if (pendSp && curObj >= 0)
    {
      uint32_t sp;
      uc_reg_read(uc, UC_ARM_REG_SP, &sp);
      if (sp < spCheck)
        spCheck = sp;
    }
    finish(addr);
  }
  if (addr >= aMark && addr < markEnd)
  {
    if (addr == aMark)
    {
      uint32_t r0, r1;
      uc_reg_read(uc, UC_ARM_REG_R0, &r0);
      uc_reg_read(uc, UC_ARM_REG_R1, &r1);
      rsp = (rsp + 7) & 7; /* its return is not simulated: keep the return stack level */
      onMark(r0, (int32_t)r1);
    }
    npendMem = 0;
    return;
  }
  Insn *in = insnAt(addr);
  if (!in)
    return;
  if ((in->flags & F_FORBID) && curObj >= 0 && nviol < MAXVIOL)
  {
    viol[nviol].obj = curObj;
    viol[nviol].sym = in->sym;
    nviol++;
  }
  pendAddr = addr;
  pendValid = 1;
  pendFpscr = (in->flags & F_FPSCR_W) != 0;
  pendSp = (in->flags & F_SPMOD) != 0;
  npendMem = 0;
}

static void hookMem(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user)
{
  (void)uc;
  (void)value;
  (void)user;
  if (npendMem < 22)
  {
    pendMem[npendMem].addr = (uint32_t)address;
    pendMem[npendMem].write = (type == UC_MEM_WRITE);
    npendMem++;
    if (((address + (uint64_t)size - 1) >> 6) != (address >> 6))
    {
      pendMem[npendMem].addr = (uint32_t)(address + (uint64_t)size - 1);
      pendMem[npendMem].write = (type == UC_MEM_WRITE);
      npendMem++;
    }
  }
}

static void hookExit(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
  (void)address;
  (void)size;
  (void)user;
  uc_emu_stop(uc);
}

static void die(const char *what)
{
  fprintf(stderr, "a8sim: %s\n", what);
  exit(2);
}

static void loadTable(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
    die("cannot open the table (run decode.py)");
  char line[1024], name[768];
  while (fgets(line, sizeof(line), f))
  {
    unsigned a, b, cls, flags, nreg, src, dst, adr;
    unsigned long long vs, vd;
    int sym, idx, size;
    if (line[0] == 'I')
    {
      if (sscanf(line, "I %x %u %u %u %x %x %x %llx %llx %d", &a, &cls, &flags, &nreg, &src, &dst, &adr, &vs, &vd, &sym) == 10)
      {
        Insn *in = insnAt(a);
        if (in)
        {
          in->cls = (uint8_t)cls;
          in->flags = (uint16_t)flags;
          in->nreg = (uint8_t)nreg;
          in->src = src;
          in->dst = dst;
          in->adr = adr;
          in->vsrc = vs;
          in->vdst = vd;
          in->sym = sym;
        }
      }
    }
    else if (sscanf(line, "text %x %x", &a, &b) == 2)
    {
      textStart = a;
      textEnd = b;
      const size_t n = (textEnd - textStart) / 4;
      tab = calloc(n, sizeof(Insn));
      for (size_t i = 0; i < n; i++)
      {
        tab[i].cls = OTHER;
        tab[i].sym = -1;
      }
      hotCyc = calloc(n, sizeof(double));
      hotCnt = calloc(n, sizeof(uint32_t));
    }
    else if (sscanf(line, "special %767s %x", name, &a) == 2)
    {
      if (!strcmp(name, "bench_mark")) aMark = a;
      else if (!strcmp(name, "bench_exit")) aExit = a;
      else if (!strcmp(name, "g_bench_cfg")) aCfg = a;
      else if (!strcmp(name, "g_bench_out")) aOut = a;
      else if (!strcmp(name, "g_bench_list")) aList = a;
      else if (!strcmp(name, "_start")) aStart = a;
    }
    else if (sscanf(line, "nsym %d", &nsym) == 1)
    {
      symName = calloc((size_t)nsym + 1, sizeof(char *));
      symCyc = calloc((size_t)nsym + 1, sizeof(double));
      symIns = calloc((size_t)nsym + 1, sizeof(uint64_t));
    }
    else if (sscanf(line, "S %d %x %d %767s", &idx, &a, &size, name) == 4)
    {
      if (idx < nsym)
        symName[idx] = strdup(name);
      if (a == aMark)
        markEnd = aMark + (uint32_t)size;
    }
  }
  fclose(f);
  if (!tab || !aMark || !aExit || !aCfg || !aOut)
    die("the table is incomplete");
  if (!markEnd)
    markEnd = aMark + 4;
}

static void loadElf(uc_engine *uc, const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    die("cannot open the image");
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc((size_t)n);
  if (fread(d, 1, (size_t)n, f) != (size_t)n)
    die("short read");
  fclose(f);
  if (memcmp(d, "\177ELF", 4) || d[4] != 1)
    die("not a 32-bit ELF");
  const uint32_t phoff = *(uint32_t *)(d + 28);
  const uint16_t phentsize = *(uint16_t *)(d + 42), phnum = *(uint16_t *)(d + 44);
  for (int i = 0; i < phnum; i++)
  {
    const uint32_t *ph = (const uint32_t *)(d + phoff + (uint32_t)i * phentsize);
    if (ph[0] == 1 && ph[4] > 0) /* PT_LOAD: offset, vaddr, paddr, filesz */
      if (uc_mem_write(uc, ph[2], d + ph[1], ph[4]) != UC_ERR_OK)
        die("cannot write a segment");
  }
  free(d);
}

static void jsonStr(FILE *o, const char *s)
{
  fputc('"', o);
  for (; *s; s++)
  {
    if (*s == '"' || *s == '\\')
      fputc('\\', o);
    fputc(*s, o);
  }
  fputc('"', o);
}

int main(int argc, char **argv)
{
  const char *elf = 0, *tabPath = 0, *json = 0, *hot = 0;
  BenchCfg cfg = {0, 48000, 96, 96};
  uint32_t fpscrEntry = 0;
  int list = 0, clockMhz = 1000;
  for (int i = 1; i < argc; i++)
  {
    const char *a = argv[i];
#define ARG(n) (!strcmp(a, n) && i + 1 < argc)
    if (ARG("--elf")) elf = argv[++i];
    else if (ARG("--tab")) tabPath = argv[++i];
    else if (ARG("--scenario")) cfg.scenario = atoi(argv[++i]);
    else if (ARG("--sr")) cfg.sampleRate = atoi(argv[++i]);
    else if (ARG("--warmup")) cfg.warmupFrames = atoi(argv[++i]);
    else if (ARG("--frames")) cfg.frames = atoi(argv[++i]);
    else if (ARG("--json")) json = argv[++i];
    else if (ARG("--hot")) hot = argv[++i];
    else if (ARG("--fpscr")) fpscrEntry = !strcmp(argv[++i], "runfast") ? 0x03000000u : (uint32_t)strtoul(argv[i], 0, 0);
    else if (ARG("--l2miss")) { M.l2_miss = atof(argv[++i]); }
    else if (ARG("--vfp-extra"))
    {
      M.vfp_extra = atof(argv[++i]);
      M.vfp_extra_lo = M.vfp_extra_lo < M.vfp_extra ? M.vfp_extra_lo : M.vfp_extra;
      M.vfp_extra_hi = M.vfp_extra_hi > M.vfp_extra ? M.vfp_extra_hi : M.vfp_extra;
    }
    else if (ARG("--clock-mhz")) clockMhz = atoi(argv[++i]);
    else if (!strcmp(a, "--list")) list = 1;
    else die("usage: a8sim --elf bench.elf --tab bench.tab [--list] [--scenario N] [--sr 48000] [--warmup N] [--frames N]"
             " [--fpscr 0|runfast] [--json out.json] [--hot out.txt]");
  }
  if (!elf || !tabPath)
    die("--elf and --tab are required");
  if (list)
    cfg.scenario = -1;
  wantHot = hot != 0;
  loadTable(tabPath);
  cacheInit(&l1i, 32 * 1024, 4);
  cacheInit(&l1d, 32 * 1024, 4);
  cacheInit(&l2, 256 * 1024, 8);
  memset(btbTag, 0xff, sizeof(btbTag));
  memset(ghb, 1, sizeof(ghb));

  uc_engine *uc;
  if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK)
    die("uc_open");
  UC = uc;
  uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A8);
  const uint64_t ramBase = 0x80000000ull, ramSize = 96ull << 20;
  if (uc_mem_map(uc, ramBase, ramSize, UC_PROT_ALL) != UC_ERR_OK)
    die("uc_mem_map");
  loadElf(uc, elf);
  uint32_t v = 0x00f00000u;
  uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &v); /* CPACR: cp10/cp11 full access */
  v = 0x40000000u;
  uc_reg_write(uc, UC_ARM_REG_FPEXC, &v);
  fpscr = fpscrEntry;
  uc_reg_write(uc, UC_ARM_REG_FPSCR, &fpscr);
  uc_mem_write(uc, aCfg, &cfg, sizeof(cfg));

  /* Start-up (static constructors, the programme-material tables) runs without the timing
     hooks; they go in at the first marker, before the warm-up frames. */
  uc_hook h0, h1, h2;
  uc_hook_add(uc, &h0, UC_HOOK_CODE, hookExit, 0, aExit, aExit + 3);
  uc_err err = uc_emu_start(uc, aStart, aMark, 0, 0);
  uint32_t pc = 0;
  uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  if (err == UC_ERR_OK && pc == aMark)
  {
    uc_hook_add(uc, &h1, UC_HOOK_CODE, hookCode, 0, textStart, textEnd - 1);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hookMem, 0, 1, 0);
    err = uc_emu_start(uc, aMark, aExit, 0, 0);
  }
  if (err != UC_ERR_OK)
  {
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    fprintf(stderr, "a8sim: emulation stopped: %s at pc=%08x\n", uc_strerror(err), pc);
    return 3;
  }

  if (list)
  {
    char *buf = malloc(BENCH_LIST_LEN);
    uc_mem_read(uc, aList, buf, BENCH_LIST_LEN);
    buf[BENCH_LIST_LEN - 1] = 0;
    fputs(buf, stdout);
    return 0;
  }

  BenchOut *out = malloc(sizeof(BenchOut));
  uc_mem_read(uc, aOut, out, sizeof(BenchOut));
  nobj = out->nobj;
  if (out->status != 1)
  {
    fprintf(stderr, "a8sim: scenario %d failed in the image, status %d\n", cfg.scenario, out->status);
    return 4;
  }
  if (frames < 1)
    die("no frames were measured");

  /* per-frame figures */
  const double nf = (double)frames;
  const double budget = (double)clockMhz * 1e6 * 128.0 / (double)cfg.sampleRate;
  FILE *o = json ? fopen(json, "w") : stdout;
  if (!o)
    die("cannot write the json");
  fprintf(o, "{\n \"name\": ");
  jsonStr(o, out->name);
  fprintf(o, ",\n \"group\": ");
  jsonStr(o, out->group);
  fprintf(o, ",\n \"scenario\": %d, \"sample_rate\": %d, \"frames\": %d, \"warmup_frames\": %d,\n", cfg.scenario, cfg.sampleRate, frames, cfg.warmupFrames);
  fprintf(o, " \"fpscr_entry\": \"0x%08x\", \"fpscr_exit\": \"0x%08x\", \"clock_mhz\": %d, \"budget_cycles\": %.0f,\n", fpscrEntry, fpscr, clockMhz, budget);
  fprintf(o, " \"out_peak\": [%.5f, %.5f], \"out_rms\": [%.5f, %.5f],\n", out->peak[0], out->peak[1], out->rms[0], out->rms[1]);
  fprintf(o, " \"insns_per_frame\": %.1f,\n", (double)totalIns / nf);
  fprintf(o, " \"cycles_per_frame\": %.1f, \"cycles_per_frame_low\": %.1f, \"cycles_per_frame_high\": %.1f, \"cycles_worst_frame\": %.1f,\n",
          totalCyc / nf, (totalCyc - bandLo) / nf, (totalCyc + bandHi) / nf, frameMax);
  fprintf(o, " \"percent\": %.4f, \"percent_low\": %.4f, \"percent_high\": %.4f, \"percent_worst_frame\": %.4f,\n",
          100.0 * totalCyc / nf / budget, 100.0 * (totalCyc - bandLo) / nf / budget, 100.0 * (totalCyc + bandHi) / nf / budget, 100.0 * frameMax / budget);
  fprintf(o, " \"classes\": {");
  for (int k = 0; k < K_N; k++)
    fprintf(o, "%s\"%s\": %.1f", k ? ", " : "", kName[k], kCyc[k] / nf);
  fprintf(o, "},\n \"ops_per_frame\": {");
  int first = 1;
  for (int k = 0; k < NCLASS; k++)
    if (opCount[k] && className[k])
    {
      fprintf(o, "%s\"%s\": %.1f", first ? "" : ", ", className[k], (double)opCount[k] / nf);
      first = 0;
    }
  fprintf(o, "},\n \"scalar_fp_ops_vfp_lite\": %.1f, \"scalar_fp_ops_runfast\": %.1f,\n", (double)vfpLiteOps / nf, (double)nfpOps / nf);
  fprintf(o, " \"vfp_sequence_per_frame\": {\"after_vfp_dependent\": %.1f, \"after_vfp_independent\": %.1f, \"after_load_dependent\": %.1f, \"after_other\": %.1f, \"loadstore_after_vfp\": %.1f, \"store_of_vfp_result\": %.1f},\n",
          (double)cVfpAfterVfpDep / nf, (double)cVfpAfterVfpIndep / nf, (double)cVfpAfterLoadDep / nf, (double)cVfpAfterOther / nf, (double)cLsAfterVfp / nf, (double)cStoreOfVfpResult / nf);
  fprintf(o, " \"events_per_frame\": {\"loads\": %.1f, \"l1d_miss\": %.2f, \"l2_data_miss\": %.2f, \"l1i_miss\": %.2f, \"l2_insn_miss\": %.2f, \"mispredict\": %.2f, \"subnormal_results\": %.2f},\n",
          (double)cLoads / nf, (double)cL1dMiss / nf, (double)cL2dMiss / nf, (double)cL1iMiss / nf, (double)cL2iMiss / nf, (double)cMispred / nf, (double)cDenormal / nf);
  fprintf(o, " \"objects\": [\n");
  uint32_t deepest = 0;
  for (int i = 0; i < nobj; i++)
  {
    if (objStack[i] > deepest)
      deepest = objStack[i];
    fprintf(o, "  {\"name\": ");
    jsonStr(o, out->obj[i]);
    fprintf(o, ", \"cycles\": %.1f, \"low\": %.1f, \"high\": %.1f, \"insns\": %.1f, \"percent\": %.4f, \"stack_bytes\": %u}%s\n",
            objCyc[i] / nf, (objCyc[i] - objLo[i]) / nf, (objCyc[i] + objHi[i]) / nf, (double)objIns[i] / nf, 100.0 * objCyc[i] / nf / budget, objStack[i], i + 1 < nobj ? "," : "");
  }
  fprintf(o, " ],\n \"deepest_stack_bytes\": %u,\n \"forbidden_calls\": [", deepest);
  for (int i = 0; i < nviol; i++)
  {
    fprintf(o, "%s{\"object\": ", i ? ", " : "");
    jsonStr(o, viol[i].obj < nobj ? out->obj[viol[i].obj] : "?");
    fprintf(o, ", \"function\": ");
    jsonStr(o, viol[i].sym >= 0 ? symName[viol[i].sym] : "?");
    fprintf(o, "}");
  }
  fprintf(o, "],\n \"symbols\": [\n");
  /* the 24 most expensive functions */
  first = 1;
  for (int n = 0; n < 24; n++)
  {
    int best = -1;
    for (int i = 0; i < nsym; i++)
      if (symCyc[i] > 0 && (best < 0 || symCyc[i] > symCyc[best]))
        best = i;
    if (best < 0)
      break;
    fprintf(o, "%s  {\"name\": ", first ? "" : ",\n");
    jsonStr(o, symName[best]);
    fprintf(o, ", \"cycles\": %.1f, \"insns\": %.1f}", symCyc[best] / nf, (double)symIns[best] / nf);
    symCyc[best] = -1;
    first = 0;
  }
  fprintf(o, "\n ]\n}\n");
  if (json)
    fclose(o);

  if (hot)
  {
    FILE *h = fopen(hot, "w");
    if (!h)
      die("cannot write the hot-spot file");
    const size_t n = (textEnd - textStart) / 4;
    for (size_t i = 0; i < n; i++)
      if (hotCnt[i])
        fprintf(h, "%08x %.2f %.2f %d\n", (unsigned)(textStart + 4 * i), hotCyc[i] / nf, (double)hotCnt[i] / nf, tab[i].cls);
    fclose(h);
  }
  return 0;
}
