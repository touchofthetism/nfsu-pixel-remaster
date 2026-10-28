"""Track streams and race tables (tools/nfstex/tracks.py) on a synthetic game folder: no game files needed.

The folder mirrors the stock layout: a region stream of sections at 2048-byte offsets with one padding chunk
in each gap, a per-race stream holding one section, and race tables (JDLZ-compressed TRACKB<race>.lzc) whose
0x34107 chunk lists the sections and whose 0x34108 chunk gives the stream sizes.
"""
import pathlib
import struct
import tempfile
import unittest

from nfstex import tpk, tracks
from nfstex.tests.test_tpk import make_format, make_record, synthetic_pack

GEOMETRY, ANIMATION, OTHER = 0x80134000, 0xB0300100, 0x00034250     # the first two are parents
GEOMETRY_DATA, ANIMATION_DATA = 0x00134002, 0x30300101


def chunk(chunk_id: int, payload: bytes) -> bytes:
    return struct.pack("<II", chunk_id, len(payload)) + payload


def texture_pack(width: int = 16, tilable: int = 3, square_last: bool = False) -> tpk.TexturePack:
    """The synthetic pack (DXT1 "SQUARE" and P8 "PALETTED") renamed TRACK, SQUARE ``width`` pixels wide."""
    pack = synthetic_pack()
    pack.header[4:9] = b"TRACK"
    pack.textures[0].record[0x4F] = tilable
    if width != 16:
        grow(pack.textures[0], width)
    if square_last:
        pack.textures.reverse()
    return pack


def grow(texture: tpk.Texture, width: int, mips: int = 2) -> None:
    """Replaces a texture by a DXT1 one of ``width`` x ``width`` with ``mips`` levels."""
    size = sum(tpk.mip_sizes(width, width, mips, tpk.COMP_DXT1))
    header = struct.pack("<IIIIII", 0x7C, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, width, width, width * 2, 0)
    header += struct.pack("<I", mips) + b"\0" * 44 + struct.pack("<II4sIIIII", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x401008, 0, 0, 0, 0)
    texture.replace_with_dds(b"DDS " + header + bytes(range(256)) * (size // 256) + bytes(size % 256))


def section(parts) -> bytes:
    """A section from (kind, payload) parts: "geometry", "anim" or "pack" (a TexturePack placed where it falls)."""
    out = bytearray()
    for kind, value in parts:
        if kind == "pack":
            out += value.serialize(len(out))
        elif kind == "geometry":
            out += chunk(GEOMETRY, chunk(GEOMETRY_DATA, value))
        else:
            out += chunk(ANIMATION, chunk(ANIMATION_DATA, value))
    return bytes(out)


def stream(sections) -> tuple:
    """(file bytes, [offset of each section]) laid out as the game's: 2048-byte offsets, one padding chunk
    per gap, nothing after the last section."""
    out = bytearray()
    offsets = []
    for number, data in enumerate(sections):
        offsets.append(len(out))
        out += data
        if number + 1 < len(sections):
            gap = (-len(out)) % 2048
            if 0 < gap < 8:
                gap += 2048
            if gap:
                out += tracks.padding(gap)
    return bytes(out), offsets


def entry_bytes(name: str, number: int, flag: int, file: int, offset: int, data: bytes) -> bytes:
    resident = len(data) - tracks.texture_payload(data)
    raw = bytearray(tracks.ENTRY_SIZE)
    raw[0:len(name)] = name.encode()
    struct.pack_into("<HB", raw, 8, number, flag)
    struct.pack_into("<5I", raw, 0x10, file, offset, len(data), resident, sum(data) & 0xFFFFFFFF)
    return bytes(raw)


class SyntheticGame:
    """Tracks/STREAMT1.BUN (Y0, A1 with a texture animation right after its pack, B1 with no textures),
    Tracks/STREAMT1_1234.BUN (the race's own Z0), TRACKB1234 (all of it) and TRACKB1235 (the region stream
    only, A1 not in the race)."""

    def __init__(self, folder: str):
        self.root = pathlib.Path(folder)
        (self.root / "Tracks").mkdir(parents=True)
        self.sections = {
            "Y0": section([("geometry", b"g" * 40), ("pack", texture_pack(32))]),
            # SQUARE (64x64, 2560 bytes) last: the animation chunk after the pack lands on 128 bytes.
            "A1": section([("geometry", b"a" * 24), ("pack", texture_pack(64, tilable=1, square_last=True)), ("anim", b"anim" * 6)]),
            "B1": section([("geometry", b"b" * 3000)]),
        }
        self.region, offsets = stream([self.sections[name] for name in ("Y0", "A1", "B1")])
        self.offsets = dict(zip(("Y0", "A1", "B1"), offsets))
        self.race_section = section([("geometry", b"z" * 16), ("pack", texture_pack(16, tilable=2))])
        self.race_file = self.race_section
        (self.root / "Tracks/STREAMT1.BUN").write_bytes(self.region)
        (self.root / "Tracks/STREAMT1_1234.BUN").write_bytes(self.race_file)
        (self.root / "Tracks/STREAMT1_1299.BUN").write_bytes(b"")          # stock ships empty race files too
        numbers = {"Y0": 2500, "A1": 100, "B1": 200}
        self.write_race(1234, [("Y0", 0), ("A1", 1), ("B1", 0)], with_race_file=True)
        self.write_race(1235, [("Y0", 0), ("A1", 0), ("B1", 1)], with_race_file=False)
        self.numbers = numbers

    def write_race(self, race: int, flags, with_race_file: bool) -> None:
        numbers = {"Y0": 2500, "A1": 100, "B1": 200}
        table = entry_bytes("--", 0, 0, tracks.FILE_TRACKB, 0, b"\0" * 64)
        for name, flag in flags:
            table += entry_bytes(name, numbers[name], flag, tracks.FILE_STREAM, self.offsets[name], self.sections[name])
        if with_race_file:
            table += entry_bytes("Z0", 2600, 0, tracks.FILE_RACE, 0, self.race_section)
        sizes = struct.pack("<3I", 0x1000, len(self.region), len(self.race_file) if with_race_file else 0)
        data = chunk(OTHER, b"x" * 20) + chunk(tracks.SECTION_TABLE, table) + chunk(tracks.STREAM_SIZES, sizes)
        (self.root / f"Tracks/TRACKB{race}.lzc").write_bytes(tpk.jdlz_compress(data))


def grow_packs(width: int, mips: int = 2):
    """A rebuild_section that grows SQUARE in every pack to ``width`` pixels with ``mips`` levels."""
    def rebuild(path: str, data: bytes, entry: tracks.Entry) -> bytes:
        chunks = tpk.find_texture_packs(data)
        if not chunks:
            return data
        packs = [tpk.TexturePack.parse(c, data, o) for o, c in chunks]
        for pack in packs:
            grow(pack.find("SQUARE"), width, mips)
        return tpk.replace_texture_packs(data, chunks, {o: (lambda base, p=p: p.serialize(base)) for (o, _), p in zip(chunks, packs)})
    return rebuild


class TrackStreamTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.game = SyntheticGame(self.folder.name)
        self.root = self.game.root

    def tearDown(self):
        self.folder.cleanup()

    def test_reads_tables_streams_and_the_pool_rule(self):
        races = tracks.find_races(self.root)
        self.assertEqual([1234, 1235], [race.number for race in races])
        race = races[0]
        self.assertEqual({tracks.FILE_STREAM: "Tracks/STREAMT1.BUN", tracks.FILE_RACE: "Tracks/STREAMT1_1234.BUN"},
                         tracks.stream_files(self.root, race))
        self.assertEqual({tracks.FILE_STREAM: "Tracks/STREAMT1.BUN"}, tracks.stream_files(self.root, races[1]))
        layout = tracks.layouts(self.root, races)
        self.assertEqual(sorted(self.game.offsets.values()), sorted(layout["Tracks/STREAMT1.BUN"]))
        for offset in layout["Tracks/STREAMT1.BUN"]:
            self.assertEqual(0, offset % tracks.SECTION_ALIGN)
        # Pool: over the race's sections (flag set, or Y0/Z0 always), max(summed resident, biggest size +
        # resident) + 2 MB. B1 holds no textures, so its resident size is its size.
        y0 = next(e for e in race.entries if e.name == "Y0")
        self.assertEqual(len(self.game.sections["Y0"]) - tracks.texture_payload(self.game.sections["Y0"]), y0.resident)
        in_race = [e for e in race.entries if race.in_race(e)]
        self.assertEqual({"Y0", "A1", "Z0"}, {e.name for e in in_race})
        expected = max(sum(e.resident for e in in_race), max(e.size + e.resident for e in in_race if e.size != e.resident))
        self.assertEqual(expected + 0x200000, tracks.pool_size(race))

    def test_unchanged_sections_change_no_file(self):
        self.assertEqual({}, tracks.build(self.root, lambda path, data, entry: data, lambda message: None))

    def test_grown_sections_move_every_table_that_names_them(self):
        log = []
        files = tracks.build(self.root, grow_packs(256), log.append)
        self.assertEqual({"Tracks/STREAMT1.BUN", "Tracks/STREAMT1_1234.BUN", "Tracks/TRACKB1234.lzc", "Tracks/TRACKB1235.lzc"}, set(files))
        region = files["Tracks/STREAMT1.BUN"]
        before = tracks.find_races(self.root)
        for relative in ("Tracks/TRACKB1234.lzc", "Tracks/TRACKB1235.lzc"):
            raw = files[relative]
            decoded, _ = tpk.jdlz_game_decode(raw)                          # the game's own decoder takes it
            race = tracks.parse_race(relative, raw)
            old = next(r for r in before if r.relative == relative)
            self.assertEqual(len(region), race.sizes[1])
            self.assertEqual(old.sizes[0], race.sizes[0])
            for entry, stock in zip(race.entries, old.entries):
                self.assertEqual((stock.name, stock.number, stock.flag, stock.file), (entry.name, entry.number, entry.flag, entry.file))
                if entry.file == tracks.FILE_TRACKB:
                    self.assertEqual((stock.offset, stock.size), (entry.offset, entry.size))
                    continue
                data = region if entry.file == tracks.FILE_STREAM else files["Tracks/STREAMT1_1234.BUN"]
                body = data[entry.offset:entry.offset + entry.size]
                self.assertEqual(0, entry.offset % tracks.SECTION_ALIGN)
                self.assertEqual(entry.size - tracks.texture_payload(body), entry.resident)
                self.assertEqual(sum(body) & 0xFFFFFFFF, entry.checksum)
                # Everything but the texture packs is the stock bytes, in the stock order.
                stock_body = self.game.sections.get(entry.name, self.game.race_section)
                stock_chunks = list(tracks.top_chunks(stock_body))
                new_chunks = [c for c in tracks.top_chunks(body) if c[1] != tpk.PADDING]
                self.assertEqual([c[1] for c in stock_chunks], [c[1] for c in new_chunks])
                for (stock_at, cid, size), (at, _, _) in zip(stock_chunks, new_chunks):
                    if cid != tpk.TPK:
                        self.assertEqual(stock_body[stock_at:stock_at + 8 + size], body[at:at + 8 + size])
                for pack_offset, pack_chunk in tpk.find_texture_packs(body):
                    self.assertEqual(256, tpk.TexturePack.parse(pack_chunk, body, pack_offset).find("SQUARE").width)
            # The stream file is sections and padding only.
            tracks.check_layout("Tracks/STREAMT1.BUN", region, {e.offset: e for e in race.entries if e.file == tracks.FILE_STREAM})
        # The pool follows the biggest section (256x256 DXT1 now outweighs everything resident).
        new_race = tracks.parse_race("Tracks/TRACKB1234.lzc", files["Tracks/TRACKB1234.lzc"])
        biggest = max(e.size + e.resident for e in new_race.entries if new_race.in_race(e) and e.size != e.resident)
        self.assertEqual(biggest + 0x200000, tracks.pool_size(new_race))
        self.assertGreater(tracks.pool_size(new_race), tracks.pool_size(before[0]))
        self.assertTrue(any("streaming pool" in line for line in log))

    def test_a_chunk_right_after_a_grown_pack_keeps_its_alignment(self):
        # A full chain down to 8x8 (128x128, 5 levels: 10912 bytes) ends 32 bytes past a 128-byte boundary.
        files = tracks.build(self.root, grow_packs(128, mips=5), lambda message: None)
        race = tracks.parse_race("Tracks/TRACKB1234.lzc", files["Tracks/TRACKB1234.lzc"])
        a1 = next(e for e in race.entries if e.name == "A1")
        body = files["Tracks/STREAMT1.BUN"][a1.offset:a1.offset + a1.size]
        stock = self.game.sections["A1"]
        stock_anim = next(o for o, cid, _ in tracks.top_chunks(stock) if cid == ANIMATION)
        anim = next(o for o, cid, _ in tracks.top_chunks(body) if cid == ANIMATION)
        alignment = next(a for a in (4096, 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4) if stock_anim % a == 0)
        self.assertGreaterEqual(alignment, 128)
        self.assertEqual(0, anim % alignment)
        pack_end = next(o + 8 + n for o, cid, n in tracks.top_chunks(body) if cid == tpk.TPK)
        self.assertEqual(tpk.PADDING, struct.unpack_from("<I", body, pack_end)[0])      # put in front of it
        self.assertEqual(stock[stock_anim:], body[anim:])

    def test_layouts_the_rebuild_cannot_keep_are_refused(self):
        races = tracks.find_races(self.root)
        sections = tracks.layouts(self.root, races)["Tracks/STREAMT1.BUN"]
        tracks.check_layout("Tracks/STREAMT1.BUN", self.game.region, sections)
        # A chunk other than padding between two sections.
        broken = bytearray(self.game.region)
        end = len(self.game.sections["Y0"])
        struct.pack_into("<I", broken, end, OTHER)
        with self.assertRaises(ValueError):
            tracks.check_layout("Tracks/STREAMT1.BUN", bytes(broken), sections)
        # Bytes after the last section.
        with self.assertRaises(ValueError):
            tracks.check_layout("Tracks/STREAMT1.BUN", self.game.region + tracks.padding(8), sections)
        # Two race tables that disagree about a section.
        race = tracks.read_race(self.root, "Tracks/TRACKB1235.lzc")
        data = bytearray(race.data)
        a1 = next(e for e in race.entries if e.name == "A1")
        struct.pack_into("<I", data, race.table_at + a1.index * tracks.ENTRY_SIZE + 0x18, a1.size + 4)
        (self.root / "Tracks/TRACKB1235.lzc").write_bytes(bytes(data))
        with self.assertRaises(ValueError):
            tracks.layouts(self.root, tracks.find_races(self.root))

    def test_a_gap_too_short_for_a_padding_header_takes_another_block(self):
        first = chunk(GEOMETRY, chunk(GEOMETRY_DATA, b"q" * 2028))   # 2044 bytes: the 4 left to 2048 cannot hold a chunk
        last = chunk(GEOMETRY, b"")
        original = first + tracks.padding(2048 + 4) + last
        sections = {0: tracks.Entry(0, "S0", 1, 1, 1, 0, len(first), len(first), 0),
                    4096: tracks.Entry(1, "S1", 2, 1, 1, 4096, len(last), len(last), 0)}
        rebuilt, moved = tracks.rebuild_stream("T.BUN", original, sections, lambda data, entry: data)
        self.assertEqual(original, rebuilt)
        self.assertEqual({0: (0, len(first), len(first), sum(first)), 4096: (4096, 8, 8, sum(last))}, moved)


if __name__ == "__main__":
    unittest.main()
