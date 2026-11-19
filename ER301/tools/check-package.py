#!/usr/bin/env python3
"""Checks a package built for the module (testing/am335x) for the three things that crash an
ER-301 without warning. Run after `make ARCH=am335x`:

    tools/check-package.py testing/am335x chloaelis

1. Stack. The audio task has 2048 bytes of stack for the whole chain of units, so no process()
   may reach deep. The frames of every function reachable from a process() are added up along
   the deepest path (prologue pushes and `sub sp`), across the package's object files.
2. NEON alignment. A NEON load or store that carries an alignment promise (`[r3 :64]`) traps on
   the Cortex-A8 when the address does not keep it, and on the module addresses on the stack do
   not reliably keep it (the habitat project found this the hard way: vectors read from arrays
   on the stack, and vector registers the compiler parks on the stack around a call, both
   crashed there). The compiler attaches the promise to anything it placed itself. None are
   allowed, anywhere in the package's own code.
3. Imports. A package can only call what the firmware exports. C-library trigonometry is
   refused outright (it miscomputes when called from a package), and anything that is not on the
   list below is reported, so that a new dependency is a decision and not an accident.

Exit status 0 when all three pass."""
import collections, os, re, subprocess, sys

BUDGET = 512          # bytes for one process() and everything under it
TOOLS = os.environ.get("ARM_TOOLS", os.path.expanduser("~/ti/gcc-arm-none-eabi-4_9-2015q3/bin"))

def tool(name):
    p = os.path.join(TOOLS, "arm-none-eabi-" + name)
    return p if os.path.exists(p) else "arm-none-eabi-" + name

def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True, stdin=subprocess.DEVNULL).stdout

ALLOWED = [
    r"^_ZN2od", r"^_ZNK2od", r"^_ZT[IVS]N2od",                 # the firmware's own classes
    r"^_ZT[IV]N10__cxxabiv1", r"^__cxa_", r"^__gxx_personality_v0$", r"^__aeabi_", r"^__getreent$", r"^__assert_func$",
    r"^_Zn[wa]j$", r"^_Zd[la]Pv$", r"^_ZNSs", r"^_ZNKSs", r"^_ZSt",   # new, delete, std::string
    r"^lua_", r"^luaL_", r"^globalConfig$",
    r"^(memcpy|memset|memmove|memcmp|strlen|strcmp|strncmp|strstr|fprintf|printf|puts|abort)$",
    r"^(expf|logf|powf|sqrtf|sqrt|fabsf)$",                    # libm: what other released packages use too
]
REFUSED = r"^(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh)(f|l)?$"

def frames_and_calls(obj):
    """per function: bytes of stack it takes, and the functions it calls"""
    frame, calls, hints = collections.defaultdict(int), collections.defaultdict(set), []
    cur, count = None, 0
    for line in run(tool("objdump"), "-dr", obj).splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:", line)
        if m:
            cur, count = m.group(1), 0
            frame[cur] += 0
            continue
        if cur is None:
            continue
        m = re.search(r"R_ARM_(CALL|JUMP24|THM_CALL|PLT32)\s+(\S+)", line)
        if m:
            calls[cur].add(m.group(2))
            continue
        if not line.startswith(" "):
            continue
        text = line.split("\t", 2)[-1] if "\t" in line else line
        if re.search(r"\bv(ld|st)\d", text) and re.search(r":(64|128|256)\]", text):
            hints.append((cur, text.strip()))
        count += 1
        if count <= 32:                                        # the prologue
            m = re.search(r"sub\s+sp, sp, #(\d+)", text)
            if m:
                frame[cur] += int(m.group(1))
            m = re.search(r"\bv?push\s+\{(.+)\}", text)
            if m:
                regs = 0
                for part in m.group(1).split(","):
                    part = part.strip()
                    r = re.match(r"([a-z]+)(\d+)-[a-z]+(\d+)", part)
                    n = int(r.group(3)) - int(r.group(2)) + 1 if r else 1
                    regs += n * (8 if part.startswith("d") else 4)
                frame[cur] += regs
            m = re.search(r"stmdb\s+sp!, \{(.+)\}", text)
            if m:
                frame[cur] += 4 * len(m.group(1).split(","))
    return frame, calls, hints

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out_dir, pkg = sys.argv[1], sys.argv[2]
    objs = []
    for root, _, files in os.walk(os.path.join(out_dir, "mods", pkg)):
        objs += [os.path.join(root, f) for f in files if f.endswith(".o")]
    lib = os.path.join(out_dir, "lib%s.so" % pkg)
    if not objs or not os.path.exists(lib):
        sys.exit("nothing built for %s in %s (run make ARCH=am335x first)" % (pkg, out_dir))
    frame, calls, hints = {}, collections.defaultdict(set), []
    for o in objs:
        f, c, h = frames_and_calls(o)
        frame.update({k: max(v, frame.get(k, 0)) for k, v in f.items()})
        for k, v in c.items():
            calls[k] |= v
        hints += h
    failed = False

    # 1. stack
    memo = {}
    def depth(fn, seen=()):
        if fn in memo:
            return memo[fn]
        if fn in seen or fn not in frame:
            return 0, [fn]
        best, path = 0, []
        for c in calls.get(fn, ()):
            d, p = depth(c, seen + (fn,))
            if d > best:
                best, path = d, p
        memo[fn] = (frame[fn] + best, [fn] + path)
        return memo[fn]
    roots = sorted(f for f in frame if f.endswith("7processEv"))
    demangle = lambda names: run("c++filt", *names).split("\n") if names else []
    print("stack under each process() (budget %d bytes of the audio task's 2048):" % BUDGET)
    for r in roots:
        d, path = depth(r)
        print("  %4d bytes  %s" % (d, " > ".join(n.split("(")[0] for n in demangle(path) if n)))
        if d > BUDGET:
            failed = True
            print("       ^ over the budget")
    if not roots:
        print("  (no process() found)"); failed = True

    # 2. alignment promises
    print("NEON loads and stores with an alignment promise: %d" % len(hints))
    for fn, text in hints[:20]:
        print("  %s: %s" % (demangle([fn])[0], text))
    failed = failed or bool(hints)

    # 3. imports
    undefined = [l.split()[-1] for l in run(tool("nm"), "-u", lib).splitlines() if l.strip()]
    refused = [s for s in undefined if re.match(REFUSED, s)]
    unknown = [s for s in undefined if not re.match(REFUSED, s) and not any(re.match(p, s) for p in ALLOWED)]
    print("imports: %d, refused: %d, not on the list: %d" % (len(undefined), len(refused), len(unknown)))
    for s in refused:
        print("  refused: %s" % s)
    for s, d in zip(unknown, demangle(unknown)):
        print("  not on the list: %s" % d)
    failed = failed or bool(refused) or bool(unknown)

    print("FAILED" if failed else "OK")
    sys.exit(1 if failed else 0)

main()
