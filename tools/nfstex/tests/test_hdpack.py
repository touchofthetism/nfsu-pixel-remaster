"""Pack-level checks for tools/nfstex/hdpack.py: the post-build verification and --repack (no game files, no torch)."""
import importlib.util
import io
import json
import os
import pathlib
import struct
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

from nfstex import endscript, tpk
from nfstex.tests.test_tpk import make_format, make_record, synthetic_pack

HAVE_PIL = importlib.util.find_spec("PIL") is not None and importlib.util.find_spec("numpy") is not None


def streaming_file(file_offset: int = 0, legacy: bool = False) -> bytes:
    """A synthetic streaming pack; ``legacy`` writes the blobs the way HD packs v1-v5 did (no terminator)."""
    pack = synthetic_pack()
    pack.compressed_source = True
    if not legacy:
        return b"\0" * file_offset + pack.serialize(file_offset)
    current = tpk.jdlz_compress

    def without_terminator(raw: bytes) -> bytes:
        blob = current(raw)
        return blob[:12] + struct.pack("<I", len(blob) - 1) + blob[16:-1]

    with mock.patch.object(tpk, "jdlz_compress", without_terminator):
        return b"\0" * file_offset + pack.serialize(file_offset)


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow and numpy")
class HdpackTests(unittest.TestCase):
    def setUp(self):
        from nfstex import hdpack
        self.hdpack = hdpack

    def test_verify_rebuilt_accepts_good_and_rejects_bad_blobs(self):
        data = streaming_file(4096 + 16)
        self.assertEqual(2, self.hdpack.verify_rebuilt("Cars/X/TEXTURES.BIN", data, data))
        with self.assertRaises(SystemExit):
            self.hdpack.verify_rebuilt("Cars/X/TEXTURES.BIN", data, streaming_file(4096 + 16, legacy=True))
        # A rebuilt file that lost a texture is refused too.
        pack = synthetic_pack()
        pack.compressed_source = True
        del pack.textures[1]
        with self.assertRaises(SystemExit):
            self.hdpack.verify_rebuilt("Cars/X/TEXTURES.BIN", data, pack.serialize(0))
        # So is one whose records lost a rendering field (here TilableUV), while a new size and format pass.
        pack = synthetic_pack()
        pack.compressed_source = True
        pack.textures[0].record[0x4F] ^= 1
        with self.assertRaises(SystemExit):
            self.hdpack.verify_rebuilt("Cars/X/TEXTURES.BIN", data, pack.serialize(0))
        pack.textures[0].record[0x4F] ^= 1
        struct.pack_into("<HH", pack.textures[0].record, 0x44, 32, 32)
        pack.textures[0].record[0x4A] = tpk.COMP_DXT3
        self.assertEqual(2, self.hdpack.verify_rebuilt("Cars/X/TEXTURES.BIN", data, pack.serialize(0)))

    def test_repack_rewrites_every_file_and_bumps_the_version(self):
        source = streaming_file(0)
        bundle = synthetic_pack().serialize(0)
        with tempfile.TemporaryDirectory() as folder:
            old = os.path.join(folder, "old.zip")
            new = os.path.join(folder, "new.zip")
            with zipfile.ZipFile(old, "w") as zf:
                zf.writestr("Cars/X/TEXTURES.BIN", streaming_file(0, legacy=True))    # a v5-era file
                zf.writestr("Global/InGameB.lzc", bundle)
                zf.writestr("Other/notes.bin", b"not a texture file")
                zf.writestr("nfsu-modpack.json", json.dumps({"format": 1, "name": "HD cars pack", "version": "5", "game": "nfsu1",
                                                              "notes": "n", "files": [], "remove": []}))
            log = []
            self.hdpack.repack(old, new, "7", None, log.append)
            with zipfile.ZipFile(new) as zf:
                manifest = json.loads(zf.read("nfsu-modpack.json"))
                self.assertEqual(("7", "HD cars pack"), (manifest["version"], manifest["name"]))
                self.assertIn("Repacked from version 5", manifest["notes"])
                self.assertEqual({"Cars/X/TEXTURES.BIN", "Global/InGameB.lzc", "Other/notes.bin"}, {f["path"] for f in manifest["files"]})
                fixed = zf.read("Cars/X/TEXTURES.BIN")
                self.assertEqual(source, fixed)                                  # the current serializer's output
                self.assertEqual(2, tpk.verify_jdlz_blobs(fixed))
                self.assertEqual(bundle, zf.read("Global/InGameB.lzc"))
                self.assertEqual(b"not a texture file", zf.read("Other/notes.bin"))
            self.assertTrue(any("2 JDLZ blobs pass" in line for line in log))


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow and numpy")
class ShardTests(unittest.TestCase):
    """Planned, sharded and cached upscaling: what the matrix workflow runs (no torch: results are faked)."""

    def setUp(self):
        from nfstex import hdpack
        from PIL import Image
        self.hdpack, self.Image = hdpack, Image
        self.folder = tempfile.TemporaryDirectory()
        self.game = os.path.join(self.folder.name, "game")
        os.makedirs(os.path.join(self.game, "Global"))
        with open(os.path.join(self.game, "Global", "HUDTEX.BIN"), "wb") as f:
            f.write(synthetic_pack().serialize(0))     # 16x16 DXT1 + 16x16 P8: both 4x under "hud"

    def tearDown(self):
        self.folder.cleanup()

    def fake_dds(self, job):
        size = (job.texture.width * job.factor, job.texture.height * job.factor)
        return self.hdpack.mip_chain_dds(self.Image.new("RGBA", size, (200, 40, 40, 255)), tpk.COMP_DXT1)

    def test_keys_follow_everything_the_result_depends_on(self):
        texture = synthetic_pack().textures[0]
        key = self.hdpack.job_key(texture, 4, "x4plus", "animevideo")
        self.assertEqual(key, self.hdpack.job_key(synthetic_pack().textures[0], 4, "x4plus", "animevideo"))
        self.assertNotEqual(key, self.hdpack.job_key(texture, 2, "x4plus", "animevideo"))
        self.assertNotEqual(key, self.hdpack.job_key(texture, 4, "animevideo", "animevideo"))
        with mock.patch.object(self.hdpack, "UPSCALE_VERSION", "test"):
            self.assertNotEqual(key, self.hdpack.job_key(texture, 4, "x4plus", "animevideo"))
        other = synthetic_pack().textures[0]
        other.pixels = bytes(reversed(other.pixels))
        self.assertNotEqual(key, self.hdpack.job_key(other, 4, "x4plus", "animevideo"))

    def test_models_by_factor(self):
        self.assertEqual(("x4plus", "animevideo"), self.hdpack.models_for(4, False))
        self.assertEqual(("x2plus", "animevideo"), self.hdpack.models_for(2, False))
        self.assertEqual(("animevideo", "animevideo"), self.hdpack.models_for(2, True))

    def test_shares_are_balanced_complete_and_stable(self):
        jobs = []
        for n, size in enumerate([512, 256, 256, 128, 128, 128, 64, 64, 32, 32, 16]):
            texture = synthetic_pack().textures[0]
            struct.pack_into("<HH", texture.record, 0x44, size, size)
            texture.pixels = bytes([n]) * 16
            jobs.append(self.hdpack.Job(f"{n:040x}", "X", texture, 2, "x2plus", "animevideo"))
        shares, loads = self.hdpack.assign_shards(jobs, 3)
        self.assertEqual(sorted(j.key for j in jobs), sorted(j.key for share in shares for j in share))
        self.assertLessEqual(max(loads), 512 * 512 * 1.0 + 1)        # the biggest job alone bounds the busiest share
        again, _ = self.hdpack.assign_shards(list(reversed(jobs)), 3)
        self.assertEqual([[j.key for j in s] for s in shares], [[j.key for j in s] for s in again])

    def test_shards_fill_the_cache_and_the_pack_needs_no_models(self):
        cache_dir = os.path.join(self.folder.name, "cache")
        cache = self.hdpack.Cache(cache_dir)
        game = self.hdpack.pathlib.Path(self.game)
        jobs = self.hdpack.plan(game, "hud")
        self.assertEqual(2, len(jobs))
        self.assertEqual({("x4plus", "animevideo", 4)}, {(j.colour, j.alpha, j.factor) for j in jobs})
        log = []
        with mock.patch.object(self.hdpack, "build_dds", side_effect=lambda job, models: self.fake_dds(job)) as build:
            done = sum(self.hdpack.run_shard(jobs, k, 2, cache, "models", log.append) for k in range(2))
            self.assertEqual((2, 2), (done, build.call_count))
            # A second pass finds everything cached.
            self.assertEqual(0, sum(self.hdpack.run_shard(jobs, k, 2, cache, "models", log.append) for k in range(2)))
            self.assertEqual(2, build.call_count)
        self.assertEqual(2, len([n for n in os.listdir(cache_dir) if n.endswith(".dds")]))
        # The pack step: every texture from the cache, no models; the rebuilt bundle holds the 64x64 results.
        out = self.hdpack.process_bundle(game, "Global/HUDTEX.BIN", "hud", None, log.append, cache=cache)
        textures = [t for offset, chunk in tpk.find_texture_packs(out) for t in tpk.TexturePack.parse(chunk, out, offset).textures]
        self.assertEqual([(64, 64, tpk.COMP_DXT1)] * 2, [(t.width, t.height, t.compression) for t in textures])
        self.assertTrue(any("2 upscaled (2 from the cache)" in line for line in log))
        # Without the cache and without models the build stops instead of shipping stock textures.
        with self.assertRaises(SystemExit):
            self.hdpack.process_bundle(game, "Global/HUDTEX.BIN", "hud", None, log.append, cache=self.hdpack.Cache(None))


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow and numpy")
class PolicyTests(unittest.TestCase):
    def setUp(self):
        from nfstex import hdpack
        self.hdpack = hdpack

    def test_factors(self):
        factor = self.hdpack.factor_for
        track, car, ui = (self.hdpack.POLICIES[name] for name in ("track", "car", "ui"))
        cases = {(64, 64): 4, (128, 128): 4, (16, 128): 4, (256, 256): 2, (512, 256): 2, (512, 512): 2,
                 (1024, 1024): 1, (1024, 512): 1,
                 # Road strips grow along their length up to 2048, within a megapixel.
                 (128, 1024): 2, (1024, 128): 2, (256, 1024): 2, (512, 2048): 1}
        for (width, height), expected in cases.items():
            self.assertEqual(expected, factor(track, width, height), (width, height))
        self.assertEqual((1, 2, 4), (factor(car, 512, 512), factor(car, 256, 64), factor(car, 32, 32)))
        self.assertEqual((1, 2), (factor(ui, 128, 1024), factor(ui, 512, 128)))      # no strip rule outside tracks

    def test_what_each_policy_touches(self):
        hdpack, pack = self.hdpack, synthetic_pack()
        square, paletted = pack.textures
        track, sky, hud = (hdpack.POLICIES[name] for name in ("track", "sky", "hud"))
        self.assertEqual("", hdpack.should_skip(square, track))
        self.assertNotEqual("", hdpack.should_skip(paletted, track))          # paletted world textures stay
        self.assertEqual("", hdpack.should_skip(paletted, hud))
        for name, upscaled in (("SKY_OVERCASTA", True), ("SKY_OVERCASTA_CAP", True), ("SKY_MOON_A", True),
                               ("SKY_OVERCASTA_ENVMAP", False), ("TRACK1001_MAP", False)):
            square.record[12:36] = name.encode().ljust(24, b"\0")
            self.assertEqual(upscaled, hdpack.should_skip(square, sky) == "", name)
        for flag, axes in ((0, ""), (1, "x"), (2, "y"), (3, "xy")):
            square.record[0x4F] = flag
            self.assertEqual(axes, hdpack.wrap_axes(square, track))
            self.assertIsNone(hdpack.wrap_axes(square, hud))

    def test_keys_cover_edges_and_kept_formats_and_keep_old_keys(self):
        import hashlib
        hdpack, texture = self.hdpack, synthetic_pack().textures[0]
        keys = {hdpack.job_key(texture, 4, "x4plus", "animevideo", wrap) for wrap in (None, "", "x", "xy")}
        keys.add(hdpack.job_key(texture, 4, "x4plus", "animevideo", None, keep_uncompressed=True))
        keys.add(hdpack.job_key(texture, 4, "x4plus", "animevideo", None, fix_colour=True))
        self.assertEqual(6, len(keys))
        legacy = hashlib.sha1(f"nfstex-hd {hdpack.UPSCALE_VERSION} 16x16 {tpk.COMP_DXT1} x4 x4plus animevideo".encode())
        legacy.update(texture.pixels)
        legacy.update(texture.palette)
        self.assertEqual(legacy.hexdigest(), hdpack.job_key(texture, 4, "x4plus", "animevideo"))

    def test_wrapped_mips_tile(self):
        import numpy as np
        from PIL import Image
        hdpack = self.hdpack
        rng = np.random.default_rng(5)
        image = Image.fromarray(rng.integers(0, 256, size=(32, 64, 4)).astype(np.uint8), "RGBA")
        level = np.asarray(hdpack.downsample(image, (16, 8), "xy")).astype(np.float32)
        endless = np.asarray(hdpack.downsample(Image.fromarray(np.tile(np.asarray(image), (3, 3, 1)), "RGBA"), (48, 24), None))
        np.testing.assert_array_equal(endless[8:16, 16:32], level)
        plain = np.asarray(hdpack.downsample(image, (16, 8), None))
        np.testing.assert_array_equal(np.asarray(image.resize((16, 8), Image.LANCZOS)), plain)
        dds = hdpack.mip_chain_dds(image, tpk.COMP_DXT1, "xy")
        width, height, mips, compression, _ = tpk.parse_dds(dds)
        self.assertEqual((64, 32, 3, tpk.COMP_DXT1), (width, height, mips, compression))


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow and numpy")
class TracksProfileTests(unittest.TestCase):
    """The tracks profile end to end on the synthetic game of test_tracks (results faked, no torch)."""

    def setUp(self):
        from nfstex import hdpack
        from nfstex.tests.test_tracks import SyntheticGame
        self.hdpack = hdpack
        self.folder = tempfile.TemporaryDirectory()
        self.game = SyntheticGame(self.folder.name)
        root = self.game.root
        sky = synthetic_pack()
        sky.header[4:9] = b"TRACK"
        square, paletted = sky.textures
        square.record[12:36] = b"SKY_TEST".ljust(24, b"\0")
        square.record[0x4F] = 1
        paletted.record[12:36] = b"TRACK1234_MAP".ljust(24, b"\0")
        (root / "Tracks/TEX1234TRACK.BIN").write_bytes(sky.serialize(0))
        hdpack._stream_layout.cache_clear()

    def tearDown(self):
        self.folder.cleanup()
        self.hdpack._stream_layout.cache_clear()

    def fake_dds(self, job, models_dir):
        import numpy as np
        from PIL import Image
        size = (job.texture.width * job.factor, job.texture.height * job.factor)
        image = Image.fromarray(np.full((size[1], size[0], 4), (90, 120, 30, 255), np.uint8), "RGBA")
        keep = job.texture.compression in (tpk.COMP_A8R8G8B8, tpk.COMP_P8) and job.keep_uncompressed
        return self.hdpack.mip_chain_dds(image, tpk.COMP_A8R8G8B8 if keep else tpk.COMP_DXT1, job.wrap)

    def test_plan_shards_and_pack(self):
        hdpack, root = self.hdpack, self.game.root
        jobs = hdpack.plan(root, "tracks")
        by_name = {}
        for job in jobs:
            by_name.setdefault(job.texture.name, []).append(job)
        # Stream SQUAREs (Y0 32 px, A1 64 px, Z0 16 px) and the sky; PALETTED (P8) and the map stay stock.
        self.assertEqual({"SQUARE", "SKY_TEST"}, set(by_name))
        self.assertEqual({(32, 4, "xy"), (64, 4, "x"), (16, 4, "y")},
                         {(j.texture.width, j.factor, j.wrap) for j in by_name["SQUARE"]})
        self.assertEqual([(4, "x", True)], [(j.factor, j.wrap, j.keep_uncompressed) for j in by_name["SKY_TEST"]])
        cache = hdpack.Cache(os.path.join(self.folder.name, "cache"))
        with mock.patch.object(hdpack, "build_dds", side_effect=self.fake_dds):
            self.assertEqual(len(jobs), sum(hdpack.run_shard(jobs, k, 3, cache, "models", lambda m: None) for k in range(3)))
        log = []
        files = hdpack.build_pack(root, "tracks", None, cache, log.append)
        self.assertEqual({"Tracks/STREAMT1.BUN", "Tracks/STREAMT1_1234.BUN", "Tracks/TRACKB1234.lzc", "Tracks/TRACKB1235.lzc",
                          "Tracks/TEX1234TRACK.BIN"}, set(files))
        region = files["Tracks/STREAMT1.BUN"]
        sizes = sorted((t.name, t.width, t.compression) for o, c in tpk.find_texture_packs(region)
                       for t in tpk.TexturePack.parse(c, region, o).textures)
        self.assertEqual([("PALETTED", 16, tpk.COMP_P8), ("PALETTED", 16, tpk.COMP_P8),
                          ("SQUARE", 128, tpk.COMP_DXT1), ("SQUARE", 256, tpk.COMP_DXT1)], sizes)
        sky = files["Tracks/TEX1234TRACK.BIN"]
        textures = {t.name: t for o, c in tpk.find_texture_packs(sky) for t in tpk.TexturePack.parse(c, sky, o).textures}
        self.assertEqual((64, 64, tpk.COMP_DXT1), (textures["SKY_TEST"].width, textures["SKY_TEST"].height, textures["SKY_TEST"].compression))
        self.assertEqual((16, tpk.COMP_P8), (textures["TRACK1234_MAP"].width, textures["TRACK1234_MAP"].compression))
        self.assertTrue(any("streaming pool" in line for line in log))
        # Without the cache the pack step refuses rather than shipping stock textures in a stream.
        with self.assertRaises(SystemExit):
            hdpack.build_pack(root, "tracks", None, hdpack.Cache(None), lambda m: None)

    def test_split_keeps_each_race_whole(self):
        hdpack, root = self.hdpack, self.game.root
        cache = hdpack.Cache(os.path.join(self.folder.name, "cache"))
        jobs = hdpack.plan(root, "tracks")
        with mock.patch.object(hdpack, "build_dds", side_effect=self.fake_dds):
            hdpack.run_shard(jobs, 0, 1, cache, "models", lambda m: None)
        files = hdpack.build_pack(root, "tracks", None, cache, lambda m: None)
        # A second region with its own race, and a file no race names, so there is something to split.
        other = dict(files)
        groups = hdpack.pack_groups(files, files.__getitem__, lambda name: len(files[name]))
        self.assertEqual([{"Tracks/STREAMT1.BUN", "Tracks/STREAMT1_1234.BUN", "Tracks/TRACKB1234.lzc", "Tracks/TRACKB1235.lzc",
                           "Tracks/TEX1234TRACK.BIN"}], groups)
        other["Global/LONE.BIN"] = bytes(range(256)) * 4
        pack = os.path.join(self.folder.name, "pack.zip")
        with zipfile.ZipFile(pack, "w", zipfile.ZIP_STORED) as zf:
            for name, data in other.items():
                zf.writestr(name, data)
            zf.writestr("nfsu-modpack.json", json.dumps({"format": 1, "name": "HD tracks pack", "version": "11", "game": "nfsu1",
                                                          "notes": "n", "files": [{"path": p, "bytes": len(b)} for p, b in other.items()],
                                                          "remove": []}))
        self.assertEqual([], hdpack.split_pack(pack, 1 << 30, lambda m: None))           # fits: nothing to do
        with zipfile.ZipFile(pack) as zf:
            race_bytes = sum(zf.getinfo(name).compress_size for name in groups[0])
        # Room for the race group (parts keep 2 % spare) but not for the lone file as well.
        parts = hdpack.split_pack(pack, int((race_bytes + 512) / 0.98), lambda m: None)
        self.assertEqual(2, len(parts))
        seen = []
        for number, part in enumerate(parts, 1):
            with zipfile.ZipFile(part) as zf:
                manifest = json.loads(zf.read("nfsu-modpack.json"))
                names = sorted(i.filename for i in zf.infolist() if i.filename != "nfsu-modpack.json")
                self.assertEqual(f"HD tracks pack ({number} of 2)", manifest["name"])
                self.assertEqual(names, sorted(f["path"] for f in manifest["files"]))
                for name in names:
                    self.assertEqual(other[name], zf.read(name))
                seen += names
                # The race group never splits across parts.
                race_files = {n for n in names if n.startswith("Tracks/")}
                self.assertIn(race_files, (set(), groups[0]))
        self.assertEqual(sorted(other), sorted(seen))
        with self.assertRaises(SystemExit):                                              # a group bigger than a part
            hdpack.split_pack(pack, 2048, lambda m: None)

    def test_only_one_stream(self):
        hdpack, root = self.hdpack, self.game.root
        cache = hdpack.Cache(os.path.join(self.folder.name, "cache"))
        jobs = hdpack.plan(root, "tracks", only="Tracks/STREAMT1_1234.BUN")
        self.assertEqual({(16, "y")}, {(j.texture.width, j.wrap) for j in jobs})
        with mock.patch.object(hdpack, "build_dds", side_effect=self.fake_dds):
            hdpack.run_shard(jobs, 0, 1, cache, "models", lambda m: None)
        files = hdpack.build_pack(root, "tracks", None, cache, lambda m: None, only="Tracks/STREAMT1_1234.BUN")
        # Its race's table moves; the region stream and the other race's table stay stock.
        self.assertEqual({"Tracks/STREAMT1_1234.BUN", "Tracks/TRACKB1234.lzc"}, set(files))


def named_pack(path: str, names, size: int = 16, compressed: bool = False, mips: int = 2) -> tpk.TexturePack:
    """A pack whose header path gives its collection name (``Global\\FrontEndTexturesPC.tpk``) and whose textures
    carry the game's name hashes, so a mod's DDS stems find them: DXT1, ``mips`` levels, each with bytes of its
    own. Record names are cut at 24 bytes as the game's are (the hash is of the whole name)."""
    pack = synthetic_pack()
    pack.header[32:96] = path.encode("latin1").ljust(64, b"\0")
    pack.compressed_source = compressed
    chain = sum(tpk.mip_sizes(size, size, mips, tpk.COMP_DXT1))
    base = tpk.mip_sizes(size, size, 1, tpk.COMP_DXT1)[0]
    pack.textures = [tpk.Texture(make_record(name[:24], size, size, tpk.COMP_DXT1, mips, chain, base, tpk.bin_hash(name)),
                                 make_format(b"DXT1"), bytes((index * 31 + i) % 251 for i in range(chain)), b"")
                     for index, name in enumerate(names)]
    return pack


def dxt1_dds(width: int, height: int, seed: int) -> bytes:
    """A one-mip DXT1 DDS of the given size, with bytes of its own."""
    size = sum(tpk.mip_sizes(width, height, 1, tpk.COMP_DXT1))
    record = make_record("DDS", width, height, tpk.COMP_DXT1, 1, size, size, 0)
    return tpk.Texture(record, make_format(b"DXT1"), bytes((seed + i * 13) % 256 for i in range(size)), b"").to_dds()


def textures_of(data: bytes) -> dict:
    """{collection: {name hash: texture}} of a bundle."""
    return {endscript.collection_name(pack.path): {t.hash: t for t in pack.textures}
            for pack in (tpk.TexturePack.parse(chunk, data, offset) for offset, chunk in tpk.find_texture_packs(data))}


MOD_SCRIPT = r'''[VERSN2]
if collection_exists GLOBAL\DYNTEX.BIN TPKBlocks DEFAULT
do
update_collection GLOBAL\DYNTEX.BIN TPKBlocks DYNAMICTEXTURES CompressionType CompressedFullData
else
end
combobox "Install" "Uninstall" "Do you want to install or uninstall this mod?"
"Install"
bind_textures synchronize GLOBAL\DYNTEX.BIN TPKBlocks DYNAMICTEXTURES "Textures\DYNTEX\DYNAMICTEXTURES"
bind_textures synchronize GLOBAL\GLOBALB.LZC TPKBlocks GLOBALTEXTURES "Textures\GlobalB\GLOBALTEXTURES"
"Uninstall"
import override GLOBAL\DYNTEX.BIN TPKBlocks TPKBlocks\DYNAMICTEXTURES.BIN
end
move_file override relative absolute Main\scripts\NFSUHDContentSupport.asi scripts\NFSUHDContentSupport.asi
watermark "Remastered UI by Tester"
'''


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow and numpy")
class OverlayTests(unittest.TestCase):
    """--overlay: a Binarius texture mod laid over installed HD packs or the stock files."""

    def setUp(self):
        from nfstex import hdpack
        self.hdpack = hdpack
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)

    def folder(self, name: str, files: dict) -> pathlib.Path:
        folder = self.root / name
        folder.mkdir(parents=True)
        for stem, data in files.items():
            (folder / f"{stem}.dds").write_bytes(data)
        return folder

    def test_textures_are_replaced_by_name_and_the_rest_kept(self):
        for compressed in (False, True):            # raw bundles, and packs that keep JDLZ blobs (the track sky files)
            with self.subTest(compressed=compressed):
                pack = named_pack("Global\\FrontEndTexturesPC.tpk", ["BUTTON", "LOGO", "3RDPERSON_DRIFTANGLE_ANGLE_DEGREES"],
                                  compressed=compressed)
                data = b"\0" * 64 + pack.serialize(64)
                folder = self.folder(f"FRONTENDTEXTURESPC-{compressed}", {
                    "BUTTON": dxt1_dds(64, 32, 1),
                    "3RDPERSON_DRIFTANGLE_ANGLE_DEGREE": dxt1_dds(32, 32, 2)})     # named after the cut record name
                binding = endscript.Binding("FRONTEND/FRONTB.LZC", "FRONTENDTEXTURESPC", folder, "synchronize")
                out, report = self.hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [binding], [], lambda line: None)
                after = textures_of(out)["FRONTENDTEXTURESPC"]
                self.assertEqual(3, len(after))                                        # replaced, never added
                button = after[tpk.bin_hash("BUTTON")]
                # The DDS's own level, then the levels it came without (the game mipmaps this texture): 64x32 -> 16x8.
                self.assertEqual((64, 32, 3, tpk.COMP_DXT1), (button.width, button.height, button.mips, button.compression))
                self.assertEqual(tpk.parse_dds(dxt1_dds(64, 32, 1))[4], button.pixels[:1024])
                self.assertEqual(1024 + 256 + 64, len(button.pixels))
                gauge = after[tpk.bin_hash("3RDPERSON_DRIFTANGLE_ANGLE_DEGREES")]      # the misnamed file found its texture
                self.assertEqual(tpk.parse_dds(dxt1_dds(32, 32, 2))[4], gauge.pixels[:512])
                self.assertEqual(2, report["mips_completed"])
                self.assertEqual(["3RDPERSON_DRIFTANGLE_ANGLE_DEGREE -> 3RDPERSON_DRIFTANGLE_ANGLE_DEGREES"], report["aliased"])
                logo = after[tpk.bin_hash("LOGO")]                                     # not in the mod: exactly as it was
                self.assertEqual((pack.textures[1].pixels, 16, 2), (logo.pixels, logo.width, logo.mips))
                self.assertEqual(bytes(pack.textures[1].record[:0x30]), bytes(logo.record[:0x30]))
                self.assertEqual(2, report["replaced"])
                self.assertEqual(3 if compressed else 0, report["blobs"])              # one per texture, each passing the game's decoder

    def test_missing_mip_levels(self):
        import numpy as np
        from PIL import Image
        hdpack = self.hdpack
        self.assertEqual([4, 5, 3, 1, 6], [hdpack.game_mip_count(*size) for size in ((256, 64), (128, 128), (64, 32), (8, 8), (1024, 256))])
        # A solid colour in, its own top level kept byte for byte, every added level the same colour.
        for compression, colour in ((tpk.COMP_DXT1, (200, 40, 40, 255)), (tpk.COMP_DXT3, (30, 90, 200, 136)),
                                    (tpk.COMP_A8R8G8B8, (10, 220, 120, 77))):
            with self.subTest(compression=compression):
                full = hdpack.mip_chain_dds(Image.new("RGBA", (64, 32), colour), compression)
                width, height, mips, _, pixels = tpk.parse_dds(full)
                top = tpk.mip_sizes(64, 32, 1, compression)[0]
                single = bytearray(full[:128] + pixels[:top])
                struct.pack_into("<I", single, 28, 1)
                done, added = hdpack.complete_mips(bytes(single))
                self.assertEqual(2, added)
                self.assertEqual((64, 32, 3, compression), tpk.parse_dds(done)[:4])
                self.assertEqual(bytes(single[128:]), tpk.parse_dds(done)[4][:top])
                self.assertEqual(sum(tpk.mip_sizes(64, 32, 3, compression)), len(done) - 128)        # nothing more
                with Image.open(io.BytesIO(done)) as image:
                    expected = np.array(image.convert("RGBA"))[0, 0]
                levels = tpk.mip_sizes(64, 32, 3, compression)
                rest = tpk.parse_dds(done)[4][top:]
                for (w, h), size in zip(((32, 16), (16, 8)), levels[1:]):
                    level, rest = rest[:size], rest[size:]
                    texture = tpk.Texture(make_record("L", w, h, compression, 1, size, size, 0),
                                          make_format(b"DXT1" if compression == tpk.COMP_DXT1 else b"DXT3"), level, b"")
                    if compression == tpk.COMP_A8R8G8B8:
                        texture.format[20:24] = struct.pack("<I", tpk.D3DFMT_A8R8G8B8)
                    with Image.open(io.BytesIO(texture.to_dds())) as image:
                        decoded = np.array(image.convert("RGBA"))
                    self.assertTrue((np.abs(decoded.astype(int) - expected.astype(int)) <= 17).all(), (w, h, decoded[0, 0], expected))
        # A full chain, and a format there is no encoder for, come back unchanged.
        full = hdpack.mip_chain_dds(Image.new("RGBA", (32, 32), (1, 2, 3, 255)), tpk.COMP_DXT1)
        self.assertEqual((full, 0), hdpack.complete_mips(full))
        dxt5 = bytearray(dxt1_dds(32, 32, 9)[:128] + bytes(1024))
        dxt5[84:88] = b"DXT5"
        self.assertEqual((bytes(dxt5), 0), hdpack.complete_mips(bytes(dxt5)))
        # In an overlay: only where the game mipmaps the texture it replaces.
        quiet = lambda line: None
        for stock_mips, expected_mips in ((2, 3), (1, 1)):
            data = named_pack("Global\\FrontEndTexturesPC.tpk", ["BUTTON"], mips=stock_mips).serialize(0)
            binding = endscript.Binding("FRONTEND/FRONTB.LZC", "FRONTENDTEXTURESPC", self.folder(f"m{stock_mips}", {"BUTTON": dxt1_dds(64, 32, 4)}), "synchronize")
            out, report = hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [binding], [], quiet)
            self.assertEqual(expected_mips, textures_of(out)["FRONTENDTEXTURESPC"][tpk.bin_hash("BUTTON")].mips)
            self.assertEqual(int(expected_mips > 1), report["mips_completed"])
        data = named_pack("Global\\FrontEndTexturesPC.tpk", ["BUTTON"]).serialize(0)
        binding = endscript.Binding("FRONTEND/FRONTB.LZC", "FRONTENDTEXTURESPC", self.folder("dxt5", {"BUTTON": bytes(dxt5)}), "synchronize")
        out, report = hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [binding], [], quiet)
        self.assertEqual((["BUTTON"], 1), (report["mips_short"], textures_of(out)["FRONTENDTEXTURESPC"][tpk.bin_hash("BUTTON")].mips))

    def test_what_an_overlay_refuses(self):
        data = named_pack("Global\\FrontEndTexturesPC.tpk", ["BUTTON"]).serialize(0)
        quiet = lambda line: None

        def bind(name: str, files: dict, collection: str = "FRONTENDTEXTURESPC"):
            return endscript.Binding("FRONTEND/FRONTB.LZC", collection, self.folder(name, files), "synchronize")

        cases = (
            (bind("unknown", {"BUTTON": dxt1_dds(32, 32, 0), "NOT_IN_THE_GAME": dxt1_dds(16, 16, 0)}),
             "1 textures the pack does not have: NOT_IN_THE_GAME"),             # Binarius would add it; nothing uses it
            (bind("elsewhere", {"BUTTON": dxt1_dds(32, 32, 0)}, "FRONTENDTEXTURES"), "no texture pack FRONTENDTEXTURES"),
            (bind("odd", {"BUTTON": dxt1_dds(24, 16, 0)}), "powers of two"),
        )
        for binding, reason in cases:
            with self.subTest(reason=reason), self.assertRaisesRegex(SystemExit, reason):
                self.hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [binding], [], quiet)
        # The TexEd repair waits on a pack named DEFAULT: none in the stock files, so it changes nothing ...
        repair = endscript.Conditional("FRONTEND/FRONTB.LZC", "DEFAULT", [endscript.UpdateCollection(
            "FRONTEND/FRONTB.LZC", "FRONTENDTEXTURESPC", "CompressionType", "RawDecompressed")], [])
        good = bind("good", {"BUTTON": dxt1_dds(32, 32, 0)})
        self.assertEqual(1, self.hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [good], [repair], quiet)[1]["replaced"])
        # ... and a file that has one is refused rather than half repaired.
        damaged = data + named_pack("Global\\Default.tpk", ["X"]).serialize(len(data))
        with self.assertRaisesRegex(SystemExit, "named DEFAULT"):
            self.hdpack.overlay_file("FrontEnd/FrontB.lzc", damaged, [good], [repair], quiet)
        # The last check reads the rebuilt file back: a texture that never took its DDS stops the build.
        with mock.patch.object(tpk.Texture, "replace_with_dds", lambda texture, dds: None), \
                self.assertRaisesRegex(SystemExit, "BUTTON.dds did not land in FRONTENDTEXTURESPC"):
            self.hdpack.overlay_file("FrontEnd/FrontB.lzc", data, [good], [], quiet)

    def mod(self, script: str = MOD_SCRIPT, game: str = "Underground1", extra: dict = None, launcher: str = "Mod.end") -> pathlib.Path:
        """A Binarius mod folder laid out as Plajer's (launcher, Assets/main.end, Assets/Textures/...); returns
        its launcher file. ``extra``: more texture folders, {path under Assets/Textures: {stem: DDS}}."""
        mod = pathlib.Path(tempfile.mkdtemp(dir=self.root))
        textures = mod.relative_to(self.root) / "Assets/Textures"
        self.folder(str(textures / "DYNTEX/DYNAMICTEXTURES"), {"CARD": dxt1_dds(128, 128, 3)})
        self.folder(str(textures / "GlobalB/GLOBALTEXTURES"), {"SPEEDO": dxt1_dds(128, 64, 4)})
        for folder, files in (extra or {}).items():
            self.folder(str(textures / folder), files)
        (mod / "Assets/main.end").write_text(script, encoding="utf-8")
        launcher = mod / launcher
        launcher.write_text('[VERSN1]\n{\n  "Usage": "User",\n  "Game": "%s",\n  "Directory": "",\n'
                            '  "Endscript": "Assets\\main.end"\n}\n' % game, encoding="utf-8")
        return launcher

    def game_and_bases(self):
        """Stock Global/DYNTEX.BIN (raw) and Global/GlobalB.lzc (JDLZ, as shipped), and two installed HD packs that
        both replace DYNTEX.BIN: v10 with a 32 px BADGE, v11 with a 64 px one."""
        game = self.root / "game"
        (game / "Global").mkdir(parents=True)
        (game / "Global/DYNTEX.BIN").write_bytes(named_pack("Global\\DynamicTextures.tpk", ["CARD", "BADGE"]).serialize(0))
        (game / "Global/GlobalB.lzc").write_bytes(tpk.jdlz_compress(
            named_pack("Global\\GlobalTextures.tpk", ["SPEEDO", "NEEDLE"]).serialize(0)))
        (game / "Cars").mkdir()
        (game / "Cars/TEXTURES.BIN").write_bytes(named_pack("Global\\CarTextures.tpk", ["DECAL_ONE", "DECAL_TWO"],
                                                            compressed=True).serialize(0))
        bases = []
        for version, badge in (("10", 32), ("11", 64)):
            base = self.root / f"nfsu-hdpack-v{version}.zip"
            with zipfile.ZipFile(base, "w") as zf:
                zf.writestr("Global/DYNTEX.BIN", named_pack("Global\\DynamicTextures.tpk", ["CARD", "BADGE"], size=badge).serialize(0))
            bases.append(str(base))
        return game, bases

    def test_a_mod_goes_on_top_of_the_installed_packs(self):
        game, bases = self.game_and_bases()
        log = []
        files, min_version, scripts = self.hdpack.overlay_pack(game, str(self.mod()), bases, log.append)
        # DYNTEX from the last base that has it; GlobalB from the stock file, decompressed and under both names.
        self.assertEqual({"Global/DYNTEX.BIN", "Global/GlobalB.lzc", "Global/GLOBALB.BUN"}, set(files))
        self.assertEqual(files["Global/GlobalB.lzc"], files["Global/GLOBALB.BUN"])
        self.assertFalse(tpk.is_jdlz(files["Global/GlobalB.lzc"]))
        dyntex = textures_of(files["Global/DYNTEX.BIN"])["DYNAMICTEXTURES"]
        self.assertEqual(128, dyntex[tpk.bin_hash("CARD")].width)                   # the mod's
        self.assertEqual(64, dyntex[tpk.bin_hash("BADGE")].width)                   # v11's, kept (not v10's 32, not stock 16)
        globalb = textures_of(files["Global/GlobalB.lzc"])["GLOBALTEXTURES"]
        self.assertEqual((128, 64), (globalb[tpk.bin_hash("SPEEDO")].width, globalb[tpk.bin_hash("SPEEDO")].height))
        self.assertEqual(16, globalb[tpk.bin_hash("NEEDLE")].width)
        # The plugin the mod copies is in build 14's runtime; the repair block found no DEFAULT pack.
        self.assertEqual(1014, min_version)
        self.assertEqual(["Remastered UI by Tester"], [s.watermark for s in scripts])
        self.assertTrue(any("Global/DYNTEX.BIN (from nfsu-hdpack-v11.zip)" in line for line in log), log)
        self.assertTrue(any("Global/GlobalB.lzc (from stock)" in line and "also as Global/GLOBALB.BUN" in line for line in log), log)

    def decals_mod(self) -> pathlib.Path:
        """A second mod shaped like Plajer's HD Decals: no combobox, the shared car textures, and DYNTEX too."""
        script = (r'[VERSN2]' '\n'
                  r'unlock_memory all' '\n'
                  r'if collection_exists CARS\TEXTURES.BIN TPKBlocks DEFAULT' '\n' 'do' '\n'
                  r'update_collection CARS\TEXTURES.BIN TPKBlocks CARTEXTURES CompressionType CompressedFullData' '\n'
                  'else' '\n' 'end' '\n'
                  r'bind_textures synchronize CARS\TEXTURES.BIN TPKBlocks CARTEXTURES "Textures\DECALS"' '\n'
                  r'bind_textures synchronize GLOBAL\DYNTEX.BIN TPKBlocks DYNAMICTEXTURES "Textures\MORE"' '\n'
                  r'delete CARS\TEXTURES.BIN' '\n' 'version 2.8.3' '\n' 'watermark "Plajer\'s mod"' '\n')
        return self.mod(script, launcher="HD_Decals.end", extra={
            "DECALS": {"DECAL_ONE": dxt1_dds(64, 16, 6)},
            "MORE": {"CARD": dxt1_dds(256, 256, 7), "BADGE": dxt1_dds(128, 128, 8)}})

    def test_mods_stack_in_the_order_given(self):
        game, bases = self.game_and_bases()
        interface, decals = str(self.mod()), str(self.decals_mod())
        log = []
        files, min_version, scripts = self.hdpack.overlay_pack(game, [interface, decals], bases, log.append)
        self.assertEqual(["Mod.end", "HD_Decals.end"], [s.label for s in scripts])
        self.assertEqual({"Global/DYNTEX.BIN", "Global/GlobalB.lzc", "Global/GLOBALB.BUN", "Cars/TEXTURES.BIN"}, set(files))
        dyntex = textures_of(files["Global/DYNTEX.BIN"])["DYNAMICTEXTURES"]
        self.assertEqual(256, dyntex[tpk.bin_hash("CARD")].width)          # the second mod's, over the first's 128
        self.assertEqual(128, dyntex[tpk.bin_hash("BADGE")].width)         # the second mod's, over v11's 64
        cars = textures_of(files["Cars/TEXTURES.BIN"])["CARTEXTURES"]
        self.assertEqual((64, 16), (cars[tpk.bin_hash("DECAL_ONE")].width, cars[tpk.bin_hash("DECAL_TWO")].width))
        self.assertEqual(1014, min_version)                                 # the first mod's plugin
        self.assertTrue(any("Global/DYNTEX.BIN (from Mod.end)" in line for line in log), log)
        self.assertTrue(any("Cars/TEXTURES.BIN (from stock)" in line for line in log), log)
        # The other way round the interface mod's CARD wins, and the rest is the same.
        files, _, _ = self.hdpack.overlay_pack(game, [decals, interface], bases, lambda line: None)
        dyntex = textures_of(files["Global/DYNTEX.BIN"])["DYNAMICTEXTURES"]
        self.assertEqual((128, 128), (dyntex[tpk.bin_hash("CARD")].width, dyntex[tpk.bin_hash("BADGE")].width))

    def test_both_names_of_a_bundle_are_one_bundle(self):
        # GLOBALB.BUN and GlobalB.lzc hold the same bundle: bindings to either land in it, written under both names.
        game, bases = self.game_and_bases()
        (game / "Global/GLOBALB.BUN").write_bytes(tpk.jdlz_decompress((game / "Global/GlobalB.lzc").read_bytes()))
        script = MOD_SCRIPT.replace('"Uninstall"\n',
                                    'bind_textures synchronize GLOBAL\\GLOBALB.BUN TPKBlocks GLOBALTEXTURES "Textures\\BUN\\GLOBALTEXTURES"\n'
                                    '"Uninstall"\n', 1)
        launcher = self.mod(script, extra={"BUN/GLOBALTEXTURES": {"NEEDLE": dxt1_dds(64, 64, 5)}})
        files, _, _ = self.hdpack.overlay_pack(game, str(launcher), bases, lambda line: None)
        self.assertEqual({"Global/DYNTEX.BIN", "Global/GlobalB.lzc", "Global/GLOBALB.BUN"}, set(files))
        self.assertEqual(files["Global/GlobalB.lzc"], files["Global/GLOBALB.BUN"])
        globalb = textures_of(files["Global/GLOBALB.BUN"])["GLOBALTEXTURES"]
        self.assertEqual((128, 64), (globalb[tpk.bin_hash("SPEEDO")].width, globalb[tpk.bin_hash("NEEDLE")].width))

    def test_mods_this_port_cannot_take_are_refused(self):
        game, bases = self.game_and_bases()
        quiet = lambda line: None
        with self.assertRaisesRegex(SystemExit, "made for Underground2"):
            self.hdpack.overlay_pack(game, str(self.mod(game="Underground2")), bases, quiet)
        with self.assertRaisesRegex(SystemExit, "PC plugin d3d9.asi"):
            self.hdpack.overlay_pack(game, str(self.mod(MOD_SCRIPT.replace("NFSUHDContentSupport.asi", "d3d9.asi"))), bases, quiet)
        with self.assertRaisesRegex(SystemExit, "not in the base packs and not in the game folder"):
            self.hdpack.overlay_pack(self.root / "nowhere", str(self.mod()), [], quiet)

    def test_the_pack_names_the_app_build_it_needs(self):
        game, bases = self.game_and_bases()
        out = self.root / "nfsu-hdpack-v12.zip"
        argv = ["hdpack.py", "--overlay", str(self.mod()), "--game", str(game), "--base", bases[0], "--base", bases[1],
                "--out", str(out), "--version", "12", "--credit", "Tester's HD User Interface 1.0"]
        with mock.patch.object(sys, "argv", argv), mock.patch("builtins.print"):
            self.hdpack.main()
        with zipfile.ZipFile(out) as zf:
            manifest = json.loads(zf.read("nfsu-modpack.json"))
            names = set(zf.namelist())
        self.assertEqual(1014, manifest["minVersionCode"])
        self.assertEqual("12", manifest["version"])
        self.assertIn("Tester's HD User Interface 1.0: textures laid over nfsu-hdpack-v10.zip, nfsu-hdpack-v11.zip", manifest["notes"])
        self.assertIn("Needs app build 14 or newer (NFSUHDContentSupport.asi is in its runtime)", manifest["notes"])
        self.assertEqual({f["path"] for f in manifest["files"]} | {"nfsu-modpack.json"}, names)
        self.assertEqual([], manifest["remove"])
        # Two mods in one pack, credited by their watermarks when no --credit is given.
        argv = ["hdpack.py", "--overlay", str(self.mod()), "--overlay", str(self.decals_mod()), "--game", str(game),
                "--out", str(out), "--version", "12"]
        with mock.patch.object(sys, "argv", argv), mock.patch("builtins.print"):
            self.hdpack.main()
        with zipfile.ZipFile(out) as zf:
            manifest = json.loads(zf.read("nfsu-modpack.json"))
        self.assertTrue(manifest["notes"].startswith("Remastered UI by Tester; Plajer's mod: textures laid over the stock files"), manifest["notes"])
        self.assertEqual(1014, manifest["minVersionCode"])
        self.assertIn("Cars/TEXTURES.BIN", {f["path"] for f in manifest["files"]})


if __name__ == "__main__":
    unittest.main()
