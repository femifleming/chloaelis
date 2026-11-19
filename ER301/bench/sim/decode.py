#!/usr/bin/env python3
"""Pre-decode the benchmark image for the timing model.

Reads the ELF, disassembles .text with capstone and writes one line per instruction:
its timing class, the core / VFP-NEON registers it reads and writes, and the function it
belongs to. sim/a8sim.c loads the table; the class numbers below are shared with it.

usage: decode.py bench.elf bench.tab
"""
import sys, struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from capstone import arm as A

# ---- timing classes (keep in step with a8sim.c)
C = dict(
    INT_ALU=0, INT_MUL=1, INT_LOAD=2, INT_STORE=3, INT_LDM=4, INT_STM=5, BRANCH=6, OTHER=7,
    VFP_ADD=10, VFP_MUL=11, VFP_MAC=12, VFP_DIV=13, VFP_SQRT=14, VFP_CPY=15, VFP_ABSNEG=16,
    VFP_CMP=17, VFP_F2I=18, VFP_I2F=19, VFP_CVT=20,
    VFP_LOAD=21, VFP_STORE=22, VFP_LDM=23, VFP_STM=24, XFER_TO_NEON=25, XFER_TO_ARM=26,
    N_FADD=30, N_FMUL=31, N_FMLA=32, N_INT=33, N_IMUL=34, N_SHIFT=35, N_PERM=36,
    N_LOAD=37, N_STORE=38,
)
# ---- flags
F_COND, F_SETFLAGS, F_DOUBLE, F_QUAD, F_CALL, F_RET, F_INDIRECT, F_CONDBR = 1, 2, 4, 8, 16, 32, 64, 128
F_FPSCR_W, F_FORBID, F_SPMOD, F_USEFLAGS = 256, 512, 1024, 2048

FORBIDDEN = (  # must never run inside an object's process()
    'malloc', 'free', 'calloc', 'realloc', 'memalign', '_malloc_r', '_free_r', '_calloc_r',
    '_realloc_r', '_memalign_r', '_Znwj', '_Znaj', '_ZdlPv', '_ZdaPv', '_ZnwjRKSt9nothrow_t',
    '_ZnajRKSt9nothrow_t', '__cxa_guard_acquire', '__cxa_guard_release', '__malloc_lock',
    '_sbrk', '_sbrk_r', 'Heap_malloc', 'Heap_memalign', 'Heap_calloc', 'Heap_realloc', 'Heap_free',
    '__cxa_allocate_exception', '__cxa_throw',
)
SPECIAL = ('bench_mark', 'bench_exit', 'g_bench_cfg', 'g_bench_out', 'g_bench_list', '_start')

CORE = {A.ARM_REG_R0 + i: i for i in range(13)}
CORE.update({A.ARM_REG_SP: 13, A.ARM_REG_LR: 14, A.ARM_REG_PC: 15})
FLAGREGS = {A.ARM_REG_CPSR, A.ARM_REG_APSR, A.ARM_REG_APSR_NZCV}
FPFLAGS = {A.ARM_REG_FPSCR, A.ARM_REG_FPSCR_NZCV}


def vmask(reg):
    """bit mask over the 64 single-precision halves of d0..d31"""
    if A.ARM_REG_S0 <= reg <= A.ARM_REG_S31:
        return 1 << (reg - A.ARM_REG_S0)
    if A.ARM_REG_D0 <= reg <= A.ARM_REG_D31:
        return 3 << (2 * (reg - A.ARM_REG_D0))
    if A.ARM_REG_Q0 <= reg <= A.ARM_REG_Q15:
        return 15 << (4 * (reg - A.ARM_REG_Q0))
    return 0


def is_s(r): return A.ARM_REG_S0 <= r <= A.ARM_REG_S31
def is_d(r): return A.ARM_REG_D0 <= r <= A.ARM_REG_D31
def is_q(r): return A.ARM_REG_Q0 <= r <= A.ARM_REG_Q15


NEON_FADD = {'vadd', 'vsub', 'vabd', 'vabs', 'vneg', 'vmax', 'vmin', 'vceq', 'vcge', 'vcgt', 'vcle', 'vclt',
             'vacge', 'vacgt', 'vpadd', 'vpmax', 'vpmin', 'vcvt', 'vrecpe', 'vrsqrte'}
NEON_FMLA = {'vmla', 'vmls', 'vrecps', 'vrsqrts'}
NEON_IMUL = {'vmul', 'vmull', 'vmla', 'vmls', 'vmlal', 'vmlsl', 'vqdmulh', 'vqrdmulh', 'vqdmull', 'vqdmlal', 'vqdmlsl'}
NEON_SHIFT = {'vshl', 'vshr', 'vsra', 'vrshl', 'vrshr', 'vrsra', 'vqshl', 'vqrshl', 'vshrn', 'vrshrn', 'vqshrn',
              'vqrshrn', 'vshll', 'vsli', 'vsri', 'vqshlu', 'vqshrun', 'vqrshrun', 'vmovl', 'vmovn', 'vqmovn', 'vqmovun'}
NEON_PERM = {'vext', 'vrev16', 'vrev32', 'vrev64', 'vzip', 'vuzp', 'vtrn', 'vtbl', 'vtbx', 'vswp', 'vdup'}
INT_MUL = {'mul', 'mla', 'mls', 'umull', 'smull', 'umlal', 'smlal', 'umaal', 'smulbb', 'smulbt', 'smultb', 'smultt',
           'smlabb', 'smlabt', 'smlatb', 'smlatt', 'smulwb', 'smulwt', 'smlawb', 'smlawt', 'smmul', 'smmla', 'smmls',
           'smuad', 'smusd', 'smlad', 'smlsd', 'smlald', 'smlsld', 'smlalbb', 'smlalbt', 'smlaltb', 'smlaltt'}
INT_LOAD = {'ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrd', 'ldrex', 'ldrexb', 'ldrexh', 'pld'}
INT_STORE = {'str', 'strb', 'strh', 'strd', 'strex', 'strexb', 'strexh'}


ALIAS = {'fmstat': 'vmrs', 'fconsts': 'vmov', 'fconstd': 'vmov', 'fldmiax': 'vldmia', 'fldmdbx': 'vldmdb',
         'fstmiax': 'vstmia', 'fstmdbx': 'vstmdb', 'hint': 'nop'}


def iname(i):
    n = i.insn_name()
    return ALIAS.get(n, n)


def classify(i):
    name = iname(i)
    mn = i.mnemonic
    ops = list(i.operands)
    regs = [o.reg for o in ops if o.type == A.ARM_OP_REG]
    vregs = [r for r in regs if vmask(r)]
    cregs = [r for r in regs if r in CORE]
    flags = 0
    nreg = 0
    if any(is_q(r) for r in vregs):
        flags |= F_QUAD
    f64 = '.f64' in mn
    if name.startswith('v'):
        scalar = bool(vregs) and (all(is_s(r) for r in vregs) or f64 or (name in ('vmov',) and False))
        if name in ('vmrs',):
            return C['XFER_TO_ARM'], flags, 0
        if name in ('vmsr',):
            return C['XFER_TO_NEON'], flags | F_FPSCR_W, 0
        if name in ('vldr',):
            return C['VFP_LOAD'], flags | (F_DOUBLE if any(is_d(r) for r in vregs) else 0), 1
        if name in ('vstr',):
            return C['VFP_STORE'], flags | (F_DOUBLE if any(is_d(r) for r in vregs) else 0), 1
        if name in ('vpush', 'vstmdb', 'vstmia'):
            return C['VFP_STM'], flags, max(1, sum(2 if is_d(r) else 1 for r in vregs) // 2)
        if name in ('vpop', 'vldmdb', 'vldmia'):
            return C['VFP_LDM'], flags, max(1, sum(2 if is_d(r) else 1 for r in vregs) // 2)
        if name.startswith('vld'):
            return C['N_LOAD'], flags, max(1, len(vregs))
        if name.startswith('vst'):
            return C['N_STORE'], flags, max(1, len(vregs))
        if name == 'vmov' or name == 'vmvn':
            if cregs and vregs:
                # which way? the first operand is the destination
                return (C['XFER_TO_ARM'] if regs[0] in CORE else C['XFER_TO_NEON']), flags, 0
            if scalar:      # fcpys / fcpyd / fconsts / fconstd
                return C['VFP_CPY'], flags | (F_DOUBLE if f64 else 0), 0
            return C['N_INT'], flags, 0
        if name == 'vdup' and cregs:
            return C['XFER_TO_NEON'], flags, 0
        if scalar:
            dbl = F_DOUBLE if (f64 and name != 'vcvt') else 0
            if name in ('vadd', 'vsub'):
                return C['VFP_ADD'], flags | dbl, 0
            if name in ('vmul', 'vnmul'):
                return C['VFP_MUL'], flags | dbl, 0
            if name in ('vmla', 'vmls', 'vnmla', 'vnmls', 'vfma', 'vfms', 'vfnma', 'vfnms'):
                return C['VFP_MAC'], flags | dbl, 0
            if name == 'vdiv':
                return C['VFP_DIV'], flags | dbl, 0
            if name == 'vsqrt':
                return C['VFP_SQRT'], flags | dbl, 0
            if name in ('vabs', 'vneg'):
                return C['VFP_ABSNEG'], flags | dbl, 0
            if name in ('vcmp', 'vcmpe'):
                return C['VFP_CMP'], flags | dbl, 0
            if name in ('vcvt', 'vcvtr'):
                # mnemonic is vcvt.<to>.<from>
                parts = mn.split('.')
                to, frm = (parts[1], parts[2]) if len(parts) >= 3 else ('', '')
                if to.startswith('f') and frm.startswith('f'):
                    # f64 <- f32 is FCVTDS (5 cycles), f32 <- f64 is FCVTSD (7 cycles)
                    return C['VFP_CVT'], flags | (F_DOUBLE if to == 'f32' else 0), 0
                if to.startswith('f'):
                    return C['VFP_I2F'], flags | (F_DOUBLE if to == 'f64' else 0), 0
                return C['VFP_F2I'], flags | (F_DOUBLE if frm == 'f64' else 0), 0
            return C['VFP_CPY'], flags | dbl, 0
        # Advanced SIMD data processing
        isf = '.f32' in mn
        if name in NEON_PERM:
            return C['N_PERM'], flags, 0
        if isf and name in NEON_FMLA:
            return C['N_FMLA'], flags, 0
        if isf and name == 'vmul':
            return C['N_FMUL'], flags, 0
        if isf or name in ('vcvt', 'vrecpe', 'vrsqrte'):
            return C['N_FADD'], flags, 0
        if name in NEON_IMUL:
            return C['N_IMUL'], flags, 0
        if name in NEON_SHIFT:
            return C['N_SHIFT'], flags, 0
        return C['N_INT'], flags, 0

    # ---- core instructions
    writes_pc = False
    try:
        rd, wr = i.regs_access()
    except Exception:
        rd, wr = [], []
    if A.ARM_REG_PC in wr:
        writes_pc = True
    if name in ('b', 'bl', 'bx', 'blx', 'bxj') or writes_pc:
        if name in ('bl', 'blx'):
            flags |= F_CALL
        if name in ('bx', 'blx') and ops and ops[0].type == A.ARM_OP_REG:
            if ops[0].reg == A.ARM_REG_LR and name == 'bx':
                flags |= F_RET
            else:
                flags |= F_INDIRECT
        if name in ('pop', 'ldm', 'ldmib', 'ldmda', 'ldmdb') and writes_pc:
            flags |= F_RET
            nreg = len(regs)
        elif name not in ('b', 'bl', 'bx', 'blx'):
            flags |= F_INDIRECT       # mov pc / ldr pc / add pc (jump tables)
            if name == 'mov' and len(regs) > 1 and regs[1] == A.ARM_REG_LR:
                flags = (flags & ~F_INDIRECT) | F_RET
        if i.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID) and name == 'b':
            flags |= F_CONDBR
        return C['BRANCH'], flags, nreg
    if name in ('push', 'stm', 'stmib', 'stmda', 'stmdb'):
        return C['INT_STM'], flags, len(cregs) - (0 if name == 'push' else 1)
    if name in ('pop', 'ldm', 'ldmib', 'ldmda', 'ldmdb'):
        return C['INT_LDM'], flags, len(cregs) - (0 if name == 'pop' else 1)
    if name in INT_LOAD:
        return C['INT_LOAD'], flags, 0
    if name in INT_STORE:
        return C['INT_STORE'], flags, 0
    if name in INT_MUL:
        return C['INT_MUL'], flags, 0
    if name in ('svc', 'bkpt', 'mrs', 'msr', 'cps', 'mrc', 'mcr', 'nop', 'udf', 'wfi', 'dmb', 'dsb', 'isb') or name[:3] in ('ldc', 'stc'):
        return C['OTHER'], flags, 0
    return C['INT_ALU'], flags, 0


def main():
    elf_path, out_path = sys.argv[1], sys.argv[2]
    ef = ELFFile(open(elf_path, 'rb'))
    text = ef.get_section_by_name('.text')
    base, data = text['sh_addr'], text.data()
    symtab = ef.get_section_by_name('.symtab')
    funcs, special, data_marks = [], {}, []
    for s in symtab.iter_symbols():
        n, v = s.name, s['st_value']
        if n in SPECIAL:
            special[n] = v
        if s['st_info']['type'] == 'STT_FUNC' and s['st_size'] > 0:
            funcs.append((v & ~1, s['st_size'], n))
        elif n.startswith('$d') or n.startswith('$a') or n.startswith('$t'):
            if base <= v < base + len(data):
                data_marks.append((v, n[:2]))
    funcs.sort()
    # drop duplicate addresses (aliases), keep the first name
    uniq = []
    for f in funcs:
        if uniq and uniq[-1][0] == f[0]:
            continue
        uniq.append(f)
    funcs = uniq
    data_marks.sort()
    if any(k == '$t' for _, k in data_marks):
        sys.exit('decode.py: Thumb code in the image; the table assumes ARM state')

    # which words are code ($a) and which are literal pools ($d)
    is_code = bytearray(len(data) // 4)
    for k, (addr, kind) in enumerate(data_marks):
        end = data_marks[k + 1][0] if k + 1 < len(data_marks) else base + len(data)
        if kind == '$a':
            for w in range((addr - base) // 4, (end - base) // 4):
                is_code[w] = 1

    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    forbid = {f[0] for f in funcs if f[2] in FORBIDDEN}
    fi = 0
    lines = []
    for w in range(len(data) // 4):
        addr = base + 4 * w
        while fi + 1 < len(funcs) and funcs[fi + 1][0] <= addr:
            fi += 1
        sym = fi if funcs and funcs[fi][0] <= addr < funcs[fi][0] + funcs[fi][1] else -1
        if not is_code[w]:
            continue
        ins = next(md.disasm(data[4 * w:4 * w + 4], addr, 1), None)
        if ins is None:
            lines.append('I %x %d %d %d 0 0 0 0 0 %d' % (addr, C['OTHER'], 0, 0, sym))
            continue
        cls, flags, nreg = classify(ins)
        try:
            rd, wr = ins.regs_access()
        except Exception:
            rd, wr = [], []
        src = dst = adr = 0
        vsrc = vdst = 0
        for r in rd:
            if r in CORE:
                src |= 1 << CORE[r]
            elif r in FLAGREGS:
                src |= 1 << 16
            else:
                vsrc |= vmask(r)
        for r in wr:
            if r in CORE:
                dst |= 1 << CORE[r]
            elif r in FLAGREGS:
                dst |= 1 << 16
            else:
                vdst |= vmask(r)
        for o in ins.operands:
            if o.type == A.ARM_OP_MEM:
                for r in (o.mem.base, o.mem.index):
                    if r in CORE:
                        adr |= 1 << CORE[r]
                        src &= ~(1 << CORE[r])
        if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
            flags |= F_COND | F_USEFLAGS
            src |= 1 << 16
            if cls != C['BRANCH']:
                src |= dst & 0xffff        # the old value is needed if the condition fails
                vsrc |= vdst
        if cls in (C['XFER_TO_NEON'], C['XFER_TO_ARM']):
            # capstone leaves the core register out of some of these
            for o in ins.operands:
                if o.type == A.ARM_OP_REG and o.reg in CORE:
                    if cls == C['XFER_TO_NEON']:
                        src |= 1 << CORE[o.reg]
                    else:
                        dst |= 1 << CORE[o.reg]
        if ins.insn_name() in ('adc', 'sbc', 'rsc', 'rrx'):
            src |= 1 << 16
        if ins.update_flags and cls < 10:
            dst |= 1 << 16
            flags |= F_SETFLAGS
        if cls == C['XFER_TO_ARM'] and iname(ins) == 'vmrs':
            # vmrs APSR_nzcv, fpscr sets the core flags; vmrs rX, fpscr writes rX
            if not (dst & 0xffff):
                dst |= 1 << 16
                flags |= F_SETFLAGS
        if dst & (1 << 13):
            flags |= F_SPMOD
        if addr in forbid:
            flags |= F_FORBID
        src &= ~(1 << 15)   # the PC is always available
        adr &= ~(1 << 15)
        lines.append('I %x %d %d %d %x %x %x %x %x %d' % (addr, cls, flags, nreg, src, dst, adr, vsrc, vdst, sym))

    with open(out_path, 'w') as f:
        f.write('text %x %x\n' % (base, base + len(data)))
        for n in SPECIAL:
            if n not in special:
                sys.exit('decode.py: symbol %s missing from the image' % n)
            f.write('special %s %x\n' % (n, special[n]))
        f.write('nsym %d\n' % len(funcs))
        for k, (a, sz, n) in enumerate(funcs):
            f.write('S %d %x %d %s\n' % (k, a, sz, n))
        f.write('ninsn %d\n' % len(lines))
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
