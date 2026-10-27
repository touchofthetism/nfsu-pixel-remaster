package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

import org.junit.Test;

public class GameConfigTest {
    /** A Pixel profile's shef.cfg as the game wrote it after its tier-0 auto-detect (CR LF, two blank lines). */
    static final String TIER0 = "BPP: 32\r\nRES: 0\r\nPERFORMANCE: 0\r\nENVMAP: 0\r\nWORLDENVMAP: 0\r\nSHADOW: 1\r\nHEADLIGHTS: 0\r\n"
            + "CARLIGHTING: 1\r\nDAMAGE: 0\r\nCROWD: 0\r\nROADREFLECTION: 0\r\nFOG: 0\r\nMOTIONBLUR: 0\r\nLIGHTSTREAKS: 0\r\n"
            + "LIGHTGLOW: 1\r\nANIMATEDTEXTURE: 1\r\nPARTICLESYSTEMS: 1\r\nDEPTHOFFIELD: 0\r\nWorldLOD: 1\r\nCarLOD: 0\r\n"
            + "SOUNDPERF: 100\r\nSOUNDENABLE: 1\r\n\r\n\r\n";
    static final String TIER3 = "BPP: 32\r\nRES: 0\r\nPERFORMANCE: 3\r\nENVMAP: 3\r\nWORLDENVMAP: 1\r\nSHADOW: 2\r\nHEADLIGHTS: 1\r\n"
            + "CARLIGHTING: 1\r\nDAMAGE: 1\r\nCROWD: 1\r\nROADREFLECTION: 2\r\nFOG: 1\r\nMOTIONBLUR: 1\r\nLIGHTSTREAKS: 1\r\n"
            + "LIGHTGLOW: 1\r\nANIMATEDTEXTURE: 1\r\nPARTICLESYSTEMS: 1\r\nDEPTHOFFIELD: 1\r\nWorldLOD: 3\r\nCarLOD: 1\r\n"
            + "SOUNDPERF: 100\r\nSOUNDENABLE: 1\r\n\r\n\r\n";

    @Test
    public void raisesEverySettingAndKeepsTheRest() {
        assertEquals(TIER3, GameConfig.applyMaximum(TIER0));
        // Resolution, colour depth and sound are not the port's business; order and line endings stay.
        String tier2 = TIER0.replace("RES: 0", "RES: 4").replace("SOUNDPERF: 100", "SOUNDPERF: 60");
        assertEquals(TIER3.replace("RES: 0", "RES: 4").replace("SOUNDPERF: 100", "SOUNDPERF: 60"), GameConfig.applyMaximum(tier2));
    }

    @Test
    public void idempotent() {
        assertEquals(TIER3, GameConfig.applyMaximum(TIER3));
    }

    @Test
    public void addsMissingSettingsAfterTheLastOne() {
        String partial = "BPP: 32\r\nRES: 1\r\nENVMAP: 0\r\nSOUNDENABLE: 1\r\n\r\n";
        String result = GameConfig.applyMaximum(partial);
        assertTrue(result.startsWith("BPP: 32\r\nRES: 1\r\nENVMAP: 3\r\nSOUNDENABLE: 1\r\n"));
        assertTrue(result.endsWith("CarLOD: 1\r\n\r\n"));
        assertFalse(result.contains("\n\n\r\n"));
        for (String[] pair : GameConfig.MAXIMUM) assertTrue(pair[0], result.contains("\r\n" + pair[0] + ": " + pair[1] + "\r\n") || result.contains(pair[0] + ": " + pair[1] + "\r\n"));
        // No trailing line end on the last setting: one is added before the new lines.
        assertTrue(GameConfig.applyMaximum("ENVMAP: 0").startsWith("ENVMAP: 3\nPERFORMANCE: 3\nWORLDENVMAP: 1\n"));
    }

    @Test
    public void rewritesEveryProfileInTheFolder() throws Exception {
        File root = Files.createTempDirectory("nfsu1-cfg").toFile();
        File folder = new File(root, "NFS Underground");
        assertTrue(folder.mkdirs());
        Files.write(new File(folder, "shef.cfg").toPath(), TIER0.getBytes(StandardCharsets.ISO_8859_1));
        Files.write(new File(folder, "other.cfg").toPath(), TIER3.getBytes(StandardCharsets.ISO_8859_1));
        Files.write(new File(folder, "shef.opt").toPath(), new byte[] {1, 2, 3});
        Files.write(new File(folder, "notes.cfg").toPath(), "something else\r\n".getBytes(StandardCharsets.ISO_8859_1));
        List<File> changed = GameConfig.applyMaximum(root);
        assertEquals(1, changed.size());
        assertEquals("shef.cfg", changed.get(0).getName());
        assertEquals(TIER3, new String(Files.readAllBytes(new File(folder, "shef.cfg").toPath()), StandardCharsets.ISO_8859_1));
        assertEquals("something else\r\n", new String(Files.readAllBytes(new File(folder, "notes.cfg").toPath()), StandardCharsets.ISO_8859_1));
        assertEquals(3, new File(folder, "shef.opt").length());
        assertFalse(new File(folder, "shef.cfg.tmp").exists());
        assertTrue(GameConfig.applyMaximum(root).isEmpty());
        // No profile folder yet: nothing to do, no failure.
        assertTrue(GameConfig.applyMaximum(Files.createTempDirectory("nfsu1-empty").toFile()).isEmpty());
    }
}
