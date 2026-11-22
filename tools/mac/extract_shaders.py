#!/usr/bin/env python3
"""Extracts the shader containers of Fable II's shader bank (data/Shaders/ShadersRelease.sbk) as <hash>.bin files.

    extract_shaders.py --bank <ShadersRelease.sbk> --out <folder> [--key <text>] [--check]
    extract_shaders.py --self-test

Each file is one shader container exactly as the game's renderer hashes it at run time (the virtual part, then the
physical part), named by that hash: the 16 lowercase hex digits of XXH3-64 (seed 0), the key the renderer's shader
cache is looked up by. XenosRecomp's directory mode reads these files as they are (the build's built-in shader cache,
cmake/native_renderer.cmake).

The bank is the game's own data, so the files stay on the player's machine: <folder> is shader_dump/ in the clone,
which is git-ignored.

The bank file (reverse engineered from the game's loader), all big-endian:
    "ShaderBankFile", u32 version (3), u8 names-are-strings, then the constant groups, the parameter names and the
    shader table (the parts this tool skips, but must walk over to find the data), then
    u32 M, M x u32 decompressed size, M x u32 offset, u32 K, K bytes of blob.
    A blob chunk is u32 length + a zlib stream (no adler trailer: a raw decompressor is used) that holds
    u32 header length (20), 4 x u32 entry offsets (from the end of the header), 4 entries.
    An entry is: u32 n, n x 16 bytes; u32 n, n x 16 bytes; 4 x u64; u32 container size; the container.
    A container starts with a header whose flags are 0x102A11xx (the low byte is 0 for a pixel shader, 1 for a
    vertex shader), u32 virtual size and u32 physical size at offsets 4 and 8.

With --key the work is skipped when the folder already holds this bank's files for the same key (bank.key and
bank.list in <folder>). Only the files it made itself (bank.list) are ever replaced or removed; other .bin files in
the folder (shaders captured by fable2_shader_dump_dir) are left alone. bank.list names each file with "+" (made by
this tool) or "=" (already there with the same content: never removed by it); it is written before the files, with
the files of the earlier bank still in it, so a run that stops half way is cleaned up by the next one.

--check only says whether the work is needed. Exit status 0: the files are there (the last line of the output is a
JSON object: state "extracted" or "up_to_date", containers, and for "extracted" pixel and vertex; with --check the
state is "up_to_date" or "needed"); 1: the bank could not be read (a message on stderr).

XXH3-64 is written out here because Python's standard library has no XXH3; the 192-byte default secret below is the
one of xxHash (BSD 2-Clause, Copyright (C) 2012-2023 Yann Collet), and the function is checked against the library's
own results (--self-test).
"""
import argparse
import json
import os
import shutil
import struct
import sys
import zlib

MASK = (1 << 64) - 1

# xxHash's default secret (XXH3_kSecret), data only.
SECRET = bytes([
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
])
P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
P64_4, P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
MX1, MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25


def _r32(b, i):
    return int.from_bytes(b[i:i + 4], "little")


def _r64(b, i):
    return int.from_bytes(b[i:i + 8], "little")


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & MASK


def _fold(a, b):
    p = a * b
    return (p & MASK) ^ (p >> 64)


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = h * P64_2 & MASK
    h ^= h >> 29
    h = h * P64_3 & MASK
    return h ^ (h >> 32)


def _xxh3_avalanche(h):
    h ^= h >> 37
    h = h * MX1 & MASK
    return h ^ (h >> 32)


def _mix16(data, i, s):
    return _fold(_r64(data, i) ^ _r64(SECRET, s), _r64(data, i + 8) ^ _r64(SECRET, s + 8))


def _accumulate_512(acc, data, i, s):
    for k in range(8):
        v = _r64(data, i + 8 * k)
        key = v ^ _r64(SECRET, s + 8 * k)
        acc[k ^ 1] = (acc[k ^ 1] + v) & MASK
        acc[k] = (acc[k] + (key & 0xFFFFFFFF) * (key >> 32)) & MASK


def xxh3_64(data):
    """XXH3 64-bit hash of `data`, seed 0, default secret."""
    n = len(data)
    if n == 0:
        return _xxh64_avalanche(_r64(SECRET, 56) ^ _r64(SECRET, 64))
    if n <= 3:
        combined = (data[0] << 16) | (data[n >> 1] << 24) | data[n - 1] | (n << 8)
        return _xxh64_avalanche(combined ^ (_r32(SECRET, 0) ^ _r32(SECRET, 4)))
    if n <= 8:
        input64 = _r32(data, n - 4) + (_r32(data, 0) << 32)
        h = input64 ^ (_r64(SECRET, 8) ^ _r64(SECRET, 16))
        h ^= _rotl(h, 49) ^ _rotl(h, 24)
        h = h * MX2 & MASK
        h ^= (h >> 35) + n
        h = h * MX2 & MASK
        return h ^ (h >> 28)
    if n <= 16:
        lo = _r64(data, 0) ^ (_r64(SECRET, 24) ^ _r64(SECRET, 32))
        hi = _r64(data, n - 8) ^ (_r64(SECRET, 40) ^ _r64(SECRET, 48))
        acc = (n + int.from_bytes(lo.to_bytes(8, "little"), "big") + hi + _fold(lo, hi)) & MASK
        return _xxh3_avalanche(acc)
    if n <= 128:
        acc = n * P64_1 & MASK
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(data, 48, 96) + _mix16(data, n - 64, 112)
                acc += _mix16(data, 32, 64) + _mix16(data, n - 48, 80)
            acc += _mix16(data, 16, 32) + _mix16(data, n - 32, 48)
        acc += _mix16(data, 0, 0) + _mix16(data, n - 16, 16)
        return _xxh3_avalanche(acc & MASK)
    if n <= 240:
        acc = n * P64_1 & MASK
        for k in range(8):
            acc += _mix16(data, 16 * k, 16 * k)
        acc = _xxh3_avalanche(acc & MASK)
        for k in range(8, n // 16):
            acc += _mix16(data, 16 * k, 16 * (k - 8) + 3)
        acc += _mix16(data, n - 16, 136 - 17)
        return _xxh3_avalanche(acc & MASK)
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    stripes_per_block = (len(SECRET) - 64) // 8
    block = 64 * stripes_per_block
    blocks = (n - 1) // block
    for b in range(blocks):
        for s in range(stripes_per_block):
            _accumulate_512(acc, data, b * block + s * 64, s * 8)
        for k in range(8):
            a = acc[k]
            a ^= a >> 47
            a ^= _r64(SECRET, len(SECRET) - 64 + 8 * k)
            acc[k] = a * P32_1 & MASK
    for s in range(((n - 1) - block * blocks) // 64):
        _accumulate_512(acc, data, blocks * block + s * 64, s * 8)
    _accumulate_512(acc, data, n - 64, len(SECRET) - 64 - 7)
    result = n * P64_1 & MASK
    for k in range(4):
        result += _fold(acc[2 * k] ^ _r64(SECRET, 11 + 16 * k), acc[2 * k + 1] ^ _r64(SECRET, 11 + 16 * k + 8))
    return _xxh3_avalanche(result & MASK)


class BankError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def need(self, n):
        if n < 0 or self.p + n > len(self.d):
            raise BankError("the bank is cut short")

    def u32(self):
        self.need(4)
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def u8(self):
        self.need(1)
        v = self.d[self.p]
        self.p += 1
        return v

    def skip(self, n):
        self.need(n)
        self.p += n

    def cstr(self):
        e = self.d.find(b"\0", self.p)
        if e < 0:
            raise BankError("a name in the bank has no end")
        self.p = e + 1


def read_chunks(data):
    """The decompressed blob chunks of a bank file."""
    if data[:14] != b"ShaderBankFile":
        raise BankError("not a shader bank (no ShaderBankFile header)")
    r = Reader(data)
    r.p = 14
    version = r.u32()
    names_are_strings = r.u8()
    if version != 3:
        raise BankError(f"shader bank version {version}, not the 3 this tool knows")
    for _ in range(r.u32()):            # constant groups
        r.skip(36)
        for _ in range(r.u32()):
            r.cstr()
            r.u32()
    for _ in range(r.u32()):            # parameter names
        r.cstr()
    count = r.u32()                     # shader table
    r.u32()
    for _ in range(count):
        if names_are_strings:
            r.cstr()
        else:
            r.u32()
        r.u8()
        r.u32()
    m = r.u32()
    sizes = [r.u32() for _ in range(m)]
    offsets = [r.u32() for _ in range(m)]
    k = r.u32()
    base = r.p
    r.skip(k)
    chunks = []
    for size, off in zip(sizes, offsets):
        if off + 4 > k:
            raise BankError("a chunk lies outside the bank")
        n = struct.unpack_from(">I", data, base + off)[0]
        if off + 4 + n > k:
            raise BankError("a chunk lies outside the bank")
        try:
            # at most one byte more than the bank says: a chunk that holds more is refused, not expanded
            x = zlib.decompressobj().decompress(data[base + off + 4:base + off + 4 + n], size + 1)
        except zlib.error as e:
            raise BankError(f"a chunk does not decompress ({e})")
        if len(x) != size:
            raise BankError("a chunk is not the size the bank says")
        chunks.append(x)
    return chunks


def containers_of(chunk):
    """The containers (virtual part + physical part) of one chunk, in order."""
    if len(chunk) < 20:
        raise BankError("a chunk is too short")
    header, = struct.unpack_from(">I", chunk, 0)
    if header != 20:
        raise BankError("a chunk has an unknown header")
    starts = struct.unpack_from(">4I", chunk, 4)
    for i, start in enumerate(starts):
        end = header + starts[i + 1] if i < 3 else len(chunk)
        p = header + start
        if not (p <= end <= len(chunk)):
            raise BankError("an entry lies outside its chunk")
        try:
            for _ in range(2):          # two lists of 16-byte records
                n, = struct.unpack_from(">I", chunk, p)
                p += 4 + 16 * n
            p += 32                     # four u64 masks
            size, = struct.unpack_from(">I", chunk, p)
            p += 4
            flags, vsize, psize = struct.unpack_from(">III", chunk, p)
        except struct.error:
            raise BankError("an entry is cut short")
        if p + size != end or (flags & 0xFFFFFF00) != 0x102A1100 or vsize + psize != size:
            raise BankError("an entry is not a shader container")
        blob = chunk[p:p + size]
        if len(blob) != size:
            raise BankError("an entry is cut short")
        yield flags, blob


def extract_all(bank_path):
    with open(bank_path, "rb") as f:
        data = f.read()
    out = []
    for chunk in read_chunks(data):
        for flags, blob in containers_of(chunk):
            out.append((xxh3_64(blob), flags & 0xFF, blob))
    if not out:
        raise BankError("the bank holds no shader containers")
    return out


def read_listing(folder):
    """{name: owned} from bank.list: "+name" is a file this tool made, "=name" one that was there already with the same
    content (never removed by this tool). A line with no prefix is "+"."""
    out = {}
    try:
        with open(os.path.join(folder, "bank.list")) as f:
            for line in f.read().split():
                owned = line[0] != "="
                name = line[1:] if line[0] in "+=" else line
                if name.endswith(".bin") and "/" not in name:
                    out[name] = owned
    except OSError:
        pass
    return out


def put_listing(folder, entries):
    path = os.path.join(folder, "bank.list")
    with open(path + ".tmp", "w") as f:
        f.write("".join(("+" if owned else "=") + name + "\n" for name, owned in sorted(entries.items())))
    os.replace(path + ".tmp", path)


def write_all(items, folder, key):
    os.makedirs(folder, exist_ok=True)
    stamp = os.path.join(folder, "bank.key")
    # A stop in the middle of an earlier run left temporary files: they are ours.
    for name in os.listdir(folder):
        if name.endswith(".tmp") and (name in ("bank.key.tmp", "bank.list.tmp") or
                                      (len(name) == 24 and name.endswith(".bin.tmp") and set(name[:16]) <= set("0123456789abcdef"))):
            try:
                os.remove(os.path.join(folder, name))
            except OSError:
                pass
    if os.path.exists(stamp):
        os.remove(stamp)                # until it is written again, nothing says the folder holds the whole bank
    old = read_listing(folder)
    wanted = {}
    for h, _, blob in items:
        wanted[f"{h:016x}.bin"] = blob
    # What each wanted file is: made by this tool (now or before), or already there with the same content.
    entries = {}
    for name, blob in wanted.items():
        path = os.path.join(folder, name)
        same = False
        try:
            with open(path, "rb") as f:
                same = f.read() == blob
        except OSError:
            pass
        # a file that exists with the right content and that this tool did not make is not listed as its own
        entries[name] = old.get(name, not same)
    # The listing goes first, holding the files of the earlier bank as well: a stop in the middle still leaves a
    # record of everything this tool may have made, for the next run to clean up.
    put_listing(folder, {**{n: o for n, o in old.items() if o}, **entries})
    for name in sorted(old):
        if old[name] and name not in wanted:
            try:
                os.remove(os.path.join(folder, name))
            except FileNotFoundError:
                pass
    for name, blob in wanted.items():
        path = os.path.join(folder, name)
        try:
            with open(path, "rb") as f:
                if f.read() == blob:
                    continue            # as it was: its date stays, so the build does not translate it again
        except OSError:
            pass
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, path)
    put_listing(folder, entries)
    with open(stamp + ".tmp", "w") as f:
        f.write(key)
    os.replace(stamp + ".tmp", stamp)


def up_to_date(folder, key):
    try:
        with open(os.path.join(folder, "bank.key")) as f:
            if f.read() != key:
                return None
    except OSError:
        return None
    names = list(read_listing(folder))
    if not names or not all(os.path.isfile(os.path.join(folder, n)) for n in names):
        return None
    return names


# XXH3_64bits(data, n) from xxHash 0.8.2 for data[i] = (i * 31 + 7) & 255, by length n: every size class of the
# algorithm (0, 1-3, 4-8, 9-16, 17-128, 129-240 and the long form with partial and whole 1 KB blocks).
SELF_TEST_VECTORS = {
    0: 0x2d06800538d394c2, 1: 0x4c5cca45d0f4811f, 2: 0xa7e250c97710ff27, 3: 0x15f7093b173d005c,
    4: 0xdca012f95811b6b9, 5: 0xb290cafc7b254345, 8: 0xdec6a9a43575982e, 9: 0xcbe393399f17ffbd,
    12: 0x46aaf92c7550afa4, 16: 0x7e484c18d74895d0, 17: 0x208bde5ee2bed407, 31: 0xa937652b0119ca11,
    32: 0x03df0ac5255d1446, 33: 0x199a362122d71f46, 64: 0xdd30702ab46b3745, 65: 0xfab36b851b94ce20,
    96: 0xd245cd2541582982, 97: 0x60e3e1d0d43785b3, 128: 0xf92b70eaa21a6288, 129: 0xf8f76713f2bb60fa,
    200: 0x12fdb864685f344d, 240: 0xccc7375172c41f03, 241: 0x0b3b630948ce4a00, 256: 0xec85b75bafe6ca74,
    500: 0xe1e3ce93f69c5043, 1023: 0xf0d330ce2b3300fb, 1024: 0x23bc880ebf0d29c6, 1025: 0xc09fdfbc398c7d82,
    2048: 0x19f6f9c987331373, 2049: 0x5045460f5d85c275, 5000: 0x559fff92c2b7f8ee,
}


def self_test():
    bad = 0
    for n, want in SELF_TEST_VECTORS.items():
        got = xxh3_64(bytes((i * 31 + 7) & 255 for i in range(n)))
        if got != want:
            print(f"XXH3 length {n}: got {got:016x}, want {want:016x}", file=sys.stderr)
            bad += 1
    print(f"self-test: {len(SELF_TEST_VECTORS) - bad} of {len(SELF_TEST_VECTORS)} XXH3 vectors match")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bank", help="the game's data/Shaders/ShadersRelease.sbk")
    ap.add_argument("--out", help="the folder for the <hash>.bin files")
    ap.add_argument("--key", default=None, help="skip the work when the folder holds this bank's files for this key")
    ap.add_argument("--check", action="store_true",
                    help="only say whether the work is needed (with --key): state up_to_date or needed")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.bank or not a.out:
        ap.error("--bank and --out are needed")
    if a.key is not None:
        names = up_to_date(a.out, a.key)
        if names is not None:
            print(json.dumps({"state": "up_to_date", "containers": len(names)}))
            return 0
    if a.check:
        print(json.dumps({"state": "needed"}))
        return 0
    try:
        items = extract_all(a.bank)
    except OSError as e:
        print(f"cannot read the shader bank {a.bank}: {e.strerror or e}", file=sys.stderr)
        return 1
    except BankError as e:
        print(f"{a.bank}: {e}", file=sys.stderr)
        return 1
    try:
        write_all(items, a.out, a.key if a.key is not None else "")
    except OSError as e:
        print(f"cannot write the shader files into {a.out}: {e.strerror or e}", file=sys.stderr)
        return 1
    pixel = sum(1 for _, kind, _ in items if kind == 0)
    print(json.dumps({"state": "extracted", "containers": len(items), "pixel": pixel, "vertex": len(items) - pixel}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
