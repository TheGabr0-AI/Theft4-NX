#!/usr/bin/env python3
"""Theft4-NX: a Switch test package of GTA IV on the Ultimate Base (docs/switch-port/20, U7), checked and hashed.

Reads a finished build folder of tools/ultimate/build_switch.bat and writes:

  <out>/
    COPY_TO_SD/switch/theft4-ultimate/   what goes on the card, in sdmc:/switch/theft4-ultimate/
      theft4-ultimate.nro                  the NRO (GLSL package and shader table built in)
      deko3d_shaders.bin                   the offline DKSH cache (tools/ultimate/gta4_dksh_cache.py), the name the
                                           NRO looks for with uam's default flags
      gta4.toml                            the player's options (app/gta4_switch.toml), as the NRO would write it
    theft4-ultimate.elf, renut.map       for crash addresses and CPU profiles only, NOT for the card
    README.txt, SHA256SUMS.txt, package.json

Checks before writing anything (each failure stops the script, nothing is half written):
  - the build is the commit HEAD names, with no uncommitted change in ultimate/ or tools/ultimate/ (the configure's
    stamp, THEFT4_BUILD_COMMIT; --allow-dirty for a trial package);
  - the build folder's options (CMakeCache.txt): deko3d only (REXGLUE_SWITCH_NVK, PLUME_SWITCH_NVK_LINK off), the
    built-in config on, the release shader mode off (the GLSL package built in), the flavour's guard / in-place options;
  - the NRO: its header, its NACP (name, author, version), the ELF it was made from (same build, newer than nothing);
  - the ELF: the commit string, the GLSL package (g_deko3dGlslPackage, its size = deko3d_glsl.bin's), the shader table
    (g_shaderCacheEntries), the deko3d host shaders (g_<stem>_deko3d), no Vulkan driver (no NVK symbol);
  - the cache: its header and compiler tag (the NRO's kCacheTag, banjo_renut_deko3d.cpp) and, with --dksh-manifest, the
    SHA-256 recorded by U6;
  - the shader inputs the NRO embedded (out/renut_switch) against the U6 manifest.
--manifest writes the same facts, without local paths, for the repository (docs/switch-port/ultimate-manifests).

Nothing of the game's files is copied (the game files stay where the README says); the package folder is under out/,
ignored by git.

    python tools/ultimate/package_switch.py --build ultimate/app/out/switch --out out/console-runs/u7-build
        [--cache ultimate/out/deko3d_shaders/deko3d_shaders.bin] [--dksh-manifest <dksh-u6.json>]
        [--manifest <switch-u7.json>] [--allow-dirty]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from datetime import datetime, timezone

SD_FOLDER = 'switch/theft4-ultimate'
NRO_NAME = 'theft4-ultimate.nro'

README = """Theft4 Ultimate - GTA IV on the Ultimate Base, U7 test package ({flavour} build of {stamp})
==========================================================================================

Built and checked on the PC only (docs/switch-port/20, U7). Nothing in this package has run on a Switch yet: the
first console run is U8.

{nro}   {nro_bytes:,} bytes, SHA-256 {nro_sha}
deko3d_shaders.bin    {cache_bytes:,} bytes, SHA-256 {cache_sha} (compiler tag {tag})
theft4-ultimate.elf / renut.map: crash addresses and CPU profiles only, NOT for the SD card.
Every file: SHA256SUMS.txt; what was checked: package.json.

The SD card: folder sdmc:/{sd}/ (a new folder: the older tree's sdmc:/switch/theft4/ is left as it is)
------------------------------------------------------------------------------------------------------
COPY      COPY_TO_SD/{sd}/*  ->  sdmc:/{sd}/   ({nro}, deko3d_shaders.bin, gta4.toml)
GAME      no copy needed: without a game_root/ folder beside the NRO it reads the older tree's
          sdmc:/switch/theft4/game/ (default.xex there), read only. A game_root/ beside the NRO takes precedence.
MUST NOT BE THERE   deko3d_glsl.bin (it would replace the GLSL package built into the NRO), renut.toml
WRITTEN BY THE NRO  logs/ (the log, logs/rex/rex_stderr.log, rex_crash.log), renut/ (user data and saves),
          pipeline_list.bin, shader_capture/ (any shader the table lacks), capturas/; deko3d_shaders.bin grows
          when the console compiles a variant the cache lacks.

Start
-----
Standard clocks (no sys-clk / Horizon OC profile). Application mode: hbmenu through title takeover (hold R while
starting a game), then {nro}. The older forwarder starts the older NRO, not this one.
The log's first lines name the build: "theft4-nx: GTA IV on the Ultimate Base, build {stamp} ({flavour})".

Bring back to the PC (everything in sdmc:/{sd}/ except the NRO): logs/, pipeline_list.bin, deko3d_shaders.bin,
shader_capture/, capturas/, and a note of what was seen.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fail(msg):
    print(f'package_switch: {msg}', file=sys.stderr)
    sys.exit(1)


def git(repo, *args):
    return subprocess.run(['git', '-C', repo, *args], check=True, capture_output=True, text=True).stdout.strip()


def cmake_cache(build):
    values = {}
    for line in open(os.path.join(build, 'CMakeCache.txt'), encoding='utf-8', errors='replace'):
        m = re.match(r'^([A-Za-z0-9_]+):[A-Z]+=(.*)$', line.rstrip('\n'))
        if m:
            values[m.group(1)] = m.group(2)
    return values


def read_nro(path):
    data = open(path, 'rb').read()
    if len(data) < 0x80 or data[0x10:0x14] != b'NRO0':
        fail(f'{path}: not an NRO (no NRO0 header)')
    size = struct.unpack_from('<I', data, 0x18)[0]
    info = {'bytes': len(data), 'nro_size': size}
    if len(data) >= size + 0x38 and data[size:size + 4] == b'ASET':
        nacp_off, nacp_size = struct.unpack_from('<QQ', data, size + 0x18)
        nacp = data[size + nacp_off:size + nacp_off + nacp_size]
        if len(nacp) >= 0x3070:
            info['name'] = nacp[0:0x200].split(b'\0', 1)[0].decode('utf-8', 'replace')
            info['author'] = nacp[0x200:0x300].split(b'\0', 1)[0].decode('utf-8', 'replace')
            info['version'] = nacp[0x3060:0x3070].split(b'\0', 1)[0].decode('utf-8', 'replace')
        icon_off, icon_size = struct.unpack_from('<QQ', data, size + 0x08)
        info['icon_bytes'] = icon_size
    else:
        fail(f'{path}: no asset section (NACP) after the NRO image')
    return info


def nm_symbols(nm, elf):
    out = subprocess.run([nm, '-S', '--defined-only', elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            syms[parts[3]] = (parts[2], int(parts[1], 16))
        elif len(parts) == 3:
            syms[parts[2]] = (parts[1], 0)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--repo', default='.')
    ap.add_argument('--cache', default='ultimate/out/deko3d_shaders/deko3d_shaders.bin')
    ap.add_argument('--dksh-manifest')
    ap.add_argument('--manifest')
    ap.add_argument('--allow-dirty', action='store_true')
    a = ap.parse_args()

    repo = os.path.abspath(a.repo)
    build = os.path.abspath(a.build)
    ult = os.path.join(repo, 'ultimate')
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        fail(f'{out} exists: choose another folder (nothing is overwritten)')

    # --- the build's identity
    cache = cmake_cache(build)
    stamp = cache.get('THEFT4_BUILD_COMMIT', '')
    flavour = cache.get('THEFT4_BUILD_NAME', '')
    head = git(repo, 'rev-parse', '--short=12', 'HEAD')
    head_full = git(repo, 'rev-parse', 'HEAD')
    dirty_now = subprocess.run(['git', '-C', repo, 'diff', '--quiet', 'HEAD', '--', 'ultimate', 'tools/ultimate']).returncode != 0
    if not stamp:
        fail('the build folder has no THEFT4_BUILD_COMMIT (not configured by tools/ultimate/build_switch.bat)')
    if not a.allow_dirty:
        if stamp != head:
            fail(f'the build is {stamp}, HEAD is {head}: build again (tools/ultimate/build_switch.bat)')
        if dirty_now:
            fail('ultimate/ or tools/ultimate/ have uncommitted changes: commit, build again, then package')
    expect = {'REXGLUE_SWITCH_NVK': 'OFF', 'PLUME_SWITCH_NVK_LINK': 'OFF', 'KATAMARI_BUILTIN_CONFIG': 'ON',
              'BANJO_RENUT_RELEASE_SHADERS': 'OFF', 'PLUME_DEKO3D': 'ON', 'REXGLUE_PLATFORM_SWITCH': 'ON'}
    if flavour == 'guard':
        expect.update({'SASRT_HOT_GUARD_FORMS': 'ON', 'SASRT_HOT_IN_PLACE': 'OFF'})
    elif flavour == 'everyday':
        expect.update({'SASRT_HOT_GUARD_FORMS': 'OFF', 'SASRT_HOT_IN_PLACE': 'ON'})
    else:
        fail(f'unknown flavour {flavour!r}')
    for k, v in expect.items():
        if cache.get(k, '').upper() != v:
            fail(f'CMakeCache {k} = {cache.get(k)!r}, expected {v}')
    options = {k: cache.get(k, '') for k in sorted(set(expect) | {
        'CMAKE_BUILD_TYPE', 'BANJO_SWITCH_LTO', 'BANJO_PGO', 'BANJO_GEN_SWITCH_THREAD', 'BANJO_SWITCH_OMIT_FRAME_POINTER',
        'BLUR_FPCR_ONE_MODE', 'KATAMARI_RENUT_HOOKS', 'BANJO_RENUT_RENDERER'})}

    # --- the NRO and its ELF
    nro = os.path.join(build, 'renut.nro')
    elf = os.path.join(build, 'renut')
    if not os.path.isfile(elf) and os.path.isfile(elf + '.elf'):
        elf += '.elf'
    mapf = os.path.join(build, 'renut.map')
    for p in (nro, elf, mapf):
        if not os.path.isfile(p):
            fail(f'missing {p}')
    if os.path.getmtime(nro) < os.path.getmtime(elf):
        fail('the NRO is older than the ELF: the packaging step did not run after the last link')
    nro_info = read_nro(nro)
    if nro_info.get('name') != 'Theft4 Ultimate':
        fail(f'NACP name {nro_info.get("name")!r}: not GTA IV\'s NRO')
    devkitpro = cache.get('DEVKITPRO', 'C:/devkitPro')
    nm = os.path.join(devkitpro, 'devkitA64', 'bin', 'aarch64-none-elf-nm.exe')
    gcc = os.path.join(devkitpro, 'devkitA64', 'bin', 'aarch64-none-elf-g++.exe')
    syms = nm_symbols(nm, elf)
    blob = open(elf, 'rb').read()
    if stamp.encode() not in blob:
        fail(f'the ELF does not hold the commit string {stamp}')
    glsl = os.path.join(ult, 'app', 'out', 'renut_switch', 'deko3d_glsl.bin')
    glsl_size = os.path.getsize(glsl)
    pkg = syms.get('g_deko3dGlslPackage')
    if not pkg or pkg[1] != glsl_size:
        fail(f'g_deko3dGlslPackage {pkg} in the ELF, deko3d_glsl.bin is {glsl_size} bytes')
    if blob.find(open(glsl, 'rb').read(4096)) < 0:
        fail('the ELF does not contain deko3d_glsl.bin\'s bytes')
    table = syms.get('g_shaderCacheEntries')
    if not table:
        fail('no g_shaderCacheEntries (the shader table) in the ELF')
    # static const arrays: internal linkage, so C++ names them _ZL<length>g_<stem>_deko3d (one per including file)
    host = sorted({m.group(1) for s in syms for m in [re.fullmatch(r'(?:_ZL\d+)?(g_\w+_deko3d)', s)] if m})
    if 'g_depth_restore_ps_deko3d' not in host:
        fail(f'no deko3d host shader g_depth_restore_ps_deko3d in the ELF ({len(host)} host shaders)')
    # No Vulkan driver: no libvulkan.a object in the link map, and the ICD entry points are the SDK's "not available"
    # stubs (sdk/src/ui/switch_vulkan_stubs.c), which a build without NVK links instead.
    map_text = open(mapf, encoding='utf-8', errors='replace').read()
    if 'libvulkan.a' in map_text:
        fail('libvulkan.a is in the link map: the Vulkan driver was linked')
    for icd in ('vk_icdGetInstanceProcAddr', 'vk_icdNegotiateLoaderICDInterfaceVersion'):
        m = re.search(r'\.text\.' + icd + r'\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+)', map_text)
        if icd in syms and (not m or 'switch_vulkan_stubs' not in m.group(1)):
            fail(f'{icd} does not come from switch_vulkan_stubs.c ({m.group(1) if m else "not in the map"})')
    # GTA IV's built-in settings compiled in (app/gta4_switch_defaults.toml, CMake's raw string), not Katamari's.
    builtin = open(os.path.join(ult, 'app', 'gta4_switch_defaults.toml'), 'rb').read().replace(b'\r\n', b'\n')
    if builtin not in blob:
        fail('the ELF does not hold app/gta4_switch_defaults.toml as its built-in settings')
    if b'katamari_hand_matrix_copy = true' in blob or b'banjo_renut_game_gamma = false' in blob:
        fail('the ELF holds Katamari\'s built-in settings')
    if b'theft4-nx: GTA IV on the Ultimate Base, build' not in blob:
        fail('the ELF lacks the build line of the log')
    zcull = 'plume::Deko3DInvalidateZcull' in subprocess.run([nm, '-C', '--defined-only', elf], check=True,
                                                             capture_output=True, text=True).stdout
    if not zcull:
        fail('plume::Deko3DInvalidateZcull is not in the ELF')
    gcc_version = subprocess.run([gcc, '--version'], check=True, capture_output=True, text=True).stdout.splitlines()[0]

    # --- the cache
    cache_path = os.path.abspath(os.path.join(repo, a.cache))
    head_bytes = open(cache_path, 'rb').read(24)
    tag_src = open(os.path.join(ult, 'app', 'src', 'renut_engine', 'gpu', 'banjo_renut_deko3d.cpp'), encoding='utf-8').read()
    tag = re.search(r'kCacheTag\[16\]\s*=\s*"([^"]*)"', tag_src).group(1)
    if head_bytes[:4] != b'BDKC' or head_bytes[8:8 + len(tag)] != tag.encode():
        fail(f'{cache_path}: not a DKSH cache with the NRO\'s tag {tag}')
    cache_sha = sha256(cache_path)
    dksh = None
    if a.dksh_manifest:
        dksh = json.load(open(a.dksh_manifest, encoding='utf-8'))
        if dksh['cache']['sha256'] != cache_sha:
            fail(f'the cache is {cache_sha[:16]}, the DKSH manifest records {dksh["cache"]["sha256"][:16]}')
        if dksh['package']['deko3d_glsl_bin_sha256'] != sha256(glsl):
            fail('deko3d_glsl.bin differs from the DKSH manifest\'s')
        table_cpp = os.path.join(ult, 'app', 'out', 'renut_switch', 'shader_cache_switch.cpp')
        if dksh['package']['shader_cache_switch_cpp_sha256'] != sha256(table_cpp):
            fail('shader_cache_switch.cpp differs from the DKSH manifest\'s')

    # --- write the package
    sd = os.path.join(out, 'COPY_TO_SD', *SD_FOLDER.split('/'))
    os.makedirs(sd)
    shutil.copy2(nro, os.path.join(sd, NRO_NAME))
    shutil.copy2(cache_path, os.path.join(sd, 'deko3d_shaders.bin'))
    shutil.copy2(os.path.join(ult, 'app', 'gta4_switch.toml'), os.path.join(sd, 'gta4.toml'))
    shutil.copy2(elf, os.path.join(out, 'theft4-ultimate.elf'))
    shutil.copy2(mapf, os.path.join(out, 'renut.map'))

    readme = README.format(flavour=flavour, stamp=stamp, sd=SD_FOLDER, nro=NRO_NAME, tag=tag,
                           nro_bytes=nro_info['bytes'], nro_sha=sha256(nro), cache_bytes=os.path.getsize(cache_path),
                           cache_sha=cache_sha)
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(readme)

    files = {}
    for d, _, fs in os.walk(out):
        for f in fs:
            p = os.path.join(d, f)
            files[os.path.relpath(p, out).replace('\\', '/')] = {'bytes': os.path.getsize(p), 'sha256': sha256(p)}
    record = {
        'what': 'Theft4-NX Switch test package, GTA IV on the Ultimate Base (docs/switch-port/20, U7)',
        'when_utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'repository_commit': head_full,
        'build_commit_stamp': stamp,
        'uncommitted_changes': dirty_now,
        'flavour': flavour,
        'compiler': gcc_version,
        'cmake_options': options,
        'nro': nro_info,
        'elf_checks': {
            'commit_string': True, 'glsl_package_bytes': pkg[1], 'shader_table_symbol': True,
            'deko3d_host_shaders': len(host), 'vulkan_driver_symbols': 0, 'zcull_invalidate': True},
        'dksh_cache': {'tag': tag, 'sha256': cache_sha, 'bytes': os.path.getsize(cache_path),
                       'matches_dksh_manifest': bool(dksh)},
        'sd_folder': 'sdmc:/' + SD_FOLDER,
        'files': files,
    }
    with open(os.path.join(out, 'SHA256SUMS.txt'), 'w', encoding='utf-8', newline='\n') as f:
        for rel, v in sorted(files.items()):
            f.write(f'{v["sha256"]} *{rel}  ({v["bytes"]} bytes)\n')
    with open(os.path.join(out, 'package.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(record, f, indent=2)
        f.write('\n')
    if a.manifest:
        with open(a.manifest, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(record, f, indent=2)
            f.write('\n')
    print(f'package_switch: {out}')
    print(f'  {flavour} build of {stamp}; NRO {nro_info["bytes"]:,} bytes, sha256 {files["COPY_TO_SD/" + SD_FOLDER + "/" + NRO_NAME]["sha256"]}')
    print(f'  NACP {nro_info.get("name")!r} / {nro_info.get("author")!r} / {nro_info.get("version")!r}; '
          f'{len(host)} deko3d host shaders; GLSL package {pkg[1]:,} bytes; cache {tag} {cache_sha[:16]}')


if __name__ == '__main__':
    main()
