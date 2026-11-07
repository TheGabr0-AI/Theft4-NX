#!/usr/bin/env python3
"""Format and validator of the deko3d renderer's shader package (gta4_deko3d_runtime.bin).

    python tools/deko3d/gta4_deko3d_runtime_check.py <gta4_deko3d_runtime.bin> [--expect-shaders N --expect-blobs N]

The single place that defines the layout: tools/deko3d/gta4_deko3d_runtime.py (the writer) imports its constants,
and the console's reader (glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/dk_shader_package.cpp,
ShaderPackage::Open) enforces the same rules before it allocates or indexes anything. The tests in
tests/deko3d/ run the C++ reader against files made here, good and broken.

Layout, version 2 (little endian):

    0   "GDKR"
    4   u32 version (2)
    8   u32 shader count
    12  u32 blob count
    16  u32 tag length (<= 4096)
    20  u32 record size (48): written so a reader never has to guess it
    24  u64 file size: the whole file, so a truncated or padded copy is caught at once
    32  the tag (the DKSH cache's), zero padding to 8 bytes                       -> table_at
    table_at   shader count x 48 bytes, sorted by (hash, late), unique:
        0   u64 hash
        8   u8 stage (0 vertex, 1 pixel)   9 u8 late (0 or 1)   10 u8 input count   11 u8 variant count
        12  u32 used texture mask          16 u32 specialization mask
        20  u32 glsl index (0xFFFFFFFF: none)
        24  u32 inputs offset      28 u32 variants offset      32 u32 texture types offset
        36  u32 outputs (pixel: colour output locations written, bit per location; vertex: 0)
        40  u32 flags (run 55; 0 before): bit 0 the pixel shader has a discard, bit 1 it writes gl_FragDepth,
            bit 2 the flags are valid, bit 3 (run 56) the io mask below is set; no other bit
        44  u32 io mask (run 56, with flags bit 3; else 0): vertex shader: the outputs it writes as the constant zero
            (bit per output location, TEXCOORD0-15 then COLOR0-1); pixel shader: the input locations it declares
            Vertex shader variants with value bit 31 (position only) or bit 30 (live outputs only) are the pruned
            ones of tools/deko3d/gta4_deko3d_vs_prune.py.
    extra, at table_at + 48 x shader count (offsets above are absolute, from the start of the file):
        inputs, 4 bytes each: u8 XenosRecomp location (0-39), u8 uam location (0-15), u8 numeric type (0 float,
            1 int, 2 uint), u8 components (1-4);
        variants, 8 bytes each: u32 specialization value, u32 blob index (0xFFFFFFFF: uam failed on the PC);
        texture types, 32 bytes per shader: per fetch slot 0 none, 1 2D, 2 2D array, 3 3D, 4 cube;
    the blob table at the next 256-byte boundary after the extra block: blob count x {u32 offset, u32 size};
    the DKSH blobs, each at a 256-byte boundary (1 byte to 32 MiB) and padded to one; the writer ends the file with
    the last blob, and the declared file size is what pins that: the reader and this validator do not check the tail.
    Not checked by either (they cannot make an access go out of range): that a vertex shader's outputs field is 0,
    that blobs do not alias each other, the range of the glsl index and of the two masks.

Version 1 (shipped before 2026-10-26) declared 48-byte records, stored offsets for 48-byte records, but wrote
44-byte records: the extra block sat 4 x shader count bytes before every offset that pointed at it. A reader of
that file computed an 'extra' block of about 4 GiB (run 36). It is rejected by version.
"""
import argparse
import hashlib
import os
import struct
import sys

MAGIC = b'GDKR'
VERSION = 2
HEADER_BYTES = 32
RECORD_BYTES = 48
BLOB_ENTRY_BYTES = 8
NO_BLOB = 0xFFFFFFFF
MAX_TAG = 4096
MAX_SHADERS = 1 << 20
MAX_BLOBS = 1 << 22
MAX_EXTRA = 64 << 20  # the real one is ~170 KB
MAX_BLOB = 32 << 20  # one DKSH; the real largest is 0.7 MB
MAX_FILE = 0x7FFFFFFF  # the reader's fseek takes a long, and the blob offsets are u32
FLAG_KILLS, FLAG_WRITES_DEPTH, FLAGS_VALID, FLAG_IO_MASK = 1, 2, 4, 8  # dk_shader_package.h PackageShader::kFlag*
FLAGS_KNOWN = FLAG_KILLS | FLAG_WRITES_DEPTH | FLAGS_VALID | FLAG_IO_MASK
VARIANT_POSITION_ONLY, VARIANT_LIVE_OUTPUTS = 0x80000000, 0x40000000  # PackageShader::kVariant*
RECORD = struct.Struct('<QBBBBIIIIIIIII')
assert RECORD.size == RECORD_BYTES, RECORD.size
HEADER = struct.Struct('<4sIIIIIQ')
assert HEADER.size == HEADER_BYTES, HEADER.size


class FormatError(Exception):
    pass


def align(value, to):
    return (value + to - 1) // to * to


def _read(f, offset, size, what):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise FormatError(f'{what}: {size} bytes at {offset} could not be read ({len(data)} read)')
    return data


def check_file(path, expect_shaders=None, expect_blobs=None):
    """Parses `path` with the reader's rules. Returns a dict of counts; raises FormatError at the first violation."""
    actual_size = os.path.getsize(path)
    with open(path, 'rb') as f:
        if actual_size < HEADER_BYTES:
            raise FormatError(f'{actual_size} bytes: shorter than the {HEADER_BYTES}-byte header')
        if actual_size > MAX_FILE:
            raise FormatError(f'{actual_size} bytes: more than the {MAX_FILE} the reader takes')
        magic, version, nshaders, nblobs, taglen, recsize, declared = HEADER.unpack(_read(f, 0, HEADER_BYTES, 'header'))
        if magic != MAGIC:
            raise FormatError(f'magic {magic!r}, expected {MAGIC!r}')
        if version != VERSION:
            raise FormatError(f'version {version}, expected {VERSION}' +
                              (' (version 1 files have 44-byte records under 48-byte offsets: regenerate with '
                               'tools/deko3d/gta4_deko3d_runtime.py)' if version == 1 else ''))
        if recsize != RECORD_BYTES:
            raise FormatError(f'record size {recsize}, expected {RECORD_BYTES}')
        if declared != actual_size:
            raise FormatError(f'file is {actual_size} bytes, its header says {declared}: truncated or wrong copy')
        if taglen > MAX_TAG:
            raise FormatError(f'tag length {taglen} > {MAX_TAG}')
        if nshaders > MAX_SHADERS or nblobs > MAX_BLOBS:
            raise FormatError(f'{nshaders} shaders / {nblobs} blobs exceed the limits {MAX_SHADERS} / {MAX_BLOBS}')
        if expect_shaders is not None and nshaders != expect_shaders:
            raise FormatError(f'{nshaders} shaders, expected {expect_shaders}')
        if expect_blobs is not None and nblobs != expect_blobs:
            raise FormatError(f'{nblobs} blobs, expected {expect_blobs}')
        table_at = align(HEADER_BYTES + taglen, 8)
        extra_at = table_at + RECORD_BYTES * nshaders
        if extra_at > actual_size:
            raise FormatError(f'the shader table ends at {extra_at}, past the end of the file ({actual_size})')
        tag = _read(f, HEADER_BYTES, taglen, 'tag')
        table = _read(f, table_at, RECORD_BYTES * nshaders, 'shader table')

        records = [RECORD.unpack_from(table, i * RECORD_BYTES) for i in range(nshaders)]
        extra_end = extra_at
        prev = None
        for i, r in enumerate(records):
            h, stage, late, ninputs, nvariants, used, mask, glsl, inputs_at, variants_at, types_at, outputs, res, pad = r
            if stage > 1 or late > 1:
                raise FormatError(f'shader {i}: stage {stage}, late {late}')
            if (res & ~FLAGS_KNOWN) or (pad and not res & FLAG_IO_MASK):
                raise FormatError(f'shader {i}: reserved/padding fields are not zero')
            if prev is not None and (h, late) <= prev:
                raise FormatError(f'shader {i}: hashes are not strictly ascending by (hash, late)')
            prev = (h, late)
            for what, at, size in (('inputs', inputs_at, 4 * ninputs), ('variants', variants_at, 8 * nvariants),
                                   ('texture types', types_at, 32)):
                if at < extra_at or at + size > actual_size:
                    raise FormatError(f'shader {i}: {what} at {at} +{size} outside [{extra_at}, {actual_size}]')
                extra_end = max(extra_end, at + size)
        if extra_end - extra_at > MAX_EXTRA:
            raise FormatError(f'extra block of {extra_end - extra_at} bytes > {MAX_EXTRA}')
        extra = _read(f, extra_at, extra_end - extra_at, 'extra block')

        total_variants = missing = 0
        blob_refs = set()
        for i, r in enumerate(records):
            _, _, _, ninputs, nvariants, _, _, _, inputs_at, variants_at, types_at, _, _, _ = r
            for k in range(ninputs):
                x, u, numeric, comps = extra[inputs_at - extra_at + 4 * k:inputs_at - extra_at + 4 * k + 4]
                if x > 39 or u > 15 or numeric > 2 or not 1 <= comps <= 4:
                    raise FormatError(f'shader {i} input {k}: xenos {x} uam {u} numeric {numeric} components {comps}')
            for k in range(nvariants):
                _, blob = struct.unpack_from('<II', extra, variants_at - extra_at + 8 * k)
                if blob == NO_BLOB:
                    missing += 1
                elif blob >= nblobs:
                    raise FormatError(f'shader {i} variant {k}: blob {blob} >= {nblobs}')
                else:
                    blob_refs.add(blob)
            for t in extra[types_at - extra_at:types_at - extra_at + 32]:
                if t > 4:
                    raise FormatError(f'shader {i}: texture type {t}')
            total_variants += nvariants

        blobs_at = align(extra_end, 256)
        data_at = blobs_at + BLOB_ENTRY_BYTES * nblobs
        if data_at > actual_size:
            raise FormatError(f'the blob table ({nblobs} entries at {blobs_at}) ends past the file ({actual_size})')
        blob_table = _read(f, blobs_at, BLOB_ENTRY_BYTES * nblobs, 'blob table')
        for b in range(nblobs):
            off, size = struct.unpack_from('<II', blob_table, b * BLOB_ENTRY_BYTES)
            if size == 0 or size > MAX_BLOB or off < data_at or off + size > actual_size or off % 256:
                raise FormatError(f'blob {b}: offset {off} size {size} (1 to {MAX_BLOB} bytes, data starts at '
                                  f'{align(data_at, 256)}, file {actual_size})')
        unreferenced = nblobs - len(blob_refs)
    return {
        'path': path, 'bytes': actual_size, 'version': version, 'shaders': nshaders, 'variants': total_variants,
        'variants_without_dksh': missing, 'blobs': nblobs, 'blobs_unreferenced': unreferenced,
        'tag': tag.decode('utf-8', 'replace'), 'table_at': table_at, 'extra_at': extra_at, 'extra_bytes': extra_end - extra_at,
        'blobs_at': blobs_at,
    }


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--expect-shaders', type=int)
    ap.add_argument('--expect-blobs', type=int)
    a = ap.parse_args()
    try:
        info = check_file(a.path, a.expect_shaders, a.expect_blobs)
    except FormatError as e:
        print(f'INVALID {a.path}: {e}')
        return 1
    print(f"OK {a.path}: version {info['version']}, {info['bytes']} bytes, {info['shaders']} shaders, "
          f"{info['variants']} variants ({info['variants_without_dksh']} without DKSH), {info['blobs']} DKSH "
          f"({info['blobs_unreferenced']} unreferenced), extra block {info['extra_bytes']} bytes at {info['extra_at']}, "
          f"blob table at {info['blobs_at']}, tag '{info['tag']}'")
    print(f'sha256 {sha256(a.path)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
