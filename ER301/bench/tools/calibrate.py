#!/usr/bin/env python3
"""Compare the model with readings taken on real ER-301s and derive the calibration.

  calibrate.py RESULTS.json [--write]

Two families of readings:
  A. the firmware source's own comments (Brian Clarkson's on-module profiler output, in CPU
     cycles per 128-sample frame at 48 kHz: "[Sine Osc]: 0.9717% (26053 ticks, 373 Hz)"),
     for stock NEON-intrinsic code;
  B. stolmine's notes on the habitat "house" reverbs (percent of CPU on a stereo chain, read
     off the module's CPU meter), for scalar float / double code.
--write stores the scale and the residual spread in bench/calibration.json, which run.py
applies to every estimate.
"""
import json, math, os, sys

BUDGET = 1e9 * 128 / 48000
# scenario, measured cycles per frame, relative error of the reading itself, family, source
ANCHORS = [
    ('stock/sine-osc', 26053, 0.15, 'A', 'mods/core/objects/oscillators/SineOscillator.cpp:32 "[Sine Osc]: 0.9717% (26053 ticks, 373 Hz)"'),
    ('stock/saw-osc', 19644, 0.15, 'A', 'SawtoothOscillator.cpp:23 "[Aliasing Saw]: 0.7360% (19644 ticks, 374 Hz)"'),
    ('stock/triangle-osc', 19172, 0.15, 'A', 'TriangleOscillator.cpp:30 "[Aliasing Triangle]: 0.7107% (19172 ticks, 370 Hz)"'),
    ('stock/adsr', 27163, 0.15, 'A', 'env/ADSR.cpp:149 "[ADSR]: 1.0228% (27163 ticks, 376 Hz)"'),
    ('stock/ladder-lpf/stereo', 34388, 0.15, 'A', 'filters/StereoLadderFilter.cpp:48 "[Ladder Filter]: 1.2840% (34388 ticks, 373 Hz)"'),
    ('stock/eq3/mono', 28399, 0.15, 'A', 'filters/Equalizer3.cpp:61 "[EQ3Unit]: 1.0709% (28399 ticks, 377 Hz) @ 48kHz" (gain inputs)'),
    ('stock/object/clipper', 600, 0.3, 'a', 'mods/core/objects/Clipper.cpp:26 "600 ticks for 128 samples"'),
    ('stock/object/constant-gain', 600, 0.3, 'a', 'od/objects/math/ConstantGain.cpp:46 "600 ticks"'),
    ('stock/object/crossfade', 1400, 0.3, 'a', 'od/objects/mixing/CrossFade.cpp:20 "1400 ticks"'),
    ('stock/limiter/mono-cubic', 2200, 0.3, 'a', 'mods/core/objects/Limiter.cpp:105 "2200 ticks" (cubic)'),
    ('stock/limiter/mono-invsqrt', 7000, 0.3, 'a', 'Limiter.cpp:85 "7k ticks" (inverse square root)'),
    ('stock/object/scalar-tanh', 55000, 0.3, 's', 'Limiter.cpp:24 "tanh approximation (regular floating point) ... 55k ticks"'),
    ('stock/object/scalar-clipper', 9600, 0.3, 's', 'Clipper.cpp:46 "9600 ticks for 128 samples" (scalar version)'),
    ('calib/creamcoat', 0.14 * BUDGET, 0.15, 'B', 'habitat planning/creamcoat-port-plan.md: "about 14% on stereo with derez at 1"'),
    ('calib/kwoodroom-as-measured', 0.29 * BUDGET, 0.1, 'B', 'habitat planning/house-ports-optimization.md: "HEAVY: 29% stereo (hw)"'),
    ('calib/woodenbox-as-measured', 0.14 * BUDGET, 0.15, 'B', 'same file: "MEDIUM: 14% stereo (hw)"'),
    ('calib/verbity-as-measured', 0.175 * BUDGET, 0.2, 'B', 'same file: "15-20% stereo (hw)"'),
    ('calib/galactic', 0.275 * BUDGET, 0.15, 'B', 'same file: "25-30% stereo (hw) - heaviest port"'),
]
# a difference rather than a level: the float rework of kWoodRoom "dropped only ~1% CPU mono"
DELTAS = [('calib/kwoodroom-as-measured', 'calib/kwoodroom-float-retrofit', 0.02 * BUDGET,
           'house-ports-optimization.md OUTCOME: "kWoodRoom dropped only ~1% CPU mono" (2% of a stereo frame at most)')]


def main():
    path = sys.argv[1]
    runs = json.load(open(path))['runs']
    rows = []
    print('| scenario | measured cycles/frame | model FPSCR=0 | ratio | model RunFast on entry | ratio | source |')
    print('|---|---:|---:|---:|---:|---:|---|')
    for name, meas, err, fam, src in ANCHORS:
        a = runs.get(name + '@48000/fpscr0')
        b = runs.get(name + '@48000/runfast')
        if not a:
            continue
        ra = a['cycles_per_frame'] / meas
        rb = b['cycles_per_frame'] / meas if b else float('nan')
        rows.append((name, fam, ra, rb, err))
        print('| %s | %.0f | %.0f | %.2f | %s | %s | %s |' % (
            name, meas, a['cycles_per_frame'], ra, ('%.0f' % b['cycles_per_frame']) if b else '', ('%.2f' % rb) if b else '', src))
    print()
    for x, y, meas, src in DELTAS:
        for mode in ('fpscr0', 'runfast'):
            a, b = runs.get('%s@48000/%s' % (x, mode)), runs.get('%s@48000/%s' % (y, mode))
            if a and b:
                print('delta %s -> %s, %s: model %.0f cycles/frame (%.1f%% of a frame), hardware about %.0f (%s)' % (
                    x, y, mode, a['cycles_per_frame'] - b['cycles_per_frame'],
                    100 * (a['cycles_per_frame'] - b['cycles_per_frame']) / BUDGET, meas, src))
    print()

    def stats(sel, col):
        v = [math.log(r[col]) for r in rows if sel(r) and r[col] == r[col]]
        if not v:
            return None
        m = sum(v) / len(v)
        sd = (sum((x - m) ** 2 for x in v) / max(1, len(v) - 1)) ** 0.5
        return math.exp(m), math.exp(sd), len(v), math.exp(min(v)), math.exp(max(v))
    for label, sel in (('A  stock units (NEON intrinsics)', lambda r: r[1] == 'A'),
                       ('a  single stock objects', lambda r: r[1] == 'a'),
                       ('s  scalar loops from the firmware source', lambda r: r[1] == 's'),
                       ('B  habitat reverbs (scalar float/double)', lambda r: r[1] == 'B'),
                       ('all', lambda r: True)):
        for col, mode in ((2, 'FPSCR=0'), (3, 'RunFast on entry')):
            s = stats(sel, col)
            if s:
                print('%-42s %-17s model/measured: geometric mean %.2f, spread x%.2f, n=%d, range %.2f .. %.2f' % ((label, mode) + s))
    s = stats(lambda r: True, 2)
    if '--write' in sys.argv and s:
        gm, sd, n, lo, hi = s
        scaled = sorted(r[2] / gm for r in rows if r[2] == r[2])
        inner = scaled[1:-1] if len(scaled) >= 10 else scaled   # without the one furthest out on each side
        cal = {
            'scale': round(1.0 / gm, 4),
            # after scaling, model/reading runs from inner[0] to inner[-1] (all but the most
            # extreme reading on each side), so the truth for an estimate may lie between
            # estimate / inner[-1] and estimate / inner[0]
            'uncertainty_low': round(1.0 / inner[-1], 3),
            'uncertainty_high': round(1.0 / inner[0], 3),
            'worst_low': round(1.0 / scaled[-1], 3),
            'worst_high': round(1.0 / scaled[0], 3),
            'anchors': n,
            'note': 'scale = 1 / geometric mean of model/reading over all anchors, FPSCR=0 model; uncertainty = spread of the '
                    'anchors after scaling without the most extreme one on each side; worst = with them',
        }
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'calibration.json')
        json.dump(cal, open(out, 'w'), indent=1)
        print('\nwrote', os.path.normpath(out), cal)


if __name__ == '__main__':
    main()
