"""Format tests for tools/nfstex/tpk.py on synthetic data (no game files needed)."""
import io
import struct
import unittest

from nfstex import tpk


def make_record(name: str, width: int, height: int, compression: int, mips: int, size: int, base: int, tex_hash: int) -> bytearray:
    record = bytearray(tpk.RECORD_SIZE)
    record[12:12 + len(name)] = name.encode()
    struct.pack_into("<II", record, 0x24, tex_hash, 0x001A93CF)
    struct.pack_into("<IIIII", record, 0x30, 0, 0, size, 0, base)
    struct.pack_into("<HH", record, 0x44, width, height)
    record[0x48], record[0x49], record[0x4A], record[0x4E] = width.bit_length() - 1, height.bit_length() - 1, compression, mips
    struct.pack_into("<HH", record, 0x60, 0x100, 0x100)
    return record


def make_format(fourcc: bytes) -> bytearray:
    fmt = bytearray(tpk.FORMAT_SIZE)
    struct.pack_into("<IIII", fmt, 8, 0, 5, 6, 0)
    fmt[20:24] = fourcc
    return fmt


def synthetic_pack(file_offset: int = 0) -> tpk.TexturePack:
    header = bytearray(tpk.HEADER_SIZE)
    struct.pack_into("<I", header, 0, 4)
    header[4:8] = b"TEST"
    header[32:51] = b"Global\\TestPack.tpk"
    struct.pack_into("<I", header, 96, 0xDEADBEEF)
    textures = []
    # 16x16 DXT1 with 2 mips (16x16=128 B, 8x8=32 B) and a 16x16 P8 with 2 mips + palette.
    dxt1 = make_record("SQUARE", 16, 16, tpk.COMP_DXT1, 2, 160, 128, 0x11111111)
    textures.append(tpk.Texture(dxt1, make_format(b"DXT1"), bytes(range(160)), b""))
    p8 = make_record("PALETTED", 16, 16, tpk.COMP_P8, 2, 320, 256, 0x22222222)
    fmt = make_format(b"\x29\0\0\0")
    struct.pack_into("<I", fmt, 8, 1)
    palette = bytes((i % 256) for i in range(1024))
    textures.append(tpk.Texture(p8, fmt, bytes((i * 7) % 256 for i in range(320)), palette))
    return tpk.TexturePack(header, textures, struct.pack("<II", 1, 0xDEADBEEF) + b"\0" * 16)


class ChunkTests(unittest.TestCase):
    def test_round_trip_and_alignment(self):
        for file_offset in (0, 23424, 1000):
            pack = synthetic_pack()
            data = b"\0" * file_offset + pack.serialize(file_offset)
            packs = tpk.find_texture_packs(data)
            self.assertEqual([file_offset], [o for o, _ in packs])
            parsed = tpk.TexturePack.parse(packs[0][1], data, file_offset)
            self.assertEqual("TEST", parsed.name)
            self.assertEqual(0xDEADBEEF, parsed.hash)
            self.assertEqual([t.pixels for t in pack.textures], [t.pixels for t in parsed.textures])
            self.assertEqual(pack.textures[1].palette, parsed.textures[1].palette)
            # The texture data base sits on a 128-byte file boundary after 120 bytes of 0x11.
            body = packs[0][1].find(tpk.TPK_DATA).find(tpk.TPK_DATA_BODY)
            position = data.find(struct.pack("<II", tpk.TPK_DATA_BODY, len(body.data)))
            self.assertEqual(8, (position + 8) % 128)
            self.assertEqual(b"\x11" * 120, body.data[:120])
            for texture in parsed.textures:
                self.assertEqual(0, struct.unpack_from("<I", texture.record, 0x30)[0] % 128)

    def test_texture_data_may_start_with_filler_bytes(self):
        # Global/INGAMEB.BUN's first texture begins with 0x11 bytes: the data base comes from the 128-byte
        # alignment of the body's payload, never from counting filler (v1-v3 shifted every texture by 3).
        for file_offset in (0, 4096 + 16):
            pack = synthetic_pack()
            pack.textures[0].pixels = b"\x11\x11\x11" + bytes(range(157))
            data = b"\0" * file_offset + pack.serialize(file_offset)
            parsed = tpk.TexturePack.parse(tpk.find_texture_packs(data)[0][1], data, file_offset)
            self.assertEqual(pack.textures[0].pixels, parsed.textures[0].pixels)
            self.assertEqual(pack.textures[1].pixels, parsed.textures[1].pixels)
            self.assertEqual(pack.textures[1].palette, parsed.textures[1].palette)
        # A body whose filler is not where the alignment says is refused rather than guessed at.
        broken = bytearray(synthetic_pack().serialize(0))
        position = broken.find(b"\x11" * 120)
        broken[position] = 0
        with self.assertRaises(ValueError):
            tpk.TexturePack.parse(tpk.find_texture_packs(bytes(broken))[0][1], bytes(broken), 0)

    def test_record_layout_fields_by_pack_kind(self):
        # Bundles: palettes first (field 0x34 = palette cursor), textures 128-byte aligned after them.
        bundle = synthetic_pack()
        data = bundle.serialize(0)
        parsed = tpk.TexturePack.parse(tpk.find_texture_packs(data)[0][1], data, 0)
        square, paletted = parsed.textures
        self.assertEqual((1024, 0, 160, 0), struct.unpack_from("<IIII", square.record, 0x30))      # after the 1 KB palette area
        self.assertEqual((1024 + 256, 0, 320, 1024), struct.unpack_from("<IIII", paletted.record, 0x30))
        # Streaming packs: offsets count pixel data only, 0x34 is the aligned end of the texture's pixels,
        # and a paletted texture's palette follows those pixels (stock CIVIC/CELICA/GOLF TEXTURES.BIN).
        streaming = synthetic_pack()
        streaming.compressed_source = True
        data = streaming.serialize(0)
        parsed = tpk.TexturePack.parse(tpk.find_texture_packs(data)[0][1], data, 0)
        square, paletted = parsed.textures
        self.assertEqual((0, 256, 160, 0), struct.unpack_from("<IIII", square.record, 0x30))
        self.assertEqual((256, 256 + 384, 320, 1024), struct.unpack_from("<IIII", paletted.record, 0x30))
        # Three textures: the one after a paletted texture starts past its palette as well.
        third = tpk.Texture(make_record("THIRD", 16, 16, tpk.COMP_DXT1, 1, 128, 128, 0x33333333), make_format(b"DXT1"), bytes(128), b"")
        streaming.textures.append(third)
        data = streaming.serialize(0)
        parsed = tpk.TexturePack.parse(tpk.find_texture_packs(data)[0][1], data, 0)
        self.assertEqual(256 + 384 + 1024, struct.unpack_from("<I", parsed.textures[2].record, 0x30)[0])
        self.assertEqual([t.pixels for t in streaming.textures], [t.pixels for t in parsed.textures])

    def test_padding_between_packs_is_preserved_or_recomputed(self):
        pack = synthetic_pack()
        first = pack.serialize(0)
        pad = struct.pack("<II", 0, 56) + b"\0" * 56
        tail = struct.pack("<II", 0x12345678, 4) + b"abcd"
        data = first + pad + tail
        packs = tpk.find_texture_packs(data)
        same = tpk.replace_texture_packs(data, packs, {0: lambda base: tpk.TexturePack.parse(packs[0][1], data, 0).serialize(base)})
        self.assertEqual(data, same)
        bigger = tpk.TexturePack.parse(packs[0][1], data, 0)
        bigger.textures[0].pixels = bytes(range(160)) * 2
        struct.pack_into("<I", bigger.textures[0].record, 0x38, 320)
        out = tpk.replace_texture_packs(data, packs, {0: lambda base: bigger.serialize(base)})
        self.assertTrue(out.endswith(tail))
        next_offset = len(out) - len(tail)
        original_next = len(first) + len(pad)
        self.assertEqual(original_next % 8, next_offset % 8)

    def test_dds_export_and_replace(self):
        pack = synthetic_pack()
        dds = pack.textures[0].to_dds()
        width, height, mips, compression, pixels = tpk.parse_dds(dds)
        self.assertEqual((16, 16, 2, tpk.COMP_DXT1), (width, height, mips, compression))
        self.assertEqual(bytes(range(160)), pixels)
        expanded = tpk.parse_dds(pack.textures[1].to_dds())
        self.assertEqual(tpk.COMP_A8R8G8B8, expanded[3])
        self.assertEqual(16 * 16 * 4 + 8 * 8 * 4, len(expanded[4]))
        # Replace with a 32x32 DXT3 chain of 3 levels.
        chain = sum(tpk.mip_sizes(32, 32, 3, tpk.COMP_DXT3))
        header = struct.pack("<IIIIII", 0x7C, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, 32, 32, 128, 0) + struct.pack("<I", 3) + b"\0" * 44
        header += struct.pack("<II4sIIIII", 32, 0x4, b"DXT3", 0, 0, 0, 0, 0) + struct.pack("<IIIII", 0x401008, 0, 0, 0, 0)
        pack.textures[1].replace_with_dds(b"DDS " + header + bytes(chain))
        texture = pack.textures[1]
        self.assertEqual((32, 32, tpk.COMP_DXT3, 3, b""), (texture.width, texture.height, texture.compression, texture.mips, texture.palette))
        self.assertEqual(b"DXT3", bytes(texture.format[20:24]))
        self.assertEqual(0, struct.unpack_from("<I", texture.format, 8)[0])
        data = pack.serialize(0)
        reparsed = tpk.TexturePack.parse(tpk.find_texture_packs(data)[0][1], data, 0)
        self.assertEqual(chain, len(reparsed.textures[1].pixels))
        self.assertEqual("PALETTED", reparsed.textures[1].name)
        with self.assertRaises(ValueError):
            pack.textures[0].replace_with_dds(b"DDS " + header[:8] + struct.pack("<II", 24, 24) + header[16:] + bytes(chain))

    def test_jdlz(self):
        literal = b"JDLZ\x02\x10\x00\x00" + struct.pack("<II", 8, 0) + b"\x00\x00" + b"12345678"
        self.assertEqual(b"12345678", tpk.jdlz_decompress(literal))
        backref = b"JDLZ\x02\x10\x00\x00" + struct.pack("<II", 8, 0) + b"\x10\x01" + b"ABCD" + b"\x03\x01"
        self.assertEqual(b"ABCDABCD", tpk.jdlz_decompress(backref))
        self.assertTrue(tpk.is_jdlz(backref))
        with self.assertRaises(ValueError):
            tpk.jdlz_decompress(b"nope")

    def test_jdlz_game_decode_follows_speed_exe(self):
        # EA's shape: flags1, flags2, 8 literals, the flags1 byte the decoder refills right after the 8th
        # token, then the dummy literal's data byte that it counts but never reads.
        stock = b"JDLZ\x02\x10\x00\x00" + struct.pack("<II", 8, 28) + b"\x00\x00" + b"12345678" + b"\x00" + b"\x00"
        self.assertEqual((b"12345678", 27), tpk.jdlz_game_decode(stock))
        # The pre-fix shape, no terminator: after the 8th token the decoder reads a flags byte past the blob
        # and its count goes negative. This is what overran the heap on the phone (HD packs v1-v5).
        short = b"JDLZ\x02\x10\x00\x00" + struct.pack("<II", 8, 26) + b"\x00\x00" + b"12345678"
        with self.assertRaises(tpk.JdlzError):
            tpk.jdlz_game_decode(short)
        self.assertEqual(b"12345678", tpk.jdlz_decompress(short))      # the lenient reader is happy with it
        # A back-reference is copied without regard to the output size.
        overrun = b"JDLZ\x02\x10\x00\x00" + struct.pack("<II", 6, 25) + b"\x10\x01" + b"ABCD" + b"\x03\x01" + b"\x00"
        with self.assertRaises(tpk.JdlzError):
            tpk.jdlz_game_decode(overrun)
        with self.assertRaises(tpk.JdlzError):
            tpk.jdlz_game_decode(b"nope" * 8)
        self.assertEqual((b"", 18), tpk.jdlz_game_decode(tpk.jdlz_compress(b"")))

    def test_jdlz_compress_round_trips(self):
        import random
        random.seed(7)
        cases = [b"", b"a", b"abc", b"\0" * 5000, bytes(range(256)) * 40, bytes(random.randrange(256) for _ in range(6000)),
                 b"abcabcabcabcabcabd" * 300, bytes(random.choice(b"ab") for _ in range(7000)),
                 bytes(random.randrange(256) for _ in range(3000)) + b"\0" * 4100 + bytes(random.randrange(256) for _ in range(100))]
        # Every token count modulo 8 (random bytes are all literals) and every reference count modulo 8
        # (a 40-byte unit repeated: far references of up to 34 bytes): the ends that tripped the game.
        cases += [bytes(random.randrange(256) for _ in range(n)) for n in range(0, 70)]
        unit = bytes(random.randrange(256) for _ in range(40))
        cases += [unit * m for m in range(1, 60)]
        cases += [b"\0" * n for n in range(1, 40)] + [b"ab" * n for n in range(1, 40)]
        for raw in cases:
            packed = tpk.jdlz_compress(raw)
            self.assertEqual(b"JDLZ\x02\x10\x00\x00", packed[:8])
            self.assertEqual((len(raw), len(packed)), struct.unpack_from("<II", packed, 8))
            self.assertEqual(raw, tpk.jdlz_decompress(packed))
            # The game's decoder finishes with its count at zero and, as with EA's streams, one byte unread.
            self.assertEqual((raw, len(packed) - 1), tpk.jdlz_game_decode(packed), raw[:16])
        self.assertLess(len(tpk.jdlz_compress(b"\0" * 5000)), 40)          # runs use the near, long form
        self.assertLess(len(tpk.jdlz_compress(b"abcabcabcabcabcabd" * 300)), 700)
        # verify_jdlz_blobs walks a file of blobs and rejects the first bad one.
        good = tpk.jdlz_compress(b"x" * 100) + b"\x11" * 5 + tpk.jdlz_compress(bytes(range(64)))
        self.assertEqual(2, tpk.verify_jdlz_blobs(good))
        bad = tpk.jdlz_compress(b"x" * 100)
        bad = bad[:12] + struct.pack("<I", len(bad) - 1) + bad[16:-1]        # terminator dropped, count adjusted
        with self.assertRaises(tpk.JdlzError):
            tpk.verify_jdlz_blobs(good + bad)

    def test_streaming_pack_keeps_table_and_blobs(self):
        # A pack parsed from a stock streaming .BIN (HUDTEX, TSELECT, Cars/*/TEXTURES.BIN) must come back as
        # one: per-texture JDLZ blobs and the 0x33310003 table the engine walks when unloading it.
        for file_offset in (0, 23424):
            pack = synthetic_pack()
            pack.compressed_source = True
            pack.textures.reverse()                      # records need not be hash-sorted; the table is
            data = b"\0" * file_offset + pack.serialize(file_offset)
            packs = tpk.find_texture_packs(data)
            chunk = packs[0][1]
            info = chunk.find(tpk.TPK_INFO)
            self.assertEqual([tpk.TPK_HEADER, tpk.TPK_HASHES, tpk.TPK_COMPRESSED, tpk.TPK_RECORDS, tpk.TPK_FORMATS],
                             [c.id for c in info.children])
            table = info.find(tpk.TPK_COMPRESSED).data
            entries = [struct.unpack_from("<IIIII", table, i * 20) for i in range(len(table) // 20)]
            self.assertEqual([0x11111111, 0x22222222], [e[0] for e in entries])      # sorted by hash
            self.assertEqual(0, entries[0][1] % 128)                                   # first blob on a boundary
            self.assertEqual(entries[0][1] + entries[0][2], entries[1][1])             # contiguous
            self.assertTrue(all(e[3] == 0x100 and e[4] == 0 for e in entries))
            for tex_hash, offset, size, _flags, _zero in entries:
                blob = data[offset:offset + size]
                self.assertTrue(tpk.is_jdlz(blob))
                texture = pack.find(tex_hash)
                self.assertEqual(texture.palette + texture.pixels, tpk.jdlz_decompress(blob))
            parsed = tpk.TexturePack.parse(chunk, data, file_offset)
            self.assertTrue(parsed.compressed_source)
            self.assertEqual([(t.name, t.pixels, t.palette) for t in pack.textures], [(t.name, t.pixels, t.palette) for t in parsed.textures])
            # The uncompressed layout in the records is kept as in the stock files.
            self.assertEqual(0, struct.unpack_from("<I", parsed.textures[0].record, 0x30)[0] % 128)
            # Rebuilding the parsed pack again gives the same bytes.
            self.assertEqual(data[file_offset:], parsed.serialize(file_offset))


if __name__ == "__main__":
    unittest.main()
