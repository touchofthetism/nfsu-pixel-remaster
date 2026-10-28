"""Need for Speed Underground (2003, PC) texture packs: read, extract and rebuild.

Formats, as found in the game's own files (Global/GLOBALB.BUN, Global/HUDTEX.BIN, FrontEnd/FrontB.lzc,
Tracks/STREAM*.BUN):

* Chunked containers: a stream of {u32 id, u32 size, data}; ids with bit 31 set are parents whose data
  is again a chunk stream; id 0 is padding (the game aligns some payloads to 128 bytes with it).
* ``JDLZ`` compression: whole .lzc files and the per-texture blobs of some standalone packs.
* Texture pack (parent 0xB3300000): info parent 0xB3310000 with
    0x33310001  pack header (124): u32 version, char[28] name, char[64] path, u32 hash, zeros
    0x33310002  n × {u32 hash, u32 0}
    0x33310003  n × {u32 hash, u32 file offset, u32 compressed size, u32 flags, u32 0}; only in packs
                whose textures are stored as JDLZ blobs (standalone .BIN packs)
    0x33310004  n × 124-byte texture records (see ``Texture``)
    0x33310005  n × 32-byte format records {0, 0, paletted, 5, 6, FourCC or D3DFORMAT, 0, 0}
  and data parent 0xB3320000 with 0x33320001 {u32 1, u32 pack hash} and 0x33320002, whose payload is
  0x11 filler up to the next 128-byte file boundary followed by the texture data: all palettes first
  (1024 bytes each), then each texture's mip chain at its 128-byte aligned ``offset``.

Texture compression codes (record byte 0x4A): 0x08 P8 (palette of 256 B8G8R8A8), 0x20 A8R8G8B8,
0x22 DXT1, 0x24 DXT3, 0x26 DXT5. The record keeps the engine's per-texture rendering flags, which a
replacement preserves.

This module is deliberately dependency free (stdlib only) so it runs anywhere, including CI.
"""
from __future__ import annotations

import dataclasses
import struct
from typing import Iterable, List, Optional, Tuple

TPK = 0xB3300000
TPK_INFO = 0xB3310000
TPK_HEADER = 0x33310001
TPK_HASHES = 0x33310002
TPK_COMPRESSED = 0x33310003
TPK_RECORDS = 0x33310004
TPK_FORMATS = 0x33310005
TPK_DATA = 0xB3320000
TPK_DATA_HEADER = 0x33320001
TPK_DATA_BODY = 0x33320002
PADDING = 0x00000000

COMP_P8, COMP_A8R8G8B8, COMP_DXT1, COMP_DXT3, COMP_DXT5 = 0x08, 0x20, 0x22, 0x24, 0x26
D3DFMT_A8R8G8B8, D3DFMT_P8 = 21, 41
FOURCC = {COMP_DXT1: b"DXT1", COMP_DXT3: b"DXT3", COMP_DXT5: b"DXT5"}
RECORD_SIZE, FORMAT_SIZE, HEADER_SIZE, ALIGN = 124, 32, 124, 128


# ---- Chunks --------------------------------------------------------------------------------------------

@dataclasses.dataclass
class Chunk:
    id: int
    data: bytes = b""
    children: Optional[List["Chunk"]] = None     # parsed for parents; None for leaves

    @property
    def is_parent(self) -> bool:
        return bool(self.id & 0x80000000)

    def serialize(self) -> bytes:
        body = self.data if self.children is None else b"".join(c.serialize() for c in self.children)
        return struct.pack("<II", self.id, len(body)) + body

    def find(self, chunk_id: int) -> Optional["Chunk"]:
        for child in self.children or []:
            if child.id == chunk_id:
                return child
        return None


def parse_chunks(data: bytes, start: int = 0, end: Optional[int] = None, parents_to_expand: Optional[set] = None) -> List[Chunk]:
    """Parses a chunk stream. Parents are expanded recursively, except ids listed as opaque."""
    end = len(data) if end is None else end
    chunks: List[Chunk] = []
    offset = start
    while offset + 8 <= end:
        chunk_id, size = struct.unpack_from("<II", data, offset)
        body_start, body_end = offset + 8, offset + 8 + size
        if body_end > end:
            raise ValueError(f"chunk {chunk_id:08x} at {offset} runs past the end")
        if chunk_id & 0x80000000 and (parents_to_expand is None or chunk_id in parents_to_expand):
            chunks.append(Chunk(chunk_id, b"", parse_chunks(data, body_start, body_end, parents_to_expand)))
        else:
            chunks.append(Chunk(chunk_id, data[body_start:body_end]))
        offset = body_end
    return chunks


def iter_chunks(chunks: Iterable[Chunk]):
    for chunk in chunks:
        yield chunk
        if chunk.children:
            yield from iter_chunks(chunk.children)


# ---- JDLZ --------------------------------------------------------------------------------------------------

def jdlz_decompress(data: bytes) -> bytes:
    """EA's JDLZ (LZ77 variant) used by .lzc files and compressed texture blobs."""
    if data[:4] != b"JDLZ":
        raise ValueError("not JDLZ")
    out_size = struct.unpack_from("<I", data, 8)[0]
    out = bytearray(out_size)
    in_pos, out_pos = 16, 0
    flags1, flags2 = 1, 1
    while in_pos < len(data) and out_pos < out_size:
        if flags1 == 1:
            flags1 = data[in_pos] | 0x100
            in_pos += 1
        if flags2 == 1:
            flags2 = data[in_pos] | 0x100
            in_pos += 1
        if flags1 & 1:
            if flags2 & 1:   # long back-reference
                length = (data[in_pos + 1] | ((data[in_pos] & 0xF0) << 4)) + 3
                distance = (data[in_pos] & 0x0F) + 1
            else:            # short back-reference
                distance = (data[in_pos + 1] | ((data[in_pos] & 0xE0) << 3)) + 17
                length = (data[in_pos] & 0x1F) + 3
            in_pos += 2
            for _ in range(length):
                if out_pos >= out_size:
                    break
                out[out_pos] = out[out_pos - distance]
                out_pos += 1
            flags2 >>= 1
        else:
            if out_pos < out_size:
                out[out_pos] = data[in_pos]
                out_pos += 1
                in_pos += 1
        flags1 >>= 1
    return bytes(out)


def is_jdlz(data: bytes) -> bool:
    return data[:4] == b"JDLZ"


class JdlzError(ValueError):
    """A JDLZ stream Speed.exe's decoder would not finish cleanly."""


def jdlz_game_decode(data: bytes, start: int = 0) -> Tuple[bytes, int]:
    """Decodes the blob at ``data[start:]`` exactly as Speed.exe does (sub_58D190) and returns
    ``(raw, consumed)``, where ``consumed`` is the number of stream bytes the decoder read.

    The game's decoder differs from the lenient ``jdlz_decompress`` in three ways that matter for anything
    we write: it loops on the header's packed size (``ebx = packed - 0x12``), it reads the next flags1 byte
    the moment 8 tokens are used up and the next flags2 byte the moment 8 references are used up (even
    when no token follows), and its back-reference copies are not bounded by the output size. A stream
    whose final token completes a flag group therefore makes it read one byte past the blob and run on
    with a negative count, copying garbage over whatever follows the destination buffer in the heap.
    EA's streams always end with a dummy literal token (flag bit 0 plus one data byte that is counted but,
    the output being complete, never read): the decoder leaves exactly one unread byte in every stock blob.

    Raises ``JdlzError`` for anything the game's decoder would get wrong: a read past the blob, a count
    that goes negative, a copy past the output, a reference before any output, or an incomplete output.
    """
    if data[start:start + 4] != b"JDLZ" or data[start + 4] != 2:
        raise JdlzError("not a JDLZ version 2 stream")
    raw_size, packed = struct.unpack_from("<II", data, start + 8)
    blob = data[start:start + packed]
    if len(blob) != packed:
        raise JdlzError(f"packed size {packed} runs past the data")
    out = bytearray(raw_size)
    if packed < 0x12:
        raise JdlzError(f"packed size {packed} is smaller than the header")
    flags1, flags2 = blob[0x10] | 0x100, blob[0x11] | 0x100
    pos, remaining, written = 0x12, packed - 0x12, 0
    while remaining:
        if flags1 & 1:
            if pos + 2 > packed:
                raise JdlzError(f"reference token read past the blob at {pos}")
            b1, b0 = blob[pos], blob[pos + 1]
            if flags2 & 1:
                length, distance = (((b1 & 0xF0) << 4) | b0) + 3, (b1 & 0x0F) + 1
            else:
                length, distance = (b1 & 0x1F) + 3, (((b1 & 0xE0) << 3) | b0) + 0x11
            if distance > written:
                raise JdlzError(f"reference reaches before the output at {pos}")
            if written + length > raw_size:
                raise JdlzError(f"copy of {length} at output {written} runs past the {raw_size}-byte output")
            if distance >= length:
                out[written:written + length] = out[written - distance:written - distance + length]
            else:                       # overlapping copy repeats the last `distance` bytes
                period = out[written - distance:written]
                reps, rest = divmod(length, distance)
                out[written:written + length] = period * reps + period[:rest]
            written += length
            flags2 >>= 1
            pos += 2
            remaining -= 2
        else:
            if written < raw_size:
                if pos >= packed:
                    raise JdlzError(f"literal read past the blob at {pos}")
                out[written] = blob[pos]
                written += 1
                pos += 1
            remaining -= 1
        flags1 >>= 1
        if flags1 == 1:
            if pos >= packed:
                raise JdlzError(f"flags1 refill past the blob at {pos}")
            flags1 = blob[pos] | 0x100
            pos += 1
            remaining -= 1
        if flags2 == 1:
            if pos >= packed:
                raise JdlzError(f"flags2 refill past the blob at {pos}")
            flags2 = blob[pos] | 0x100
            pos += 1
            remaining -= 1
        if remaining < 0:
            raise JdlzError("the decoder's byte count went negative (it would run on into the next blob)")
    if written != raw_size:
        raise JdlzError(f"output incomplete: {written} of {raw_size} bytes")
    return bytes(out), pos


def _jdlz_match_length(raw: bytes, earlier: int, pos: int, limit: int) -> int:
    """Common prefix length of raw[earlier:] and raw[pos:], at most limit (32-byte slice compares first)."""
    n = 0
    while n + 32 <= limit and raw[earlier + n:earlier + n + 32] == raw[pos + n:pos + n + 32]:
        n += 32
    while n < limit and raw[earlier + n] == raw[pos + n]:
        n += 1
    return n


def _jdlz_find_match(raw: bytes, pos: int, n: int, head: list, chain: list, depth: int) -> Tuple[int, int]:
    """Longest (length, distance) the format can express at pos, from the hash chain; (0, 0) for none."""
    if pos + 3 > n:
        return 0, 0
    best_len = best_dist = 0
    cand = head[((raw[pos] << 16 | raw[pos + 1] << 8 | raw[pos + 2]) * 0x9E3779B1 >> 8) & 0xFFFF]
    tries = 0
    while cand >= 0 and tries < depth:
        dist = pos - cand
        if dist > 2064:
            break
        limit = min(4098 if dist <= 16 else 34, n - pos)
        if limit > best_len and raw[cand + best_len] == raw[pos + best_len]:
            length = _jdlz_match_length(raw, cand, pos, limit)
            if length >= 3 and length > best_len:
                best_len, best_dist = length, dist
                if length >= limit and dist <= 16:
                    break
        cand = chain[cand]
        tries += 1
    return best_len, best_dist


def jdlz_compress(raw: bytes, depth: int = 32) -> bytes:
    """A JDLZ stream Speed.exe's decoder (``jdlz_game_decode``) turns back into ``raw``.

    LZ77 over hash chains (up to ``depth`` earlier positions per 3-byte key) with one-step lazy matching:
    near references (distance 1..16, up to 4098 bytes) for runs and flat areas, far references (17..2064,
    up to 34 bytes) for repeats, literals otherwise. On the game's own texture data this comes out within
    1 % of EA's compressor (the single-candidate greedy version was 17-70 % larger). The flag bytes are
    placed where the decoder refills them: a new flags1 byte right after every 8th token and a new flags2
    byte right after every 8th reference, whether or not a token follows. The stream ends the way EA's do,
    with a dummy literal token the decoder counts but never reads (its output is already complete), so
    the byte count reaches zero exactly at the end of the blob.
    """
    n = len(raw)
    out = bytearray(18)             # header, then the first flags1 and flags2 bytes
    head = [-1] * 65536
    chain = [-1] * n
    f1_pos, f2_pos = 16, 17
    f1_bits = f2_bits = 0

    def insert(at: int) -> None:
        if at + 3 <= n:
            key = ((raw[at] << 16 | raw[at + 1] << 8 | raw[at + 2]) * 0x9E3779B1 >> 8) & 0xFFFF
            chain[at] = head[key]
            head[key] = at

    pos = 0
    while pos < n:
        length, dist = _jdlz_find_match(raw, pos, n, head, chain, depth)
        insert(pos)
        if 3 <= length < 64 and pos + 1 < n:
            # Lazy step: a clearly longer match one byte on is worth a literal now.
            next_len, next_dist = _jdlz_find_match(raw, pos + 1, n, head, chain, depth)
            if next_len > length + 1 or (next_len == length + 1 and next_dist <= 16 < dist):
                length = 0
        if length >= 3:
            out[f1_pos] |= 1 << f1_bits
            if dist <= 16:
                out[f2_pos] |= 1 << f2_bits
                out.append((((length - 3) >> 8) << 4) | (dist - 1))
                out.append((length - 3) & 0xFF)
            else:
                out.append((((dist - 17) >> 8) << 5) | (length - 3))
                out.append((dist - 17) & 0xFF)
            f2_bits += 1
            for skipped in range(pos + 1, pos + length):
                insert(skipped)
            pos += length
        else:
            out.append(raw[pos])
            pos += 1
        f1_bits += 1
        # The decoder's refill step: it reads these bytes now, before it even checks for another token.
        if f1_bits == 8:
            f1_pos = len(out); out.append(0); f1_bits = 0
        if f2_bits == 8:
            f2_pos = len(out); out.append(0); f2_bits = 0
    # EA's terminator: a dummy literal (flag bit already 0) whose data byte is counted, never read.
    out.append(0)
    f1_bits += 1
    if f1_bits == 8:
        out.append(0)                # the decoder refills flags1 once more after that 8th token
    struct.pack_into("<4sBBHII", out, 0, b"JDLZ", 2, 16, 0, n, len(out))
    return bytes(out)


def verify_jdlz_blobs(data: bytes) -> int:
    """Checks every JDLZ blob in ``data`` with the game's decoder semantics; returns how many it found.

    Each blob must decode completely, leave the decoder's count at zero exactly at its end and, as in
    every stock blob, leave exactly one byte unread (the dummy literal's data byte). Raises ``JdlzError``
    naming the first offending blob.
    """
    count = 0
    offset = data.find(b"JDLZ\x02\x10\x00\x00")
    while offset >= 0:
        packed = struct.unpack_from("<I", data, offset + 12)[0]
        try:
            _, consumed = jdlz_game_decode(data, offset)
        except JdlzError as e:
            raise JdlzError(f"blob at {offset:#x}: {e}") from None
        if consumed != packed - 1:
            raise JdlzError(f"blob at {offset:#x}: the decoder stops {packed - consumed} byte(s) before the end, EA's streams leave exactly 1")
        count += 1
        offset = data.find(b"JDLZ\x02\x10\x00\x00", offset + max(packed, 16))
    return count


# ---- Textures ---------------------------------------------------------------------------------------------

def mip_sizes(width: int, height: int, mips: int, compression: int) -> List[int]:
    sizes = []
    w, h = width, height
    for _ in range(mips):
        if compression in (COMP_DXT1, COMP_DXT3, COMP_DXT5):
            blocks = max(1, (w + 3) // 4) * max(1, (h + 3) // 4)
            sizes.append(blocks * (8 if compression == COMP_DXT1 else 16))
        elif compression == COMP_P8:
            sizes.append(w * h)
        elif compression == COMP_A8R8G8B8:
            sizes.append(w * h * 4)
        else:
            raise ValueError(f"unsupported compression {compression:#x}")
        w, h = max(1, w // 2), max(1, h // 2)
    return sizes


@dataclasses.dataclass
class Texture:
    record: bytearray          # the 124-byte engine record (flags preserved on replacement)
    format: bytearray          # the 32-byte format record
    pixels: bytes              # the mip chain, uncompressed (JDLZ removed)
    palette: bytes = b""       # 1024 bytes for P8

    @property
    def name(self) -> str:
        return bytes(self.record[12:36]).split(b"\0")[0].decode("latin1")

    @property
    def hash(self) -> int:
        return struct.unpack_from("<I", self.record, 0x24)[0]

    @property
    def class_hash(self) -> int:
        return struct.unpack_from("<I", self.record, 0x28)[0]

    @property
    def width(self) -> int:
        return struct.unpack_from("<H", self.record, 0x44)[0]

    @property
    def height(self) -> int:
        return struct.unpack_from("<H", self.record, 0x46)[0]

    @property
    def compression(self) -> int:
        return self.record[0x4A]

    @property
    def mips(self) -> int:
        return self.record[0x4E]

    @property
    def tilable_uv(self) -> int:
        """TilableUV (+0x4F): bit 0 set when the game wraps the texture across its left and right edges, bit 1
        across its top and bottom (clamped otherwise): 3 for road and grass, 1 for walls and the sky panorama, 2
        for road strips and ladders, 0 for signs."""
        return self.record[0x4F]

    def describe(self) -> str:
        return f"{self.name} {self.width}x{self.height} comp {self.compression:#04x} mips {self.mips} {len(self.pixels)} B"

    def to_dds(self) -> bytes:
        """DDS with the same layout the game stores (DXTn or A8R8G8B8; P8 is expanded to A8R8G8B8)."""
        comp = self.compression
        pixels = self.pixels
        if comp == COMP_P8:
            comp = COMP_A8R8G8B8
            pixels = expand_p8(self.pixels, self.palette)
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if self.mips > 1 else 0) | (0x80000 if comp != COMP_A8R8G8B8 else 0x8)
        pitch = (max(1, (self.width + 3) // 4) * (8 if comp == COMP_DXT1 else 16)) if comp != COMP_A8R8G8B8 else self.width * 4
        if comp == COMP_A8R8G8B8:
            pf = struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
        else:
            pf = struct.pack("<II4sIIIII", 32, 0x4, FOURCC[comp], 0, 0, 0, 0, 0)
        caps = 0x1000 | (0x400008 if self.mips > 1 else 0)
        header = struct.pack("<IIIIII", 0x7C, flags, self.height, self.width, pitch, 0) + struct.pack("<I", self.mips) + b"\0" * 44 + pf + struct.pack("<IIIII", caps, 0, 0, 0, 0)
        return b"DDS " + header + pixels

    def replace_with_dds(self, dds: bytes) -> None:
        """Replaces pixels (and size/format fields) from a DDS; the engine's rendering flags stay."""
        width, height, mips, compression, pixels = parse_dds(dds)
        if width & (width - 1) or height & (height - 1):
            raise ValueError("texture sizes must be powers of two")
        expected = sum(mip_sizes(width, height, mips, compression))
        if len(pixels) < expected:
            raise ValueError(f"DDS holds {len(pixels)} bytes, mip chain needs {expected}")
        pixels = pixels[:expected]
        struct.pack_into("<HH", self.record, 0x44, width, height)
        self.record[0x48] = width.bit_length() - 1
        self.record[0x49] = height.bit_length() - 1
        self.record[0x4A] = compression
        self.record[0x4B] = 0
        struct.pack_into("<H", self.record, 0x4C, 0)
        self.record[0x4E] = mips
        base = mip_sizes(width, height, 1, compression)[0]
        # offset/palette offset/size/palette size/base size: offsets are assigned at serialization
        struct.pack_into("<IIIII", self.record, 0x30, 0, 0, expected, 0, base)
        self.pixels = pixels
        self.palette = b""
        struct.pack_into("<IIIIII", self.format, 0, 0, 0, 0, 5, 6, 0)
        if compression == COMP_A8R8G8B8:
            struct.pack_into("<I", self.format, 20, D3DFMT_A8R8G8B8)
        else:
            self.format[20:24] = FOURCC[compression]


def expand_p8(pixels: bytes, palette: bytes) -> bytes:
    table = [palette[i * 4:i * 4 + 4] for i in range(256)]
    return b"".join(table[p] for p in pixels)


def parse_dds(dds: bytes) -> Tuple[int, int, int, int, bytes]:
    if dds[:4] != b"DDS " or len(dds) < 128:
        raise ValueError("not a DDS file")
    size, flags, height, width = struct.unpack_from("<IIII", dds, 4)
    mips = struct.unpack_from("<I", dds, 28)[0] if flags & 0x20000 else 1
    pf_flags = struct.unpack_from("<I", dds, 80)[0]
    fourcc = dds[84:88]
    if pf_flags & 0x4:
        compression = {b"DXT1": COMP_DXT1, b"DXT3": COMP_DXT3, b"DXT5": COMP_DXT5}.get(fourcc)
        if compression is None:
            raise ValueError(f"unsupported DDS FourCC {fourcc!r}")
    else:
        bits = struct.unpack_from("<I", dds, 88)[0]
        if bits != 32:
            raise ValueError("uncompressed DDS must be 32-bit A8R8G8B8")
        compression = COMP_A8R8G8B8
    return width, height, max(1, mips), compression, dds[128:]


# ---- Texture packs ----------------------------------------------------------------------------------------

@dataclasses.dataclass
class TexturePack:
    header: bytearray               # 124 bytes
    textures: List[Texture]
    data_header: bytes              # 33320001 payload (24 bytes)
    compressed_source: bool = False

    @property
    def name(self) -> str:
        return bytes(self.header[4:32]).split(b"\0")[0].decode("latin1")

    @property
    def path(self) -> str:
        return bytes(self.header[32:96]).split(b"\0")[0].decode("latin1")

    @property
    def hash(self) -> int:
        return struct.unpack_from("<I", self.header, 96)[0]

    def find(self, name_or_hash) -> Optional[Texture]:
        for texture in self.textures:
            if texture.name == name_or_hash or texture.hash == name_or_hash:
                return texture
        return None

    @staticmethod
    def parse(chunk: Chunk, whole_file: Optional[bytes] = None, chunk_file_offset: int = 0) -> "TexturePack":
        """Parses a 0xB3300000 chunk. For packs with JDLZ blobs (0x33310003) pass the whole file: the
        blob offsets are absolute file positions."""
        if chunk.id != TPK or chunk.children is None:
            raise ValueError("not a parsed texture pack chunk")
        info = chunk.find(TPK_INFO)
        data = chunk.find(TPK_DATA)
        if info is None or data is None:
            raise ValueError("texture pack without info or data")
        header = bytearray(info.find(TPK_HEADER).data)
        hashes = info.find(TPK_HASHES).data
        records = info.find(TPK_RECORDS).data
        formats = info.find(TPK_FORMATS).data
        count = len(hashes) // 8
        if len(records) != count * RECORD_SIZE or len(formats) != count * FORMAT_SIZE:
            raise ValueError(f"inconsistent texture pack tables ({count} textures)")
        compressed = info.find(TPK_COMPRESSED)
        body = data.find(TPK_DATA_BODY)
        data_header = data.find(TPK_DATA_HEADER).data
        # The texture data base: the body's payload starts with 0x11 filler up to a 128-byte file boundary.
        textures: List[Texture] = []
        if compressed is not None:
            if whole_file is None:
                raise ValueError("compressed pack: the whole file is needed for its absolute offsets")
            entries = {}
            for i in range(len(compressed.data) // 20):
                tex_hash, offset, size, flags, _ = struct.unpack_from("<IIIII", compressed.data, i * 20)
                entries[tex_hash] = (offset, size, flags)
        # The engine aligns the body's payload pointer up to the next 128-byte file boundary; the bytes before
        # it are 0x11 filler. Counting 0x11 bytes instead is wrong when the first texture's data itself starts
        # with 0x11 (Global/INGAMEB.BUN: three such bytes shifted every texture in HD interface packs v1-v3).
        payload_offset = chunk_file_offset + 8
        for child in chunk.children:
            if child is data:
                payload_offset += 8
                for grandchild in data.children:
                    if grandchild is body:
                        break
                    payload_offset += len(grandchild.serialize())
                break
            payload_offset += len(child.serialize())
        payload_offset += 8                     # past the body chunk's own header
        base_in_body = (-payload_offset) % ALIGN
        if base_in_body > len(body.data) or any(b != 0x11 for b in body.data[:base_in_body]):
            raise ValueError(f"texture data body at {payload_offset:#x}: expected {base_in_body} bytes of 0x11 filler")
        for i in range(count):
            record = bytearray(records[i * RECORD_SIZE:(i + 1) * RECORD_SIZE])
            fmt = bytearray(formats[i * FORMAT_SIZE:(i + 1) * FORMAT_SIZE])
            offset, palette_offset, size, palette_size, _base = struct.unpack_from("<IIIII", record, 0x30)
            tex_hash = struct.unpack_from("<I", record, 0x24)[0]
            palette = b""
            if compressed is not None:
                file_offset, blob_size, _flags = entries[tex_hash]
                blob = whole_file[file_offset:file_offset + blob_size]
                pixels = jdlz_decompress(blob) if is_jdlz(blob) else blob
                if palette_size:
                    palette, pixels = pixels[:palette_size], pixels[palette_size:]
            else:
                pixels = body.data[base_in_body + offset:base_in_body + offset + size]
                if palette_size:
                    palette = body.data[base_in_body + palette_offset:base_in_body + palette_offset + palette_size]
            if len(pixels) > size:
                pixels = pixels[:size]
            elif len(pixels) < size:
                raise ValueError(f"texture {i} short data: {len(pixels)} of {size}")
            textures.append(Texture(record, fmt, pixels, palette))
        return TexturePack(header, textures, data_header, compressed is not None)

    def serialize(self, file_offset: int = 0) -> bytes:
        """The pack as a 0xB3300000 chunk placed at ``file_offset`` in its file (needed for the 128-byte
        alignment of the texture data and for the absolute blob offsets of streaming packs). Packs that were
        parsed from a streaming pack (``compressed_source``) are written as one again; the rest uncompressed."""
        count = len(self.textures)
        hashes = b"".join(struct.pack("<II", t.hash, 0) for t in self.textures)
        # Bundles lay out palettes first, then textures, 128-byte aligned, exactly as the game's files do;
        # field 0x34 is the writer's palette cursor there (the texture's own palette offset when it has one,
        # the running total otherwise). Streaming packs count pixel data only, each texture's palette sitting
        # after its aligned pixel data, and field 0x34 is that aligned end (every stock HUDTEX, TSELECT and
        # Cars/*/TEXTURES.BIN record, paletted CIVIC/CELICA/GOLF ones included). Both are reproduced so a
        # re-serialized stock pack has byte-identical info chunks.
        cursor = 0
        palette_blob = bytearray()
        for texture in self.textures:
            struct.pack_into("<I", texture.record, 0x34, cursor)
            struct.pack_into("<I", texture.record, 0x3C, len(texture.palette))
            if texture.palette:
                palette_blob += texture.palette
                cursor += len(texture.palette)
        if self.compressed_source:
            cursor = 0              # streaming packs: palettes travel inside the blobs; offsets count pixel data only
        else:
            cursor = (cursor + ALIGN - 1) // ALIGN * ALIGN
        pixel_blob = bytearray(b"\0" * max(0, cursor - len(palette_blob)))
        for index, texture in enumerate(self.textures):
            struct.pack_into("<I", texture.record, 0x30, cursor)
            struct.pack_into("<I", texture.record, 0x38, len(texture.pixels))
            pixel_blob += texture.pixels
            cursor += len(texture.pixels)
            aligned = (cursor + ALIGN - 1) // ALIGN * ALIGN
            if self.compressed_source:
                struct.pack_into("<I", texture.record, 0x34, aligned)
                aligned += len(texture.palette)                          # the palette follows its pixels
            if index + 1 < len(self.textures):                          # the body ends with the last texture
                pixel_blob += b"\0" * (aligned - cursor)
                cursor = aligned
        records = b"".join(bytes(t.record) for t in self.textures)
        formats = b"".join(bytes(t.format) for t in self.textures)
        # Standalone "streaming" packs (HUDTEX, DYNTEX, TSELECT, MAGAZINE_TEXTURES, Cars/*/TEXTURES.BIN) keep
        # every texture as its own JDLZ blob and carry a 0x33310003 table {hash, absolute file offset,
        # blob size, 0x100, 0} sorted by hash. The engine walks that table when it unloads the pack (the
        # HUD at the end of every race), so a rebuilt streaming pack must stay a streaming pack: without
        # the table the unload dereferences a null entry list. Records keep the uncompressed layout above.
        blobs: List[bytes] = []
        if self.compressed_source:
            order = sorted(range(count), key=lambda i: self.textures[i].hash)
            blobs = [jdlz_compress(self.textures[i].palette + self.textures[i].pixels) for i in order]
        # The game's files place the data parent on a 128-byte file boundary and arrange its body so the
        # payload starts 8 bytes past a boundary, followed by 120 bytes of 0x11 filler: the texture data
        # base is then aligned. Both paddings are reproduced here.
        # The game's bundles also put a padding chunk first so the info parent's header sits 16 bytes
        # past a 128-byte boundary (an empty one when already there).
        leading_pad = (16 - (file_offset + 8 + 8)) % ALIGN
        info_size = 8 + (8 + len(self.header)) + (8 + len(hashes)) + (8 + count * 20 if self.compressed_source else 0) \
            + (8 + len(records)) + (8 + len(formats))
        after_info = file_offset + 8 + 8 + leading_pad + info_size
        outer_pad = (-(after_info + 8)) % ALIGN
        data_parent_at = after_info + 8 + outer_pad
        after_data_header = data_parent_at + 8 + 8 + len(self.data_header)
        inner_pad = (8 - (after_data_header + 8 + 8)) % ALIGN
        body_payload_at = after_data_header + 8 + inner_pad + 8
        info_children = [Chunk(TPK_HEADER, bytes(self.header)), Chunk(TPK_HASHES, hashes)]
        if self.compressed_source:
            table = bytearray()
            blob_at = body_payload_at + (ALIGN - 8)
            for i, blob in zip(order, blobs):
                table += struct.pack("<IIIII", self.textures[i].hash, blob_at, len(blob), 0x100, 0)
                blob_at += len(blob)
            info_children.append(Chunk(TPK_COMPRESSED, bytes(table)))
            body_data = b"".join(blobs)
            body_data += b"\0" * ((-len(body_data)) % 4)
        else:
            body_data = bytes(palette_blob) + bytes(pixel_blob)
        info_children += [Chunk(TPK_RECORDS, records), Chunk(TPK_FORMATS, formats)]
        info = Chunk(TPK_INFO, children=info_children)
        assert len(info.serialize()) == info_size
        body = b"\x11" * (ALIGN - 8) + body_data
        data = Chunk(TPK_DATA, children=[Chunk(TPK_DATA_HEADER, self.data_header), Chunk(PADDING, b"\0" * inner_pad),
                                         Chunk(TPK_DATA_BODY, body)])
        return Chunk(TPK, children=[Chunk(PADDING, b"\0" * leading_pad), info, Chunk(PADDING, b"\0" * outer_pad), data]).serialize()

    def summary(self) -> str:
        lines = [f"pack {self.name!r} path {self.path!r} hash {self.hash:08x} textures {len(self.textures)}"
                 + (" (stored compressed)" if self.compressed_source else "")]
        for texture in self.textures:
            lines.append("  " + texture.describe())
        return "\n".join(lines)


def find_texture_packs(data: bytes) -> List[Tuple[int, Chunk]]:
    """All texture packs in a (decompressed) file with their file offsets, in order."""
    results = []

    def visit(chunks: List[Chunk], base: int):
        offset = base
        for chunk in chunks:
            if chunk.id == TPK:
                results.append((offset, chunk))
            elif chunk.children:
                visit(chunk.children, offset + 8)
            offset += 8 + (len(chunk.data) if chunk.children is None else len(chunk.serialize()) - 8)

    visit(parse_chunks(data), 0)
    return results


def _alignment(offset: int) -> int:
    """The largest power of two up to 4096 that divides ``offset``."""
    return next(a for a in (4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1) if offset % a == 0)


def replace_texture_packs(data: bytes, packs: List[Tuple[int, Chunk]], rebuilt: dict) -> bytes:
    """Rebuilds the file with the given packs (by original file offset) replaced by serialized bytes.
    The chunk after a pack keeps the alignment it had: a padding chunk right after the pack is regenerated
    to the size that does it, and when the next chunk follows the pack directly (texture animations after the
    pack in track sections) a padding chunk is put in front of it when the pack's new end does not land it
    on that alignment."""
    out = bytearray()
    cursor = 0
    for offset, chunk in packs:
        if offset not in rebuilt:
            continue
        original_size = len(chunk.serialize())
        out += data[cursor:offset]
        new_bytes = rebuilt[offset](len(out)) if callable(rebuilt[offset]) else rebuilt[offset]
        out += new_bytes
        cursor = offset + original_size
        if cursor + 8 > len(data):
            continue
        next_id, next_size = struct.unpack_from("<II", data, cursor)
        if next_id == PADDING:
            # Absorb the padding chunk and re-emit it: unchanged when the pack kept its size, otherwise the
            # smallest padding that keeps the next chunk on the alignment it had.
            next_offset = cursor + 8 + next_size
            if len(new_bytes) == original_size or next_offset >= len(data):
                new_pad = next_size          # unchanged size, or the file's trailing pad: keep it as it was
            else:
                new_pad = (-(len(out) + 8)) % _alignment(next_offset)
            out += struct.pack("<II", PADDING, new_pad) + b"\0" * new_pad
            cursor = next_offset
        elif len(new_bytes) != original_size:
            alignment = _alignment(cursor)
            gap = (-len(out)) % alignment
            if gap:
                while gap < 8:                  # a padding chunk is at least its 8-byte header
                    gap += alignment
                out += struct.pack("<II", PADDING, gap - 8) + b"\0" * (gap - 8)
    out += data[cursor:]
    return bytes(out)
