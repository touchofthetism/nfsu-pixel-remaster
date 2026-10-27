package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Test;

/** Save backup entries and the restore's safety rules (a Context is only used for messages; null here). */
public class SaveBackupTest {
    @Test public void entryNamesStayInsideTheSaveFolder() {
        assertEquals("PLAYER/career.sav", SaveBackup.entryName("Save/PLAYER/career.sav"));
        assertEquals("PLAYER/career.sav", SaveBackup.entryName("save\\PLAYER\\career.sav"));
        assertEquals("options.opt", SaveBackup.entryName("/options.opt"));
        assertNull(SaveBackup.entryName("../Speed.exe"));
        assertNull(SaveBackup.entryName("Save/../../x"));
        assertNull(SaveBackup.entryName("C:/Games/save.sav"));
        assertNull("game data is not a save", SaveBackup.entryName("Tracks/STREAML1.BUN"));
        assertNull(SaveBackup.entryName("CARS/CIVIC/geometry.bin"));
    }

    @Test public void backupZipHoldsRelativePathsAndRestoreReplacesAtomically() throws Exception {
        File root = Files.createTempDirectory("nfsu1").toFile();
        File save = new File(root, "Save");
        assertTrue(new File(save, "PLAYER").mkdirs());
        Files.write(new File(save, "PLAYER/career.sav").toPath(), "career".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(save, "options.opt").toPath(), "opt".getBytes(StandardCharsets.UTF_8));
        List<File> files = new ArrayList<>();
        SaveBackup.collect(save, files, 0);
        assertEquals(2, files.size());
        ByteArrayOutputStream zip = new ByteArrayOutputStream();
        long bytes = SaveBackup.writeZip(files, save, zip);
        assertEquals(9, bytes);
        // Restore a different backup over it: the old folder is gone, the new contents are in place.
        ByteArrayOutputStream other = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(other)) {
            out.putNextEntry(new ZipEntry("Save/")); out.closeEntry();
            out.putNextEntry(new ZipEntry("Save/PLAYER2/career.sav")); out.write("second".getBytes(StandardCharsets.UTF_8)); out.closeEntry();
        }
        SaveBackup.Result result = SaveBackup.restore(null, new ByteArrayInputStream(other.toByteArray()), root);
        assertEquals(1, result.files);
        assertTrue(new File(root, "Save/PLAYER2/career.sav").isFile());
        assertTrue(!new File(root, "Save/options.opt").exists());
        String[] leftovers = root.list((dir, name) -> name.startsWith(".save-"));
        assertEquals("no staging or previous folders left behind", 0, leftovers == null ? 0 : leftovers.length);
        // The original backup restores the original files.
        SaveBackup.restore(null, new ByteArrayInputStream(zip.toByteArray()), root);
        assertEquals("career", new String(Files.readAllBytes(new File(root, "Save/PLAYER/career.sav").toPath()), StandardCharsets.UTF_8));
    }

    @Test public void restoreRefusesAGameDataArchive() throws Exception {
        File root = Files.createTempDirectory("nfsu1").toFile();
        ByteArrayOutputStream bad = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(bad)) {
            out.putNextEntry(new ZipEntry("Tracks/STREAML1.BUN")); out.write(new byte[16]); out.closeEntry();
        }
        try {
            SaveBackup.restore(null, new ByteArrayInputStream(bad.toByteArray()), root);
            throw new AssertionError("accepted game data as a save");
        } catch (Exception expected) {
            // A null Context makes the message lookup fail; either way the restore must not write anything.
        }
        String[] leftovers = root.list();
        assertEquals(0, leftovers == null ? 0 : leftovers.length);
    }
}
