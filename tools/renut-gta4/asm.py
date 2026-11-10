#!/usr/bin/env python3
"""The PowerPC listing of generated functions, from the comments of the generated code.

    python tools/renut-gta4/asm.py 829D8810 829CD908 [--callers 829D8810] [--gen DIR]

Each instruction comment (`// lwz r4,48(r31)`) is printed with the labels and the names of the functions called,
so a function reads in a tenth of the lines of its C++. `--callers X` lists the functions that call sub_X.
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "..", "glue", "rexglue-sdk-main", "gta4-recomp", "generated", "eu-base")
DEFINE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
CALL = re.compile(r"^\t+(?:__imp__)?((?:sub_[0-9A-F]{8}|__imp__\w+|\w+))\(ctx, base\);")
INDIRECT = re.compile(r"REX_CALL_INDIRECT_FUNC\((.*)\)")


def files(gen):
    return sorted(glob.glob(os.path.join(gen, "gta4_recomp.*.cpp")))


def listing(gen, names):
    want = set(names)
    out = {}
    for path in files(gen):
        current = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = DEFINE.match(line)
                if m:
                    current = m.group(1) if m.group(1) in want else None
                    if current:
                        out[current] = []
                    continue
                if current is None:
                    continue
                if line.startswith("}"):
                    current = None
                    continue
                s = line.strip()
                if s.startswith("// "):
                    out[current].append("    " + s[3:])
                elif s.startswith("loc_") and s.endswith(":"):
                    out[current].append(s)
                else:
                    m = CALL.match(line)
                    if m and out[current] and "->" not in out[current][-1]:
                        out[current][-1] += "    -> " + m.group(1)
                    elif "REX_CALL_INDIRECT_FUNC" in s and out[current]:
                        out[current][-1] += "    -> (indirect)"
        if len(out) == len(want):
            break
    return out


def callers(gen, name):
    found = set()
    pattern = re.compile(r"\b(?:__imp__)?%s\(ctx, base\)" % name)
    for path in files(gen):
        current = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = DEFINE.match(line)
                if m:
                    current = m.group(1)
                elif current and pattern.search(line):
                    found.add(current)
    return sorted(found)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("functions", nargs="*")
    ap.add_argument("--callers")
    ap.add_argument("--gen", default=GEN)
    a = ap.parse_args()
    if a.callers:
        name = "sub_" + a.callers.upper().replace("SUB_", "")
        print(name, "is called by:", " ".join(callers(a.gen, name)) or "(no direct caller)")
    names = ["sub_" + x.upper().replace("SUB_", "") for x in a.functions]
    got = listing(a.gen, names)
    for n in names:
        print("==", n, "(%d instructions)" % sum(1 for l in got.get(n, []) if l.startswith("    ")))
        print("\n".join(got.get(n, ["   not found"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
