#!/usr/bin/env python3
"""Run the benchmark scenarios through a8sim and write the report.

  run.py run     --out DIR [--only REGEX] [--jobs 4] [--quick] [--no-runfast] [--no-96k]
  run.py report  --out DIR                      rebuild results.json / RESULTS.md from DIR/runs
  run.py compare BEFORE/results.json AFTER/results.json [--only REGEX]
  run.py hot     --out DIR SCENARIO [--sr 48000] [--fpscr 0|runfast] [--top 40]
                                                the hottest instructions of one scenario, with source lines

DIR holds bench.elf and bench.tab (made by the Makefile). Every scenario runs at 48 kHz with
FPSCR = 0 on entry (what the firmware appears to give the audio thread); the "chloaelis"
group also at 96 kHz; everything again with RunFast set on entry unless --no-runfast.
"""
import argparse, json, os, re, subprocess, sys, concurrent.futures

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SIM = os.path.join(ROOT, 'build', 'bench', 'a8sim')
TI = os.path.expanduser(os.environ.get('TI', '~/ti/gcc-arm-none-eabi-4_9-2015q3'))
CAL_FILE = os.path.join(HERE, '..', 'calibration.json')


def calibration():
    """The scale applied to modelled cycles (1.0 until calibration.json says otherwise)."""
    try:
        return json.load(open(CAL_FILE))
    except Exception:
        return {'scale': 1.0, 'uncertainty_low': 1.0, 'uncertainty_high': 1.0}


def scenarios(out):
    txt = subprocess.run([SIM, '--elf', out + '/bench.elf', '--tab', out + '/bench.tab', '--list'],
                         check=True, capture_output=True, text=True).stdout
    res = []
    for i, line in enumerate(txt.strip().split('\n')):
        name, group = line.split('\t')
        res.append((i, name, group))
    return res


def key(name, sr, fpscr):
    return '%s@%d/%s' % (name, sr, 'runfast' if fpscr == 'runfast' else 'fpscr0')


def run_one(out, idx, name, sr, fpscr, warmup, frames):
    path = os.path.join(out, 'runs', re.sub(r'[^A-Za-z0-9_.-]', '_', key(name, sr, fpscr)) + '.json')
    cmd = [SIM, '--elf', out + '/bench.elf', '--tab', out + '/bench.tab', '--scenario', str(idx), '--sr', str(sr),
           '--fpscr', fpscr, '--warmup', str(warmup), '--frames', str(frames), '--json', path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return key(name, sr, fpscr), None, p.stderr.strip()
    return key(name, sr, fpscr), path, ''


def cmd_run(a):
    out = os.path.abspath(a.out)
    os.makedirs(out + '/runs', exist_ok=True)
    scn = scenarios(out)
    if a.only:
        rx = re.compile(a.only)
        scn = [s for s in scn if rx.search(s[1])]
    warmup, frames = (48, 32) if a.quick else (a.warmup, a.frames)
    jobs = []
    for idx, name, group in scn:
        modes = ['0'] + ([] if a.no_runfast else ['runfast'])
        rates = [48000] + ([96000] if (group == 'chloaelis' and not a.no_96k) else [])
        for sr in rates:
            for m in modes:
                # at 96 kHz the same wall-clock warm-up takes twice the frames
                jobs.append((out, idx, name, sr, m, warmup * (2 if sr == 96000 else 1), frames))
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for k, path, err in ex.map(lambda j: run_one(*j), jobs):
            if path is None:
                failed += 1
                print('FAILED %s: %s' % (k, err), file=sys.stderr)
            elif not a.quiet:
                d = json.load(open(path))
                print('%-64s %9.0f cycles  %6.2f%%' % (k, d['cycles_per_frame'], d['percent']))
    cmd_report(a)
    return 1 if failed else 0


def load_runs(out):
    res = {}
    rd = os.path.join(out, 'runs')
    for f in sorted(os.listdir(rd)):
        if f.endswith('.json'):
            d = json.load(open(os.path.join(rd, f)))
            fp = 'runfast' if d['fpscr_entry'] == '0x03000000' else '0'
            res[key(d['name'], d['sample_rate'], fp)] = d
    return res


def units_of(d):
    """Sum the objects of a run by unit (the part of the object name before the slash)."""
    units = {}
    for o in d['objects']:
        u = o['name'].split('/')[0]
        units.setdefault(u, 0.0)
        units[u] += o['cycles']
    return units


def pct(c, budget):
    return 100.0 * c / budget


def cmd_report(a):
    out = os.path.abspath(a.out)
    runs = load_runs(out)
    cal = calibration()
    sc, ulo, uhi = cal.get('scale', 1.0), cal.get('uncertainty_low', 1.0), cal.get('uncertainty_high', 1.0)
    for k, d in runs.items():
        b = d['budget_cycles']
        d['estimate'] = {
            'cycles': d['cycles_per_frame'] * sc,
            'percent': pct(d['cycles_per_frame'] * sc, b),
            # low / high: the spread of the hardware readings around the calibrated model
            # (calibration.json). The run's own cycles_per_frame_low/high (the TRM's ranges for
            # the VFP coprocessor and the assumed range of an L2 miss) are narrower and kept
            # in the run for reference.
            'percent_low': pct(d['cycles_per_frame'] * sc * ulo, b),
            'percent_high': pct(d['cycles_per_frame'] * sc * uhi, b),
            'percent_worst_frame': pct(d['cycles_worst_frame'] * sc, b),
        }
    json.dump({'calibration': cal, 'runs': runs}, open(out + '/results.json', 'w'), indent=1)

    L = []
    L.append('# Benchmark results\n')
    L.append('Modelled Cortex-A8 cycles per 128-sample frame; budget %d cycles at 48 kHz (1 GHz core).' % 2666667)
    L.append('Calibration scale applied to the "est." columns: x%.3f, uncertainty x%.2f .. x%.2f.\n' % (sc, ulo, uhi))
    groups = {}
    for k, d in runs.items():
        groups.setdefault(d['group'], []).append((k, d))
    for g in sorted(groups):
        L.append('## %s\n' % g)
        L.append('| scenario | rate | insns/frame | cycles/frame FPSCR=0 | est. % (low .. high) | worst frame % | cycles/frame RunFast on entry | est. % RunFast | stack B | out peak |')
        L.append('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
        names = sorted({(d['name'], d['sample_rate']) for _, d in groups[g]}, key=lambda t: (t[1], t[0]))
        order = {}
        for k, d in groups[g]:
            order.setdefault((d['name'], d['sample_rate']), d['scenario'])
        names.sort(key=lambda t: (t[1], order[t]))
        for name, sr in names:
            a0 = runs.get(key(name, sr, '0'))
            a1 = runs.get(key(name, sr, 'runfast'))
            if not a0:
                continue
            e = a0['estimate']
            rf = ('%.0f | %.2f' % (a1['cycles_per_frame'], a1['estimate']['percent'])) if a1 else ' | '
            L.append('| %s | %dk | %.0f | %.0f | %.2f (%.2f .. %.2f) | %.2f | %s | %d | %.3f |' % (
                name, sr // 1000, a0['insns_per_frame'], a0['cycles_per_frame'], e['percent'], e['percent_low'], e['percent_high'],
                e['percent_worst_frame'], rf, a0['deepest_stack_bytes'], max(a0['out_peak'])))
        L.append('')
    # where the cycles go, FPSCR=0, 48 kHz
    L.append('## Where the cycles go (48 kHz, FPSCR = 0 on entry), cycles per frame\n')
    cols = ['integer', 'vfp_lite', 'vfp_runfast', 'neon', 'neon_to_arm', 'mispredict', 'icache_miss', 'dcache_miss']
    L.append('| scenario | ' + ' | '.join(cols) + ' | scalar FP ops |')
    L.append('|---|' + '---:|' * (len(cols) + 1))
    for k in sorted(runs, key=lambda k: (runs[k]['group'], runs[k]['scenario'])):
        d = runs[k]
        if d['sample_rate'] != 48000 or d['fpscr_entry'] != '0x00000000':
            continue
        L.append('| %s | ' % d['name'] + ' | '.join('%.0f' % d['classes'][c] for c in cols) +
                 ' | %.0f |' % (d['scalar_fp_ops_vfp_lite'] + d['scalar_fp_ops_runfast']))
    L.append('')
    # multi-unit scenarios: share per unit
    multi = [d for d in runs.values() if len(units_of(d)) > 1 and d['sample_rate'] == 48000 and d['fpscr_entry'] == '0x00000000']
    if multi:
        L.append('## Several units in one frame (48 kHz, FPSCR = 0 on entry)\n')
        for d in sorted(multi, key=lambda d: (d['group'], d['scenario'])):
            e = d['estimate']
            L.append('### %s: %.2f%% (%.2f .. %.2f), worst frame %.2f%%\n' % (d['name'], e['percent'], e['percent_low'], e['percent_high'], e['percent_worst_frame']))
            L.append('| unit | cycles/frame | est. % |')
            L.append('|---|---:|---:|')
            for u, c in units_of(d).items():
                L.append('| %s | %.0f | %.2f |' % (u, c, pct(c * sc, d['budget_cycles'])))
            L.append('')
    # audio-thread rules
    bad = [(d['name'], d['forbidden_calls']) for d in runs.values() if d['forbidden_calls']]
    L.append('## Audio-thread checks\n')
    deep = max(runs.values(), key=lambda d: d['deepest_stack_bytes']) if runs else None
    if deep:
        L.append('Deepest stack use inside one object\'s process(): %d bytes (%s).' % (deep['deepest_stack_bytes'], deep['name']))
    if bad:
        seen = set()
        L.append('Allocation / lock calls made from process():')
        for n, calls in bad:
            for c in calls:
                t = (c['object'], c['function'])
                if t not in seen:
                    seen.add(t)
                    L.append('- %s: %s calls %s' % (n, c['object'], c['function']))
    else:
        L.append('No allocation, free, operator new/delete, static-guard or heap call was made from any process().')
    L.append('')
    open(out + '/RESULTS.md', 'w').write('\n'.join(L))
    print('wrote %s/results.json and RESULTS.md (%d runs)' % (out, len(runs)))
    return 0


def cmd_compare(a):
    A = json.load(open(a.before))['runs']
    B = json.load(open(a.after))['runs']
    rx = re.compile(a.only) if a.only else None
    print('| scenario | before cycles | before % | after cycles | after % | change |')
    print('|---|---:|---:|---:|---:|---:|')
    for k in sorted(set(A) | set(B)):
        if rx and not rx.search(k):
            continue
        x, y = A.get(k), B.get(k)
        f = lambda d: ('%.0f | %.2f' % (d['cycles_per_frame'], d['estimate']['percent'])) if d else '- | -'
        ch = ('%+.1f%%' % (100.0 * (y['cycles_per_frame'] / x['cycles_per_frame'] - 1.0))) if (x and y) else ''
        print('| %s | %s | %s | %s |' % (k, f(x), f(y), ch))
    return 0


def cmd_hot(a):
    out = os.path.abspath(a.out)
    scn = [s for s in scenarios(out) if s[1] == a.scenario]
    if not scn:
        sys.exit('no scenario called %s' % a.scenario)
    hot = out + '/hot.txt'
    subprocess.run([SIM, '--elf', out + '/bench.elf', '--tab', out + '/bench.tab', '--scenario', str(scn[0][0]), '--sr', str(a.sr),
                    '--fpscr', a.fpscr, '--warmup', str(a.warmup), '--frames', str(a.frames), '--json', out + '/hot.json', '--hot', hot], check=True)
    rows = []
    for line in open(hot):
        addr, cyc, cnt, cls = line.split()
        rows.append((float(cyc), float(cnt), int(addr, 16)))
    total = sum(r[0] for r in rows)
    # by source line
    addrs = ['%x' % r[2] for r in rows]
    al = subprocess.run([TI + '/bin/arm-none-eabi-addr2line', '-e', out + '/bench.elf', '-f', '-C', '-i', '-a'] + addrs,
                        capture_output=True, text=True).stdout.split('\n')
    # addr2line -a -f -i prints: 0xaddr / func / file:line [/ func / file:line ...inlined-by]
    where, cur = {}, None
    i = 0
    while i < len(al):
        if al[i].startswith('0x'):
            cur = int(al[i], 16)
            where[cur] = []
            i += 1
        elif cur is not None and i + 1 < len(al):
            where[cur].append((al[i], al[i + 1]))
            i += 2
        else:
            i += 1
    lines = {}
    for cyc, cnt, addr in rows:
        w = where.get(addr) or [('?', '?')]
        func, loc = w[0]
        loc = '/'.join(loc.split('/')[-2:])
        loc = re.sub(r' \(discriminator \d+\)', '', loc)
        outer = w[-1][0] if len(w) > 1 else ''
        k = (loc, func, outer)
        t = lines.setdefault(k, [0.0, 0.0])
        t[0] += cyc
        t[1] += cnt
    print('%s @ %d Hz, FPSCR %s: %.0f cycles/frame\n' % (a.scenario, a.sr, a.fpscr, total))
    print('%10s %7s %9s  %s' % ('cycles', '%', 'insns', 'source line (innermost function) [in]'))
    for (loc, func, outer), (cyc, cnt) in sorted(lines.items(), key=lambda kv: -kv[1][0])[:a.top]:
        print('%10.0f %6.1f%% %9.0f  %s (%s)%s' % (cyc, 100 * cyc / total, cnt, loc, func[:60], (' [' + outer[:40] + ']') if outer else ''))
    if a.insns:
        dis = subprocess.run([TI + '/bin/arm-none-eabi-objdump', '-d', out + '/bench.elf'], capture_output=True, text=True).stdout
        text = {}
        for l in dis.split('\n'):
            m = re.match(r'^([0-9a-f]{8}):\s+[0-9a-f]{8}\s+(.*)$', l)
            if m:
                text[int(m.group(1), 16)] = m.group(2).replace('\t', ' ')
        print('\n%10s %9s %7s  %s' % ('cycles', 'execs', 'cyc/ex', 'instruction'))
        for cyc, cnt, addr in sorted(rows, reverse=True)[:a.top]:
            print('%10.0f %9.0f %7.1f  %08x  %s' % (cyc, cnt, cyc / cnt if cnt else 0, addr, text.get(addr, '?')))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--out', required=True)
    r.add_argument('--only')
    r.add_argument('--jobs', type=int, default=4)
    r.add_argument('--warmup', type=int, default=144)
    r.add_argument('--frames', type=int, default=96)
    r.add_argument('--quick', action='store_true')
    r.add_argument('--quiet', action='store_true')
    r.add_argument('--no-runfast', action='store_true')
    r.add_argument('--no-96k', action='store_true')
    p = sub.add_parser('report')
    p.add_argument('--out', required=True)
    c = sub.add_parser('compare')
    c.add_argument('before')
    c.add_argument('after')
    c.add_argument('--only')
    h = sub.add_parser('hot')
    h.add_argument('--out', required=True)
    h.add_argument('scenario')
    h.add_argument('--sr', type=int, default=48000)
    h.add_argument('--fpscr', default='0')
    h.add_argument('--warmup', type=int, default=144)
    h.add_argument('--frames', type=int, default=32)
    h.add_argument('--top', type=int, default=40)
    h.add_argument('--insns', action='store_true', help='also list the hottest single instructions')
    a = ap.parse_args()
    sys.exit({'run': cmd_run, 'report': cmd_report, 'compare': cmd_compare, 'hot': cmd_hot}[a.cmd](a))


if __name__ == '__main__':
    main()
