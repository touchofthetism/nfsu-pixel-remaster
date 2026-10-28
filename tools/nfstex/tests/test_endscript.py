"""Binarius endscripts (tools/nfstex/endscript.py) on synthetic mods shaped like Plajer's HD User Interface."""
import pathlib
import tempfile
import unittest

from nfstex import endscript

LAUNCHER = r'''[VERSN1]

{
  "Usage": "User",
  "Game": "Underground1",
  "Directory": "",
  "Endscript": "Assets\main.end",
  "Files": ["GLOBAL\DYNTEX.BIN", "FRONTEND\FRONTB.LZC"]
}'''

SCRIPT = r'''[VERSN2]
// Minimum required version of Binary in order to run the endscript
version 2.10.1
unlock_memory all

// fix the TPKBlock corruption caused by TexEd
if collection_exists GLOBAL\DYNTEX.BIN TPKBlocks DEFAULT
do
update_collection GLOBAL\DYNTEX.BIN TPKBlocks DYNAMICTEXTURES CompressionType CompressedFullData
else
end

combobox "Install" "Uninstall" "Do you want to install or uninstall this mod?"

"Install"

// replace DYNTEX.BIN textures
bind_textures synchronize GLOBAL\DYNTEX.BIN TPKBlocks DYNAMICTEXTURES "Textures\DYNTEX\DYNAMICTEXTURES"
bind_textures synchronize FRONTEND\FRONTB.LZC TPKBlocks FRONTENDTEXTURESPC "Textures\FrontB\FRONTENDTEXTURESPC"   // a comment

"Uninstall"

import override GLOBAL\DYNTEX.BIN TPKBlocks TPKBlocks\DYNAMICTEXTURES.BIN

end

delete GLOBAL\DYNTEX.BIN
create_folder absolute scripts
speedreflect auto
move_file override relative absolute Main\scripts\NFSUHDContentSupport.asi scripts\NFSUHDContentSupport.asi
watermark "Remastered UI by Plajer"
'''


class EndscriptTests(unittest.TestCase):
    def mod(self, script: str = SCRIPT) -> pathlib.Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = pathlib.Path(temporary.name)
        (root / "Assets/Textures/DYNTEX/DYNAMICTEXTURES").mkdir(parents=True)
        (root / "Assets/Textures/FRONTB/FRONTENDTEXTURESPC").mkdir(parents=True)   # the script writes FrontB
        (root / "Assets/main.end").write_text(script, encoding="utf-8")
        (root / "Mod.end").write_text(LAUNCHER, encoding="utf-8")
        return root

    def test_tokens(self):
        self.assertEqual(["bind_textures", "synchronize", "A\\B.BIN", "TPKBlocks", "X", "Textures\\With Space"],
                         endscript.tokenize('bind_textures synchronize A\\B.BIN TPKBlocks X "Textures\\With Space" // note'))
        self.assertEqual([], endscript.tokenize("   // only a comment"))
        self.assertEqual(["Install"], endscript.tokenize('"Install"'))
        with self.assertRaises(endscript.EndscriptError):
            endscript.tokenize('watermark "unterminated')

    def test_install_branch_of_a_launcher(self):
        root = self.mod()
        script = endscript.parse(root / "Mod.end")       # the launcher's JSON has unescaped Windows paths
        self.assertEqual("Underground1", script.game)
        self.assertEqual(root / "Assets/main.end", script.path)
        self.assertEqual([("GLOBAL/DYNTEX.BIN", "DYNAMICTEXTURES", "synchronize"), ("FRONTEND/FRONTB.LZC", "FRONTENDTEXTURESPC", "synchronize")],
                         [(b.game_file, b.collection, b.mode) for b in script.bindings])
        self.assertTrue(script.bindings[1].folder.samefile(root / "Assets/Textures/FRONTB/FRONTENDTEXTURESPC"))   # FrontB, matched without case
        self.assertEqual(["NFSUHDContentSupport.asi"], script.plugins)
        self.assertEqual("Remastered UI by Plajer", script.watermark)
        # The TexEd repair waits on a pack named DEFAULT; the uninstall section's import is never read.
        (condition,) = script.conditionals
        self.assertEqual(("GLOBAL/DYNTEX.BIN", "DEFAULT"), (condition.game_file, condition.collection))
        self.assertEqual([endscript.UpdateCollection("GLOBAL/DYNTEX.BIN", "DYNAMICTEXTURES", "CompressionType", "CompressedFullData")],
                         condition.then)
        self.assertEqual([], condition.otherwise)
        # The endscript on its own reads the same, without the game.
        self.assertEqual(len(script.bindings), len(endscript.parse(root / "Assets/main.end").bindings))

    def test_anything_else_is_refused(self):
        root = self.mod()
        with self.assertRaisesRegex(endscript.EndscriptError, "'import' is not supported"):   # uninstall imports whole packs
            endscript.parse(root / "Mod.end", option="Uninstall")
        with self.assertRaisesRegex(endscript.EndscriptError, "'Repair' is not offered"):
            endscript.parse(root / "Mod.end", option="Repair")
        for broken, reason in (
                (SCRIPT.replace("speedreflect auto", "add_texture GLOBAL\\DYNTEX.BIN TPKBlocks X Y"), "'add_texture' is not supported"),
                (SCRIPT.replace("\nend\n\ndelete", "\n\ndelete"), "left open"),                    # the combobox never closes
                (SCRIPT.replace("[VERSN2]", "[VERSN3]"), r"not a Binarius endscript"),
                (SCRIPT.replace('"Textures\\DYNTEX\\DYNAMICTEXTURES"', '"Textures\\NOPE"'), "not found")):
            with self.subTest(reason=reason), self.assertRaisesRegex(endscript.EndscriptError, reason):
                endscript.parse(self.mod(broken) / "Assets/main.end")
        launcher = self.mod()
        (launcher / "Mod.end").write_text(LAUNCHER.replace('"User"', '"Developer"'), encoding="utf-8")
        with self.assertRaisesRegex(endscript.EndscriptError, "only User mode"):
            endscript.parse(launcher / "Mod.end")

    def test_collection_names(self):
        self.assertEqual("FRONTENDTEXTURESPC", endscript.collection_name("Global\\FrontEndTexturesPC.tpk"))
        self.assertEqual("TRACKTEXTURES", endscript.collection_name("Location1\\Track_1004\\TrackTextures.tpk"))
        self.assertEqual("MAGAZINES", endscript.collection_name("Global\\Magazines.tpk"))


if __name__ == "__main__":
    unittest.main()
