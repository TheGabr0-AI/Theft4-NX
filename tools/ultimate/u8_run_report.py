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

Times (the timing audit, 2026-11-15). A [renut] report is written at the end of the first frame that ends 10 s or more
after the last one, with its length printed to 0.1 s and its frame count exact:

- lengths and positions come from the log lines' own time stamps (ms) where the file has them and they agree with the
  printed length; the printed lengths alone otherwise, never summed rounded fps. The fps of an interval is its frames
  over its length, and "fps, frames / seconds" is the whole selection's;
- an interval is in the selection when it STARTS at --from-s or later (one that only ends there holds earlier time);
- a report longer than 10 s + --stall-s (default 2) held a frame that took at least the excess: a stall inside it;
- the time between the last report and the end of the log is in no report. 10 s + --stall-s or more of it means no
  frame ended for at least (that time - 10 s): a stall at the end of the log, which no fps figure shows. The profile's
  trailing windows at 0.0 game fps say the same where the renut log has no time stamps;
- coverage: the selected intervals' seconds over the time from --from-s to the end of the log (the tail included when
  the log has time stamps; "tail unknown" when it has none).

The audio's share: the output asks the game for one 256-sample frame every 5.333 ms (187.5 Hz,
switch_audio_system.cpp); "requests without data" are frames the game had not made in time, played as silence. The
game's audio clock advances one frame per frame made, so the share is also the speed its sound plays at.
"""
import argparse
import datetime
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


# The file sink's default pattern (rex/logging/types.h file_pattern): "[2026-11-14 18:03:22.417] [info] ...".
STAMP_RE = re.compile(r'^\[(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]')
REPORT_S = 10.0  # banjo_renut_tuning.cpp: a report at the first frame end 10 s or more after the last


def stamp_seconds(line):
    """The line's time stamp in seconds (ms resolution), or None."""
    m = STAMP_RE.match(line)
    if not m:
        return None
    y, mo, d, h, mi, sec, ms = (int(g) for g in m.groups())
    # as UTC: only differences are used, and a local zone would put a daylight-saving step into them
    return datetime.datetime(y, mo, d, h, mi, sec, ms * 1000, tzinfo=datetime.timezone.utc).timestamp()


def renut_series(path, old_audio):
    """The [renut] reports as intervals, and what the log says about the time outside them.

    Each row: start and t (its end), seconds since the run's first frame; seconds (its length); frames (exact); fps
    (frames / seconds). info: reports, stamped (every report line had a time stamp), log_span_s (first to last stamped
    line), head_s (log start to the first interval's start), tail_s (last report to the last stamped line), or None
    for what the log cannot say."""
    rows = []
    first_stamp = last_stamp = None
    t = 0.0
    prev_stamp = None      # the previous report line's
    anchor = None          # (stamp, t) of the first stamped report: later positions are stamp differences from it
    all_stamped = True
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    for line in lines:
        stamp = stamp_seconds(line)
        if stamp is not None:
            if first_stamp is None:
                first_stamp = stamp
            if last_stamp is None or stamp > last_stamp:
                last_stamp = stamp
        m = FPS_RE.search(line)
        if m:
            printed = float(m[1])  # to 0.1 s
            frames = int(m[2])
            seconds = printed
            # The stamps of two reports in a row are a report apart (the stamp is taken when the line is logged, right
            # after the interval's end): the precise length when it agrees with the printed one (half a printed digit
            # and 50 ms for the logging itself), so a clock set during the run or a missing line cannot pass as one.
            if stamp is not None and prev_stamp is not None and abs((stamp - prev_stamp) - printed) <= 0.1:
                seconds = stamp - prev_stamp
            start = t
            if stamp is not None and anchor is not None and abs((anchor[1] + stamp - anchor[0]) - (t + seconds)) <= 0.5:
                t = anchor[1] + stamp - anchor[0]  # no drift from the summed lengths
            else:
                t = t + seconds
                anchor = (stamp, t) if stamp is not None else None
            if stamp is None:
                all_stamped = False
            prev_stamp = stamp
            rows.append({'start': start, 't': t, 'seconds': seconds, 'printed_s': printed, 'frames': frames,
                         'fps': frames / seconds if seconds > 0 else float('nan'), 'printed_fps': float(m[3]),
                         'rt': float(m[4]), 'gpu': float(m[5]), 'gpu_frames': int(m[6]),
                         'draws': int(m[7]) / max(1, frames), 'stamp': stamp})
            continue
        m = OLD_AUDIO_RE.search(line)
        if m:
            old_audio.append((t, int(m[2]), int(m[3])))
            continue
        m = UAM_RE.search(line)
        if m and rows and 'compiles' not in rows[-1]:
            rows[-1].update(compiles=int(m[1]), blocking=int(m[3]), blocking_ms=int(m[4]), hits=int(m[5]),
                            added=int(m[6]))
    info = {'reports': len(rows), 'stamped': bool(rows) and all_stamped, 'log_span_s': None, 'head_s': None,
            'tail_s': None}
    if first_stamp is not None:
        info['log_span_s'] = last_stamp - first_stamp
        last_report = next((r['stamp'] for r in reversed(rows) if r['stamp'] is not None), None)
        if rows and rows[-1]['stamp'] is not None:
            info['tail_s'] = max(0.0, last_stamp - last_report)
        elif not rows:
            info['tail_s'] = info['log_span_s']  # no report at all: the whole log is outside them
        if rows and rows[0]['stamp'] is not None:
            info['head_s'] = max(0.0, rows[0]['stamp'] - rows[0]['seconds'] - first_stamp)
    return rows, info


def timing_audit(rows, info, from_s, stall_s):
    """What the selection covers and where time went missing: a dict of the summary's timing keys."""
    eps = 1e-6
    late = [r for r in rows if r['start'] >= from_s - eps]
    out = {'intervals': len(late)}
    measured = sum(r['seconds'] for r in late)
    frames = sum(r['frames'] for r in late)
    if late:
        out.update(measured_s=measured, frames=frames, fps_overall=frames / measured if measured > 0 else float('nan'))
    # A stall inside a report: its last frame took at least the excess over the report period.
    long_rows = [r for r in late if r['seconds'] - REPORT_S >= stall_s]
    out['stalls'] = len(long_rows)
    out['stall_s'] = sum(r['seconds'] - REPORT_S for r in long_rows)
    tail = info.get('tail_s')
    out['tail_s'] = tail
    end_stall = 0.0
    if tail is not None and tail - REPORT_S >= stall_s:
        end_stall = tail - REPORT_S  # no frame ended for at least this long, or a report would have been written
    out['end_stall_s'] = end_stall if tail is not None else None
    # The window asked for: from --from-s (on the reports' clock) to the end of the log.
    end = rows[-1]['t'] if rows else 0.0
    window = max(0.0, end - from_s) + (tail or 0.0)
    if not rows and tail is not None:
        window = max(0.0, tail - from_s)
    out['window_s'] = window
    out['coverage'] = 100.0 * measured / window if window > eps else float('nan')
    out['coverage_note'] = '' if tail is not None else ' (tail unknown: no time stamps)'
    return late, out


def profile_series(path):
    rows, t = [], 0.0
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    for line in lines:
        m = PROF_HEAD_RE.match(line)
        if m:
            if m[2] == 'startup':
                continue
            start = t
            t += float(m[1])
            cur = {'start': start, 't': t, 'seconds': float(m[1]), 'fps': float(m[3]), 'cpu': int(m[4]),
                   'threads': []}
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


def profile_trailing_stall(rows):
    """The seconds of the profile's last windows in a row at 0.0 game fps (printed to 0.1: under half a frame in the
    window): the game presented nothing while the profiler still ran."""
    seconds = 0.0
    for r in reversed(rows):
        if r['fps'] != 0.0:
            break
        seconds += r['seconds']
    return seconds


def pct(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float('nan')


def summary(folder, from_s, series, stall_s=2.0):
    renut = find(folder, [r'renut_\d+\.log', r'game\.log'])
    prof = find(folder, [r'rex_profile\.log'])
    out = {'folder': os.path.basename(os.path.normpath(folder))}
    old_audio = []
    if renut:
        rows, info = renut_series(renut, old_audio)
        if series:
            print(f'== {renut}')
            print('   start_s     end_s  length_s frames    fps  rt_ms gpu_ms draws/f compiles blocked blocked_ms')
            for r in rows:
                print(f"{r['start']:10.3f} {r['t']:9.3f} {r['seconds']:9.3f} {r['frames']:6d} {r['fps']:6.2f} "
                      f"{r['rt']:6.1f} {r['gpu']:6.1f} {r['draws']:7.0f} "
                      f"{r.get('compiles', ''):>8} {r.get('blocking', ''):>7} {r.get('blocking_ms', ''):>10}"
                      + ('  STALL >= {:.1f} s'.format(r['seconds'] - REPORT_S)
                         if r['seconds'] - REPORT_S >= stall_s else ''))
        late, audit = timing_audit(rows, info, from_s, stall_s)
        out.update(audit)
        out['time_source'] = ('log time stamps (ms)' if info['stamped'] else
                              'printed lengths (0.1 s)' if rows else 'no fps report')
        if late:
            out.update(fps_median=statistics.median(r['fps'] for r in late), fps_p10=pct([r['fps'] for r in late], 0.1),
                       rt_median=statistics.median(r['rt'] for r in late),
                       gpu_median=statistics.median(r['gpu'] for r in late),
                       draws_median=statistics.median(r['draws'] for r in late))
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
            print('   end_s    fps  cpu% mixed missing audio%   clocks       top threads')
            for r in rows:
                asked = r.get('mixed', 0) + r.get('missing', 0)
                share = 100.0 * r.get('mixed', 0) / asked if asked else float('nan')
                clock = '{:.0f}/{:.1f}'.format(*r['clock']) if 'clock' in r else ''
                top = ' '.join(f'{n[:15]}={c:.0f}' for n, c in r['threads'][:4])
                print(f"{r['t']:8.1f} {r['fps']:6.1f} {r['cpu']:4d} {r.get('mixed', 0):5d} {r.get('missing', 0):7d} "
                      f"{share:6.1f} {clock:>12}  {top}")
        out['profile_end_stall_s'] = profile_trailing_stall(rows)
        late = [r for r in rows if r['start'] >= from_s - 1e-6 and 'mixed' in r]
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


KEYS = [('time_source', 'interval lengths from', '{}'),
        ('intervals', 'intervals starting at --from-s or later', '{:.0f}'),
        ('measured_s', 'seconds in those intervals', '{:.3f}'), ('frames', 'game frames in them', '{}'),
        ('window_s', 'seconds from --from-s to the log end', '{:.3f}'),
        ('coverage', 'coverage, % of that time measured', '{:.1f}'),
        ('tail_s', 'seconds after the last report', '{:.3f}'),
        ('end_stall_s', 'STALL at the end of the log, s (at least)', '{:.3f}'),
        ('profile_end_stall_s', 'profile: trailing seconds at 0.0 fps', '{:.1f}'),
        ('stalls', 'reports holding a stalled frame', '{}'),
        ('stall_s', 'seconds stalled in them (at least)', '{:.3f}'),
        ('fps_overall', 'fps, frames / seconds', '{:.3f}'), ('fps_median', 'fps, median of the intervals', '{:.2f}'),
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
    ap.add_argument('--stall-s', type=float, default=2.0,
                    help='a frame, or the end of the log, this much longer than the 10 s report period is a stall')
    a = ap.parse_args()
    runs = [summary(a.run, a.from_s, a.series, a.stall_s)]
    if a.baseline:
        runs.insert(0, summary(a.baseline, a.from_s, a.series, a.stall_s))
    print('== summary from {:.0f} s on'.format(a.from_s))
    print(f"{'':42}" + ''.join(f'{r["folder"][:24]:>26}' for r in runs))
    for key, label, fmt in KEYS:
        cells = []
        for r in runs:
            v = r.get(key)
            cell = '-' if v is None else fmt.format(v)
            if key == 'coverage' and v is not None:
                cell += r.get('coverage_note', '')
            cells.append(cell)
        if key == 'clocks':
            for r, c in zip(runs, cells):
                print(f'{label} [{r["folder"]}]: {c}')
        else:
            print(f'{label:42}' + ''.join(f'{c:>26}' for c in cells))
    for r in runs:
        end = r.get('end_stall_s') or 0.0
        prof_end = r.get('profile_end_stall_s') or 0.0
        if end or prof_end:
            print(f'WARNING [{r["folder"]}]: the run ends in a stall the fps figures do not hold: '
                  + (f'no frame ended for at least {end:.1f} s before the last log line' if end else
                     f'the profile\'s last {prof_end:.0f} s are at 0.0 game fps'))
        if r.get('stalls'):
            print(f'WARNING [{r["folder"]}]: {r["stalls"]} report(s) hold a frame of {a.stall_s:.1f} s or more '
                  f'({r["stall_s"]:.1f} s in all, at least)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
