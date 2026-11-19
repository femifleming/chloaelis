#!/usr/bin/env python3
"""The short CPU tables of the README and the manual, from the benchmark's results.json files.

    python3 bench/tools/summary.py [--html]    (reads build/bench/current and build/bench/baseline)
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCH = os.path.join(ROOT, "build", "bench")

# (label, scenario) — mono units unless said; 48 kHz, FPSCR = 0 on entry (as the firmware runs)
UNITS = [
    ("PT2399 Delay, mono, default (342 ms)", "chloe/pt2399/mono/default"),
    ("PT2399 Delay, stereo, default", "chloe/pt2399/stereo/default"),
    ("PT2399 Delay, mono, 30 ms (its dearest)", "chloe/pt2399/mono/30ms"),
    ("BBD Delay, stereo, default", "chloe/bbd/stereo/default"),
    ("BBD Delay, mono, 30 ms", "chloe/bbd/mono/30ms"),
    ("Tape Loop, stereo, default", "chloe/tape/stereo/default"),
    ("Tube, stereo, 12AX7, 2x", "chloe/tube/stereo/12ax7-2x"),
    ("Tube, mono, 6L6, 2x", "chloe/tube/mono/6l6-2x"),
    ("Tube, stereo, 12AX7, 4x", "chloe/tube/stereo/12ax7-4x"),
    ("Tube, mono, 6L6, 4x (its dearest)", "chloe/tube/mono/6l6-4x"),
    ("Cell Filter, stereo, default", "chloe/filter/stereo/default"),
    ("Cell Filter, stereo, V/oct + LFO patched", "chloe/filter/stereo/voct-lfo"),
    ("Lorenz", "chloe/lorenz/mono/default"),
    ("Flock", "chloe/flock/mono/default"),
    ("Scale String, stereo, one string", "chloe/string/stereo/default"),
    ("Scale String, stereo, poly 1.0 (seven strings)", "chloe/string/stereo/poly-1.0"),
    ("Scale String, poly 1.0, V/oct moving, fade", "chloe/string/mono/poly-1.0-voct-lfo-fade"),
    ("Tube, mono, after the input stops", "chloe/tube/mono/after-silence"),
    ("Scale String, mono, after the strings die away", "chloe/string/mono/after-silence"),
]
STOCK = [
    ("Sine Osc", "stock/sine-osc"),
    ("Ladder LPF, stereo", "stock/ladder-lpf/stereo"),
    ("EQ3, stereo", "stock/eq3/stereo"),
    ("Grain Delay, stereo", "stock/grain-delay/stereo"),
    ("Freeverb, stereo", "stock/freeverb/stereo"),
]
CONFIGS = [
    ("four mono channels, 14 units: Sine Osc > Ladder LPF > Tube > PT2399; Aliasing Saw > Cell Filter > Tape Loop; "
     "Scale String (poly 0.5) > Delay; Freeverb > Limiter, with Lorenz, ADSR and a VCA", "config/four-channels-mixed"),
    ("the same four channels with stock units only (14 units)", "config/four-channels-stock-only"),
    ("two stereo chains: Cell Filter > Tube > Tape Loop > Freeverb; Sine Osc > Scale String (poly 0.5) > PT2399 > "
     "Limiter, plus Lorenz", "config/two-stereo-chains"),
    ("two stereo chains, stock only: Ladder LPF > EQ3 > Delay > Freeverb; Sine Osc > Grain Delay > Doppler Delay > Limiter, plus a triangle LFO", "config/two-stereo-chains-stock-only"),
    ("seven Chloaelis units at once, stereo, defaults (all but Flock)", "config/all-chloaelis-stereo"),
    ("Tube 6L6 4x + PT2399 at 30 ms + Scale String poly 1.0, stereo", "config/chloaelis-heavy-corners-stereo"),
]


def load(which):
    p = os.path.join(BENCH, which, "results.json")
    if not os.path.exists(p):
        return {}
    return json.load(open(p))["runs"]


def est(runs, scen, sr=48000):
    r = runs.get(f"{scen}@{sr}/fpscr0")
    return r["estimate"] if r else None


def pct(e, rng=True):
    if e is None:
        return "–"
    s = f"{e['percent']:.1f}%"
    return s + (f" ({e['percent_low']:.1f}–{e['percent_high']:.1f})" if rng else "")


def rows():
    cur, base = load("current"), load("baseline")
    units = [(l, pct(est(base, s), False), pct(est(cur, s)), pct(est(cur, s, 96000), False)) for l, s in UNITS]
    stock = [(l, pct(est(cur, s))) for l, s in STOCK]
    configs = []
    for l, s in CONFIGS:
        e = est(cur, s)
        w = f"{e['percent_worst_frame']:.1f}%" if e else "–"
        configs.append((l, pct(e), w))
    return units, stock, configs


def markdown():
    units, stock, configs = rows()
    out = ["| Unit | 1.0.0 | now | now at 96 kHz |", "|---|---:|---:|---:|"]
    out += [f"| {a} | {b} | {c} | {d} |" for a, b, c, d in units]
    out += ["", "| Stock unit, for comparison (same method) | now |", "|---|---:|"]
    out += [f"| {a} | {b} |" for a, b in stock]
    out += ["", "| Several units in one frame | total | worst frame |", "|---|---:|---:|"]
    out += [f"| {a} | {b} | {c} |" for a, b, c in configs]
    return "\n".join(out)


def html():
    units, stock, configs = rows()
    t = lambda head, rs: ("<table class='cpu'><tr>" + "".join(f"<th>{h}</th>" for h in head) + "</tr>" +
                          "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rs) + "</table>")
    return (t(("Unit", "1.0.0", "now", "96 kHz"), units) + t(("Stock unit (same method)", "now"), stock) +
            t(("Several units in one frame", "total", "worst frame"), configs))


if __name__ == "__main__":
    print(html() if "--html" in sys.argv else markdown())
