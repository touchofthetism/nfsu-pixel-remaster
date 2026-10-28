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

HAVE_PIL = importlib.util.find_spec("PIL") is not None


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


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow")
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


@unittest.skipUnless(HAVE_PIL, "hdpack imports Pillow")
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


if __name__ == "__main__":
    unittest.main()
