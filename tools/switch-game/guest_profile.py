#!/usr/bin/env python3
"""A thread's profile samples of a console run charged to guest functions (run 64, devlog section AL).

    python tools/switch-game/guest_profile.py game.log switch-game.elf [thread id, default 16: the guest mixer]

where.py --profile names each sampled pc by its innermost frame, so a pc inside a guest function's inlined 128-bit
load is listed as `vld1q_s64` and the function that holds it stays unknown (run 62: "the profiler's stacks do not name
the callees"). Here every pc of the "profile id N" lines (cvar nx_profile) is given to addr2line -i and charged to
its OUTERMOST frame, the function whose code the pc is in; the "profile stack id N" lines give the chains of guest
functions above it, which name the callees behind an indirect call. The ELF must be the one of the build that ran.
"""
import collections
import os
import re
import shutil
import subprocess
import sys


def addr2line_tool():
    for name in ('aarch64-none-elf-addr2line', '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line',
                 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe'):
        if shutil.which(name) or os.path.exists(name):
            return name
    return None


def profile(log, elf, thread='16'):
    """(samples, samples with a logged pc, {function: samples}, stack weight, {chain of guest functions: weight})."""
    tool = addr2line_tool()
    if not tool:
        raise RuntimeError('aarch64-none-elf-addr2line not found (devkitA64)')
    pcs, stacks, total = collections.Counter(), collections.Counter(), 0
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r"profile id (\d+) \S+ '[^']*' \([\d.]+% cpu\) (\d+) samples:(.*)", line)
        if m and m[1] == thread:
            total += int(m[2])
            for a, n in re.findall(r'\+0x([0-9a-fA-F]+) x(\d+)', m[3].split(' lr ')[0]):
                pcs[int(a, 16)] += int(n)
        m = re.search(r'profile stack id (\d+) x(\d+): (.*)', line)
        if m and m[1] == thread:
            frames = tuple(int(a, 16) for a in re.findall(r'\+0x([0-9a-fA-F]+)', m[3]))
            if frames:
                stacks[frames] += int(m[2])
    addresses = sorted(set(pcs) | {a for s in stacks for a in s})
    out = subprocess.run([tool, '-f', '-C', '-i', '-p', '-e', elf], input='\n'.join(hex(a) for a in addresses) + '\n',
                         capture_output=True, text=True, check=True).stdout
    outer, current, it = {}, None, iter(addresses)
    for l in out.splitlines():
        if l.startswith(' (inlined by)'):
            outer[current] = l.strip()[len('(inlined by) '):].split(' at ')[0]
        else:
            current = next(it)
            outer[current] = l.strip().split(' at ')[0]
    by = collections.Counter()
    for a, n in pcs.items():
        by[outer[a]] += n
    chains = collections.Counter()
    for s, n in stacks.items():
        guest = [outer[a] for a in s if re.match(r'(__imp__)?sub_[0-9A-F]{8}', outer[a])]
        chains[' < '.join(guest[:5]) or outer[s[0]]] += n
    return total, sum(pcs.values()), by, sum(stacks.values()), chains


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    thread = sys.argv[3] if len(sys.argv) > 3 else '16'
    total, logged, by, weight, chains = profile(sys.argv[1], sys.argv[2], thread)
    print(f'thread {thread}: {total} samples, {logged} with a logged pc ({100 * logged / max(total, 1):.1f} %)')
    for name, n in by.most_common(24):
        print(f'  pc in {100 * n / max(total, 1):5.1f} % of the thread  {n:7d}  {name}')
    print(f'stacks: weight {weight}')
    for name, n in chains.most_common(14):
        print(f'  {100 * n / max(weight, 1):5.1f} %  {name}')


if __name__ == '__main__':
    main()
