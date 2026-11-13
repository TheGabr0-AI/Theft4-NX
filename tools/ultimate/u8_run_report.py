#!/usr/bin/env python3
"""Theft4-NX (docs/switch-port/20, U8): the numbers of a console run on the Ultimate Base, from its SD logs, and the
comparison with another run.

    python tools/ultimate/u8_run_report.py <run folder> [--baseline <run folder>] [--from-s N] [--series]

<run folder> holds what the SD gave (any layout): logs/renut_001.log (or the newest renut_*.log), logs/rex/
rex_profile.log, deko3d.log. Reads only; prints:

- per 10 s (--series): game fps, render thread and GPU ms a frame, draws a frame, uam compiles and the time draws waited
  for them (renut log); CPU total, the audio blocks mixed and missing, clocks (rex_profile.log);
- a summary over the intervals from --from-s seconds on (default 60: past the start-up and the logos): median and 10th
  percentile of the fps, medians of the rest, the audio's share of real time (blocks mixed / blocks asked for), the
  compiles and the waits for them, every CPU / GPU clock pair the profile read and how often.

The audio's share: the output asks the game for one 256-sample frame every 5.333 ms (187.5 Hz,
switch_audio_system.cpp); "requests without data" are frames the game had not made in time, played as silence. The
game's audio clock advances one frame per frame made, so the share is also the speed its sound plays at.
"""
import argparse
import os
import re
import statistics
import sys

FPS_RE = re.compile(r'\[renut\] ([\d.]+) s: (\d+) game frames \(([\d.]+) fps\), render '
                    r'thread busy ([\d.]+) ms a frame \(\d+ frames\), GPU ([\d.]+) ms a frame \((\d+) frames\); (\d+) draws')
# The old tree's game.log (runs 70-72, thirdparty/renut): the same [renut] lines, and its own audio line every 10 s.
OLD_AUDIO_RE = re.compile(r'\[game\] audio: (\d+) frames in the window \((\d+)% of real time\), output ran dry (\d+)')
UAM_RE = re.compile(r'uam (\d+) compiles \(avg (\d+) ms.*?blocking: (\d+) a draw waited for \((\d+) ms in all.*?'
                    r'DKSH cache (\d+) hits, (\d+) added')
PROF_HEAD_RE = re.compile(r'^==== ([\d.]+) s \| mode: (\w+) \| game ([\d.]+) fps \| CPU total (\d+)%')
AUDIO_RE = re.compile(r'audio: (\d+) client blocks mixed, (\d+) requests without data')
CLOCK_RE = re.compile(r'clocks: CPU ([\d.]+) MHz, GPU ([\d.]+) MHz, memory ([\d.]+) MHz')
THREAD_RE = re.compile(r'^-- thread (\d+) "([^"]*)": CPU ([\d.]+)%')


def find(folder, names):
    for root, _, files in os.walk(folder):
        for n in sorted(files, reverse=True):
            if any(re.fullmatch(p, n) for p in names):
                return os.path.join(root, n)
    return None


def renut_series(path, old_audio):
    rows, t = [], 0.0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = FPS_RE.search(line)
        if m:
            t += float(m[1])  # seconds since the first report (the reports are back to back)
            frames = int(m[2])
            rows.append({'t': t, 'fps': float(m[3]), 'rt': float(m[4]), 'gpu': float(m[5]),
                         'gpu_frames': int(m[6]), 'draws': int(m[7]) / max(1, frames)})
            continue
        m = OLD_AUDIO_RE.search(line)
        if m:
            old_audio.append((t, int(m[2]), int(m[3])))
            continue
        m = UAM_RE.search(line)
        if m and rows and 'compiles' not in rows[-1]:
            rows[-1].update(compiles=int(m[1]), blocking=int(m[3]), blocking_ms=int(m[4]), hits=int(m[5]),
                            added=int(m[6]))
    return rows


def profile_series(path):
    rows, t = [], 0.0
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = PROF_HEAD_RE.match(line)
        if m:
            if m[2] == 'startup':
                continue
            t += float(m[1])
            cur = {'t': t, 'fps': float(m[3]), 'cpu': int(m[4]), 'threads': []}
            rows.append(cur)
            continue
        if cur is None:
            continue
        m = AUDIO_RE.search(line)
        if m:
            cur['mixed'], cur['missing'] = int(m[1]), int(m[2])
        m = CLOCK_RE.search(line)
        if m:
            cur['clock'] = (float(m[1]), float(m[2]))
        m = THREAD_RE.match(line)
        if m and len(cur['threads']) < 6:
            cur['threads'].append((m[2], float(m[3])))
    return rows


def pct(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float('nan')


def summary(folder, from_s, series):
    renut = find(folder, [r'renut_\d+\.log', r'game\.log'])
    prof = find(folder, [r'rex_profile\.log'])
    out = {'folder': os.path.basename(os.path.normpath(folder))}
    old_audio = []
    if renut:
        rows = renut_series(renut, old_audio)
        if series:
            print(f'== {renut}')
            print('     s    fps  rt_ms gpu_ms draws/f compiles blocked blocked_ms')
            for r in rows:
                print(f"{r['t']:6.0f} {r['fps']:6.2f} {r['rt']:6.1f} {r['gpu']:6.1f} {r['draws']:7.0f} "
                      f"{r.get('compiles', ''):>8} {r.get('blocking', ''):>7} {r.get('blocking_ms', ''):>10}")
        late = [r for r in rows if r['t'] >= from_s]
        if late:
            out.update(fps_median=statistics.median(r['fps'] for r in late), fps_p10=pct([r['fps'] for r in late], 0.1),
                       rt_median=statistics.median(r['rt'] for r in late),
                       gpu_median=statistics.median(r['gpu'] for r in late),
                       draws_median=statistics.median(r['draws'] for r in late), intervals=len(late))
        last = next((r for r in reversed(rows) if 'compiles' in r), None)
        if last:
            out.update(compiles=last['compiles'], blocking=last['blocking'], blocking_s=last['blocking_ms'] / 1000.0,
                       cache_hits=last['hits'], cache_added=last['added'])
        late_audio = [a for a in old_audio if a[0] >= from_s]
        if late_audio and not prof:  # the old tree: its own percentage (frames made against real time)
            out.update(audio_share=statistics.mean(a[1] for a in late_audio),
                       audio_share_min=min(a[1] for a in late_audio))
    if prof:
        rows = profile_series(prof)
        if series:
            print(f'== {prof}')
            print('     s    fps  cpu% mixed missing audio%   clocks       top threads')
            for r in rows:
                asked = r.get('mixed', 0) + r.get('missing', 0)
                share = 100.0 * r.get('mixed', 0) / asked if asked else float('nan')
                clock = '{:.0f}/{:.1f}'.format(*r['clock']) if 'clock' in r else ''
                top = ' '.join(f'{n[:15]}={c:.0f}' for n, c in r['threads'][:4])
                print(f"{r['t']:6.0f} {r['fps']:6.1f} {r['cpu']:4d} {r.get('mixed', 0):5d} {r.get('missing', 0):7d} "
                      f"{share:6.1f} {clock:>12}  {top}")
        late = [r for r in rows if r['t'] >= from_s and 'mixed' in r]
        if late:
            mixed = sum(r['mixed'] for r in late)
            asked = sum(r['mixed'] + r['missing'] for r in late)
            out.update(cpu_median=statistics.median(r['cpu'] for r in late),
                       audio_share=100.0 * mixed / asked if asked else float('nan'),
                       audio_share_min=min(100.0 * r['mixed'] / max(1, r['mixed'] + r['missing']) for r in late))
        clocks = {}
        for r in rows:
            if 'clock' in r:
                clocks[r['clock']] = clocks.get(r['clock'], 0) + 1
        out['clocks'] = ', '.join(f'CPU {c:.0f} / GPU {g:.1f} MHz x{n}' for (c, g), n in sorted(clocks.items()))
    if not renut and not prof:
        sys.exit(f'u8_run_report: no renut_*.log, game.log or rex_profile.log under {folder}')
    return out


KEYS = [('intervals', 'intervals of 10 s from --from-s', '{:.0f}'), ('fps_median', 'fps, median', '{:.2f}'),
        ('fps_p10', 'fps, 10th percentile', '{:.2f}'), ('rt_median', 'render thread ms a frame, median', '{:.1f}'),
        ('gpu_median', 'GPU ms a frame, median', '{:.1f}'), ('draws_median', 'draws a frame, median', '{:.0f}'),
        ('cpu_median', 'CPU total % (of 400), median', '{:.0f}'),
        ('audio_share', 'audio, % of real time (mixed / asked)', '{:.1f}'),
        ('audio_share_min', 'audio, worst 10 s', '{:.1f}'), ('compiles', 'uam compiles in the run', '{}'),
        ('blocking', 'compiles a draw waited for', '{}'), ('blocking_s', 'seconds draws waited for compiles', '{:.1f}'),
        ('cache_hits', 'DKSH cache hits', '{}'), ('cache_added', 'DKSH cache records added', '{}'),
        ('clocks', 'clocks read (profile)', '{}')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--baseline', default=None)
    ap.add_argument('--from-s', type=float, default=60.0)
    ap.add_argument('--series', action='store_true')
    a = ap.parse_args()
    runs = [summary(a.run, a.from_s, a.series)]
    if a.baseline:
        runs.insert(0, summary(a.baseline, a.from_s, a.series))
    print('== summary from {:.0f} s on'.format(a.from_s))
    print(f"{'':42}" + ''.join(f'{r["folder"][:24]:>26}' for r in runs))
    for key, label, fmt in KEYS:
        cells = []
        for r in runs:
            v = r.get(key)
            cells.append('-' if v is None else fmt.format(v))
        if key == 'clocks':
            for r, c in zip(runs, cells):
                print(f'{label} [{r["folder"]}]: {c}')
        else:
            print(f'{label:42}' + ''.join(f'{c:>26}' for c in cells))
    return 0


if __name__ == '__main__':
    sys.exit(main())
