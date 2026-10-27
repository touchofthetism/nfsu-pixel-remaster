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
        base_in_body = 0
        while base_in_body < len(body.data) and body.data[base_in_body] == 0x11:
            base_in_body += 1
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
                # Global/INGAMEB.BUN ships its last texture 3 bytes short; the game reads past the chunk.
                if size - len(pixels) > 64:
                    raise ValueError(f"texture {i} short data: {len(pixels)} of {size}")
                pixels = pixels + b"\0" * (size - len(pixels))
            textures.append(Texture(record, fmt, pixels, palette))
        return TexturePack(header, textures, data_header, compressed is not None)

    def serialize(self, file_offset: int = 0) -> bytes:
        """The pack as an uncompressed 0xB3300000 chunk placed at ``file_offset`` in its file (needed
        for the 128-byte alignment of the texture data, which is file-relative)."""
        count = len(self.textures)
        hashes = b"".join(struct.pack("<II", t.hash, 0) for t in self.textures)
        # Lay out palettes first, then textures, 128-byte aligned, exactly as the game's files do.
        # Field 0x34 is the writer's palette cursor when the texture was written: its own palette offset
        # for paletted textures, the running total for the others (reproduced for byte-identical output).
        cursor = 0
        palette_blob = bytearray()
        for texture in self.textures:
            struct.pack_into("<I", texture.record, 0x34, cursor)
            struct.pack_into("<I", texture.record, 0x3C, len(texture.palette))
            if texture.palette:
                palette_blob += texture.palette
                cursor += len(texture.palette)
        cursor = (cursor + ALIGN - 1) // ALIGN * ALIGN
        pixel_blob = bytearray(b"\0" * (cursor - len(palette_blob)))
        for index, texture in enumerate(self.textures):
            struct.pack_into("<I", texture.record, 0x30, cursor)
            struct.pack_into("<I", texture.record, 0x38, len(texture.pixels))
            pixel_blob += texture.pixels
            cursor += len(texture.pixels)
            aligned = (cursor + ALIGN - 1) // ALIGN * ALIGN
            if index + 1 < len(self.textures):                          # the body ends with the last texture
                pixel_blob += b"\0" * (aligned - cursor)
                cursor = aligned
        records = b"".join(bytes(t.record) for t in self.textures)
        formats = b"".join(bytes(t.format) for t in self.textures)
        info = Chunk(TPK_INFO, children=[Chunk(TPK_HEADER, bytes(self.header)), Chunk(TPK_HASHES, hashes),
                                         Chunk(TPK_RECORDS, records), Chunk(TPK_FORMATS, formats)])
        info_bytes = info.serialize()
        # The game's files place the data parent on a 128-byte file boundary and arrange its body so the
        # payload starts 8 bytes past a boundary, followed by 120 bytes of 0x11 filler: the texture data
        # base is then aligned. Both paddings are reproduced here.
        # The game's bundles also put a padding chunk first so the info parent's header sits 16 bytes
        # past a 128-byte boundary (an empty one when already there).
        leading_pad = (16 - (file_offset + 8 + 8)) % ALIGN
        after_info = file_offset + 8 + 8 + leading_pad + len(info_bytes)
        outer_pad = (-(after_info + 8)) % ALIGN
        data_parent_at = after_info + 8 + outer_pad
        after_data_header = data_parent_at + 8 + 8 + len(self.data_header)
        inner_pad = (8 - (after_data_header + 8 + 8)) % ALIGN
        body = b"\x11" * (ALIGN - 8) + bytes(palette_blob) + bytes(pixel_blob)
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


def replace_texture_packs(data: bytes, packs: List[Tuple[int, Chunk]], rebuilt: dict) -> bytes:
    """Rebuilds the file with the given packs (by original file offset) replaced by serialized bytes.
    Padding chunks after a pack are regenerated so the following chunk keeps its 128-byte alignment."""
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
        # Absorb an immediately following padding chunk and re-emit it: unchanged when the pack kept its
        # size, otherwise the smallest padding that keeps the next chunk on the alignment it had.
        if cursor + 8 <= len(data):
            pad_id, pad_size = struct.unpack_from("<II", data, cursor)
            if pad_id == PADDING:
                next_offset = cursor + 8 + pad_size
                if len(new_bytes) == original_size:
                    new_pad = pad_size
                else:
                    alignment = next(a for a in (4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4) if next_offset % a == 0)
                    new_pad = (-(len(out) + 8)) % alignment
                out += struct.pack("<II", PADDING, new_pad) + b"\0" * new_pad
                cursor = next_offset
    out += data[cursor:]
    return bytes(out)
