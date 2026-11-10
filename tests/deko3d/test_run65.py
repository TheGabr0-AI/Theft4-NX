#!/usr/bin/env python3
"""Offline test of run 65 (devlog section AM): run 64 read; the geometry store's trust across frames; the characters'
snapshot.

    python tests/deko3d/test_run65.py

FRAMES / RAW / BENCH: dk_run65_store.cpp (see its header), Visual Studio clang, -Wall -Wextra -Werror: with every write
  signalled no range is bound with other bytes than the draw's at trust_frames = 8 and fewer bytes are hashed; with
  writes that pass no Lock an old range lives less than 8 frames, its object goes back to a check a frame, and the
  eighth such change ends the trust across frames.
SWITCH (run 63's and run 64's logs): what run 64 measured on the console against run 63: the recorder's time per draw,
  the store's hashed share, the changes without a write signal, the audio windows; tools/switch-game/guest_profile.py
  on both (the mixer thread by guest function, with each run's ELF).
INDICES (run 60's probe files): every vertex of the probed character meshes was written by the GPU (no slot left at
  its fill value) and the draw's indices name every one of them: the GPU's index stream reached all of the mesh.
WIRING: the option reaches the store; the snapshot is taken last in the draw and again at the surface's resolve.
CONFIG: out/console-runs/run65-build/game.toml (when it exists).
Also re-runs test_run64.py. Skipped parts are reported, not passed.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
NATIVE = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK = os.path.join(NATIVE, 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'deko3d'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'switch-game'))


def probe_test(failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('FRAMES skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, 'store.exe')
        bat = os.path.join(work, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul 2>nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Werror -I "{DK}" '
                    f'-I "{NATIVE}" -I "{os.path.join(SDK, "thirdparty", "xxHash")}" -DXXH_INLINE_ALL '
                    f'"{os.path.join(HERE, "dk_run65_store.cpp")}" "{os.path.join(DK, "dk_geometry_store.cpp")}" '
                    f'-o "{exe}"\r\n')
        p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                           capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:] + p.stderr[-4000:])
            failures.append('FRAMES: dk_run65_store.cpp did not compile')
            return
        run = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    sys.stdout.write(run.stdout)
    m = re.search(r'RUN65 failures (\d+)', run.stdout)
    if run.returncode != 0 or not m or m[1] != '0':
        failures.append('FRAMES: a store check failed')


def read_run(name):
    path = os.path.join(RUNS, name + '-logs', 'game.log')
    if not os.path.isfile(path):
        return None
    log = open(path, encoding='utf-8', errors='replace').read()
    r = {}
    times = re.findall(r'recorder time per drawn draw in the interval \((\d+) draws\): ([\d.]+) us = textures ([\d.]+) \+ '
                       r'vertices ([\d.]+) \+ indices ([\d.]+) \+ index range scan ([\d.]+)', log)
    draws = sum(int(t[0]) for t in times)
    for key, k in (('total', 1), ('vertices', 3), ('scan', 5)):
        r[key] = sum(int(t[0]) * float(t[k]) for t in times) / max(draws, 1)
    r['draws'] = draws
    store = re.findall(r'run 52 geometry store \(totals\): lookups (\d+), hits (\d+) .*?changed (\d+), .*?hashed (\d+) MiB'
                       r'.*?trusted in their epoch, run 53\) (\d+)', log)
    r['lookups'], r['hits'], r['changed'], r['hashed_mib'], r['epoch'] = (int(v) for v in store[-1])
    signal = re.findall(r'run 64 geometry store write signals .*?: hits without a hash .*? (\d+), write signals .*? (\d+), '
                        r'content changes without a signal (\d+) of (\d+) changes', log)
    r['signal_hits'], r['signals'], r['unsignalled'] = (int(v) for v in signal[-1][:3]) if signal else (0, 0, None)
    audio = [(int(a), int(b)) for a, b in
             re.findall(r'audio: \d+ frames in the window \((\d+)% of real time\), output ran dry (\d+) times', log)]
    play = [a for a in audio if a[0] < 90]
    r['audio_pct'] = sum(p[0] for p in play) / max(len(play), 1)
    r['audio_dry'] = sum(p[1] for p in play) / max(len(play), 1)
    r['audio_windows'] = len(play)
    r['routing'] = [int(v) for v in re.findall(r'native routing calls (\d+)', log)][-1:] or [0]
    crash = os.path.join(RUNS, name + '-logs', 'game-crash.log')
    text = open(crash, encoding='utf-8', errors='replace').read() if os.path.isfile(crash) else ''
    r['faults'] = [int(v) for v in re.findall(r'fault ring: (\d+) faults', text)][-1:] or [-1]
    r['uptime'] = [int(v) // 1000 for v in re.findall(r'uptime ms (\d+)', text)][-1:] or [0]
    r['skipped'] = re.findall(r'drawn \d+ \(skipped by reason: ([^)]*)\)', log)[-1:]
    return r


def switch_test(failures):
    a, b = read_run('run63'), read_run('run64')
    if not (a and b):
        failures.append('SWITCH skipped: no run 63 or run 64 log')
        return
    for name, r in (('run 63', a), ('run 64', b)):
        hashed = r['lookups'] - r['epoch'] - r['signal_hits']
        print(f'SWITCH {name}: {r["uptime"][0]} s, {r["faults"][0]} faults, draws skipped: {(r["skipped"] or ["?"])[0]}; '
              f'recorder {r["total"]:.2f} us a draw (vertices {r["vertices"]:.2f}, index range scan {r["scan"]:.2f}) over '
              f'{r["draws"]} draws; store {r["lookups"]} lookups, hashed {hashed} ({100 * hashed / r["lookups"]:.0f} %), '
              f'{r["hashed_mib"] * 1048576 // r["lookups"]} bytes hashed a lookup, changes {r["changed"]}, without a signal '
              f'{r["unsignalled"]}; audio {r["audio_pct"]:.1f} % of real time and {r["audio_dry"]:.1f} dry a window over '
              f'{r["audio_windows"]} windows, native routing calls {r["routing"][0]}')
    if b['unsignalled'] != 0 or b['changed'] < 100:
        failures.append('SWITCH: run 64 did not count 0 changes without a signal among its changes')
    if b['faults'][0] != 0 or (b['skipped'] or [''])[0] != 'none':
        failures.append('SWITCH: run 64 faulted or skipped draws')
    if not (b['signal_hits'] > 0 and b['routing'][0] > 0):
        failures.append('SWITCH: run 64 did not use the write signals or the native routing function')
    try:
        import guest_profile
        if not guest_profile.addr2line_tool():
            raise RuntimeError('no addr2line')
        shares = {}
        for name in ('run63', 'run64'):
            elf = os.path.join(RUNS, name + '-build', 'switch-game-dk-debug.elf')
            total, logged, by, weight, chains = guest_profile.profile(os.path.join(RUNS, name + '-logs', 'game.log'), elf)
            shares[name] = {k.replace('__imp__', ''): 100 * v / total for k, v in by.items()}
            top = ', '.join(f'{k} {v:.1f} %' for k, v in sorted(shares[name].items(), key=lambda kv: -kv[1])[:6])
            print(f'SWITCH {name} mixer thread ({total} samples): {top}')
        before = shares['run63'].get('sub_8216D7E0', 0) + shares['run63'].get('sub_8216DBB0', 0)
        after = shares['run64'].get('sub_8216D7E0', 0) + shares['run64'].get('sub_8216DBB0', 0)
        print(f'SWITCH routing function and voice loop: {before:.1f} % -> {after:.1f} % of the mixer thread')
        if not (before > 25 and after < 15):
            failures.append('SWITCH: the routing function did not fall on the console')
    except Exception as e:  # noqa: BLE001
        failures.append(f'SWITCH profile skipped: {e}')


def indices_test(failures):
    folder = os.path.join(RUNS, 'run60-logs', 'probe')
    if not os.path.isdir(folder):
        failures.append('INDICES skipped: no run 60 probe files')
        return
    import probe_compare as pc
    meshes = unwritten = unnamed = 0
    for name in sorted(os.listdir(folder)):
        if not re.match(r'vs_[0-9A-F]{16}_', name):
            continue
        p = pc.read_probe(open(os.path.join(folder, name), 'rb').read())
        n = len(p.positions)
        if n < 200 or len(p.indices) < 600:
            continue
        named = {i for i in p.indices if i < n}
        meshes += 1
        unwritten += sum(1 for v in p.positions if all(c == 0xFFFFFFFF for c in v))
        unnamed += n - len(named)
    print(f'INDICES run 60 probes: {meshes} skinned meshes; vertices the GPU left unwritten {unwritten}; vertices the '
          f'draw\'s indices never name {unnamed}')
    if meshes < 15 or unwritten:
        failures.append('INDICES: a probed mesh has a vertex the GPU never reached')


def wiring_test(failures):
    recorder = open(os.path.join(DK, 'dk_recorder.cpp'), encoding='utf-8').read()
    system = open(os.path.join(DK, 'dk_graphics_system.cpp'), encoding='utf-8').read()
    main = open(os.path.join(ROOT, 'tools', 'switch-game', 'main.cpp'), encoding='utf-8').read()
    snapshot = recorder.find('"character-drawn-colour"')
    checks = {
        'trust_frames reaches the store':
            'g.trust_frames = std::max<uint32_t>(options_.geometry_trust_frames, 1);' in recorder and
            'ro.geometry_trust_frames = options_.geometry_trust_frames;' in system and
            'REXCVAR_GET(nx_dk_geometry_trust_frames)' in main,
        'the snapshot is the last thing a draw records':
            0 < recorder.find('++draws_drawn_;') < snapshot < recorder.find('// ---- Present and the output'),
        'the snapshot surfaces are saved again at their resolve':
            'snapshot_surfaces_.erase(c.source.handle)' in recorder and '"character-resolved-depth"' in recorder,
        'the snapshot has its cvar':
            'dk.character_snapshot = REXCVAR_GET(nx_dk_character_snapshot);' in main and
            'ro.character_snapshot = options_.character_snapshot;' in system,
    }
    for name, ok in checks.items():
        print(f'WIRING {name}: {"ok" if ok else "MISSING"}')
        if not ok:
            failures.append(f'WIRING: {name}')


def config_test(failures):
    toml = os.path.join(RUNS, 'run65-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run65-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_gpu': '"deko3d"', 'nx_dk_ab': 'false', 'nx_dk_geometry_signals': 'true',
            'nx_dk_geometry_trust_frames': '8', 'nx_audio_native_routes': 'true', 'nx_dk_character_snapshot': 'true',
            'nx_profile': 'true'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')


def main():
    failures = []
    probe_test(failures)
    switch_test(failures)
    indices_test(failures)
    wiring_test(failures)
    config_test(failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run64.py')], capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or ['?'])[-1]
    print(f'test_run64.py: {tail}')
    if r.returncode != 0:
        failures.append('test_run64.py failed')
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 65 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
