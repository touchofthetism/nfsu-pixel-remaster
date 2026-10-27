# nfstex — Need for Speed Underground texture toolkit

Stdlib-only Python for the game's texture packs, written from the game's own files and verified by
byte-identical round trips of every bundle in a stock copy (GLOBALB.BUN, INGAMEB.BUN, FrontB.lzc,
HUDTEX.BIN, TSELECT.BIN, MAGAZINE_TEXTURES.BIN, DYNTEX.BIN, Road_Segment.bin, the track streams).

- `tpk.py` — chunk streams, JDLZ decompression, texture pack parse/serialize (uncompressed layout with the
  engine's 128-byte alignment rules reproduced), DDS export, in-place texture replacement, whole-file rebuild
  that keeps the neighbouring chunks aligned.
- `upscale.py` — Real-ESRGAN inference without basicsr (RRDBNet and the compact anime-video net), alpha
  through the same network, tiled.
- `hdpack.py` — builds an HD texture pack (a launcher mod pack ZIP with `nfsu-modpack.json`) from a game
  folder: HUD gauges 4×, interface 2–4×, track previews 2×, world photographs 2× up to 512 px, DXT3/DXT1 with
  mip chains that stop at 8 px like the originals, bundles written uncompressed (the engine accepts raw chunk
  streams under the `.lzc` name). A bundle whose textures all stay stock is left out of the pack; `--limit N`
  upscales only the first N textures per bundle for a quick look at a policy.
- `tests/` — synthetic-data tests run by `.github/workflows/check.yml`.

Format notes (Underground 1):

| Chunk | Content |
|---|---|
| `B3300000` | texture pack; children: `00000000` pad, `B3310000` info, `00000000` pad, `B3320000` data |
| `33310001` | 124 B: u32 version 4, char[28] name, char[64] path, u32 hash |
| `33310002` | n × {u32 hash, u32 0} |
| `33310003` | only when textures are JDLZ blobs: n × {hash, file offset, size, flags, 0} |
| `33310004` | n × 124 B records: name @0x0C, hash @0x24, class @0x28, offset @0x30, palette offset @0x34, size @0x38, palette size @0x3C, base size @0x40, w/h @0x44, log2 @0x48, compression @0x4A, mips @0x4E, render flags … |
| `33310005` | n × 32 B: {0, 0, paletted, 5, 6, FourCC or D3DFORMAT, 0, 0} |
| `33320001` | {1, pack hash}; `33320002`: 0x11 filler to a 128-byte boundary, palettes (1 KB each), textures at 128-byte aligned offsets |

Compression codes: 0x08 P8, 0x20 A8R8G8B8, 0x22 DXT1, 0x24 DXT3, 0x26 DXT5 (unused by the stock game).
`.lzc` files are JDLZ (`JDLZ`, 0x02, 0x10, u32 raw size, u32 packed size); TRACKB*.lzc are raw.
