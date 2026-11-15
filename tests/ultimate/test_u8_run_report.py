#!/usr/bin/env python3
"""Regression test of tools/ultimate/u8_run_report.py's timing (the timing audit, 2026-11-15).

    python tests/ultimate/test_u8_run_report.py

The defects: (1) positions were the sum of the reports' printed lengths (0.1 s each) and the fps the printed, rounded
one; (2) an interval was selected by its END, so `--from-s 60` took in 50-60 s; (3) a run that stopped drawing at its
end wrote no more reports, and nothing said so; (4) nothing said how much of the time asked for the figures cover.
Synthetic logs in the renderer's and the profiler's formats; no console data.
"""
import datetime
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, 'tools', 'ultimate', 'u8_run_report.py')
sys.path.insert(0, os.path.dirname(TOOL))
import u8_run_report as rep  # noqa: E402

T0 = datetime.datetime(2026, 11, 14, 18, 0, 0)


def stamp(seconds):
    t = T0 + datetime.timedelta(seconds=seconds)
    return t.strftime('[%Y-%m-%d %H:%M:%S.') + f'{t.microsecond // 1000:03d}] [info] [core] [t12] '


def report(length, frames):
    """The renderer's line (banjo_renut_tuning.cpp): the length to 0.1 s, the fps to 0.01."""
    return (f'[renut] {length:.1f} s: {frames} game frames ({frames / length:.2f} fps), render thread busy 5.00 ms a '
            f'frame ({frames} frames), GPU 30.00 ms a frame ({frames} frames); {frames * 4000} draws, SharedConstants')


class Log:
    def __init__(self, stamped=True, first_frame_at=5.0):
        self.lines, self.stamped, self.now = [], stamped, 0.0
        self.line('start')
        self.now = first_frame_at

    def line(self, text):
        self.lines.append((stamp(self.now) if self.stamped else '[info] [core] [t12] ') + text)

    def interval(self, length, frames):
        self.now += length
        self.line(report(length, frames))
        return self

    def idle(self, seconds, text='[renut] something else'):
        self.now += seconds
        self.line(text)
        return self

    def write(self, folder, name='renut_001.log'):
        os.makedirs(os.path.join(folder, 'logs'), exist_ok=True)
        with open(os.path.join(folder, 'logs', name), 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.lines) + '\n')


def profile(folder, windows):
    """rex_profile.log (switch_perf.cpp): (seconds, game fps, mixed, missing) a window."""
    os.makedirs(os.path.join(folder, 'logs', 'rex'), exist_ok=True)
    with open(os.path.join(folder, 'logs', 'rex', 'rex_profile.log'), 'w', encoding='utf-8') as f:
        f.write('==== 10.0 s | mode: startup | game 0.0 fps | CPU total 100% (400% = 4 cores) | faults/s\n')
        for seconds, fps, mixed, missing in windows:
            f.write(f'==== {seconds:.1f} s | mode: game | game {fps:.1f} fps | CPU total 300% (400% = 4 cores) | x\n')
            f.write(f'audio: {mixed} client blocks mixed, {missing} requests without data\n')


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def summary(self, log=None, from_s=60.0, stall_s=2.0):
        if log is not None:
            log.write(self.dir)
        return rep.summary(self.dir, from_s, False, stall_s)


class Precision(Case):
    def test_lengths_from_the_stamps_not_the_rounded_print(self):
        log = Log()
        for _ in range(30):
            log.interval(10.04, 104)  # printed "10.0 s", "10.36 fps"
        out = self.summary(log, from_s=0.0)
        rows, info = rep.renut_series(os.path.join(self.dir, 'logs', 'renut_001.log'), [])
        self.assertTrue(info['stamped'])
        self.assertEqual(out['time_source'], 'log time stamps (ms)')
        # the first report has no stamp before it: its printed length; the 29 others: 10.04 s each
        self.assertAlmostEqual(rows[-1]['t'], 10.0 + 29 * 10.04, places=3)   # summed prints gave 300.0
        self.assertAlmostEqual(rows[5]['seconds'], 10.04, places=3)
        self.assertAlmostEqual(rows[5]['fps'], 104 / 10.04, places=3)
        self.assertEqual(out['frames'], 30 * 104)
        self.assertAlmostEqual(out['measured_s'], 10.0 + 29 * 10.04, places=3)
        self.assertAlmostEqual(out['fps_overall'], 30 * 104 / (10.0 + 29 * 10.04), places=4)

    def test_no_drift_over_a_long_run(self):
        log = Log()
        for _ in range(360):          # an hour: summed prints lose 0.04 s a report, 14.4 s in all
            log.interval(10.04, 100)
        rows, _ = rep.renut_series(self.write(log), [])
        self.assertAlmostEqual(rows[-1]['t'] - rows[0]['t'], 359 * 10.04, places=2)
        self.assertAlmostEqual(rows[-1]['start'], rows[-2]['t'], places=6)

    def write(self, log):
        log.write(self.dir)
        return os.path.join(self.dir, 'logs', 'renut_001.log')

    def test_without_stamps_the_printed_lengths_and_exact_frames(self):
        log = Log(stamped=False)
        for _ in range(12):
            log.interval(10.04, 103)  # printed 10.0 s and "10.26 fps"
        out = self.summary(log, from_s=0.0)
        self.assertEqual(out['time_source'], 'printed lengths (0.1 s)')
        self.assertAlmostEqual(out['measured_s'], 120.0, places=6)
        self.assertAlmostEqual(out['fps_overall'], 10.3, places=6)   # 1236 frames / 120.0 s, not a mean of 10.26
        self.assertIsNone(out['tail_s'])
        self.assertIsNone(out['end_stall_s'])
        self.assertIn('tail unknown', out['coverage_note'])

    def test_a_clock_step_does_not_pass_as_a_length(self):
        log = Log()
        log.interval(10.0, 100).interval(10.0, 100)
        log.now += 3600.0             # the console's clock set during the run
        log.interval(10.0, 100).interval(10.0, 100)
        rows, _ = rep.renut_series(self.write(log), [])
        self.assertEqual([round(r['seconds'], 3) for r in rows], [10.0, 10.0, 10.0, 10.0])
        self.assertAlmostEqual(rows[-1]['t'], 40.0, places=3)


class Selection(Case):
    def test_an_interval_that_only_ends_at_from_s_is_not_in(self):
        log = Log()
        for i in range(9):
            log.interval(10.0, 50 if i < 6 else 100)   # 0-60 s loading at 5 fps, then 10 fps
        out = self.summary(log, from_s=60.0)
        self.assertEqual(out['intervals'], 3)           # 60-70, 70-80, 80-90; the old rule took 50-60 too
        self.assertAlmostEqual(out['fps_overall'], 10.0, places=3)
        self.assertAlmostEqual(out['fps_median'], 10.0, places=3)
        self.assertAlmostEqual(out['measured_s'], 30.0, places=3)
        self.assertAlmostEqual(out['coverage'], 100.0, places=1)

    def test_profile_windows_by_their_start(self):
        Log().interval(10.0, 100).write(self.dir)
        profile(self.dir, [(10.0, 10.0, 0, 1000)] * 6 + [(10.0, 10.0, 1500, 375)] * 3)
        out = self.summary(from_s=60.0)
        self.assertAlmostEqual(out['audio_share'], 80.0, places=6)   # the 50-60 s window (0 %) is not in


class Stalls(Case):
    def steady(self, n=12):
        log = Log()
        for _ in range(n):
            log.interval(10.0, 100)
        return log

    def test_stall_at_the_end_of_the_log(self):
        log = self.steady()                       # reports to 120 s
        for _ in range(9):
            log.idle(5.0, '[audio] still alive')  # 45 s of other threads' lines, no frame
        out = self.summary(log, from_s=60.0)
        self.assertAlmostEqual(out['tail_s'], 45.0, places=3)
        self.assertAlmostEqual(out['end_stall_s'], 35.0, places=3)   # a frame end after 130 s would have reported
        self.assertAlmostEqual(out['fps_overall'], 10.0, places=3)   # the reports alone say all is well
        self.assertAlmostEqual(out['window_s'], 60.0 + 45.0, places=3)
        self.assertAlmostEqual(out['coverage'], 100.0 * 60.0 / 105.0, places=3)

    def test_the_tool_warns_about_it(self):
        log = self.steady()
        log.idle(45.0, '[audio] still alive')
        log.write(self.dir)
        p = subprocess.run([sys.executable, TOOL, self.dir], capture_output=True, text=True, check=True)
        self.assertIn('WARNING', p.stdout)
        self.assertIn('no frame ended for at least 35.0 s', p.stdout)
        self.assertIn('STALL at the end of the log', p.stdout)

    def test_a_short_tail_is_unmeasured_time_not_a_stall(self):
        log = self.steady()
        log.idle(6.0)
        out = self.summary(log, from_s=60.0)
        self.assertAlmostEqual(out['tail_s'], 6.0, places=3)
        self.assertEqual(out['end_stall_s'], 0.0)
        self.assertAlmostEqual(out['coverage'], 100.0 * 60.0 / 66.0, places=3)
        log.write(self.dir)
        p = subprocess.run([sys.executable, TOOL, self.dir], capture_output=True, text=True, check=True)
        self.assertNotIn('WARNING', p.stdout)

    def test_a_tail_just_under_the_threshold(self):
        log = self.steady()
        log.idle(11.9)
        self.assertEqual(self.summary(log)['end_stall_s'], 0.0)
        log = self.steady()
        log.idle(12.0)
        self.assertAlmostEqual(self.summary(log)['end_stall_s'], 2.0, places=3)

    def test_a_stalled_frame_inside_a_report(self):
        log = self.steady(8)
        log.interval(37.2, 95)                    # its last frame took 27.2 s or more
        log.interval(10.0, 100)
        out = self.summary(log, from_s=60.0)
        self.assertEqual(out['stalls'], 1)
        self.assertAlmostEqual(out['stall_s'], 27.2, places=3)
        self.assertAlmostEqual(out['fps_overall'], (200 + 95 + 100) / (20.0 + 37.2 + 10.0), places=3)
        self.assertEqual(out['end_stall_s'], 0.0)

    def test_a_log_with_no_report_at_all(self):
        log = Log()
        for _ in range(16):
            log.idle(5.0, '[audio] still alive')  # 85 s of log, never a report
        out = self.summary(log, from_s=60.0)
        self.assertEqual(out['intervals'], 0)
        self.assertEqual(out['time_source'], 'no fps report')
        self.assertAlmostEqual(out['end_stall_s'], 75.0, places=3)
        self.assertEqual(out['coverage'], 0.0)

    def test_the_profile_says_it_where_the_log_has_no_stamps(self):
        log = Log(stamped=False)
        for _ in range(12):
            log.interval(10.0, 100)
        log.write(self.dir)
        profile(self.dir, [(10.0, 10.0, 1500, 375)] * 12 + [(10.0, 0.0, 0, 1875)] * 4)
        out = self.summary(from_s=60.0)
        self.assertIsNone(out['end_stall_s'])
        self.assertAlmostEqual(out['profile_end_stall_s'], 40.0, places=6)
        p = subprocess.run([sys.executable, TOOL, self.dir], capture_output=True, text=True, check=True)
        self.assertIn("the profile's last 40 s are at 0.0 game fps", p.stdout)

    def test_a_profile_that_ends_running(self):
        Log().interval(10.0, 100).write(self.dir)
        profile(self.dir, [(10.0, 0.0, 0, 1875)] * 2 + [(10.0, 10.0, 1500, 375)] * 3)
        self.assertEqual(self.summary()['profile_end_stall_s'], 0.0)


if __name__ == '__main__':
    unittest.main(verbosity=1)
