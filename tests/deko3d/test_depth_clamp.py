#!/usr/bin/env python3
"""Offline test of run 43 (devlog section N): the depth clamp of the deko3d recorder.

    python tests/deko3d/test_depth_clamp.py

1. The decision (native_stencil_volume_policy.h, the Vulkan path's own header, called the way the recorder calls it): a
   two-sided z-fail light volume is clamped, a G-buffer draw, a light contribution (colour write), a z-pass volume, a
   culled or one-sided draw, a zero stencil mask are not; the title's own request is honoured; and all 12,288 combinations
   of the fields the rule reads agree with the rule written out again in the probe.
2. The wiring, checked in the text (the Switch code does not compile on the PC): the recorder computes the flag with
   ShouldEnableNativeDepthClampForDraw before the rasterizer state is built and sets rs.depthClampEnable from it; the
   Vulkan path in graphics_system.cpp makes the same call with the same header; the policy header is unchanged from
   commit 17f3a18.
What no offline test shows: that the hardware clamp changes the picture. That is run 43's measurement.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GTA = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native')
DK = os.path.join(GTA, 'deko3d')


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_run43_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{GTA}" '
                f'"{os.path.join(HERE, "dk_run43_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def wiring():
    problems = []
    rec = read(os.path.join(DK, 'dk_recorder.cpp'))
    call = rec.find('ShouldEnableNativeDepthClampForDraw(x.depth_clamp_enable != 0, true, volume)')
    rs = rec.find('rs.depthClampEnable = depth_clamp ? 1 : 0;')
    bind = rec.find('dkCmdBufBindRasterizerState(cmd, &rs);', max(rs, 0))
    if call < 0:
        problems.append('the recorder does not call ShouldEnableNativeDepthClampForDraw with the volume state')
    if rs < 0:
        problems.append('the recorder does not set rs.depthClampEnable from the decision')
    if not (0 <= call < rs < bind):
        problems.append('order: decision, then the rasterizer state, then its bind')
    if '#include "../native_stencil_volume_policy.h"' not in rec:
        problems.append('the recorder does not include the Vulkan path\'s policy header')
    vk = read(os.path.join(GTA, 'graphics_system.cpp'))
    if 'ShouldEnableNativeDepthClampForDraw(fixed.depth_clamp_enable != 0,' not in vk or \
            'IsLocalStencilSetup(command.lighting)' not in vk:
        problems.append('the Vulkan path\'s call changed: re-read it')
    p = subprocess.run(['git', '-C', ROOT, 'diff', '--quiet', '17f3a18', '--',
                        'glue/rexglue-sdk-main/src/graphics/gta4_native/native_stencil_volume_policy.h'])
    if p.returncode != 0:
        problems.append('native_stencil_volume_policy.h differs from commit 17f3a18')
    return problems


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2
        out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout
        sys.stdout.write(out)
        expected = {
            'ZFAIL volume: 1': 'a two-sided z-fail light volume is not clamped',
            'ZFAIL with colour write: 0': 'a light contribution is clamped',
            'ZFAIL with depth write (a G-buffer draw): 0': 'a draw that writes depth is clamped',
            'Z-pass volume: 0': 'a z-pass volume is clamped',
            'culled: 0': 'a culled draw is clamped',
            'one-sided: 0': 'a one-sided draw is clamped',
            'zero write mask: 0': 'a draw with a zero stencil mask is clamped',
            'one face z-fail: 1': 'a volume whose only z-fail face is the back one is not clamped',
            'plain draw: 0': 'a plain draw is clamped',
            "the title's own request on a plain draw: 1": "the title's own request is not honoured",
        }
        for line, why in expected.items():
            if line not in out.splitlines():
                failures.append(why)
        m = re.search(r'RULE cases (\d+) bad (\d+) clamped (\d+)', out)
        if not m or m[2] != '0' or m[1] != '12288' or int(m[3]) == 0:
            failures.append('the decision differs from the rule over the combinations')
    for problem in wiring():
        print('wiring: ' + problem)
        failures.append(problem)
    if not failures:
        print('wiring: the recorder decides with the Vulkan path\'s policy and sets rs.depthClampEnable before the bind')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
