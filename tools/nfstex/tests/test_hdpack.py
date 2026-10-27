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


if __name__ == "__main__":
    unittest.main()
