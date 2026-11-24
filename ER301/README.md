# Chloaelis for the ER-301

The Chloaelis plug-in as eight separate units for the Orthogonal Devices ER-301 sound computer
running the [stolmine firmware](https://github.com/stolmine/er-301-stolmine): **PT2399 Delay,
BBD Delay, Tape Loop, Tube, Cell Filter, Lorenz, Flock and Scale String**. Every parameter is a
standard ER-301 fader with its own CV sub-chain, gain and offset; each unit has a cell-style
display; every unit works in mono and stereo chains and is saved in presets and quicksaves.
Scale String reads Scala (`.scl`) files from the front card's `ER-301/scales` folder.

Manual: [docs/Chloaelis-ER301-Manual.pdf](docs/Chloaelis-ER301-Manual.pdf).

## Status

Package version 1.1.1, built for the module against release 0.7.0-stolmine.9.7.0 with the
firmware's own compiler and SWIG. In the ER-301 emulator on macOS every unit loads and runs in a
mono and a stereo chain, the state that goes beyond faders (Tape Loop's wear, Scale String's
scale) survives being saved and given to a new unit, and six of the units run in one chain with
stock units. The sound of each unit is checked offline against a plain transcription of the
plug-in's code (`make test`). **It has not yet been
run on a real ER-301**, and the processor figures below are estimates from a model of the
module's processor, not readings from a module. The manual's section 15 is the list of things to
try first on a module.

## Install

Download `chloaelis-1.1.1.pkg` from the
[er301-v1.1.1 release](https://github.com/femifleming/chloaelis/releases/tag/er301-v1.1.1) (or build it, below), copy it to
`ER-301/packages` on the front SD card, and install it in the admin screen's Package Manager.
The units appear under **Chloaelis**. The package only works with the firmware it was built
for; it changes nothing in the firmware and uninstalls cleanly.

## Changes

- **1.1.1:** Chloaelis' class names no longer clash with the ER-301's own Delay unit; the stock
  Delay works whatever order units are created in. (The firmware keeps one table of classes for
  all packages, and in 1.1.0 a PT2399, BBD or Tape Loop created before any core unit took the
  name `Delay` from core. The classes are now registered under their namespace,
  `lib.chloaelis.Delay` and so on.) Patches and presets saved with 1.1.0 load unchanged.
- **1.1.0:** reworked for the module's processor (see below).
- **1.0.0:** first version.

## Processor load

Estimated percentage of the ER-301's processor (one 128-sample frame at 48 kHz on the 1 GHz
Cortex-A8), with the range the estimate is good to in brackets. 1.0.0 is the first build of
this package, measured the same way; 1.1.0 was reworked for the processor. A stereo unit costs
the same as a mono one (both channels share the vector registers).

| Unit | 1.0.0 | now | now at 96 kHz |
|---|---:|---:|---:|
| PT2399 Delay, mono, default (342 ms) | 19.4% | 2.9% (2.4–3.6) | 5.2% |
| PT2399 Delay, stereo, default | 38.5% | 2.9% (2.4–3.6) | 5.2% |
| PT2399 Delay, mono, 30 ms (its dearest) | 68.8% | 9.6% (7.9–11.6) | 11.8% |
| BBD Delay, stereo, default | 30.2% | 2.8% (2.3–3.4) | 5.4% |
| BBD Delay, mono, 30 ms | 26.5% | 4.9% (4.1–6.0) | 7.7% |
| Tape Loop, stereo, default | 55.0% | 3.1% (2.6–3.8) | 6.3% |
| Tube, stereo, 12AX7, 2x | 49.3% | 3.0% (2.5–3.7) | 6.0% |
| Tube, mono, 6L6, 2x | 28.8% | 5.2% (4.3–6.4) | 10.5% |
| Tube, stereo, 12AX7, 4x | 103.9% | 6.5% (5.4–7.9) | 13.0% |
| Tube, mono, 6L6, 4x (its dearest) | 58.8% | 11.0% (9.1–13.4) | 21.9% |
| Cell Filter, stereo, default | 21.3% | 1.1% (0.9–1.3) | 2.1% |
| Cell Filter, stereo, V/oct + LFO patched | 21.3% | 1.8% (1.5–2.2) | 3.6% |
| Lorenz | 1.4% | 0.4% (0.3–0.4) | 0.7% |
| Flock | – | 0.2% (0.2–0.2) | 0.4% |
| Scale String, stereo, one string | 17.0% | 2.1% (1.7–2.5) | 4.2% |
| Scale String, stereo, poly 1.0 (seven strings) | 139.1% | 6.7% (5.5–8.1) | 13.8% |
| Scale String, poly 1.0, V/oct moving, fade | 111.1% | 8.9% (7.3–10.8) | 17.8% |
| Tube, mono, after the input stops | 25.9% | 0.1% (0.1–0.1) | 0.2% |
| Scale String, mono, after the strings die away | 35.6% | 0.3% (0.3–0.4) | 0.6% |

| Stock unit, for comparison (same method) | now |
|---|---:|
| Sine Osc | 0.9% (0.8–1.2) |
| Ladder LPF, stereo | 1.3% (1.1–1.6) |
| EQ3, stereo | 2.3% (1.9–2.8) |
| Grain Delay, stereo | 3.0% (2.5–3.6) |
| Freeverb, stereo | 2.4% (2.0–2.9) |

| Several units in one frame | total | worst frame |
|---|---:|---:|
| four mono channels, 14 units: Sine Osc > Ladder LPF > Tube > PT2399; Aliasing Saw > Cell Filter > Tape Loop; Scale String (poly 0.5) > Delay; Freeverb > Limiter, with Lorenz, ADSR and a VCA | 23.7% (19.6–28.8) | 24.2% |
| the same four channels with stock units only (14 units) | 12.1% (10.1–14.8) | 13.3% |
| two stereo chains: Cell Filter > Tube > Tape Loop > Freeverb; Sine Osc > Scale String (poly 0.5) > PT2399 > Limiter, plus Lorenz | 20.5% (16.9–24.9) | 20.6% |
| two stereo chains, stock only: Ladder LPF > EQ3 > Delay > Freeverb; Sine Osc > Grain Delay > Doppler Delay > Limiter, plus a triangle LFO | 13.5% (11.2–16.5) | 15.0% |
| seven Chloaelis units at once, stereo, defaults (all but Flock) | 15.4% (12.8–18.8) | 15.5% |
| Tube 6L6 4x + PT2399 at 30 ms + Scale String poly 1.0, stereo | 27.5% (22.8–33.5) | 27.7% |

How it was measured ([bench/README.md](bench/README.md) has everything): each unit is compiled
with the module's compiler and flags, built the way its Lua file builds it (DSP object, a
parameter adapter and sub-chain per fader), and run instruction by instruction in an emulated
Cortex-A8 on programme material; every instruction is charged what ARM's Cortex-A8 manual says
it costs. The model was checked against 18 published readings from real modules (stock units
and objects from the firmware source, four reverbs from stolmine's habitat project) and scaled
to match them on average; 16 of the 18 then fall within -18% and +21% of the model, which is
the bracketed range. Nothing was measured on hardware. The firmware reports dropouts from about
80%.

The "several units" rows run every object of every unit back to back in one frame, so caches
and branch prediction are shared as on the module.

## Layout

    mods/chloaelis/   ChloeSimd.h                two-lane NEON helpers (scalar fallback for the tests)
                      ChloeDsp.h, ChloeLines.h,  the DSP, plain C++ (also used by the tests)
                      ChloeCores.h, ChloeString.h
                      Engine.h, Engine.cpp       plain-function boundary, compiled at -O3
                      Units.h, Graphics.h        ER-301 objects and displays, header-only
                      assets/                    Lua unit definitions and controls
    tests/            test.cpp + ref/ (offline DSP tests against a plain transcription of the
                      plug-in), emu/*.test (headless-emulator tests)
    bench/            the CPU benchmark (Cortex-A8 model, scenarios, calibration)
    tools/            fetch-firmware.sh, build helpers for macOS, check-package.py
    docs/             the manual and its generator

Unit and display classes are header-only on purpose: a package class derived from a firmware
class must keep its virtual functions inline or it can crash on the module (see the habitat
project's graphics authoring guide). No C-library sin/cos/tan is used, for the same reason.
`make check` (tools/check-package.py) looks over a module build for the three things that crash
a module without warning: stack use under each `process()` (the audio task's stack is 2 KB),
NEON loads and stores that promise an alignment, and imports the firmware does not export.

## Build for the module

The firmware and its packages are built with `gcc-arm-none-eabi-4_9-2015q3` in `~/ti` and SWIG
4.2.1 in `~/.local/swig-4.2.1` (see `er-301-release/scripts/am335x.mk`; the CI workflow
`.github/workflows/er301.yml` shows how to install both on Linux).

    tools/fetch-firmware.sh release                                 # the firmware headers, pinned
    make chloaelis ARCH=am335x SDKPATH=er-301-release PROFILE=release   # -> release/am335x/chloaelis-1.1.1.pkg
    tools/check-package.py release/am335x chloaelis
    make test                                                        # offline DSP tests (any C++ compiler)

The emulator package (below) must be built with Homebrew's SWIG (4.3 or later); a package built
with SWIG 4.2.1 does not load in the emulator.

## Build and test in the emulator (macOS)

Needs Homebrew `gcc sdl2 sdl2_ttf fftw swig zip`.

    tools/fetch-firmware.sh emu   # er-301/ (stolmine), with a small link fix for macOS
    tools/fw-make.sh emu          # the emulator
    tools/fw-make.sh core         # the core units it needs
    tools/pkg-make.sh chloaelis   # -> testing/darwin/chloaelis-1.1.1.pkg
    tools/emu-test.sh             # loads every unit in the headless emulator, takes screenshots
    python3 docs/make_manual.py   # the PDF (needs Pillow, Chrome, and a benchmark run)

To try the package by hand, copy `testing/darwin/*.pkg` to `~/.od/front/ER-301/packages` and
run `er-301/testing/darwin/emu/emu.elf` from inside `er-301`.

## CPU benchmark

    cd bench && make              # -> ../build/bench/current/RESULTS.md
    python3 tools/summary.py      # the table above

Needs the module compiler and `er-301-release` (above); see [bench/README.md](bench/README.md).

## Credits

Chloaelis and this port: sadnoise (built with Claude Code). ER-301 and its firmware: Orthogonal
Devices (Brian Clarkson), MIT licence; the firmware fork and the habitat project whose build
notes this port follows: stolmine. Unit conventions after tmfset's and yrn1's custom units.
The firmware is fetched at build time, not kept in this repository.
