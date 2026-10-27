# nfstex — Need for Speed Underground texture toolkit

Stdlib-only Python for the game's texture packs, written from the game's own files and verified by
byte-identical round trips of every bundle in a stock copy (GLOBALB.BUN, INGAMEB.BUN, FrontB.lzc,
Road_Segment.bin, the track streams) and identical info chunks for every streaming pack (HUDTEX.BIN,
TSELECT.BIN, MAGAZINE_TEXTURES.BIN, DYNTEX.BIN, Cars/*/TEXTURES.BIN), whose blobs are recompressed.

- `tpk.py` — chunk streams, JDLZ decompression and compression, an exact emulation of Speed.exe's JDLZ
  decoder for verification, texture pack parse/serialize (bundle and streaming layouts with the engine's
  128-byte alignment rules reproduced), DDS export, in-place texture replacement, whole-file rebuild that
  keeps the neighbouring chunks aligned.
- `upscale.py` — Real-ESRGAN inference without basicsr (RRDBNet and the compact anime-video net), alpha
  through the same network, tiled.
- `hdpack.py` — builds an HD texture pack (a launcher mod pack ZIP with `nfsu-modpack.json`) from a game
  folder: HUD gauges 4×, interface 2–4×, track previews 2×, world photographs 2× up to 512 px, car detail
  maps 4×/2×, DXT3/DXT1 with mip chains that stop at 8 px like the originals, bundles written uncompressed
  (the engine accepts raw chunk streams under the `.lzc` name), streaming packs written as streaming packs.
  Every rebuilt file is checked before it is packed: each JDLZ blob through the game's decoder emulation,
  the texture list against the source. A bundle whose textures all stay stock is left out of the pack;
  `--limit N` upscales only the first N textures per bundle for a quick look at a policy;
  `--repack old.zip` rewrites an existing pack with the current code (no upscaling, no game folder).
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
blobs; `jdlz_game_decode` reproduces the decoder so no pack can again. `.lzc` files are whole-file
JDLZ; TRACKB*.lzc are raw.
