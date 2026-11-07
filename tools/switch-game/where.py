#!/usr/bin/env python3
"""Decode the module offsets in a switch-game log with addr2line.

Usage: where.py game.log [game-crash.log ...] [--elf out/build/switch-game/switch-game.elf]
       where.py --profile game.log [--elf ...] [--top 25]

Every "+0x..." in the thread snapshot lines ("  thread ..."), in TERMINATION
lines ("module base + 0x...") and every "pc - base"/"lr - base" of
game-crash.log is an ELF address of switch-game.elf. The script prints each
line followed by the function (and inlined chain) of each address. The ELF
must be the one of the build that ran.

--profile: the "profile id ..." lines of cvar nx_profile (nx_profiler.h), one
per sampled thread every 10 s, are added up over the whole log per thread and
per function: the share of the samples whose pc is in each function, then the
same for the lr (the caller, which names the wait when the pc is in a svc).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

DEFAULT_ELF = os.path.join(os.path.dirname(__file__), "..", "..", "out", "build", "switch-game",
                           "switch-game.elf")
OFFSET = re.compile(r"(?:\+ ?0x|base \+ 0x|- base\s+0x)([0-9a-fA-F]+)")


def addr2line_tool():
    for name in ("aarch64-none-elf-addr2line",
                 "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line",
                 "C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe"):
        if shutil.which(name) or os.path.exists(name):
            return name
    sys.exit("aarch64-none-elf-addr2line not found (devkitA64)")


def decode(tool, elf, addresses):
    if not addresses:
        return {}
    # Run 57: the addresses on stdin (12 stacks a thread every 10 s are more than a Windows command line holds).
    out = subprocess.run([tool, "-f", "-C", "-i", "-p", "-e", elf],
                         input="\n".join(hex(a) for a in addresses) + "\n", capture_output=True, text=True,
                         check=True).stdout
    # -p -i: one line per address, inlined frames on following lines starting with " (inlined by)"
    result, current = {}, None
    it = iter(addresses)
    for line in out.splitlines():
        if line.startswith(" (inlined by)") and current is not None:
            result[current].append(line.strip())
        else:
            current = next(it)
            result[current] = [line.strip()]
    return result


PROFILE_LINE = re.compile(r"profile (id \d+ \S+ '[^']*') \([\d.]+% cpu\) (\d+) samples:(.*)")
PROFILE_ITEM = re.compile(r"\+0x([0-9a-fA-F]+) x(\d+)")


def function_name(chain):
    # "name at file:line" -> name; inlined frames are not used for the totals
    return chain[0].split(" at ")[0] if chain else "?"


def profile(tool, elf, paths, top):
    threads = {}  # name -> [samples, {pc: n}, {lr: n}]
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = PROFILE_LINE.search(line)
                if not m:
                    continue
                entry = threads.setdefault(m.group(1), [0, {}, {}])
                entry[0] += int(m.group(2))
                pcs, _, lrs = m.group(3).partition("; lr:")
                for part, table in ((pcs, entry[1]), (lrs, entry[2])):
                    for a, n in PROFILE_ITEM.findall(part):
                        table[int(a, 16)] = table.get(int(a, 16), 0) + int(n)
    addresses = sorted({a for e in threads.values() for t in e[1:] for a in t})
    names = decode(tool, elf, addresses)
    for thread, (samples, pcs, lrs) in sorted(threads.items(), key=lambda kv: -kv[1][0]):
        print(f"== {thread}: {samples} samples")
        for label, table in (("pc in", pcs), ("lr in", lrs)):
            per_function = {}
            for a, n in table.items():
                key = function_name(names.get(a, ["?"]))
                per_function[key] = per_function.get(key, 0) + n
            for key, n in sorted(per_function.items(), key=lambda kv: -kv[1])[:top]:
                print(f"  {label} {100.0 * n / max(samples, 1):5.1f}%  {n:6d}  {key}")
        print("  (only the top pcs of each 10 s window are logged: the shares are of all samples)")


STACK_LINE = re.compile(r"profile stack id (\d+) x(\d+):((?: \S+)+)")
# Run 56: frames that only say "waiting" (libnx, newlib, the C++ runtime); the
# first frame after them is who waits, the answer the pc and lr cannot give.
WAIT_FRAMES = re.compile(r"^(svc\w+|mutexLock|mutexUnlock|condvarWait\w*|__syscall_\w+|_waitObjectsImpl|"
                         r"serviceDispatchImpl|__gthread_\w+|pthread_\w+|std::mutex::\w+|"
                         r"std::condition_variable::\w+|std::unique_lock<.*|std::lock_guard<.*|"
                         r"__libc_lock_\w+|__malloc_lock|__malloc_unlock|_malloc_r|_free_r|_memalign_r|"
                         r"_realloc_r|malloc|free|operator new.*|operator delete.*|sched_yield|"
                         r"rex::thread::\w+|__aarch64_\w+|\?\?)")


def stacks(tool, elf, paths, top):
    """Run 56: the 'profile stack' lines (the five most frequent whole stacks of each
    thread every 10 s) added up over the log: per thread, who is under each wait
    (the first frame that is not a lock or wait primitive), then the whole stacks.
    Only the top stacks of each window are logged: the shares are of those samples."""
    rows = []  # (thread id, count, [addresses])
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = STACK_LINE.search(line)
                if not m:
                    continue
                addresses = [int(a[3:], 16) for a in m.group(3).split() if a.startswith("+0x")]
                rows.append((int(m.group(1)), int(m.group(2)), addresses))
    names = decode(tool, elf, sorted({a for _, _, addrs in rows for a in addrs}))
    by_thread = {}
    for tid, n, addrs in rows:
        by_thread.setdefault(tid, []).append((n, [function_name(names.get(a, ["?"])) for a in addrs]))
    for tid, items in sorted(by_thread.items(), key=lambda kv: -sum(n for n, _ in kv[1])):
        total = sum(n for n, _ in items)
        print(f"== id {tid}: {total} samples in the logged top stacks")
        waiter, waits = {}, {}
        for n, frames in items:
            k = 0
            while k < len(frames) and WAIT_FRAMES.match(frames[k]):
                k += 1
            if k == 0:
                continue  # running, not waiting
            who = frames[k] if k < len(frames) else "?"
            what = frames[0] + (" <- " + frames[k - 1] if k > 1 else "")
            waiter[who] = waiter.get(who, 0) + n
            waits[(what, who)] = waits.get((what, who), 0) + n
        print("  waiting, by the first frame that is not a wait primitive:")
        for who, n in sorted(waiter.items(), key=lambda kv: -kv[1])[:top]:
            print(f"    {100.0 * n / total:5.1f}%  {n:6d}  {who}")
        for (what, who), n in sorted(waits.items(), key=lambda kv: -kv[1])[:top]:
            print(f"      {n:6d}  {what}  under  {who}")
        merged = {}
        for n, frames in items:
            merged[" <- ".join(frames[:8])] = merged.get(" <- ".join(frames[:8]), 0) + n
        print("  whole stacks:")
        for text, n in sorted(merged.items(), key=lambda kv: -kv[1])[:top]:
            print(f"    {100.0 * n / total:5.1f}%  {n:6d}  {text[:400]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    parser.add_argument("--profile", action="store_true",
                        help="add up the nx_profile samples per thread and function")
    parser.add_argument("--stacks", action="store_true",
                        help="add up the nx_profile whole stacks: who waits under each lock")
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()
    tool = addr2line_tool()
    if args.profile:
        profile(tool, args.elf, args.logs, args.top)
        return
    if args.stacks:
        stacks(tool, args.elf, args.logs, args.top)
        return
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        wanted = [line for line in lines
                  if line.lstrip("[game] ").startswith("thread ") or "  thread " in line
                  or "TERMINATION" in line or "- base" in line]
        addresses = sorted({int(m, 16) for line in wanted for m in OFFSET.findall(line)})
        names = decode(tool, args.elf, addresses)
        print(f"== {path}")
        for line in wanted:
            found = [int(m, 16) for m in OFFSET.findall(line)]
            if not found:
                continue
            print(line)
            for a in found:
                chain = names.get(a, ["?"])
                print(f"    +0x{a:x}  {chain[0]}")
                for inlined in chain[1:]:
                    print(f"             {inlined}")


if __name__ == "__main__":
    main()
