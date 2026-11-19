# CPU benchmark for the Chloaelis ER-301 units

How much of the ER-301's processor each unit takes, estimated without a module: the units are
compiled with the module's compiler, run instruction by instruction in an emulator, and
every instruction is charged what the Cortex-A8 manual says it costs. The estimate is then
checked against figures that were read off real modules.

**Nothing here has been run on an ER-301.** The numbers are a model's output. See
[Limits](#limits) before quoting them.

## Run it

Needs the module toolchain in `~/ti/gcc-arm-none-eabi-4_9-2015q3` (see ../README.md), the
firmware tree in `../er-301-release`, Python 3 and a C compiler.

    cd ER301/bench
    ./fetch-calibrators.sh        # once: the calibration units (optional, see Calibration)
    make                          # builds, runs every scenario, writes ../build/bench/current/RESULTS.md
    make RUNFLAGS="--only chloe/tube --no-runfast --no-96k"     # a subset (regex on the scenario name)
    make RUNFLAGS=--quick         # fewer frames
    make baseline                 # the same for the 1.0.0 package: put its mods/chloaelis in ../build/baseline/chloaelis
    make compare                  # baseline against current, scenario by scenario

    ../build/bench/venv/bin/python tools/run.py hot --out ../build/bench/current chloe/tube/mono/12ax7-2x --insns
                                  # where one scenario spends its cycles: by source line, then by instruction
    ../build/bench/venv/bin/python tools/calibrate.py ../build/bench/current/results.json
                                  # model against the hardware readings (--write refits calibration.json)

`make` creates a Python environment in `../build/bench/venv` (unicorn, capstone, pyelftools).
Everything generated goes under `../build/bench/`, which git ignores.

Per run, `runs/*.json` has: instructions and cycles per frame (mean, worst frame, low and high
bound), where the cycles went (integer, VFP coprocessor, NEON, NEON-to-ARM transfers, branch
mispredictions, cache misses), operation counts, the cost of every object, the 24 most
expensive functions, the deepest stack use inside one `process()`, any allocation or lock
call made from a `process()`, the number of subnormal results, and the output's peak and RMS
(so a silent or exploding unit shows).

## What is measured

- **The image** (`target/`): a bare-metal ARM program. It links the firmware's own object
  model compiled from `../er-301-release` with the firmware's flags (`-O3 -ftree-vectorize
  -ffast-math -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard`), the stock units used for
  comparison (same source, same flags), and the Chloaelis package sources compiled the way
  `mods/chloaelis/mod.mk` compiles them: `*.cpp` at `-O3 -ffast-math -fno-tree-vectorize`,
  and the classes of `Units.h` at `-Os` because on the module they are compiled inside the
  SWIG wrapper. `target/runtime.cpp` stands in for what the firmware has and the image has
  not: the audio frame pool (1024 cache-line aligned frames, as `AudioThread::init` makes),
  the heap, the log, the clock. C library and libm come from the toolchain's ARM-mode
  hard-float multilib, the flavour the firmware links.
- **Scenarios** build a unit the way its Lua file does, by object, port, parameter and option
  name: one DSP object (or one per channel), a `ParameterAdapter` per fader, a `Comparator`
  per gate, `GainBias` + `MinMax` where the unit has audio-rate controls, and behind every
  control an empty sub-chain (`bench::Branch`: the 128-sample copy an `od::UnitChain` makes
  each frame, which is why a control's object sees a full buffer and not a constant). Each
  frame runs every object in graph order with `updateParameters()` then `process()`, as
  `od::Unit::process()` does.
  - `target/scenarios_chloaelis.cpp`: the Chloaelis units, single (defaults and the expensive
    corners) – **edit this when a unit, port or parameter changes**. It also implements the
    `chloe::` builders of `target/chloe_units.h`.
  - `target/scenarios_chloaelis_1.0.0.cpp`: the same for the 1.0.0 package (the "before").
  - `target/scenarios_stock.cpp`: stock units and single stock objects.
  - `target/scenarios_calib.cpp`: the calibration units (only when fetched).
  - `target/scenarios_config.cpp`: several units in one frame, stock and Chloaelis mixed.
- **Input** is programme material, not silence or a constant: five plucked partials and noise
  bursts per channel peaking at 0.5 (±5 V), a 0.7 Hz LFO, a gate and a trigger for the
  sub-chains. The `after-silence` scenarios play 0.2 s of it and then nothing for 3 s.
- **Frames**: 128 samples at 48 kHz (the Chloaelis group also at 96 kHz). 144 warm-up frames
  (0.38 s, longer than the default delay time; twice as many at 96 kHz), then 96 measured.

## The simulator (`sim/`)

`decode.py` disassembles the image once (capstone) into a table: for every instruction its
timing class and the core and VFP/NEON registers it reads and writes. `a8sim.c` runs the image
in the [Unicorn engine](https://www.unicorn-engine.org) (a CPU emulator, here as a Cortex-A8)
and, for every instruction executed between the frame markers, advances a model of the
processor. Unicorn supplies what really happens in this binary (which instructions, which
way each branch goes, which addresses are read and written, what FPSCR holds); the model
supplies the time.

The model follows the Cortex-A8 Technical Reference Manual, revision r3p2 (ARM DDI 0344K),
chapter 16 "Instruction Cycle Timing":

| What | Model | Source |
|---|---|---|
| Core pipeline | in order, two instructions per cycle; one load/store and one branch per cycle; multiplies and multi-cycle instructions start alone; a result is usable the next cycle, a loaded value one cycle later, an address operand one cycle earlier than a data operand | TRM 16.2, table 16-15 |
| Branch misprediction | 13 cycles; predictor: 512-entry 2-way BTB, 4096 two-bit counters indexed by global history and PC, 8-entry return stack | TRM 16.4.1, chapter 5 |
| Caches | L1 I and D 32 KB 4-way, L2 256 KB 8-way, 64-byte lines; L1 miss served by L2: 8 cycles; L2 miss: 18 cycles + memory, taken as 78 (range 40 to 148) | sizes: AM335x; penalties: TRM table 16-16; the memory part is an assumption |
| Stores | never stall; a store that misses L1 goes to L2 without filling L1 | stall: TRM table 16-16; no L1 fill: from memory, not checked |
| NEON instructions | queue of 16 between the core and the NEON unit; in order; one data-processing and one load/store/permute instruction can issue together; d-register float add/mul: 1 cycle, result 4 cycles later; q-register: 2 cycles, 5; `vmla`-type: result 8 (9) cycles later, and an add or multiply after it waits 4 cycles; integer 3; shifts 4; loads 2 or 3 cycles | TRM 16.5, 16.6, tables 16-21 to 16-24 |
| NEON to ARM (`vmov rN, sM`, `vmrs` after a float compare) | 20 cycles, and the core stalls at the next instruction that touches a core register | TRM 16.5.2 |
| Scalar float with FPSCR = 0 | the VFP coprocessor, one instruction at a time, no forwarding: add/sub 9-10 cycles, multiply 10-12 (double 11-17), multiply-accumulate 18-21 (19-26), divide 20-37 (29-65), square root 19-33 (29-60), copy/constant/abs/neg 4, compare 4-7, int conversions 8-9, float↔double 5/7; the middle of each range is used | TRM table 16-25 |
| ... plus 11 cycles per VFP-coprocessor instruction (range 8 to 14) | **not from the TRM**: fitted to hardware readings, see Calibration | this benchmark |
| Scalar float in RunFast mode (FPSCR.FZ and .DN set) | single-precision add, sub, mul, mac, abs, neg, compare and int conversion run in the NEON floating-point pipeline: one per cycle, result 7 cycles later (mac: 2 cycles, 11); divide, square root, register copies, constants and everything double stay on the VFP coprocessor | TRM 16.7.2, example 16-8 |
| Subnormal results (FPSCR.FZ clear) | +12 cycles each (6-7 to format the result, 5-6 when it is next an operand) | TRM 16.7.1 |
| Clock | 1 GHz, so a 128-sample frame at 48 kHz is 2,666,667 cycles | `pbl/hal/board/pll.c` (`mpuDpllMult = 1000`) |

The simulator tracks FPSCR as the code writes it, so a unit that sets RunFast for the length
of its `process()` is timed accordingly. On entry FPSCR is 0, or FZ+DN for the "RunFast on
entry" column.

Not modelled: the TLB, the L1 fill buffer and store buffer, alignment penalties, `ACTLR`
settings (L1NEON, L2 prefetch), interrupts, and everything in SYS/BIOS (each chain's mutex,
the task scheduler, the profiler's timers).

## Calibration

Two sets of readings from real modules:

- **Stock code**: comments in the firmware source with the original author's on-module
  profiler output, in CPU cycles per 128-sample frame (`ticks()` is the PMU cycle counter:
  `arch/am335x/hal/timing.c`), e.g. `SineOscillator.cpp`: "[Sine Osc]: 0.9717% (26053 ticks,
  373 Hz)". Units (name in brackets) and single objects ("600 ticks for 128 samples"). They
  date from an older firmware build.
- **Scalar float and double code**: stolmine's notes on the "house" reverbs of er-301-habitat
  (percent of CPU on a stereo chain, read from the module's CPU meter:
  `planning/house-ports-optimization.md`, `planning/creamcoat-port-plan.md`), and two scalar
  loops that the firmware source keeps beside their NEON replacements with their readings.
  `fetch-calibrators.sh` clones that repository at a pinned commit into
  `../build/bench/habitat` and extracts the three files as they were when measured. It has no
  licence file, so none of it is in this tree; without it the calibration scenarios are
  simply left out.

`tools/calibrate.py` prints model against reading for each. What it showed, in order:

1. NEON-intrinsic code needs no correction: the four single-object readings that are plain
   NEON loops come out at 0.81 to 1.01 of the reading with the TRM's numbers as they are.
2. Scalar float code was at 0.41 to 0.81 of its readings with the TRM's numbers alone.
   Adding a constant to every instruction the VFP coprocessor executes, fitted by least
   squares over the seven scalar readings, gives **11 cycles**, and 0.76 to 1.37 after it.
   The mechanism is not established (the size is about the length of the NEON pipeline, which
   each coprocessor instruction may have to traverse alone); it is an empirical term.
3. The firmware does not appear to set RunFast: the rework of kWoodRoom from double to float
   bought "about 1% mono" on the module; the model gives a change of -0.5% of a stereo frame
   (slightly slower) with FPSCR = 0 on entry and a saving of 16.0% with RunFast on entry. Scalar float is therefore as slow
   as double on the module, unless the unit sets RunFast itself.
4. What is left after 1 to 3 is a single scale over all readings, stored in
   `calibration.json` and applied to every "est." figure by `tools/run.py`: **x1.104** (the
   model was 9% low on average, about what the stock-unit readings carry in profiler and
   sub-chain overhead). After it, 16 of the 18 readings are within -18% and +21% of the
   model; that band (estimate x0.83 to x1.22) is the "low .. high" printed with each
   estimate. The two outside it are Verbity (model 51% above a reading quoted as "15-20%") and
   the stock limiter's inverse-square-root loop (model 41% below "7k ticks").

Model against reading, before the x1.104 scale (cycles per 128-sample frame, 48 kHz):

| What | Reading | Model, FPSCR = 0 | ratio | Model, RunFast on entry | ratio |
|---|---:|---:|---:|---:|---:|
| Sine Osc unit | 26053 | 22822 | 0.88 | 20732 | 0.80 |
| Aliasing Saw unit | 19644 | 16654 | 0.85 | 14738 | 0.75 |
| Aliasing Triangle unit | 19172 | 17315 | 0.90 | 15388 | 0.80 |
| ADSR unit | 27163 | 20179 | 0.74 | 15309 | 0.56 |
| Ladder LPF unit | 34388 | 31277 | 0.91 | 30939 | 0.90 |
| EQ3 unit, mono | 28399 | 29997 | 1.06 | 14852 | 0.52 |
| Clipper object (NEON) | 600 | 483 | 0.81 | 498 | 0.83 |
| ConstantGain object (vectorised) | 600 | 522 | 0.87 | 489 | 0.82 |
| CrossFade object (NEON) | 1400 | 1315 | 0.94 | 1372 | 0.98 |
| Limiter, cubic (NEON) | 2200 | 2223 | 1.01 | 2135 | 0.97 |
| Limiter, inverse square root (NEON) | 7000 | 3751 | 0.54 | 3675 | 0.52 |
| scalar tanh loop | 55000 | 48467 | 0.88 | 27366 | 0.50 |
| scalar clipper loop | 9600 | 10503 | 1.09 | 8009 | 0.83 |
| CreamCoat, stereo (14%) | 373333 | 354567 | 0.95 | 249024 | 0.67 |
| kWoodRoom as measured, stereo (29%) | 773333 | 810030 | 1.05 | 809812 | 1.05 |
| WoodenBox as measured, stereo (14%) | 373333 | 282640 | 0.76 | 282564 | 0.76 |
| Verbity as measured, stereo (15-20%) | 466667 | 637243 | 1.37 | 637118 | 1.37 |
| Galactic, stereo (25-30%) | 733333 | 727390 | 0.99 | 727216 | 0.99 |

Geometric mean of the ratios: 0.91 with FPSCR = 0 on entry, 0.78 with RunFast on entry.
The unit readings are taken to be whole units (the bracketed names are unit titles); the
"N ticks" comments are taken to be single objects, except the limiter's, which may include
its two gain objects.

RunFast has **no** hardware reading behind it: no measured code on the module uses it. Its
figures rest on the TRM's 7-cycle statement alone.

## Limits

- No module was involved. The model is a hand model of an in-order core, not a cycle-accurate
  one, and its scalar-float term is fitted to seven readings of other people's code.
- The readings themselves are rough: CPU-meter percentages quoted as "about 14%", "15-20%";
  profiler comments from an older firmware; unit-level readings include profiler and
  sub-chain overhead that the image has only in part.
- A unit's cost on the module also includes SYS/BIOS work per chain (a mutex per sub-chain per
  frame) that is not in the image. The stock-unit readings suggest this is small.
- The firmware reports dropouts from about 80% CPU at 128-sample frames, 48 kHz
  (`hal/constants.h`); headroom should be judged against that, not against 100%.
- Costs that depend on the sound (subnormals, mispredicted branches, cache contents) are those
  of the programme material used here.

## Files

    Makefile                 builds the image, the table, the simulator; runs the scenarios
    fetch-calibrators.sh     clones the calibration units (not redistributed)
    calibration.json         the scale and uncertainty that tools/run.py applies
    target/                  the image: start-up, linker script, runtime stand-ins, harness, scenarios
    sim/decode.py            image -> instruction table
    sim/a8sim.c              the simulator and the timing model
    tools/run.py             run / report / compare / hot
    tools/calibrate.py       model against hardware readings
