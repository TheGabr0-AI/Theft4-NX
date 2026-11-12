#!/usr/bin/env python3
"""Theft4-NX: a reproducible record of one full generation of ultimate/app/generated (docs/switch-port/20, U3).

Written by tools/ultimate/regenerate.ps1 after the code generator and tools/post_codegen.py ran on an empty
generated/ folder. Records: the repository and base commits, the generator's and the game executable's SHA-256, every
input configuration file's SHA-256, the environment switches the passes read, the function count, every output file's
SHA-256 with one combined hash, the generator's warnings and each post-codegen step's result line.

    python tools/ultimate/generation_manifest.py --ultimate ultimate --logs <dir> --out <manifest.json>

Hashes only: nothing of the game's code or data is written into the manifest.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_log(path):
    # Windows PowerShell 5's `*>` writes UTF-16 LE with a BOM (ultimate/tools/codegen.ps1, regenerate.ps1).
    raw = open(path, 'rb').read()
    if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        return raw.decode('utf-16', errors='replace')
    return raw.decode('utf-8-sig', errors='replace')


def environment(logs):
    # The switches the passes ran with, as tools/ultimate/regenerate.ps1 recorded them before running them.
    path = os.path.join(logs, 'environment.txt')
    if not os.path.isfile(path):
        sys.exit('generation_manifest: no environment.txt in the logs folder (regenerate.ps1 writes it)')
    env = {}
    for line in read_log(path).splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k.strip()] = v.strip()
    return env


def git(repo, *args):
    try:
        return subprocess.run(['git', '-C', repo, *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ultimate', required=True, help='the ultimate/ folder')
    ap.add_argument('--logs', required=True, help='folder with codegen.log and post_codegen.log of this generation')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    ult = os.path.abspath(a.ultimate)
    repo = os.path.dirname(ult)
    app = os.path.join(ult, 'app')
    gen = os.path.join(app, 'generated')

    codegen_log = read_log(os.path.join(a.logs, 'codegen.log'))
    post_log = read_log(os.path.join(a.logs, 'post_codegen.log'))

    m = re.search(r'Codegen summary: (\d+) written, (\d+) unchanged, (\d+) deleted', codegen_log)
    if not m:
        sys.exit('generation_manifest: no "Codegen summary" line in codegen.log: the generation did not finish')
    if 'optimised codegen steps done' not in post_log:
        sys.exit('generation_manifest: post_codegen.log does not end with "optimised codegen steps done"')
    if re.search(r'^Traceback', post_log, re.M):
        sys.exit('generation_manifest: post_codegen.log holds a Python traceback')

    xex_info = {k: (re.search(k + r':\s*(\S+)', codegen_log) or [None, None])[1]
                for k in ('Title ID', 'Media ID', 'Version')}

    inputs = ['renut_manifest.toml', 'renut_config.toml', 'overrides.toml', 'indirect_promotions.txt']
    cfg = os.path.join(app, 'config')
    inputs += sorted('config/' + n for n in os.listdir(cfg) if os.path.isfile(os.path.join(cfg, n)))
    inputs += ['src/cpu/hot/localized.txt', 'src/cpu/hot/localized_bulk.txt', 'src/cpu/hot/hand.txt']
    input_hashes = {p: sha256(os.path.join(app, p)) for p in inputs if os.path.isfile(os.path.join(app, p))}

    outputs = {}
    for root, _, files in os.walk(gen):
        for n in files:
            p = os.path.join(root, n)
            outputs[os.path.relpath(p, gen).replace('\\', '/')] = sha256(p)
    combined = hashlib.sha256(''.join(f'{k} {v}\n' for k, v in sorted(outputs.items())).encode()).hexdigest()

    define = re.compile(r'^DEFINE_REX_FUNC\(sub_[0-9A-Fa-f]+\)', re.M)
    functions = 0
    for n in os.listdir(gen):
        if n.startswith('renut_recomp') and n.endswith('.cpp'):
            functions += len(define.findall(open(os.path.join(gen, n), encoding='utf-8', errors='replace').read()))

    stubs = sorted(set(re.findall(r'Function (0x[0-9A-F]+) has no blocks', codegen_log)))
    no_region = sorted(set(re.findall(r'function (0x[0-9A-F]+) not in any code region', codegen_log)))

    xex = os.path.join(app, 'assets', 'default.xex')
    gen_exe = os.path.join(ult, 'sdk', 'out', 'win-amd64', 'rexglue.exe')
    base_commit = open(os.path.join(repo, 'tools', 'ultimate', 'BASE_COMMIT')).read().split()[0]
    head = git(repo, 'rev-parse', 'HEAD')
    dirty = git(repo, 'status', '--porcelain', '--untracked-files=no', '--', 'ultimate', 'tools/ultimate')

    manifest = {
        'what': 'one full generation of ultimate/app/generated: codegen + tools/post_codegen.py on an empty folder',
        'when_utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'repository_commit': head,
        'repository_changes_in_ultimate_or_tools_ultimate': (dirty or '').splitlines(),
        'base_commit': base_commit,
        'generator': {'path': 'ultimate/sdk/out/win-amd64/rexglue.exe', 'sha256': sha256(gen_exe),
                      'version': (re.search(r'ReXGlue (v\S+)', codegen_log) or [None, None])[1]},
        'game_executable': {'sha256': sha256(xex) if os.path.isfile(xex) else None, **xex_info},
        'environment': environment(a.logs),
        'inputs_sha256': input_hashes,
        'codegen': {'written': int(m[1]), 'unchanged': int(m[2]), 'deleted': int(m[3]),
                    'stub_functions': stubs, 'not_in_code_region': no_region},
        'functions_defined': functions,
        'post_codegen_steps': [l.rstrip().replace(repo + os.sep, '<repo>' + os.sep).replace(repo, '<repo>')
                               for l in post_log.splitlines() if l.strip()],
        'outputs': {'files': len(outputs), 'combined_sha256': combined, 'sha256': dict(sorted(outputs.items()))},
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(manifest, f, indent=1)
        f.write('\n')
    print(f'generation_manifest: {len(outputs)} files, {functions} functions, combined {combined[:16]}, '
          f'{len(manifest["post_codegen_steps"])} step lines -> {a.out}')


if __name__ == '__main__':
    main()
