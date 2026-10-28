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

    private static File profileFolder() throws Exception {
        File root = Files.createTempDirectory("nfsu1-cfg").toFile();
        File folder = new File(root, "NFS Underground");
        assertTrue(folder.mkdirs());
        return folder;
    }

    private static void put(File file, String text) throws Exception {
        Files.write(file.toPath(), text.getBytes(StandardCharsets.ISO_8859_1));
    }

    private static String text(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.ISO_8859_1);
    }

    @Test
    public void fingerprintIsTheDisplaySettingsOnly() {
        String max = GameConfig.graphicsFingerprint(TIER3);
        assertTrue(max.startsWith("PERFORMANCE=3;ENVMAP=3;WORLDENVMAP=1;SHADOW=2;"));
        assertTrue(max.endsWith(";MOTIONBLUR=1;LIGHTSTREAKS=1;LIGHTGLOW=1;ANIMATEDTEXTURE=1;PARTICLESYSTEMS=1;DEPTHOFFIELD=1;WorldLOD=3;CarLOD=1"));
        // Resolution, colour depth and sound are saved in the same file but are not graphics choices.
        assertEquals(max, GameConfig.graphicsFingerprint(TIER3.replace("RES: 0", "RES: 4").replace("SOUNDPERF: 100", "SOUNDPERF: 40").replace("BPP: 32", "BPP: 16")));
        assertFalse(max.equals(GameConfig.graphicsFingerprint(TIER3.replace("MOTIONBLUR: 1", "MOTIONBLUR: 0"))));
        // Line endings and spacing do not matter; missing keys are left out.
        assertEquals(max, GameConfig.graphicsFingerprint(TIER3.replace("\r\n", "\n").replace(": ", ":")));
        assertEquals("ENVMAP=0", GameConfig.graphicsFingerprint("BPP: 32\r\nENVMAP: 0\r\nSOUNDENABLE: 1\r\n"));
    }

    @Test
    public void firstLaunchRaisesEveryProfile() throws Exception {
        File folder = profileFolder();
        File root = folder.getParentFile();
        put(new File(folder, "shef.cfg"), TIER0);
        put(new File(folder, "other.cfg"), TIER3);
        Files.write(new File(folder, "shef.opt").toPath(), new byte[] {1, 2, 3});
        put(new File(folder, "notes.cfg"), "something else\r\n");
        GameConfig.Outcome outcome = GameConfig.applyMaximum(root, new java.util.HashMap<>());
        assertEquals(1, outcome.raised.size());
        assertEquals("shef.cfg", outcome.raised.get(0).getName());
        assertTrue(outcome.kept.isEmpty());
        assertEquals(TIER3, text(new File(folder, "shef.cfg")));
        assertEquals("something else\r\n", text(new File(folder, "notes.cfg")));
        assertEquals(3, new File(folder, "shef.opt").length());
        assertFalse(new File(folder, "shef.cfg.tmp").exists());
        // Both profiles are now known at the top tier; the other .cfg is not a profile.
        assertEquals(2, outcome.seen.size());
        assertEquals(GameConfig.graphicsFingerprint(TIER3), outcome.seen.get("shef.cfg"));
        assertEquals(GameConfig.graphicsFingerprint(TIER3), outcome.seen.get("other.cfg"));
        // No profile folder yet: nothing to do, no failure.
        GameConfig.Outcome none = GameConfig.applyMaximum(Files.createTempDirectory("nfsu1-empty").toFile(), new java.util.HashMap<>());
        assertTrue(none.raised.isEmpty() && none.kept.isEmpty() && none.seen.isEmpty());
    }

    @Test
    public void choicesMadeInTheGameStay() throws Exception {
        File folder = profileFolder();
        File root = folder.getParentFile();
        File shef = new File(folder, "shef.cfg");
        put(shef, TIER0);
        java.util.Map<String, String> seen = GameConfig.applyMaximum(root, new java.util.HashMap<>()).seen;
        // In the game: Motion Blur off and Light Trails off in Display Settings; the game saves the profile.
        String chosen = TIER3.replace("MOTIONBLUR: 1", "MOTIONBLUR: 0").replace("LIGHTSTREAKS: 1", "LIGHTSTREAKS: 0");
        put(shef, chosen);
        GameConfig.Outcome next = GameConfig.applyMaximum(root, seen);
        assertTrue(next.raised.isEmpty());
        assertEquals(1, next.kept.size());
        assertEquals(chosen, text(shef));
        assertEquals(GameConfig.graphicsFingerprint(chosen), next.seen.get("shef.cfg"));
        // The launch after that has nothing new to report and still leaves the choice alone.
        GameConfig.Outcome third = GameConfig.applyMaximum(root, next.seen);
        assertTrue(third.raised.isEmpty() && third.kept.isEmpty());
        assertEquals(chosen, text(shef));
        // Saving sound or resolution in the game is not a graphics change.
        put(shef, chosen.replace("SOUNDPERF: 100", "SOUNDPERF: 70").replace("RES: 0", "RES: 4"));
        GameConfig.Outcome fourth = GameConfig.applyMaximum(root, third.seen);
        assertTrue(fourth.raised.isEmpty() && fourth.kept.isEmpty());
        // Choosing Maximum again in the launcher forgets what was seen: every profile goes back to the top.
        GameConfig.Outcome reset = GameConfig.applyMaximum(root, new java.util.HashMap<>());
        assertEquals(1, reset.raised.size());
        assertEquals(TIER3.replace("SOUNDPERF: 100", "SOUNDPERF: 70").replace("RES: 0", "RES: 4"), text(shef));
    }

    @Test
    public void newProfilesAreRaisedAndDeletedOnesForgotten() throws Exception {
        File folder = profileFolder();
        File root = folder.getParentFile();
        put(new File(folder, "shef.cfg"), TIER3.replace("MOTIONBLUR: 1", "MOTIONBLUR: 0"));
        put(new File(folder, "old.cfg"), TIER3);
        java.util.Map<String, String> seen = new java.util.HashMap<>();
        seen.put("shef.cfg", GameConfig.graphicsFingerprint(TIER3.replace("MOTIONBLUR: 1", "MOTIONBLUR: 0")));
        seen.put("old.cfg", GameConfig.graphicsFingerprint(TIER3));
        seen.put("gone.cfg", GameConfig.graphicsFingerprint(TIER3));
        // A profile created during the last session carries the auto-detect's tier.
        put(new File(folder, "new.cfg"), TIER0);
        GameConfig.Outcome outcome = GameConfig.applyMaximum(root, seen);
        assertEquals(1, outcome.raised.size());
        assertEquals("new.cfg", outcome.raised.get(0).getName());
        assertEquals(TIER3, text(new File(folder, "new.cfg")));
        assertTrue(outcome.kept.isEmpty());
        assertEquals(TIER3.replace("MOTIONBLUR: 1", "MOTIONBLUR: 0"), text(new File(folder, "shef.cfg")));
        assertFalse(outcome.seen.containsKey("gone.cfg"));
        assertEquals(3, outcome.seen.size());
    }

    @Test
    public void observeRecordsWithoutWriting() throws Exception {
        File folder = profileFolder();
        File root = folder.getParentFile();
        put(new File(folder, "Shef.cfg"), TIER0);
        java.util.Map<String, String> seen = GameConfig.observe(root);
        assertEquals(1, seen.size());
        assertEquals(GameConfig.graphicsFingerprint(TIER0), seen.get("shef.cfg"));
        assertEquals(TIER0, text(new File(folder, "Shef.cfg")));
        // A restored profile recorded this way keeps its own values at the next launch.
        GameConfig.Outcome outcome = GameConfig.applyMaximum(root, seen);
        assertTrue(outcome.raised.isEmpty() && outcome.kept.isEmpty());
        assertEquals(TIER0, text(new File(folder, "Shef.cfg")));
    }
}
