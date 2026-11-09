#!/usr/bin/env python3
"""Offline test of run 61 (devlog section AI): the game's shaders extracted offline, the package completed.

    python tests/deko3d/test_run61.py

XXH3 (dk_run61_xxh3.cpp, the repository's xxHash, clang): tools/deko3d/gta4_xbox_data.py's XXH3-64 equals the
  reference on 41 lengths covering every path (0-16, 17-128, 129-240, long inputs with one and several blocks).
AES: FIPS-197 AES-128 and AES-256 single blocks, SP 800-38A CBC-AES128 (two blocks, the chain carried).
CONTAINERS: find_containers on synthetic data: unaligned containers, two back to back, a truncated one and one whose
  virtual size is smaller than the header are reported with their reason, never dropped; XenosRecomp's own rule
  (field1C / field20) is named by gta4_fxc_shaders.xenosrecomp_verdict.
RPF2: a synthetic unencrypted archive (a directory, a stored file, a deflated file) parsed and read back.
MERGE: GDK1 caches written and read back by gta4_deko3d_merge.py.
RECORDER: dk_recorder.cpp names the missing shader at both rejection sites (run 61's line).
EVIDENCE (run 60's log): no draw skipped, the front end rejected none, the probe phase swapped the characters' shaders
  (9A16E1CEAD4ABA0D torso, A2D8E2F3F87A09AE head) and the package served every shader asked for.
REAL (only with THEFT4_GAME_DIR and THEFT4_AES_KEY; THEFT4_SHADER_DUMP and THEFT4_PACKAGE add their checks): the
  game's files give 89 .fxc files and the executable, 1,370 unique containers, 0 rejected; every runtime-dumped
  container is among them; the package lacks none. Game data stays in a temporary directory.
CONFIG: out/console-runs/run61-build/game.toml (when it exists).
Skipped parts are reported, not passed.
"""
import io
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(ROOT, 'tools', 'deko3d')
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
DK = os.path.join(SDK, 'src', 'graphics', 'gta4_native', 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')
sys.path.insert(0, TOOLS)
import gta4_xbox_data as xd  # noqa: E402
import gta4_fxc_shaders as fxc  # noqa: E402
import gta4_deko3d_merge as merge  # noqa: E402


def lcg_buffer(n):
    s = (0x12345678 ^ n) & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (s >> 16) & 0xFF
    return bytes(out)


def xxh3_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('XXH3 skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'xxh3.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O2 -Wall -Werror -DXXH_INLINE_ALL -I "{os.path.join(SDK, "thirdparty", "xxHash")}" '
                f'-I "{DK}" "{os.path.join(HERE, "dk_run61_xxh3.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('XXH3: the reference probe did not compile')
        return
    run = subprocess.run([exe], capture_output=True, text=True)
    ref = re.findall(r'XXH3 (\d+) ([0-9A-F]{16})', run.stdout)
    wrong = [(n, h) for n, h in ref if f'{xd.xxh3_64(lcg_buffer(int(n))):016X}' != h]
    print(f'XXH3 {len(ref)} lengths against the reference, {len(wrong)} wrong')
    if len(ref) != 41 or wrong:
        failures.append(f'XXH3: {wrong[:5]}')
    ab = [l for l in run.stdout.splitlines() if l.startswith('AB ')]
    sys.stdout.write(''.join(l + '\n' for l in ab))
    if run.returncode != 0 or len(ab) != 5 or any('FAIL' in l for l in ab):
        failures.append('AB: the run 61 phase schedule is wrong')


def aes_test(failures):
    checks = [
        (bytes(range(16)), '69c4e0d86a7b0430d8cdb78070b4c55a', '00112233445566778899aabbccddeeff'),
        (bytes(range(32)), '8ea2b7ca516745bfeafc49904b496089', '00112233445566778899aabbccddeeff'),
    ]
    bad = [k.hex()[:8] for k, c, p in checks if xd.AesDecryptor(k).decrypt_block(bytes.fromhex(c)).hex() != p]
    key = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')
    iv = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
    ct = bytes.fromhex('7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2')
    pt = '6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51'
    chain = [iv]
    got = xd.AesDecryptor(key).decrypt_cbc(ct[:16], chain) + xd.AesDecryptor(key).decrypt_cbc(ct[16:], chain)
    if got.hex() != pt:
        bad.append('cbc')
    print(f'AES FIPS-197 128/256 and SP 800-38A CBC: {"ok" if not bad else bad}')
    if bad:
        failures.append(f'AES: {bad}')


def container(magic, payload_words, physical=b''):
    body = struct.pack('>9I', magic, 0, 0, 36, 0, 0, 0, 0, 0) + payload_words
    v = len(body)
    return struct.pack('>I', magic) + struct.pack('>II', v, len(physical)) + body[12:] + physical


def containers_test(failures):
    a = container(0x102A1101, b'\x11' * 20, b'\x22' * 7)
    b = container(0x102A1100, b'\x33' * 40)
    bad_v = struct.pack('>III', 0x102A1100, 8, 0) + b'\0' * 30
    trunc = struct.pack('>III', 0x102A1101, 400, 100) + b'\0' * 50
    data = b'\x01\x02\x03' + a + b + b'\x09' * 5 + bad_v + trunc[:60]
    found = xd.find_containers(data)
    ok = [(o, s, blob) for o, s, blob, r in found if not r]
    rejected = [r for _, _, _, r in found if r]
    good = (len(ok) == 2 and ok[0][0] == 3 and ok[0][1] == 'vs' and ok[0][2] == a and ok[1][1] == 'ps'
            and ok[1][2] == b and len(rejected) == 2 and 'smaller than the header' in rejected[0]
            and 'past the end' in rejected[1])
    odd = bytearray(b)
    struct.pack_into('>I', odd, 0x1C, 5)
    verdict = fxc.xenosrecomp_verdict(bytes(odd), None)
    print(f'CONTAINERS: {len(ok)} found at 3 and {ok[1][0] if len(ok) > 1 else "?"}, rejected {rejected}; '
          f'field1C 5 -> {verdict!r}')
    if not good or not verdict or 'field1C' not in verdict or fxc.xenosrecomp_verdict(b, None) is not None:
        failures.append('CONTAINERS: wrong find or verdict')


def rpf_test(failures):
    stored = b'stored file bytes'
    plain = b'rgxa' + bytes(random.Random(7).getrandbits(8) for _ in range(3000)) + b'\x10\x2a\x11\x00'
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    deflated = comp.compress(plain) + comp.flush()
    names = b'\0shaders\0a.txt\0b.fxc\0'
    off_names = {'shaders': 1, 'a.txt': 9, 'b.fxc': 15}
    entries = 4
    toc = io.BytesIO()
    toc.write(struct.pack('<4I', 0, 0, 0x80000000 | 1, 1))                   # root: child 1
    toc.write(struct.pack('<4I', off_names['shaders'], 0, 0x80000000 | 2, 2))  # shaders: children 2-3
    data_at = 0x2000
    toc.write(struct.pack('<4I', off_names['a.txt'], len(stored), data_at, len(stored)))
    toc.write(struct.pack('<4I', off_names['b.fxc'], len(plain), data_at + 0x800, 0x40000000 | len(deflated)))
    toc.write(names)
    t = toc.getvalue()
    t += b'\0' * ((-len(t)) % 16)
    archive = bytearray(data_at + 0x800 + len(deflated))
    archive[0:20] = struct.pack('<5I', xd.RPF2_MAGIC, len(t), entries, 0, 0)
    archive[0x800:0x800 + len(t)] = t
    archive[data_at:data_at + len(stored)] = stored
    archive[data_at + 0x800:] = deflated
    try:
        r = xd.RpfArchive(bytes(archive), None, 'synthetic')
        files = {e.path: r.read(e) for e in r.files()}
        ok = files == {'shaders/a.txt': stored, 'shaders/b.fxc': plain}
    except Exception as e:  # noqa: BLE001
        ok, files = False, str(e)
    print(f'RPF2 synthetic: {"ok" if ok else files}')
    if not ok:
        failures.append('RPF2: synthetic archive misread')


def merge_test(work, failures):
    blobs = {5: b'a' * 300, 1: b'bb', 99: b'c' * 513}
    p = os.path.join(work, 'c.bin')
    merge.write_gdk1(p, b'tag', blobs)
    tag, back = merge.read_gdk1(p)
    ok = tag == b'tag' and back == blobs
    print(f'MERGE GDK1 round trip: {"ok" if ok else "WRONG"}')
    if not ok:
        failures.append('MERGE: GDK1 round trip')


def recorder_test(failures):
    text = open(os.path.join(DK, 'dk_recorder.cpp'), encoding='utf-8').read()
    vs = re.search(r'if \(!vs\) \{\s*ShaderMissing\(d\.vertex_shader, false\);\s*DrawRejected\(kSkipNoVertexShader', text)
    ps = re.search(r'if \(!early\) \{\s*ShaderMissing\(d\.pixel_shader, true\);\s*DrawRejected\(kSkipNoPixelShader', text)
    line = 'run 61 shader not in the package' in text
    print(f'RECORDER missing-shader line at the vertex site {bool(vs)}, the pixel site {bool(ps)}, text {line}')
    if not (vs and ps and line):
        failures.append('RECORDER: the missing shader is not named at both sites')


def evidence_test(failures):
    path = os.path.join(RUNS, 'run60-logs', 'game.log')
    if not os.path.isfile(path):
        failures.append('EVIDENCE skipped: no run 60 log')
        return
    log = open(path, encoding='utf-8', errors='replace').read()
    skipped = re.findall(r'drawn (\d+) \(skipped by reason: ([^)]*)\)', log)
    rejected = re.findall(r'deko3d: front end: commands \d+, draws (\d+),.*?rejected (\d+);', log)
    probe = re.findall(r'run 60 probe shaders \(totals\): skinned draws drawn with their probe\'s shader (\d+)', log)
    swapped = set(re.findall(r'probe shaders: vertex shader ([0-9A-F]{16}) variant', log))
    dksh = re.findall(r'shaders: (\d+) from the PC\'s DKSH, (\d+) compiled by the console\'s uam, (\d+) failed', log)
    if not (skipped and rejected and probe and dksh):
        failures.append('EVIDENCE: run 60 lines missing')
        return
    print(f'EVIDENCE run 60: {skipped[-1][0]} draws drawn, skipped by reason: {skipped[-1][1]}; front end rejected '
          f'{rejected[-1][1]} of {rejected[-1][0]}; probe-shader draws {probe[-1]}, shaders swapped {len(swapped)}; '
          f'DKSH from the package {dksh[-1][0]}, console compiles {dksh[-1][1]}, failed {dksh[-1][2]}')
    if skipped[-1][1] != 'none' or rejected[-1][1] != '0' or int(probe[-1]) == 0:
        failures.append('EVIDENCE: run 60 skipped or rejected draws, or swapped none')
    if not {'9A16E1CEAD4ABA0D', 'A2D8E2F3F87A09AE'} <= swapped:
        failures.append('EVIDENCE: the characters\' torso and head shaders were not swapped')
    if dksh[-1][2] != '0':
        failures.append('EVIDENCE: a shader failed')


def real_test(work, failures):
    game, key = os.environ.get('THEFT4_GAME_DIR', ''), os.environ.get('THEFT4_AES_KEY', '')
    if not (game and key and os.path.isfile(os.path.join(game, 'common.rpf')) and os.path.isfile(key)):
        failures.append('REAL skipped: THEFT4_GAME_DIR / THEFT4_AES_KEY not set')
        return
    out = os.path.join(work, 'offline')
    r = subprocess.run([sys.executable, os.path.join(TOOLS, 'gta4_fxc_shaders.py'), 'extract', '--game', game,
                        '--key', key, '--out', out], capture_output=True, text=True)
    sys.stdout.write(''.join('REAL ' + l + '\n' for l in r.stdout.splitlines()[:4]))
    m = re.search(r'common\.rpf \.fxc files (\d+), default\.xex images 1', r.stdout)
    n = re.search(r'candidates (\d+), valid (\d+), rejected (\d+)', r.stdout)
    u = re.search(r'unique containers (\d+) \(duplicates (\d+)\)', r.stdout)
    if r.returncode or not (m and n and u) or m[1] != '89' or n[3] != '0' or u[1] != '1370':
        failures.append('REAL: not 89 .fxc files / 1,370 containers / 0 rejected')
        return
    args = [sys.executable, os.path.join(TOOLS, 'gta4_fxc_shaders.py'), 'coverage', '--manifest',
            os.path.join(out, 'manifest.tsv')]
    dump, package = os.environ.get('THEFT4_SHADER_DUMP', ''), os.environ.get('THEFT4_PACKAGE', '')
    if not package:
        failures.append('REAL coverage skipped: THEFT4_PACKAGE not set')
        return
    args += ['--package', package] + (['--runtime-dump', dump] if dump else [])
    c = subprocess.run(args, capture_output=True, text=True).stdout
    sys.stdout.write(''.join('REAL ' + l + '\n' for l in c.splitlines()[:7]))
    miss = re.search(r'in the package (\d+), missing (\d+)', c)
    if not miss or miss[2] != '0':
        failures.append('REAL: the package lacks containers the game holds')
    d = re.search(r'runtime dump (\d+): found offline (\d+)', c)
    if dump and (not d or d[1] != d[2]):
        failures.append('REAL: a runtime-dumped container is not in the game files')


def config_test(failures):
    toml = os.path.join(RUNS, 'run61-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run61-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_gpu': '"deko3d"', 'nx_dk_anatomy': 'false', 'nx_dk_ab': 'true', 'nx_dk_ab_phases': '513'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        xxh3_test(work, failures)
        aes_test(failures)
        containers_test(failures)
        rpf_test(failures)
        merge_test(work, failures)
        recorder_test(failures)
        evidence_test(failures)
        real_test(work, failures)
    config_test(failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run60.py')], capture_output=True, text=True)
    print('test_run60.py:', (r.stdout.strip().splitlines() or ['?'])[-1])
    if r.returncode != 0:
        failures.append('test_run60.py failed')
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 61 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
