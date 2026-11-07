#!/usr/bin/env python3
"""Offline tests of the deko3d shader package: the writer's format, the Python validator, and the console's C++ reader.

    python tests/deko3d/test_runtime_package.py [--real <deko3d-out dir>] [--old-v1 <old runtime.bin>]

Builds small synthetic packages (good and broken in one way each) from tools/deko3d/gta4_deko3d_runtime_check.py's
layout, then

  1. runs the Python validator on each (check_file), and
  2. compiles glue/.../deko3d/dk_shader_package.cpp UNCHANGED with the host clang of Visual Studio (stub logging,
     tests/deko3d/stubs) and runs it on each,

and requires both to agree: a package one accepts the other accepts, one refuses the other refuses, and a good
package gives the same counts and the same lookups. With --real the package in that folder (the game's own, made
on the PC, never in the repository) goes through both as well; with --old-v1 the run 36 file (44-byte records
under 48-byte offsets, version 1) must be refused by both before anything is allocated.

Environment: THEFT4_VS_ROOT = the Visual Studio folder with VC\\Auxiliary\\Build\\vcvars64.bat (as
tools/pc-eu-base/build.bat). Without it only step 1 runs and the exit code is 2.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'deko3d'))
import gta4_deko3d_runtime_check as fmt  # noqa: E402

DK_DIR = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native', 'deko3d')


def build_package(shaders=None, blobs=None, tag=b'test tag', *, version=fmt.VERSION, record_header=fmt.RECORD_BYTES,
                  record_physical=fmt.RECORD_BYTES, declared_size=None, magic=fmt.MAGIC, tag_len_override=None,
                  count_override=None, blob_count_override=None):
    """shaders: list of dicts (hash, late, stage, inputs [(x,u,n,c)], variants [(value, blob)], types)."""
    shaders = shaders if shaders is not None else default_shaders()
    blobs = blobs if blobs is not None else [bytes([i + 1]) * (700 + 90 * i) for i in range(4)]
    header_len = fmt.align(fmt.HEADER_BYTES + len(tag), 8)
    table_pos = header_len
    extra_pos = table_pos + fmt.RECORD_BYTES * len(shaders)  # the offsets always assume 48-byte records
    extra = b''
    table = b''
    for s in shaders:
        io = extra_pos + len(extra)
        extra += b''.join(struct.pack('<BBBB', *i) for i in s['inputs'])
        vo = extra_pos + len(extra)
        extra += b''.join(struct.pack('<II', *v) for v in s['variants'])
        to = extra_pos + len(extra)
        extra += bytes(s.get('types', [0] * 32))
        rec = fmt.RECORD.pack(s['hash'], s['stage'], s['late'], len(s['inputs']), len(s['variants']), 0, 0,
                              s.get('glsl', 0xFFFFFFFF), io, vo, to, 0, 0, 0)
        table += rec[:record_physical]
    blobs_pos = fmt.align(extra_pos + len(extra), 256)
    data_pos = fmt.align(blobs_pos + 8 * len(blobs), 256)
    blob_table = b''
    data = b''
    for b in blobs:
        blob_table += struct.pack('<II', data_pos + len(data), len(b))
        data += b + b'\0' * ((-len(b)) % 256)
    total = data_pos + len(data)
    header = fmt.HEADER.pack(magic, version, count_override if count_override is not None else len(shaders),
                             blob_count_override if blob_count_override is not None else len(blobs),
                             tag_len_override if tag_len_override is not None else len(tag), record_header,
                             total if declared_size is None else declared_size)
    out = header + tag
    out += b'\0' * (header_len - len(out))
    out += table + extra
    out += b'\0' * (blobs_pos - len(out))
    out += blob_table
    out += b'\0' * (data_pos - len(out))
    out += data
    return bytearray(out)


def default_shaders():
    types = [0] * 32
    types[0] = 1
    types[3] = 4
    return [
        dict(hash=0x0000000000000010, late=0, stage=0, inputs=[(0, 0, 0, 3), (5, 1, 0, 2)], variants=[(0, 0)], types=types,
             glsl=0),
        dict(hash=0x0000000000000020, late=0, stage=1, inputs=[], variants=[(0, 1), (1, 2), (2, 0xFFFFFFFF)], types=types),
        dict(hash=0x0000000000000020, late=1, stage=1, inputs=[], variants=[(0, 3)], types=types),
        dict(hash=0x00000000DEADBEEF, late=0, stage=0, inputs=[(2, 2, 1, 4)], variants=[], types=[0] * 32),
        dict(hash=0xFFFFFFFF00000001, late=0, stage=1, inputs=[], variants=[(7, 1)], types=types),
    ]


def record_at(pkg, i, tag_len=len(b'test tag')):
    table = fmt.align(fmt.HEADER_BYTES + tag_len, 8)
    return table + i * fmt.RECORD_BYTES


def fixtures():
    """name -> (bytes, should_open)."""
    out = {}
    out['good'] = (build_package(), True)
    out['good_empty'] = (build_package(shaders=[], blobs=[]), True)
    out['good_no_variants_blobs_only'] = (build_package(shaders=[], blobs=[b'x' * 300]), True)
    out['good_long_tag'] = (build_package(tag=b'uam 1.1.0 ' + b't' * 60), True)
    # the format bug of run 36 (version 1: 44-byte records under 48-byte offsets) and its header cousins
    out['v1_header'] = (build_package(version=1, record_header=0), False)
    out['v2_header_44_byte_records'] = (build_package(record_physical=44), False)
    out['record_size_44_declared'] = (build_package(record_header=44), False)
    out['bad_magic'] = (build_package(magic=b'GDKX'), False)
    out['version_3'] = (build_package(version=3), False)
    g = bytes(build_package())
    out['truncated_tail'] = (bytearray(g[:-300]), False)
    t = bytearray(g[:-300])
    struct.pack_into('<Q', t, 24, len(t))
    out['truncated_size_patched'] = (t, False)
    out['padded'] = (bytearray(g + b'\0' * 256), False)
    out['only_header'] = (bytearray(g[:fmt.HEADER_BYTES]), False)
    out['shorter_than_header'] = (bytearray(g[:20]), False)
    out['empty_file'] = (bytearray(), False)
    out['huge_shader_count'] = (build_package(count_override=0xFFFFFFFF), False)
    out['shader_count_over_limit'] = (build_package(count_override=(1 << 20) + 1), False)
    out['shader_count_in_limit_beyond_file'] = (build_package(count_override=1 << 20), False)
    out['tag_len_over_limit'] = (build_package(tag_len_override=5000), False)
    # a tag length that puts the shader table past the end of a short file (the tag itself is within the limit)
    short = bytearray(g[:100])
    struct.pack_into('<I', short, 16, 4000)  # tag length
    struct.pack_into('<Q', short, 24, len(short))  # declared size = the short file's
    out['tag_len_puts_table_past_file'] = (short, False)
    out['blob_count_over_limit'] = (build_package(blob_count_override=(1 << 22) + 1), False)
    out['blob_count_in_limit_table_past_file'] = (build_package(blob_count_override=1 << 22), False)
    # bytes after the last blob, with the declared size patched to match: the reader and the validator take it
    # (the declared size is what pins the end of the file)
    tail = bytearray(g + b'\0' * 256)
    struct.pack_into('<Q', tail, 24, len(tail))
    out['trailing_bytes_declared'] = (tail, True)

    def mut(fn):
        b = bytearray(g)
        fn(b)
        return b

    r0 = record_at(g, 0)
    out['inputs_offset_below_extra'] = (mut(lambda b: struct.pack_into('<I', b, r0 + 24, 0)), False)
    out['variants_offset_beyond_file'] = (mut(lambda b: struct.pack_into('<I', b, r0 + 28, 0xFFFFFFF0)), False)
    out['types_offset_beyond_file'] = (mut(lambda b: struct.pack_into('<I', b, r0 + 32, len(g) - 8)), False)
    # 255 inputs (1020 bytes) from an offset 100 bytes before the end of the file: runs past it
    def inputs_past_file(b):
        b[record_at(g, 4) + 10] = 255
        struct.pack_into('<I', b, record_at(g, 4) + 24, len(g) - 100)

    out['input_count_runs_past_file'] = (mut(inputs_past_file), False)
    out['bad_stage'] = (mut(lambda b: b.__setitem__(r0 + 8, 2)), False)
    out['bad_late'] = (mut(lambda b: b.__setitem__(r0 + 9, 7)), False)
    # Run 55 made bits 0-2 of word 40 the shader flags, run 56 bit 3 (the io mask in word 44 is set): an unknown bit
    # must still be refused, the io mask only with its flag.
    out['reserved_not_zero'] = (mut(lambda b: struct.pack_into('<I', b, r0 + 40, 1 << 4)), False)
    out['padding_not_zero'] = (mut(lambda b: struct.pack_into('<I', b, r0 + 44, 1)), False)
    out['io_mask_with_flag'] = (mut(lambda b: (struct.pack_into('<I', b, r0 + 40, 8),
                                               struct.pack_into('<I', b, r0 + 44, 0x3FFFD))), True)
    out['hash_not_ascending'] = (mut(lambda b: struct.pack_into('<Q', b, r0, 0xFFFFFFFFFFFFFFF0)), False)

    def swap_late(b):  # shaders 1 and 2 share a hash: late must go 0 then 1
        a, c = record_at(g, 1), record_at(g, 2)
        b[a:a + 48], b[c:c + 48] = bytes(b[c:c + 48]), bytes(b[a:a + 48])

    out['same_hash_late_descending'] = (mut(swap_late), False)

    def duplicate(b):
        b[record_at(g, 1):record_at(g, 1) + 48] = b[record_at(g, 2):record_at(g, 2) + 48]
        # shader 1 is now a copy of shader 2 (same hash, late=1 twice)

    out['duplicate_hash_late'] = (mut(duplicate), False)
    out['bad_input_uam_location'] = (mut(lambda b: b.__setitem__(struct.unpack_from('<I', b, r0 + 24)[0] + 1, 16)), False)
    out['bad_input_components'] = (mut(lambda b: b.__setitem__(struct.unpack_from('<I', b, r0 + 24)[0] + 3, 0)), False)
    out['bad_texture_type'] = (mut(lambda b: b.__setitem__(struct.unpack_from('<I', b, r0 + 32)[0], 9)), False)
    r1 = record_at(g, 1)
    out['variant_blob_index_out_of_range'] = (mut(lambda b: struct.pack_into('<I', b, struct.unpack_from('<I', b, r1 + 28)[0] + 4, 4)), False)
    blobs_at = None
    # the blob table sits at the next 256 boundary after the extra block: find it from the record offsets
    ext = max(struct.unpack_from('<I', g, record_at(g, i) + 32)[0] + 32 for i in range(5))
    blobs_at = fmt.align(ext, 256)
    out['blob_size_zero'] = (mut(lambda b: struct.pack_into('<I', b, blobs_at + 4, 0)), False)
    out['blob_size_huge'] = (mut(lambda b: struct.pack_into('<I', b, blobs_at + 4, 0x7FFFFFF0)), False)
    out['blob_offset_into_tables'] = (mut(lambda b: struct.pack_into('<I', b, blobs_at, 0)), False)
    out['blob_offset_misaligned'] = (mut(lambda b: struct.pack_into('<I', b, blobs_at, struct.unpack_from('<I', b, blobs_at)[0] + 1)), False)
    return out


def write_dir(base, name, data, glsl=None):
    d = os.path.join(base, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'gta4_deko3d_runtime.bin'), 'wb') as f:
        f.write(data)
    if glsl is not None:
        with open(os.path.join(d, 'gta4_deko3d_glsl.bin'), 'wb') as f:
            f.write(glsl)
    return d


def build_glsl(texts, *, magic=b'GDKG', version=1, count=None, size_override=None, offset_shift=0):
    """gta4_deko3d_package.py's layout: 12-byte header, 20-byte entries {stage, text offset, text size, values
    offset, value count}, then the texts."""
    n = len(texts)
    pos = 12 + 20 * n
    entries = b''
    body = b''
    for t in texts:
        entries += struct.pack('<IIIII', 0, pos + len(body) + offset_shift, len(t) if size_override is None else size_override, 0, 0)
        body += t
    return magic + struct.pack('<II', version, n if count is None else count) + entries + body


GLSL_TEXTS = [b'#version 460\nvoid main() {}\n', b'#version 460\n// second\n']


def glsl_cases():
    """name -> (glsl bytes or None, should the GLSL be used)."""
    return {
        'glsl_good': (build_glsl(GLSL_TEXTS), True),
        'glsl_absent': (None, False),
        'glsl_bad_magic': (build_glsl(GLSL_TEXTS, magic=b'GDKX'), False),
        'glsl_version_2': (build_glsl(GLSL_TEXTS, version=2), False),
        'glsl_count_beyond_file': (build_glsl(GLSL_TEXTS, count=1 << 20), False),
        'glsl_count_over_limit': (build_glsl(GLSL_TEXTS, count=(1 << 22) + 1), False),
        'glsl_text_beyond_file': (build_glsl(GLSL_TEXTS, size_override=1000), False),
        'glsl_text_offset_beyond_file': (build_glsl(GLSL_TEXTS, offset_shift=500), False),
        'glsl_too_short': (b'GDKG\x01\x00', False),
    }


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'shader_package_probe.exe')
    # a .bat: cmd /c cannot take this many quoted paths on one line
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        sanitize = '-fsanitize=address,undefined ' if os.environ.get('THEFT4_TEST_SANITIZE') else ''
        f.write(f'"{clang}" -std=c++23 -O1 -g {sanitize}-D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter '
                f'-I "{os.path.join(HERE, "stubs")}" -I "{DK_DIR}" "{os.path.join(HERE, "shader_package_probe.cpp")}" '
                f'"{os.path.join(DK_DIR, "dk_shader_package.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def run_probe(exe, dirpath, extra=()):
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = 'detect_leaks=0'
    # THEFT4_TEST_SANITIZE: the ASan runtime DLL of VS's clang must be findable
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    libroot = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'lib', 'clang')
    if os.path.isdir(libroot):
        for ver in os.listdir(libroot):
            env['PATH'] = os.path.join(libroot, ver, 'lib', 'windows') + os.pathsep + env.get('PATH', '')
    p = subprocess.run([exe, dirpath, *extra], capture_output=True, text=True, timeout=300, env=env)
    out = p.stdout
    result = re.search(r'RESULT open=(\d) opened\(\)=(\d) shaders=(\d+) blobs=(\d+) blob_reads=(\d+)/(\d+) blob_bytes=(\d+)', out)
    if not result or p.returncode not in (0, 3):
        return None, out + p.stderr, p.returncode
    return dict(open=int(result[1]), opened=int(result[2]), shaders=int(result[3]), blobs=int(result[4]),
                reads_ok=int(result[5]), reads=int(result[6]), blob_bytes=int(result[7])), out, p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--real', help='folder with the game\'s gta4_deko3d_runtime.bin (version 2)')
    ap.add_argument('--old-v1', help='the run 36 file (version 1) that both readers must refuse')
    ap.add_argument('--expect-shaders', type=int, default=1912)
    ap.add_argument('--expect-blobs', type=int, default=6285)
    a = ap.parse_args()

    failures = []
    work = tempfile.mkdtemp(prefix='theft4-dkpkg-')
    try:
        exe = compile_probe(work)
        cases = fixtures()
        for name, (data, should_open) in cases.items():
            d = write_dir(work, name, bytes(data))
            path = os.path.join(d, 'gta4_deko3d_runtime.bin')
            try:
                fmt.check_file(path)
                py_ok, py_why = True, ''
            except fmt.FormatError as e:
                py_ok, py_why = False, str(e)
            line = f'{name:42s} python={"accept" if py_ok else "refuse"}'
            if py_ok != should_open:
                failures.append(f'{name}: python validator {"accepted" if py_ok else "refused"} (expected {"accept" if should_open else "refuse"}): {py_why}')
            if exe:
                res, out, rc = run_probe(exe, d)
                if res is None:
                    failures.append(f'{name}: probe crashed or printed nothing (rc {rc}): {out[-400:]}')
                    line += ' c++=CRASH'
                else:
                    cpp_ok = bool(res['open'])
                    line += f' c++={"accept" if cpp_ok else "refuse"}'
                    if cpp_ok != should_open:
                        failures.append(f'{name}: C++ reader {"accepted" if cpp_ok else "refused"} (expected {"accept" if should_open else "refuse"})\n{out}')
                    if cpp_ok and res['opened'] != 1:
                        failures.append(f'{name}: opened() is false after a successful Open')
                    if not cpp_ok and (res['shaders'] or res['blobs'] or res['opened']):
                        failures.append(f'{name}: a refused package is not empty: {res}')
                    if not cpp_ok and 'log error' not in out:
                        failures.append(f'{name}: refused with no error logged')
            print(line)

        if exe:
            # lookups and DKSH reads on the good package
            d = os.path.join(work, 'good')
            res, out, rc = run_probe(exe, d, ['10:0', '20:0', '20:1', 'DEADBEEF:0', 'FFFFFFFF00000001:0', '21:0', '0:0'])
            expect = {'10:0': 'found=1 pixel=0 inputs=2 variants=1 first_blob=0',
                      '20:0': 'found=1 pixel=1 inputs=0 variants=3 first_blob=1',
                      '20:1': 'found=1 pixel=1 inputs=0 variants=1 first_blob=3',
                      'DEADBEEF:0': 'found=1 pixel=0 inputs=1 variants=0 first_blob=-1',
                      'FFFFFFFF00000001:0': 'found=1 pixel=1 inputs=0 variants=1 first_blob=1',
                      '21:0': 'found=0', '0:0': 'found=0'}
            for key, want in expect.items():
                m = re.search(rf'^FIND {re.escape(key)} (.*)$', out, re.M)
                got = m[1] if m else '(missing)'
                if got != want and not got.startswith(want + ' glsl_len=0 glsl_define=0'):
                    failures.append(f'lookup {key}: got "{got}", want "{want}"')

            # a package directory without the file at all
            empty_dir = os.path.join(work, 'no_file')
            os.makedirs(empty_dir, exist_ok=True)
            res, out, rc = run_probe(exe, empty_dir)
            if res is None or res['open'] != 0 or 'no shader package' not in out:
                failures.append(f'missing file: {res} {out[-300:]}')
            else:
                print('missing file: C++ refuses it ("no shader package"), package left empty')

            # the optional GLSL package: used when sound, dropped whole (with an error logged) when not; the runtime
            # package opens either way
            for name, (glsl, usable) in glsl_cases().items():
                d = write_dir(work, name, bytes(build_package()), glsl)
                res, out, rc = run_probe(exe, d, ['10:0'])
                m = re.search(r'^FIND 10:0 found=1 .*glsl_len=(\d+) glsl_define=(\d)', out, re.M)
                if res is None or res['open'] != 1 or not m:
                    failures.append(f'{name}: runtime package did not open or lookup missing: {res} {out[-300:]}')
                    continue
                text_len, has_define = int(m[1]), int(m[2])
                logged_glsl_error = bool(re.search(r'log error: deko3d: GLSL package', out))
                ok = (text_len > 0 and has_define == 1 and not logged_glsl_error) if usable else \
                     (text_len == 0 and (logged_glsl_error or glsl is None))
                print(f'{name:42s} glsl text_len={text_len} define={has_define} error_logged={logged_glsl_error} -> {"ok" if ok else "WRONG"}')
                if not ok:
                    failures.append(f'{name}: GLSL package {"should be used" if usable else "should be refused"}: text_len={text_len} define={has_define} error_logged={logged_glsl_error}\n{out}')
            if not res or res['shaders'] != 5 or res['blobs'] != 4 or res['reads_ok'] != 4:
                failures.append(f'good package counts/blob reads: {res}')
            else:
                print('good package: 5 shaders, 4 DKSH read back, lookups as expected')

            if a.old_v1:
                d = os.path.join(work, 'real_old_v1')
                os.makedirs(d, exist_ok=True)
                dst = os.path.join(d, 'gta4_deko3d_runtime.bin')
                try:
                    os.link(a.old_v1, dst)
                except OSError:
                    shutil.copyfile(a.old_v1, dst)
                try:
                    fmt.check_file(dst)
                    failures.append('old v1 file: the Python validator accepted it')
                except fmt.FormatError as e:
                    print(f'old v1 file: python refuses ({e})')
                res, out, rc = run_probe(exe, d)
                if res is None or res['open'] != 0 or res['shaders'] or 'version 1' not in out:
                    failures.append(f'old v1 file: C++ did not refuse it by version: {res} {out[-300:]}')
                else:
                    print('old v1 file: C++ refuses it (version 1), package left empty')
            if a.real:
                path = os.path.join(a.real, 'gta4_deko3d_runtime.bin')
                info = fmt.check_file(path, a.expect_shaders, a.expect_blobs)
                print(f"real package: python OK, {info['bytes']} bytes, {info['shaders']} shaders, {info['blobs']} DKSH, sha256 {fmt.sha256(path)}")
                res, out, rc = run_probe(exe, a.real)
                if res is None or res['open'] != 1 or res['shaders'] != a.expect_shaders or res['blobs'] != a.expect_blobs:
                    failures.append(f'real package through the C++ reader: {res} {out[-500:]}')
                else:
                    print(f"real package: C++ reader OK, {res['shaders']} shaders, {res['blobs']} DKSH, first 64 DKSH read back: {res['reads_ok']}/{res['reads']}")
                    for line in out.splitlines():
                        if line.startswith('log '):
                            print('   ' + line[:240])
        else:
            print('SKIPPED the C++ half: THEFT4_VS_ROOT (or D:\\Microsoft Visual Studio) has no clang/vcvars64')
            failures.append('C++ half not run')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f'\n{len(failures)} FAILURE(S):')
        for f in failures:
            print(' -', f)
        return 1 if exe else 2
    print('\nall package tests passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
