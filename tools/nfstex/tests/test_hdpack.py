"""Pack-level checks for tools/nfstex/hdpack.py: the post-build verification and --repack (no game files, no torch)."""
import importlib.util
import json
import os
import struct
import tempfile
import unittest
import zipfile
from unittest import mock

from nfstex import tpk
from nfstex.tests.test_tpk import synthetic_pack

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


if __name__ == "__main__":
    unittest.main()
