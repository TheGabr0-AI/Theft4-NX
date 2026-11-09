#!/usr/bin/env python3
"""Offline test of run 64 (devlog section AL): the geometry store's validation by write signal; the mixer's routing
function as native code.

    python tests/deko3d/test_run64.py

SIGNALS / RELEASE / BENCH: dk_run64_store.cpp (see its header), Visual Studio clang, -Wall -Wextra -Werror: every range
  bound from the store holds the bytes the draw is given with the title writing its buffers at any moment (through a
  Lock with its signal, while locked, through a submit copy, through a second object, and with no Lock at all between
  frames); the signals save hashes; a change without a signal is counted when there is one and only then; a released
  object's memory reused in the same frame; the lookup's cost with the signals off and on in run 63's shape.
EVIDENCE (run 63's log, out/console-runs/run63-logs): the numbers the change rests on: the store's lookups, the hits
  trusted within an epoch, the bytes hashed, the content changes; the recorder's time per draw; and the read of the
  overwrite tracker (no clear, placement, hand-off or resolve depth clear on the characters' surfaces).
ROUTES / SPEED: dk_run64_routes.cpp: tools/switch-game/nx_audio_routes.h against the generated sub_8216D7E0 and
  sub_8216DBB0 themselves, extracted here from the local gta4_recomp.2.cpp (game-derived, never in the repository):
  6,000 random mix objects; guest memory, r1, r3, the flush mode and the calls of the guest's other functions equal.
AUDIO (run 63's log and ELF): tools/switch-game/guest_profile.py: the mixer thread's samples by guest function;
  sub_8216D7E0 holds the largest share; the stacks name the callees behind the two indirect calls.
WIRING: the recorder passes each range's buffer object unless the title held it locked, turns guest-write unlocks and
  releases into write signals, the front end records the locks, the cvar reaches the store.
CONFIG: out/console-runs/run64-build/game.toml (when it exists).
Also re-runs test_run53.py and test_run52.py (the store's older tests). Skipped parts are reported, not passed.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
NATIVE = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK = os.path.join(NATIVE, 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')


def probe_test(failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('SIGNALS skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, 'store.exe')
        bat = os.path.join(work, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Werror -I "{DK}" '
                    f'-I "{NATIVE}" -I "{os.path.join(SDK, "thirdparty", "xxHash")}" -DXXH_INLINE_ALL '
                    f'"{os.path.join(HERE, "dk_run64_store.cpp")}" "{os.path.join(DK, "dk_geometry_store.cpp")}" '
                    f'-o "{exe}"\r\n')
        p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                           capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:] + p.stderr[-4000:])
            failures.append('SIGNALS: dk_run64_store.cpp did not compile')
            return
        run = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    out = run.stdout
    sys.stdout.write(out)
    m = re.search(r'RUN64 failures (\d+)', out)
    if run.returncode != 0 or not m or m[1] != '0':
        failures.append('SIGNALS: a store check failed')
    off = re.search(r'BENCH signals off .*hashed_bytes_per_lookup (\d+) ns_per_lookup ([\d.]+)', out)
    on = re.search(r'BENCH signals on .*hashed_bytes_per_lookup (\d+) ns_per_lookup ([\d.]+)', out)
    if off and on:
        print(f'BENCH (this PC): {float(off[2]):.0f} -> {float(on[2]):.0f} ns a lookup '
              f'({100 * (1 - float(on[2]) / float(off[2])):.0f} % less), {off[1]} -> {on[1]} bytes hashed a lookup')
    else:
        failures.append('BENCH: no result')


def evidence_test(failures):
    path = os.path.join(RUNS, 'run63-logs', 'game.log')
    if not os.path.isfile(path):
        failures.append('EVIDENCE skipped: no run 63 log')
        return
    log = open(path, encoding='utf-8', errors='replace').read()
    store = re.findall(r'run 52 geometry store \(totals\): lookups (\d+), hits (\d+) .*?changed (\d+), .*?hashed (\d+) MiB'
                       r'.*?trusted in their epoch, run 53\) (\d+)', log)
    times = re.findall(r'recorder time per drawn draw in the interval \((\d+) draws\): ([\d.]+) us = textures ([\d.]+) \+ '
                       r'vertices ([\d.]+) \+ indices ([\d.]+) \+ index range scan ([\d.]+)', log)
    after = re.findall(r"run 63 after the characters' draws.*?: clears \d+/(\d+), EDRAM placements materialized \d+/(\d+), "
                       r'depth hand-offs \d+/(\d+), resolve depth clears \d+/(\d+), draws not testing depth \d+/(\d+), '
                       r'draws testing depth \d+/(\d+); surfaces resolved after them (\d+); character draws on a depth '
                       r'surface the scenery never uses (\d+)', log)
    events = re.findall(r'run 63 overwrite: (.+?) on ([0-9A-F]{8}) at frame \d+ \(draw \d+, vertex shader ([0-9A-F]{16})\)',
                        log)
    state = re.search(r'depth test 0 func 7 write 0, stencil \d func \d ref \w+ mask \w+ write mask \w+ ops fail/zfail/pass '
                      r'\d/\d/\d two-sided \d, z-fail volume false, colour write ([0-9A-F]+), .*?vertex shader '
                      r'2299F2D447D96CC6 pixel shader A11692B372C58D98', log)
    if not (store and times and after):
        failures.append('EVIDENCE: run 63 lines missing')
        return
    lookups, hits, changed, hashed_mib, trusted = (int(v) for v in store[-1])
    hashed = lookups - trusted
    draws = sum(int(t[0]) for t in times)
    vertices = sum(int(t[0]) * float(t[3]) for t in times) / draws
    scan = sum(int(t[0]) * float(t[5]) for t in times) / draws
    total = sum(int(t[0]) * float(t[1]) for t in times) / draws
    print(f'EVIDENCE run 63 store: lookups {lookups}, trusted in their epoch {trusted} ({100 * trusted / lookups:.0f} %), '
          f'hashed lookups {hashed}, {hashed_mib} MiB hashed ({hashed_mib * 1048576 // hashed} bytes each), content '
          f'changes {changed}')
    print(f'EVIDENCE run 63 recorder: {total:.1f} us a drawn draw over {draws} draws, vertices {vertices:.1f}, index '
          f'range scan {scan:.1f}')
    a = [int(v) for v in after[-1]]
    kinds = sorted({(e[0], e[1], e[2]) for e in events})
    print(f'EVIDENCE run 63 tracker: clears {a[0]}, placements {a[1]}, depth hand-offs {a[2]}, resolve depth clears '
          f'{a[3]}, draws not testing depth {a[4]} over {a[6]} resolved surfaces, draws testing depth {a[5]}, characters '
          f'on a depth of their own {a[7]}; events: {kinds}; that draw\'s colour write mask '
          f'{state[1] if state else "(not logged)"}')
    if changed > 2000 or trusted * 2 > lookups or hashed_mib < 100000:
        failures.append('EVIDENCE: run 63\'s store numbers are not the ones the change rests on')
    if any(a[:4]) or a[7]:
        failures.append('EVIDENCE: run 63 counted a clear, a placement, a hand-off or an own depth surface')
    if len(kinds) != 1 or kinds[0][2] != '2299F2D447D96CC6' or not state or state[1] != '800':
        failures.append('EVIDENCE: the draws without a depth test are not the one-channel pass')


def routes_test(failures):
    generated = os.path.join(SDK, 'gta4-recomp', 'generated', 'eu-base')
    source = os.path.join(generated, 'gta4_recomp.2.cpp')
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not os.path.isfile(source):
        failures.append('ROUTES skipped: no generated code (glue/rexglue-sdk-main/gta4-recomp/generated/eu-base)')
        return
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('ROUTES skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    text = open(source, encoding='utf-8').read()
    bodies = ''
    for name in ('sub_8216DBB0', 'sub_8216D7E0'):
        m = re.search(r'^DEFINE_REX_FUNC\(' + name + r'\) \{\n.*?^\}\n', text, re.S | re.M)
        if not m:
            failures.append(f'ROUTES: {name} not found in the generated code')
            return
        bodies += m[0] + '\n'
    # The routing function must be reached through the function table only (the strong definition replaces the weak
    # alias there), and call the voice loop by its overridable name (run 51's patch).
    direct = len(re.findall(r'\bsub_8216D7E0\(ctx, base\)', text))
    voice = bodies.count('sub_8216DBB0 /* run 51')
    print(f'ROUTES generated code: {direct} direct calls of sub_8216D7E0 in gta4_recomp.2.cpp, {voice} calls of the '
          f'voice loop by its overridable name')
    if direct or voice != 2:
        failures.append('ROUTES: the generated code does not reach the two functions as the override needs')
    with tempfile.TemporaryDirectory() as work:
        open(os.path.join(work, 'run64_generated.inc'), 'w', encoding='utf-8').write(bodies)
        exe = os.path.join(work, 'routes.exe')
        bat = os.path.join(work, 'b.bat')
        include = [work, generated, os.path.join(SDK, 'include'), os.path.join(SDK, 'thirdparty', 'simde'),
                   os.path.join(SDK, 'thirdparty', 'simde', 'simde'), os.path.join(ROOT, 'tools', 'switch-game')]
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul 2>nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++23 -O2 -mssse3 -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Werror '
                    f'-Wno-unused-parameter ' + ' '.join(f'-I "{i}"' for i in include) +
                    f' "{os.path.join(HERE, "dk_run64_routes.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                           capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:] + p.stderr[-4000:])
            failures.append('ROUTES: dk_run64_routes.cpp did not compile')
            return
        out = subprocess.run([exe], capture_output=True, text=True, timeout=900).stdout
    sys.stdout.write(out)
    m = re.search(r'ROUTES rounds (\d+) errors (\d+) .*with a table (\d+) \(overwrite (\d+), overlapping (\d+)\), '
                  r'without (\d+)', out)
    if not m or m[2] != '0' or int(m[1]) < 6000 or not all(int(m[k]) > 300 for k in (3, 4, 5, 6)):
        failures.append('ROUTES: the native routing function differs from the generated one')
    speed = re.findall(r'SPEED (\w+) generated_ns (\d+) native_ns (\d+)', out)
    if len(speed) != 2 or any(int(n) >= int(g) for _, g, n in speed):
        failures.append('SPEED: the native routing function is not faster on this PC')


def audio_test(failures):
    log = os.path.join(RUNS, 'run63-logs', 'game.log')
    elf = os.path.join(RUNS, 'run63-build', 'switch-game-dk-debug.elf')
    if not (os.path.isfile(log) and os.path.isfile(elf)):
        failures.append('AUDIO skipped: no run 63 log or ELF')
        return
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'switch-game'))
    import guest_profile
    if not guest_profile.addr2line_tool():
        failures.append('AUDIO skipped: no aarch64-none-elf-addr2line (devkitA64)')
        return
    total, logged, by, weight, chains = guest_profile.profile(log, elf, '16')
    top = by.most_common(6)
    print(f'AUDIO run 63 mixer thread: {total} samples, {logged} with a logged pc; ' +
          ', '.join(f'{name} {100 * n / total:.1f} %' for name, n in top))
    behind = {'sub_821737C0': set(), 'sub_82185E38': set()}
    for chain in chains:
        names = [n.replace('__imp__', '') for n in chain.split(' < ')]
        for i in range(1, len(names)):
            if names[i] in behind and names[i - 1] != names[i]:
                behind[names[i]].add(names[i - 1])
    print('AUDIO callees in the stacks: ' +
          '; '.join(f'{k} -> {", ".join(sorted(v)) or "none"}' for k, v in behind.items()))
    text = open(log, encoding='utf-8', errors='replace').read()
    audio = [(int(a), int(b)) for a, b in
             re.findall(r'audio: \d+ frames in the window \((\d+)% of real time\), output ran dry (\d+) times', text)]
    play = [a for a in audio if a[0] < 90]
    if play:
        print(f'AUDIO run 63 windows below 90 % of real time: {len(play)} of {len(audio)}, {min(p[0] for p in play)}-'
              f'{max(p[0] for p in play)} %, output dry {sum(p[1] for p in play) / len(play):.0f} times a window')
    if not top or top[0][0] != 'sub_8216D7E0' or top[0][1] < 0.15 * total:
        failures.append("AUDIO: sub_8216D7E0 is not the mixer thread's largest function")
    if 'sub_828C6740' not in behind['sub_821737C0'] or 'sub_82188C28' not in behind['sub_82185E38']:
        failures.append('AUDIO: the stacks do not name the callees behind the indirect calls')


def wiring_test(failures):
    recorder = open(os.path.join(DK, 'dk_recorder.cpp'), encoding='utf-8').read()
    front = open(os.path.join(NATIVE, 'native_front_end.cpp'), encoding='utf-8').read()
    main = open(os.path.join(ROOT, 'tools', 'switch-game', 'main.cpp'), encoding='utf-8').read()
    system = open(os.path.join(DK, 'dk_graphics_system.cpp'), encoding='utf-8').read()
    checks = {
        'the index range passes its buffer object unless locked':
            '(d.locked_buffers >> kVertexStreamCount) & 1u ? 0u : d.index_buffer' in recorder and
            'index_owner,' in recorder,
        'a vertex range passes its buffer object unless locked':
            '(d.locked_buffers >> s.stream) & 1u ? 0u : d.streams[s.stream].buffer' in recorder and
            'stream_owner, [&]' in recorder,
        'unlocks and releases are write signals': 'geometry_.Written(event.handle);' in recorder,
        'the front end records the locks': 'draw.locked_buffers |= uint32_t(locked[s]) << s;' in front,
        'the cvar reaches the store':
            'g.trust_signals = options_.geometry_signals;' in recorder and
            'ro.geometry_signals = options_.geometry_signals;' in system and
            'dk.geometry_signals = REXCVAR_GET(nx_dk_geometry_signals);' in main,
    }
    mixer = open(os.path.join(ROOT, 'tools', 'switch-game', 'nx_audio_mixer.cpp'), encoding='utf-8').read()
    checks['the native routing function replaces the weak alias, behind its cvar'] = (
        'extern "C" void sub_8216D7E0(PPCContext& ctx, uint8_t* base) {' in mixer and
        '!REXCVAR_GET(nx_audio_native_routes) || !REXCVAR_GET(nx_audio_neon_mixer)' in mixer and
        '__imp__sub_8216D7E0(ctx, base);' in mixer)
    checks['the audio line counts the native routing calls'] = 'theft4::nx_audio_mixer::RouteCalls()' in main
    for name, ok in checks.items():
        print(f'WIRING {name}: {"ok" if ok else "MISSING"}')
        if not ok:
            failures.append(f'WIRING: {name}')


def config_test(failures):
    toml = os.path.join(RUNS, 'run64-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run64-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_gpu': '"deko3d"', 'nx_dk_ab': 'false', 'nx_dk_geometry_signals': 'true',
            'nx_audio_native_routes': 'true', 'nx_profile': 'true'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')


def main():
    failures = []
    probe_test(failures)
    routes_test(failures)
    evidence_test(failures)
    audio_test(failures)
    wiring_test(failures)
    config_test(failures)
    for name in ('test_run53.py', 'test_run51.py'):
        r = subprocess.run([sys.executable, os.path.join(HERE, name)], capture_output=True, text=True)
        tail = (r.stdout.strip().splitlines() or ['?'])[-1]
        print(f'{name}: {tail}')
        if r.returncode != 0:
            failures.append(f'{name} failed')
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 64 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
