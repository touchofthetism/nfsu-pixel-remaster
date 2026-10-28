# nfstex — Need for Speed Underground texture toolkit

Python for the game's texture packs (the format code, `tpk.py` and `tracks.py`, is stdlib only), written from the
game's own files and verified by byte-identical round trips of every bundle in a stock copy (GLOBALB.BUN,
INGAMEB.BUN, FrontB.lzc, Road_Segment.bin, all 20 track streams; every race table by content) and identical info chunks
for every streaming pack (HUDTEX.BIN, TSELECT.BIN, MAGAZINE_TEXTURES.BIN, DYNTEX.BIN, Cars/*/TEXTURES.BIN), whose
blobs are recompressed.

- `tpk.py` — chunk streams, JDLZ decompression and compression (hash chains with lazy matching: within 1 % of
  EA's compressor on the game's own textures), an exact emulation of Speed.exe's JDLZ decoder for verification, texture pack parse/serialize (bundle and streaming layouts with the engine's
  128-byte alignment rules reproduced), DDS export, in-place texture replacement, whole-file rebuild that
  keeps the neighbouring chunks aligned.
- `upscale.py` — Real-ESRGAN inference without basicsr (RRDBNet: x4plus, x2plus, anime 6B; the compact nets:
  animevideo, general); alpha through the colour net or a cheaper one. Two cores manage about 65 thousand input
  pixels a second with animevideo, 30k with general, 16k with x2plus, 2–4k with x4plus. Inputs larger than a tile
  (128 px for x4plus, 512 for the others) are cut into overlapping tiles that are cross-faded, not cut: the 23-block
  nets look far enough that a hard cut shows.
- `tiling.py` (numpy only) — the edge handling for world textures. A texture the game wraps (its record's TilableUV
  byte: bit 0 across the left and right edges, bit 1 across the top and bottom; 3 for road and grass, 1 for walls
  and the sky panorama, 2 for road strips and ladders, 0 for signs) is upscaled with 16 pixels of its own
  continuation past each wrapping edge and a mirror image past the others, and each wrapping edge is then
  cross-faded with the net's view of the same pixels from across it, which makes the result exactly periodic (a
  plain margin leaves the wrap at 1.2–1.5 times the texture's own pixel-to-pixel contrast; a margin alone never
  closes it). `colour_fix` then takes the nets' colour drift back out region by region: x4plus and x2plus darken
  busy photographs by 2–3 levels on average and some by ten (a grass texture turns from yellow-green to dark
  green); the difference between the source and the result averaged back down, smoothed over a source pixel and
  spread back up, brings the mean shift to 0.1 and the regional drift from 3.5 to 0.5 levels with the detail
  untouched.
- `tracks.py` — the track streams. Rebuilds `Tracks/STREAM*.BUN` section by section, lays the sections out as the
  game's are, and moves every race's section table (`Tracks/TRACKB*.lzc`) to match; checks every table against the
  new files before anything is packed (see *Track streams* below).
- `hdpack.py` — builds an HD texture pack (a launcher mod pack ZIP with `nfsu-modpack.json`) from a game
  folder: HUD gauges 4×, interface 2–4×, track previews 2×, world photographs 2× up to 512 px, car detail
  maps 4×/2×; with `--profile tracks` the city (every stream section: up to 128 px 4×, 256 and 512 px 2×, the
  128×1024 road strips to 256×2048, paletted world textures and 1024-px ones kept) and each race's sky (panorama,
  cap and moon, uncompressed as stored; not the reflection maps nor the HUD's track map); DXT3/DXT1 with mip
  chains that stop at 8 px like the originals (wrapping textures' mips filtered across their edges too), bundles written uncompressed
  (the engine accepts raw chunk streams under the `.lzc` name), streaming packs written as streaming packs.
  Every rebuilt file is checked before it is packed: each JDLZ blob through the game's decoder emulation,
  the texture list against the source. A bundle whose textures all stay stock is left out of the pack;
  `--limit N` upscales only the first N textures per bundle for a quick look at a policy;
  `--repack old.zip` rewrites an existing pack with the current code (no upscaling, no game folder).
  Colour goes through x4plus for 4× and x2plus for 2× (they recover detail on the game's photographs that the
  compact anime-tuned net of packs v1–v9 smoothed into paint), alpha through animevideo; `--fast` uses
  animevideo throughout. Each texture is a job keyed by its source bytes, factor, models, edge handling and
  `UPSCALE_VERSION`: `--shard K/N --cache DIR` upscales one of N cost-balanced shares into finished DDS files,
  and a build with `--cache DIR` and no models packs from them (the private workflow runs the shares as a
  matrix of jobs).
- `tests/` — synthetic-data tests run by `.github/workflows/check.yml`.

Format notes (Underground 1):

| Chunk | Content |
|---|---|
| `B3300000` | texture pack; children: `00000000` pad, `B3310000` info, `00000000` pad, `B3320000` data |
| `33310001` | 124 B: u32 version 4, char[28] name, char[64] path, u32 hash |
| `33310002` | n × {u32 hash, u32 0} |
| `33310003` | streaming packs only: n × {hash, absolute file offset, blob size, 0x100, 0} sorted by hash; the blobs are contiguous from a 128-byte boundary. The engine walks this table when it unloads the pack |
| `33310004` | n × 124 B records: name @0x0C, hash @0x24, class @0x28, offset @0x30, @0x34 (below), size @0x38, palette size @0x3C, mip-0 size @0x40, w/h @0x44, log2 @0x48, compression @0x4A, palette flag @0x4D, mips @0x4E, render flags … |
| `33310005` | n × 32 B: {0, 0, paletted, 5, 6, FourCC or D3DFORMAT, 0, 0} |
| `33320001` | {1, pack hash}; `33320002`: 0x11 filler up to the next 128-byte file boundary (the engine aligns the pointer; INGAMEB's first texture itself begins with 0x11 bytes, so never count them), then the data |

Bundle data: palettes first (1 KB each), textures at 128-byte aligned offsets; record field 0x34 is the
writer's palette cursor. Streaming data: one JDLZ blob per texture (palette, then mips); the records keep
a virtual layout that counts pixel data only, each palette placed after its texture, and field 0x34 is the
128-byte aligned end of the texture's pixels.

Compression codes: 0x08/0x80 P8, 0x20 A8R8G8B8, 0x22 DXT1, 0x24 DXT3, 0x26 DXT5 (unused by the stock game).

JDLZ (`JDLZ`, 0x02, 0x10, u16 0, u32 raw size, u32 packed size, then flags1, flags2 and the tokens):
flags1 has one bit per token (1 = back-reference), flags2 one bit per reference (1 = near form: distance
1–16, length 3–4098; 0 = far form: distance 17–2064, length 3–34). Speed.exe's decoder (sub_58D190) loops
on `packed − 0x12`, refills a flag byte the moment its 8 bits are used up, even after the last token, and
copies references without checking the output size; EA's streams therefore end with a dummy literal
token whose data byte is counted but never read. A stream without it makes the decoder read one byte
past the blob and run on with a negative count, writing across the heap — HD packs v1–v5 shipped such
blobs; `jdlz_game_decode` reproduces the decoder so no pack can again. `.lzc` files, TRACKB*.lzc among them, are
whole-file JDLZ.

Track streams (read from Speed.exe; every rule below holds for every stock file):

| Where | What |
|---|---|
| `TRACKB<race>.lzc` chunk `00034107` | the race's section table: 56-byte entries {char[8] name ("Y0", "A12"), u16 number (letter × 100 + n), u8 in-race flag, u32 status @0x0C, u32 file @0x10 (0 the TRACKB itself, 1 the region stream, 2 the race's own stream), u32 offset @0x14, u32 size @0x18, u32 resident size @0x1C, u32 byte sum @0x20, then the position} |
| chunk `00034108` | {TRACKB size, size of `STREAM<region>.BUN`, size of `STREAM<region>_<race>.BUN` or 0} |
| `STREAM<region>[_<race>].BUN` | the sections at 2048-byte offsets, each gap one padding chunk, nothing after the last; a race's own stream is one section (its copy of Y0, the city textures) |
| resident size | the section's size less the payload of its texture-data chunks (`B3320000`): the pixels go to Direct3D and their memory is freed, the rest stays |
| byte sum | matches the data for only 61 of STREAML1RA's 400 sections: nothing checks it (written for the new data all the same) |
| streaming pool | sub_444AF0 over the race's sections (in-race flag set, or X0, X99, Y0, Z0): the larger of the summed resident sizes and the biggest size + resident, plus 2 MB (sub_444480, from 0x4499AA). Y0, loaded first, sets it in every stock race: 20.8 MB in the city, 121 MB with the HD tracks pack, all from the tables |

A texture-animation chunk (`B0300100`) follows its section's texture pack directly in 33 sections; a rebuilt pack
puts a padding chunk in front of it whenever its new end would not keep the chunk on the alignment it had.
