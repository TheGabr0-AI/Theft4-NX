"""Readers for GTA IV's Xbox 360 data, pure Python (no game data in this file).

- AES-256 decryption (ECB), as the game's RPF2 table of contents needs it (16 passes);
- the RPF2 archive (the layout of glue/rexglue-sdk-main/gta4-recomp/src/install/gta4_rpf_extractor.cpp);
- XXH3-64 with seed 0 and the default secret: the key gta4_native finds a shader container by
  (tools/pc-eu-base/native_hooks.cpp LogShaderHash, RegisterNativeShader).

The AES key is the user's own file (LibertyRecompLib/aes_key.bin, 32 bytes): never in the repository.
"""

import struct
import zlib

# ---------------------------------------------------------------------------------------------------------------
# AES-256, decryption only

_SBOX = [0] * 256
_INV_SBOX = [0] * 256


def _init_sbox():
    p = q = 1
    first = True
    while first or p != 1:
        first = False
        # p *= 3
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        # q /= 3
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) & 0xFF ^ ((q << 2) | (q >> 6)) & 0xFF ^ ((q << 3) | (q >> 5)) & 0xFF \
            ^ ((q << 4) | (q >> 4)) & 0xFF
        x = (x ^ 0x63) & 0xFF
        _SBOX[p] = x
    _SBOX[0] = 0x63
    for i, v in enumerate(_SBOX):
        _INV_SBOX[v] = i


_init_sbox()


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xtime(a)
        b >>= 1
    return r


_M9 = [_gmul(i, 9) for i in range(256)]
_M11 = [_gmul(i, 11) for i in range(256)]
_M13 = [_gmul(i, 13) for i in range(256)]
_M14 = [_gmul(i, 14) for i in range(256)]


def _ror(w, r):
    return ((w >> r) | (w << (32 - r))) & 0xFFFFFFFF


_TD0 = [(_M14[_INV_SBOX[x]] << 24) | (_M9[_INV_SBOX[x]] << 16) | (_M13[_INV_SBOX[x]] << 8) | _M11[_INV_SBOX[x]]
        for x in range(256)]
_TD1 = [_ror(w, 8) for w in _TD0]
_TD2 = [_ror(w, 16) for w in _TD0]
_TD3 = [_ror(w, 24) for w in _TD0]


def _expand_key_words(key):
    nk = len(key) // 4
    nr = nk + 6
    w = [int.from_bytes(key[4 * i:4 * i + 4], 'big') for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = w[i - 1]
        if i % nk == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (_SBOX[t >> 24] << 24) | (_SBOX[(t >> 16) & 255] << 16) | (_SBOX[(t >> 8) & 255] << 8) \
                | _SBOX[t & 255]
            t ^= rcon << 24
            rcon = _xtime(rcon)
        elif nk > 6 and i % nk == 4:
            t = (_SBOX[t >> 24] << 24) | (_SBOX[(t >> 16) & 255] << 16) | (_SBOX[(t >> 8) & 255] << 8) \
                | _SBOX[t & 255]
        w.append(w[i - nk] ^ t)
    return w, nr


class AesDecryptor:
    """AES-128/256 decryption (the T-table form of rijndael-alg-fst.c, which the repository's tools use)."""

    def __init__(self, key):
        key = bytes(key)
        if len(key) not in (16, 32):
            raise ValueError('AES key of 16 or 32 bytes expected')
        w, nr = _expand_key_words(key)
        rounds = [w[4 * r:4 * r + 4] for r in range(nr + 1)][::-1]
        for r in range(1, nr):
            rounds[r] = [_TD0[_SBOX[k >> 24]] ^ _TD1[_SBOX[(k >> 16) & 255]] ^ _TD2[_SBOX[(k >> 8) & 255]]
                         ^ _TD3[_SBOX[k & 255]] for k in rounds[r]]
        self.rk = rounds
        self.nr = nr

    def decrypt_block(self, block):
        s0, s1, s2, s3 = struct.unpack('>4I', block)
        rk = self.rk
        k = rk[0]
        s0 ^= k[0]
        s1 ^= k[1]
        s2 ^= k[2]
        s3 ^= k[3]
        T0, T1, T2, T3 = _TD0, _TD1, _TD2, _TD3
        for r in range(1, self.nr):
            k = rk[r]
            s0, s1, s2, s3 = (
                T0[s0 >> 24] ^ T1[(s3 >> 16) & 255] ^ T2[(s2 >> 8) & 255] ^ T3[s1 & 255] ^ k[0],
                T0[s1 >> 24] ^ T1[(s0 >> 16) & 255] ^ T2[(s3 >> 8) & 255] ^ T3[s2 & 255] ^ k[1],
                T0[s2 >> 24] ^ T1[(s1 >> 16) & 255] ^ T2[(s0 >> 8) & 255] ^ T3[s3 & 255] ^ k[2],
                T0[s3 >> 24] ^ T1[(s2 >> 16) & 255] ^ T2[(s1 >> 8) & 255] ^ T3[s0 & 255] ^ k[3])
        k = rk[self.nr]
        S = _INV_SBOX
        return struct.pack(
            '>4I',
            ((S[s0 >> 24] << 24) | (S[(s3 >> 16) & 255] << 16) | (S[(s2 >> 8) & 255] << 8) | S[s1 & 255]) ^ k[0],
            ((S[s1 >> 24] << 24) | (S[(s0 >> 16) & 255] << 16) | (S[(s3 >> 8) & 255] << 8) | S[s2 & 255]) ^ k[1],
            ((S[s2 >> 24] << 24) | (S[(s1 >> 16) & 255] << 16) | (S[(s0 >> 8) & 255] << 8) | S[s3 & 255]) ^ k[2],
            ((S[s3 >> 24] << 24) | (S[(s2 >> 16) & 255] << 16) | (S[(s1 >> 8) & 255] << 8) | S[s0 & 255]) ^ k[3])

    def decrypt_ecb(self, data, passes=1):
        out = bytes(data)
        n = len(out) - len(out) % 16
        for _ in range(passes):
            out = b''.join(self.decrypt_block(out[o:o + 16]) for o in range(0, n, 16)) + out[n:]
        return out

    def decrypt_cbc(self, data, iv=bytes(16)):
        """CBC with the chain carried in `iv` (a list holding the bytes, updated in place when given one)."""
        chain = iv[0] if isinstance(iv, list) else iv
        parts = []
        dec = self.decrypt_block
        for o in range(0, len(data) - len(data) % 16, 16):
            c = data[o:o + 16]
            parts.append((int.from_bytes(dec(c), 'big') ^ int.from_bytes(chain, 'big')).to_bytes(16, 'big'))
            chain = c
        if isinstance(iv, list):
            iv[0] = chain
        return b''.join(parts)


Aes256Decryptor = AesDecryptor  # the RPF2 key is 32 bytes


# ---------------------------------------------------------------------------------------------------------------
# RPF2

RPF2_MAGIC = 0x32465052
TOC_OFFSET = 0x800


class RpfEntry:
    __slots__ = ('index', 'name', 'path', 'is_directory', 'is_resource', 'is_compressed', 'content_index',
                 'content_count', 'size', 'archived_size', 'offset', 'word3')

    def __repr__(self):
        return f'RpfEntry({self.path!r}, size={self.size}, compressed={self.is_compressed})'


class RpfArchive:
    """An RPF2 archive in memory (`data`: the whole file, or the bytes of a nested archive)."""

    def __init__(self, data, key, name='archive'):
        self.name = name
        self.data = data
        magic, toc_size, entry_count, _unknown, encrypted = struct.unpack_from('<5I', data, 0)
        if magic != RPF2_MAGIC:
            raise ValueError(f'{name}: not an RPF2 archive')
        if entry_count == 0 or entry_count * 16 > toc_size or TOC_OFFSET + toc_size > len(data):
            raise ValueError(f'{name}: malformed table of contents')
        toc = data[TOC_OFFSET:TOC_OFFSET + toc_size]
        if encrypted:
            if key is None:
                raise ValueError(f'{name}: encrypted table of contents and no key')
            toc = Aes256Decryptor(key).decrypt_ecb(toc, passes=16)
        self.encrypted = bool(encrypted)
        names = entry_count * 16
        self.entries = []
        for i in range(entry_count):
            name_off, w1, w2, w3 = struct.unpack_from('<4I', toc, i * 16)
            e = RpfEntry()
            e.index = i
            end = toc.find(b'\0', names + name_off)
            e.name = toc[names + name_off:end].decode('latin-1') if i else '/'
            e.word3 = w3
            e.is_directory = bool(w2 & 0x80000000)
            e.is_resource = e.is_compressed = False
            e.content_index = e.content_count = e.size = e.archived_size = e.offset = 0
            if e.is_directory:
                e.content_index = w2 & 0x7FFFFFFF
                e.content_count = w3 & 0x0FFFFFFF
            else:
                e.size = w1
                e.is_resource = (w3 & 0xC0000000) == 0xC0000000
                if e.is_resource:
                    e.offset = w2 & 0x7FFFFF00
                    e.archived_size = e.size
                else:
                    e.offset = w2
                    e.archived_size = w3 & 0xBFFFFFFF
                    e.is_compressed = bool(w3 & 0x40000000)
                if e.offset + e.archived_size > len(data):
                    raise ValueError(f'{name}: entry {i} out of range')
            self.entries.append(e)
        if not self.entries[0].is_directory:
            raise ValueError(f'{name}: the root is not a directory')
        self.entries[0].path = ''
        self._paths(0, '', set())

    def _paths(self, index, prefix, seen):
        if index in seen:
            raise ValueError(f'{self.name}: directory cycle')
        seen.add(index)
        d = self.entries[index]
        for c in range(d.content_index, d.content_index + d.content_count):
            e = self.entries[c]
            e.path = f'{prefix}{e.name}' if not prefix else f'{prefix}/{e.name}'
            if e.is_directory:
                self._paths(c, e.path, seen)

    def files(self):
        return [e for e in self.entries if not e.is_directory]

    def read(self, entry):
        raw = self.data[entry.offset:entry.offset + entry.archived_size]
        if not entry.is_compressed:
            return raw
        out = zlib.decompressobj(-zlib.MAX_WBITS).decompress(raw)
        if len(out) != entry.size:
            raise ValueError(f'{self.name}: {entry.path}: {len(out)} bytes decompressed, {entry.size} declared')
        return out


# ---------------------------------------------------------------------------------------------------------------
# XEX2 image (glue/rexglue-sdk-main/src/system/xex_module.cpp: normal encryption, basic compression only)

# xex_module.cpp xe_xex2_retail_key: the public retail key every XEX2 loader carries.
XEX2_RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                         0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])


def xex2_image(xex):
    """The loaded image of an XEX2 (bytes from its base address), or raise for the forms not handled here."""
    magic, _flags, header_size, _r, security_offset, count = struct.unpack_from('>4sIIIII', xex, 0)
    if magic != b'XEX2':
        raise ValueError('not an XEX2')
    file_format = None
    for i in range(count):
        k, v = struct.unpack_from('>II', xex, 24 + 8 * i)
        if k == 0x3FF:
            file_format = v
    if file_format is None:
        raise ValueError('XEX2 without file format information')
    info_size, encryption, compression = struct.unpack_from('>IHH', xex, file_format)
    if compression != 1:
        raise ValueError(f'XEX2 compression {compression} not handled (basic only)')
    if encryption not in (0, 1):
        raise ValueError(f'XEX2 encryption {encryption} not handled')
    session = AesDecryptor(XEX2_RETAIL_KEY).decrypt_block(xex[security_offset + 0x150:security_offset + 0x160])
    dec = AesDecryptor(session)
    blocks = [struct.unpack_from('>II', xex, file_format + 8 + 8 * n) for n in range((info_size - 8) // 8)]
    out = bytearray()
    p = header_size
    chain = [bytes(16)]
    for data_size, zero_size in blocks:
        data = xex[p:p + data_size]
        out += dec.decrypt_cbc(data, chain) if encryption else data
        out += bytes(zero_size)
        p += data_size
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------
# Xenos shader containers (the guest blobs D3DDevice_CreateVertexShader / CreatePixelShader take)

CONTAINER_MAGIC_PS = 0x102A1100
CONTAINER_MAGIC_VS = 0x102A1101


def find_containers(data):
    """Every Xenos shader container in `data`: (offset, stage 'vs'|'ps', bytes, reject reason or None).

    A container starts with its magic, then the big-endian virtual and physical sizes (+4, +8): the
    runtime hashes exactly virtual + physical bytes (native_hooks.cpp LogShaderHash). A candidate whose
    sizes do not fit the data is reported with its reason, never dropped silently.
    """
    out = []
    pos = 0
    while True:
        a = data.find(b'\x10\x2a\x11\x00', pos)
        b = data.find(b'\x10\x2a\x11\x01', pos)
        i = min(x for x in (a, b, len(data)) if x >= 0)
        if i >= len(data):
            return out
        stage = 'vs' if data[i + 3] == 1 else 'ps'
        if i + 12 > len(data):
            out.append((i, stage, b'', 'header past the end of the file'))
        else:
            v, p = struct.unpack_from('>II', data, i + 4)
            if v < 12:
                out.append((i, stage, b'', f'virtual size {v} smaller than the header'))
            elif v + p > len(data) - i:
                out.append((i, stage, b'', f'sizes {v}+{p} past the end of the file'))
            else:
                out.append((i, stage, bytes(data[i:i + v + p]), None))
        pos = i + 4


def container_source(blob):
    """The source path the compiler stored in a container's debug data (or '')."""
    import re
    m = re.search(rb'[A-Za-z]:\\[\x20-\x7e]{4,}?\.updb', blob)
    return m.group().decode('latin-1') if m else ''


# ---------------------------------------------------------------------------------------------------------------
# XXH3-64, seed 0, default secret (xxhash.h XXH3_64bits)

_M64 = (1 << 64) - 1
_P32_1, _P32_2, _P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
_P64_1, _P64_2, _P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
_P64_4, _P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
_PMX1, _PMX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25
_SECRET = bytes.fromhex(
    'b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f'
    'cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c'
    '3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8'
    'a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364'
    'eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e'
    '2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e')
_SECRET_U64 = struct.unpack('<24Q', _SECRET)


def _r64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def _r32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _fold(a, b):
    p = a * b
    return (p ^ (p >> 64)) & _M64


def _avalanche(h):
    h ^= h >> 37
    h = (h * _PMX1) & _M64
    return h ^ (h >> 32)


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = (h * _P64_2) & _M64
    h ^= h >> 29
    h = (h * _P64_3) & _M64
    return h ^ (h >> 32)


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64


def _mix16(b, o, so):
    return _fold(_r64(b, o) ^ _r64(_SECRET, so), _r64(b, o + 8) ^ _r64(_SECRET, so + 8))


def xxh3_64(data):
    b = bytes(data)
    n = len(b)
    if n == 0:
        return _xxh64_avalanche(_r64(_SECRET, 56) ^ _r64(_SECRET, 64))
    if n <= 3:
        combined = (b[0] << 16) | (b[n >> 1] << 24) | b[n - 1] | (n << 8)
        return _xxh64_avalanche(combined ^ (_r32(_SECRET, 0) ^ _r32(_SECRET, 4)))
    if n <= 8:
        x = (_r32(b, n - 4) + (_r32(b, 0) << 32)) ^ (_r64(_SECRET, 8) ^ _r64(_SECRET, 16))
        x ^= _rotl(x, 49) ^ _rotl(x, 24)
        x = (x * _PMX2) & _M64
        x ^= (x >> 35) + n
        x = (x * _PMX2) & _M64
        return x ^ (x >> 28)
    if n <= 16:
        lo = _r64(b, 0) ^ (_r64(_SECRET, 24) ^ _r64(_SECRET, 32))
        hi = _r64(b, n - 8) ^ (_r64(_SECRET, 40) ^ _r64(_SECRET, 48))
        swapped = int.from_bytes(lo.to_bytes(8, 'little'), 'big')
        return _avalanche((n + swapped + hi + _fold(lo, hi)) & _M64)
    if n <= 128:
        acc = (n * _P64_1) & _M64
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(b, 48, 96) + _mix16(b, n - 64, 112)
                acc += _mix16(b, 32, 64) + _mix16(b, n - 48, 80)
            acc += _mix16(b, 16, 32) + _mix16(b, n - 32, 48)
        acc += _mix16(b, 0, 0) + _mix16(b, n - 16, 16)
        return _avalanche(acc & _M64)
    if n <= 240:
        acc = (n * _P64_1) & _M64
        for i in range(8):
            acc += _mix16(b, 16 * i, 16 * i)
        acc = _avalanche(acc & _M64)
        for i in range(8, n // 16):
            acc += _mix16(b, 16 * i, 16 * (i - 8) + 3)
        acc += _mix16(b, n - 16, 136 - 17)
        return _avalanche(acc & _M64)
    # long input
    acc = [_P32_3, _P64_1, _P64_2, _P64_3, _P64_4, _P32_2, _P64_5, _P32_1]
    sec = _SECRET_U64

    def stripe(off, sidx_bytes):
        d = struct.unpack_from('<8Q', b, off)
        k = struct.unpack_from('<8Q', _SECRET, sidx_bytes)
        for i in range(8):
            dk = d[i] ^ k[i]
            acc[i ^ 1] = (acc[i ^ 1] + d[i]) & _M64
            acc[i] = (acc[i] + (dk & 0xFFFFFFFF) * (dk >> 32)) & _M64

    stripes_per_block = (192 - 64) // 8
    block_len = 64 * stripes_per_block
    nb_blocks = (n - 1) // block_len
    for blk in range(nb_blocks):
        for s in range(stripes_per_block):
            stripe(blk * block_len + 64 * s, 8 * s)
        for i in range(8):
            a = acc[i]
            a ^= a >> 47
            a ^= sec[16 + i]  # secret + 192 - 64
            acc[i] = (a * _P32_1) & _M64
    nb_stripes = ((n - 1) - block_len * nb_blocks) // 64
    for s in range(nb_stripes):
        stripe(nb_blocks * block_len + 64 * s, 8 * s)
    stripe(n - 64, 192 - 64 - 7)
    result = (n * _P64_1) & _M64
    for i in range(4):
        result += _fold(acc[2 * i] ^ _r64(_SECRET, 11 + 16 * i), acc[2 * i + 1] ^ _r64(_SECRET, 11 + 16 * i + 8))
    return _avalanche(result & _M64)
