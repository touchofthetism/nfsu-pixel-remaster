"""Track streams for HD packs: Tracks/STREAM<region>[_<race>].BUN rebuilt section by section, and every race's
section table (Tracks/TRACKB<race>.lzc) moved to the new layout.

The format (Underground 1; read from Speed.exe and checked against every stock file by tests/scripts):

- TRACKB<race>.lzc (whole-file JDLZ) chunk 0x00034107 is the race's section table, 56-byte entries:
  +0x00 char name[8] ("A1", "Y0"), +0x08 u16 number (letter * 100 + n), +0x0A u8 set for the race's own
  sections, +0x0C status, +0x10 u32 file (0 the TRACKB itself, 1 STREAM<region>.BUN, 2 STREAM<region>_<race>.BUN),
  +0x14 u32 file offset, +0x18 u32 size, +0x1C u32 resident size, +0x20 u32 byte sum, then the section's
  position. The resident size is the size minus the payload of every texture-data chunk (0xB3320000) in the
  section: the pixels are handed to Direct3D and their memory freed, the rest stays (true of all 7776 stock
  entries). The byte sum matches the data for only 61 of the 400 stock sections of STREAML1RA, so nothing in
  the game checks it; it is written for the new data all the same.
- chunk 0x00034108 is {TRACKB size, size of STREAM<region>.BUN, size of STREAM<region>_<race>.BUN or 0}.
  sub_4442A0 reads both chunks into the track streamer (0x77A980: sections, count, sizes).
- A stream file is its sections at 2048-byte file offsets, the gap after each section one padding chunk,
  nothing after the last. A race file (file 2) is one section, the race's own copy of Y0.
- The streamer's memory pool (sub_444480, called from 0x4499AA) is sized from the race's table by sub_444AF0:
  over the race's sections (flag set, or numbers 2400, 2499, 2500, 2600: X0, X99, Y0, Z0), the larger of the
  summed resident sizes and the biggest size + resident size, plus 2 MB. Y0, the shared city textures
  (19.5 MB stock, loaded first), sets it in every stock race; HD textures grow it through the table, so the
  game needs no change.
"""
from __future__ import annotations

import dataclasses
import fnmatch
import pathlib
import re
import struct
from typing import Callable, Dict, List, Optional, Tuple

from nfstex import tpk

# The stream files, as a game-folder glob (region streams and per-race files alike).
STREAMS = "Tracks/STREAM*.BUN"
SECTION_TABLE, STREAM_SIZES = 0x00034107, 0x00034108
ENTRY_SIZE = 56
SECTION_ALIGN = 2048
FILE_TRACKB, FILE_STREAM, FILE_RACE = 0, 1, 2
# Sections every race loads whatever their flag (sub_444AF0's list): X0, X99, Y0, Z0.
ALWAYS = (2400, 2499, 2500, 2600)


def is_stream(relative: str) -> bool:
    """True for a track stream file (Tracks/STREAM<region>[_<race>].BUN), any case."""
    return fnmatch.fnmatch(relative.replace("\\", "/").lower(), STREAMS.lower())


@dataclasses.dataclass
class Entry:
    index: int
    name: str
    number: int
    flag: int
    file: int
    offset: int
    size: int
    resident: int
    checksum: int


@dataclasses.dataclass
class Race:
    number: int
    relative: str             # Tracks/TRACKB<n>.lzc as found
    data: bytes               # decompressed
    table_at: int             # payload offset of chunk 0x34107 in data
    sizes_at: int             # payload offset of chunk 0x34108
    entries: List[Entry]
    sizes: Tuple[int, int, int]

    def in_race(self, entry: Entry) -> bool:
        return entry.flag != 0 or entry.number in ALWAYS


def top_chunks(data: bytes, start: int = 0, end: Optional[int] = None):
    """(offset, id, size) of each top-level chunk."""
    end = len(data) if end is None else end
    offset = start
    while offset + 8 <= end:
        chunk_id, size = struct.unpack_from("<II", data, offset)
        if offset + 8 + size > end:
            raise ValueError(f"chunk {chunk_id:08x} at {offset} runs past the end")
        yield offset, chunk_id, size
        offset += 8 + size


def read_entries(data: bytes, table_at: int, count: int) -> List[Entry]:
    entries = []
    for index in range(count):
        at = table_at + index * ENTRY_SIZE
        name = data[at:at + 8].split(b"\0")[0].decode("latin1")
        number, flag = struct.unpack_from("<HB", data, at + 8)
        file, offset, size, resident, checksum = struct.unpack_from("<5I", data, at + 0x10)
        entries.append(Entry(index, name, number, flag, file, offset, size, resident, checksum))
    return entries


def parse_race(relative: str, raw: bytes) -> Race:
    data = tpk.jdlz_decompress(raw) if tpk.is_jdlz(raw) else raw
    table_at = sizes_at = None
    for offset, chunk_id, size in top_chunks(data):
        if chunk_id == SECTION_TABLE:
            table_at, table_size = offset + 8, size
        elif chunk_id == STREAM_SIZES:
            sizes_at = offset + 8
    if table_at is None or sizes_at is None:
        raise ValueError(f"{relative}: no section table")
    number = int(re.search(r"(\d+)", pathlib.Path(relative).name).group(1))
    return Race(number, relative, data, table_at, sizes_at, read_entries(data, table_at, table_size // ENTRY_SIZE),
                struct.unpack_from("<3I", data, sizes_at))


def read_race(game: pathlib.Path, relative: str) -> Race:
    return parse_race(relative, (game / relative).read_bytes())


def find_races(game: pathlib.Path) -> List[Race]:
    return [read_race(game, path.relative_to(game).as_posix())
            for path in sorted(game.glob("Tracks/TRACKB*.lzc"), key=lambda p: p.name.lower())]


def stream_files(game: pathlib.Path, race: Race) -> Dict[int, str]:
    """{file kind: Tracks/... path} for the race's stream files: the region stream by its size in 0x34108, the
    race file by the region's name and the race number."""
    regions = [p for p in game.glob("Tracks/STREAM*.BUN") if "_" not in p.stem]
    matches = [p for p in regions if p.stat().st_size == race.sizes[1]]
    if len(matches) != 1:
        raise ValueError(f"{race.relative}: {len(matches)} region streams of {race.sizes[1]} bytes")
    files = {FILE_STREAM: matches[0].relative_to(game).as_posix()}
    if race.sizes[2]:
        candidates = [p for p in game.glob(f"Tracks/{matches[0].stem}_{race.number}.BUN")]
        if len(candidates) != 1 or candidates[0].stat().st_size != race.sizes[2]:
            raise ValueError(f"{race.relative}: no race stream of {race.sizes[2]} bytes")
        files[FILE_RACE] = candidates[0].relative_to(game).as_posix()
    return files


def layouts(game: pathlib.Path, races: List[Race]) -> Dict[str, Dict[int, Entry]]:
    """{stream path: {offset: entry}} over all races, refusing tables that disagree about a section."""
    found: Dict[str, Dict[int, Entry]] = {}
    for race in races:
        files = stream_files(game, race)
        for entry in race.entries:
            if entry.file == FILE_TRACKB:
                continue
            path = files[entry.file]
            known = found.setdefault(path, {}).get(entry.offset)
            if known and (known.size, known.resident, known.number) != (entry.size, entry.resident, entry.number):
                raise ValueError(f"{race.relative}: section {entry.name} at {entry.offset} disagrees with another race's table")
            found[path].setdefault(entry.offset, entry)
    return found


def texture_payload(section: bytes) -> int:
    """Bytes in the payloads of the section's texture-data chunks (what the game frees after loading)."""
    total = 0
    for offset, chunk_id, size in top_chunks(section):
        if chunk_id == tpk.TPK:
            for _, child, child_size in top_chunks(section, offset + 8, offset + 8 + size):
                if child == tpk.TPK_DATA:
                    total += child_size
    return total


def padding(total: int) -> bytes:
    """A padding chunk of `total` bytes (header included; total >= 8)."""
    return struct.pack("<II", tpk.PADDING, total - 8) + b"\0" * (total - 8)


def check_layout(path: str, data: bytes, sections: Dict[int, Entry]) -> None:
    """The stock layout this module rebuilds: sections at 2048-byte offsets, each gap one padding chunk, the
    file ending with the last section."""
    cursor = 0
    for offset in sorted(sections):
        if offset % SECTION_ALIGN:
            raise ValueError(f"{path}: section {sections[offset].name} at {offset} is not on a 2048-byte boundary")
        while cursor < offset:
            chunk_id, size = struct.unpack_from("<II", data, cursor)
            if chunk_id != tpk.PADDING:
                raise ValueError(f"{path}: chunk {chunk_id:08x} between sections at {cursor}")
            cursor += 8 + size
        if cursor != offset:
            raise ValueError(f"{path}: the gap before section {sections[offset].name} is not whole chunks")
        cursor = offset + sections[offset].size
    if cursor != len(data):
        raise ValueError(f"{path}: {len(data) - cursor} bytes after the last section")


def _alignment(offset: int) -> int:
    return next(a for a in (2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1) if offset % a == 0)


def check_section(label: str, stock: bytes, section: bytes) -> None:
    """A rebuilt section against its stock bytes: the same chunks in the same order (padding aside), every chunk
    but the texture packs byte-identical, and the chunk after each texture pack still on (at least) the
    alignment it had within the section (sections sit on 2048-byte boundaries)."""
    stock_chunks = [c for c in top_chunks(stock) if c[1] != tpk.PADDING]
    new_chunks = [c for c in top_chunks(section) if c[1] != tpk.PADDING]
    if [c[1] for c in stock_chunks] != [c[1] for c in new_chunks]:
        raise SystemExit(f"{label}: the rebuilt section's chunks differ from the original's")
    after_pack = False
    for (stock_at, chunk_id, size), (at, _, new_size) in zip(stock_chunks, new_chunks):
        if chunk_id != tpk.TPK:
            if stock[stock_at:stock_at + 8 + size] != section[at:at + 8 + new_size]:
                raise SystemExit(f"{label}: chunk {chunk_id:08x} changed in the rebuild")
            if after_pack and _alignment(at) < _alignment(stock_at):
                raise SystemExit(f"{label}: chunk {chunk_id:08x} after a texture pack lost its alignment "
                                 f"({_alignment(stock_at)} -> {_alignment(at)})")
        after_pack = chunk_id == tpk.TPK


def rebuild_stream(path: str, data: bytes, sections: Dict[int, Entry],
                   rebuild: Callable[[bytes, Entry], bytes]) -> Tuple[bytes, Dict[int, Tuple[int, int, int, int]]]:
    """The stream with each section replaced by rebuild(section bytes, entry), laid out as the game's are;
    each rebuilt section passes check_section. Returns the file and {old offset: (new offset, size, resident
    size, byte sum)}."""
    check_layout(path, data, sections)
    out = bytearray()
    moved = {}
    offsets = sorted(sections)
    for number, offset in enumerate(offsets):
        entry = sections[offset]
        stock = data[offset:offset + entry.size]
        section = rebuild(stock, entry)
        if section != stock:
            check_section(f"{path} {entry.name}", stock, section)
        start = len(out)
        out += section
        moved[offset] = (start, len(section), len(section) - texture_payload(section), sum(section) & 0xFFFFFFFF)
        if number + 1 < len(offsets):
            gap = (-len(out)) % SECTION_ALIGN
            if 0 < gap < 8:
                gap += SECTION_ALIGN
            if gap:
                out += padding(gap)
    return bytes(out), moved


def patch_race(race: Race, files: Dict[int, str], moved: Dict[str, Dict[int, Tuple[int, int, int, int]]],
               sizes: Dict[str, int]) -> bytes:
    """The race's TRACKB with its table and stream sizes moved to the new layout, JDLZ-compressed as shipped
    (and checked through the game's own decoder)."""
    data = bytearray(race.data)
    for entry in race.entries:
        if entry.file == FILE_TRACKB:
            continue
        offset, size, resident, checksum = moved[files[entry.file]][entry.offset]
        struct.pack_into("<4I", data, race.table_at + entry.index * ENTRY_SIZE + 0x14, offset, size, resident, checksum)
    struct.pack_into("<I", data, race.sizes_at + 4, sizes[files[FILE_STREAM]])
    if FILE_RACE in files:
        struct.pack_into("<I", data, race.sizes_at + 8, sizes[files[FILE_RACE]])
    packed = tpk.jdlz_compress(bytes(data))
    decoded, _ = tpk.jdlz_game_decode(packed)
    if decoded != bytes(data):
        raise SystemExit(f"{race.relative}: the compressed table does not decode to itself")
    return packed


def pool_size(race: Race) -> int:
    """The streamer's memory pool for this race, as sub_444AF0 + 2 MB compute it."""
    resident = biggest = 0
    for entry in race.entries:
        if not race.in_race(entry):
            continue
        resident += entry.resident
        if entry.size != entry.resident:
            biggest = max(biggest, entry.size + entry.resident)
    return max(resident, biggest) + 0x200000


def verify(streams: Dict[str, bytes], tables: Dict[str, bytes], races: List[Race], game: pathlib.Path) -> None:
    """Every race's new table against the new stream files: each section inside its file on a 2048-byte
    boundary, its resident size by the rule, the stream sizes in 0x34108 matching the files."""
    for race in races:
        updated = parse_race(race.relative, tables[race.relative])
        files = stream_files(game, race)
        for entry in updated.entries:
            if entry.file == FILE_TRACKB:
                continue
            section = streams[files[entry.file]][entry.offset:entry.offset + entry.size]
            if entry.offset % SECTION_ALIGN or len(section) != entry.size:
                raise SystemExit(f"{race.relative}: section {entry.name} misplaced in {files[entry.file]}")
            if entry.size - texture_payload(section) != entry.resident:
                raise SystemExit(f"{race.relative}: section {entry.name} resident size off")
        if updated.sizes[1] != len(streams[files[FILE_STREAM]]) or \
                (FILE_RACE in files and updated.sizes[2] != len(streams[files[FILE_RACE]])):
            raise SystemExit(f"{race.relative}: stream sizes in 0x34108 do not match the files")


def build(game: pathlib.Path, rebuild_section: Callable[[str, bytes, Entry], bytes], log) -> Dict[str, bytes]:
    """Rebuilt stream files and race tables: {Tracks/... path: bytes}, streams whose sections all came back
    unchanged left out (their races then keep stock tables too)."""
    races = find_races(game)
    layout = layouts(game, races)
    new_files: Dict[str, bytes] = {}
    moved: Dict[str, Dict[int, Tuple[int, int, int, int]]] = {}
    sizes: Dict[str, int] = {}
    for path in sorted(layout):
        data = (game / path).read_bytes()
        rebuilt, mapping = rebuild_stream(path, data, layout[path], lambda section, entry, p=path: rebuild_section(p, section, entry))
        moved[path], sizes[path] = mapping, len(rebuilt)
        if rebuilt != data:
            new_files[path] = rebuilt
            log(f"{path}: {len(layout[path])} sections, {len(data) >> 20} MB -> {len(rebuilt) >> 20} MB")
        else:
            log(f"{path}: unchanged")
    streams = {path: new_files.get(path) or (game / path).read_bytes() for path in layout}
    tables = {}
    for race in races:
        files = stream_files(game, race)
        if any(path in new_files for path in files.values()):
            tables[race.relative] = patch_race(race, files, moved, sizes)
    changed = [race for race in races if race.relative in tables]
    verify(streams, tables, changed, game)
    for race in changed:
        after = parse_race(race.relative, tables[race.relative])
        log(f"{race.relative}: table moved; streaming pool {pool_size(race) / 2**20:.1f} MB -> {pool_size(after) / 2**20:.1f} MB")
    new_files.update(tables)
    return new_files
