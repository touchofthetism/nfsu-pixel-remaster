package com.nfsu1.androidevolved;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Test;

/** The importer end to end on the host: unpacking, its guards, the executable gate and the swap into place. */
public class GameImportTest {
    private static File temp() throws Exception { return Files.createTempDirectory("import").toFile(); }
    private static final GameImport.Progress NO_PROGRESS = (done, total, entry) -> { };
    private static final byte[] STOCK_EXE = stockExe();

    /** A stand-in Speed.exe of the real size whose SHA-256 the test substitutes for the real one. */
    private static byte[] stockExe() {
        byte[] data = new byte[(int) GameImport.SPEED_EXE_SIZE];
        data[0] = 'M'; data[1] = 'Z';
        for (int i = 2; i < data.length; i++) data[i] = (byte) (i * 31);
        return data;
    }

    private static String sha256(byte[] data) throws Exception {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) hex.append(String.format(Locale.ROOT, "%02x", b));
        return hex.toString();
    }

    /** Builds a ZIP in memory: entries are {name, content} pairs; a name ending in "/" is a directory. */
    private static byte[] zip(Object[]... entries) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(bytes)) {
            for (Object[] entry : entries) {
                out.putNextEntry(new ZipEntry((String) entry[0]));
                if (entry.length > 1) out.write(entry[1] instanceof byte[] ? (byte[]) entry[1] : ((String) entry[1]).getBytes(StandardCharsets.UTF_8));
                out.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    private static Object[][] gameEntries(String prefix, byte[] exe) {
        List<Object[]> list = new ArrayList<>();
        list.add(new Object[] {prefix + "Speed.exe", exe});
        for (String folder : GameImport.GAME_FOLDERS) list.add(new Object[] {prefix + folder + "/a.bin", folder + " data"});
        list.add(new Object[] {prefix + "_README.txt", "ignored"});
        return list.toArray(new Object[0][]);
    }

    private static int failure(Throwable error) {
        return error instanceof GameImport.Failure ? ((GameImport.Failure) error).messageRes : -1;
    }

    @Test public void unpackWritesEveryEntryAndCountsBytes() throws Exception {
        File staging = temp();
        byte[] data = zip(new Object[] {"a/"}, new Object[] {"a/b.txt", "hello"}, new Object[] {"c.txt", "world!"});
        List<String> seen = new ArrayList<>();
        GameImport.Result result = GameImport.unpack(new ByteArrayInputStream(data), data.length, staging, new AtomicBoolean(),
                (done, total, entry) -> seen.add(entry));
        assertEquals(3, result.files);          // the directory entry counts as an entry, as in the ZIP
        assertEquals(11, result.bytes);
        assertEquals("hello", new String(Files.readAllBytes(new File(staging, "a/b.txt").toPath()), StandardCharsets.UTF_8));
        assertEquals("world!", new String(Files.readAllBytes(new File(staging, "c.txt").toPath()), StandardCharsets.UTF_8));
        assertEquals("", seen.get(seen.size() - 1));   // the final progress call marks completion
        GameImport.deleteTree(staging);
    }

    @Test public void zipSlipIsRejected() throws Exception {
        for (String bad : new String[] {"../evil.txt", "a/../../evil.txt", "/abs.txt", "C:\\win.txt", "a\\b.txt"}) {
            File staging = temp();
            byte[] data = zip(new Object[] {bad, "x"});
            try {
                GameImport.unpack(new ByteArrayInputStream(data), data.length, staging, new AtomicBoolean(), NO_PROGRESS);
                fail("accepted " + bad);
            } catch (IOException error) {
                assertEquals(bad, R.string.import_err_entry, failure(error));
            }
            assertFalse(new File(staging.getParentFile(), "evil.txt").exists());
            GameImport.deleteTree(staging);
        }
        assertTrue(GameImport.safeEntryName("Cars/BMW/GEOMETRY.BIN"));
        assertFalse(GameImport.safeEntryName(""));
        StringBuilder deep = new StringBuilder();
        for (int i = 0; i <= GameImport.MAX_DEPTH; i++) deep.append("d/");
        assertFalse(GameImport.safeEntryName(deep + "x"));
    }

    @Test public void cancellationStopsTheUnpack() throws Exception {
        File staging = temp();
        byte[] data = zip(new Object[] {"a.txt", "1"}, new Object[] {"b.txt", "2"});
        AtomicBoolean cancelled = new AtomicBoolean(true);
        try {
            GameImport.unpack(new ByteArrayInputStream(data), data.length, staging, cancelled, NO_PROGRESS);
            fail("ran while cancelled");
        } catch (IOException error) {
            assertEquals(R.string.import_cancelled, failure(error));
        }
        GameImport.deleteTree(staging);
    }

    @Test public void importRejectsAnExecutableOfTheRightSizeButWrongContent() throws Exception {
        // Several modified Speed.exe builds (no-CD, free-roam) are exactly 3,178,496 bytes: size alone must not pass.
        File destination = temp();
        byte[] data = zip(gameEntries("", STOCK_EXE));
        try {
            GameImport.run(new ByteArrayInputStream(data), data.length, destination, new AtomicBoolean(), NO_PROGRESS);
            fail("accepted a different executable");
        } catch (IOException error) {
            assertEquals(R.string.import_err_exe_hash, failure(error));
        }
        assertNull(GameImport.child(destination, "Speed.exe"));
        assertNull(GameImport.child(destination, "Cars"));
        assertEquals("staging cleaned up", 0, destination.list().length);
        GameImport.deleteTree(destination);
    }

    @Test public void importRejectsWrongSizeMissingExeAndMissingFolders() throws Exception {
        File destination = temp();
        byte[] small = zip(new Object[] {"Speed.exe", "tiny"}, new Object[] {"Cars/a", "x"});
        try { GameImport.run(new ByteArrayInputStream(small), small.length, destination, new AtomicBoolean(), NO_PROGRESS); fail(); }
        catch (IOException error) { assertEquals(R.string.import_err_bad_exe, failure(error)); }
        byte[] noExe = zip(new Object[] {"Cars/a", "x"});
        try { GameImport.run(new ByteArrayInputStream(noExe), noExe.length, destination, new AtomicBoolean(), NO_PROGRESS); fail(); }
        catch (IOException error) { assertEquals(R.string.import_err_no_exe, failure(error)); }
        byte[] nested = zip(new Object[] {"Need For Speed Underground - Clean Edition.7z", "7z\u00bc\u00af'"});
        try { GameImport.run(new ByteArrayInputStream(nested), nested.length, destination, new AtomicBoolean(), NO_PROGRESS); fail(); }
        catch (IOException error) { assertEquals(R.string.import_err_nested_archive, failure(error)); }
        GameImport.deleteTree(destination);
    }

    @Test public void swapReplacesExistingFoldersUnderCanonicalNamesAndKeepsOtherFiles() throws Exception {
        File destination = temp();
        File oldCars = new File(destination, "CARS");            // a previous import spelled upper case
        assertTrue(oldCars.mkdirs());
        Files.write(new File(oldCars, "old.bin").toPath(), "old".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(destination, "save.dat").toPath(), "progress".getBytes(StandardCharsets.UTF_8));
        File root = temp();
        Files.write(new File(root, "speed.exe").toPath(), STOCK_EXE);
        for (String folder : new String[] {"cars", "frontend", "Global"}) {
            File f = new File(root, folder);
            assertTrue(f.mkdirs());
            Files.write(new File(f, "new.bin").toPath(), "new".getBytes(StandardCharsets.UTF_8));
        }
        GameImport.swapIntoPlace(root, destination);
        String[] names = destination.list();
        java.util.Arrays.sort(names);
        assertArrayEquals(new String[] {"Cars", "FrontEnd", "Global", "Speed.exe", "save.dat"}, names);
        assertTrue(new File(destination, "Cars/new.bin").isFile());
        assertFalse(new File(destination, "Cars/old.bin").exists());
        assertEquals("progress", new String(Files.readAllBytes(new File(destination, "save.dat").toPath()), StandardCharsets.UTF_8));
        assertEquals(GameImport.SPEED_EXE_SIZE, new File(destination, "Speed.exe").length());
        GameImport.deleteTree(destination);
        GameImport.deleteTree(root);
    }

    @Test public void swapRestoresTheOldInstallWhenAMoveFails() throws Exception {
        File destination = temp();
        for (String folder : new String[] {"Cars", "FrontEnd"}) {
            File old = new File(destination, folder);
            assertTrue(old.mkdirs());
            Files.write(new File(old, "old.bin").toPath(), "old".getBytes(StandardCharsets.UTF_8));
        }
        File root = temp();
        Files.write(new File(root, "Speed.exe").toPath(), STOCK_EXE);
        for (String folder : new String[] {"Cars", "FrontEnd"}) {
            assertTrue(new File(root, folder).mkdirs());
            Files.write(new File(root, folder + "/new.bin").toPath(), "new".getBytes(StandardCharsets.UTF_8));
        }
        // Speed.exe and Cars move; setting the old FrontEnd aside fails (a locked file, a dying filesystem).
        GameImport.Mover flaky = (from, to) -> !(from.getName().equals("FrontEnd") && to.getName().startsWith(".previous-")) && from.renameTo(to);
        try {
            GameImport.swapIntoPlace(root, destination, flaky);
            fail("the failing rename must surface");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("FrontEnd"));
        }
        assertTrue("old Cars restored", new File(destination, "Cars/old.bin").isFile());
        assertFalse("new Cars rolled back", new File(destination, "Cars/new.bin").exists());
        assertTrue("old FrontEnd untouched", new File(destination, "FrontEnd/old.bin").isFile());
        assertFalse("Speed.exe rolled back", new File(destination, "Speed.exe").exists());
        assertFalse("no backup left behind", hasPrevious(destination));
        GameImport.deleteTree(destination);
        GameImport.deleteTree(root);
    }

    private static boolean hasPrevious(File directory) {
        String[] names = directory.list();
        if (names != null) for (String name : names) if (name.startsWith(".previous-")) return true;
        return false;
    }

    @Test public void gameRootAtTheTopOrUnderAWrapper() throws Exception {
        File staging = temp();
        assertTrue(new File(staging, "Speed.exe").createNewFile());
        assertEquals(staging, GameImport.findGameRoot(staging, 0));
        GameImport.deleteTree(staging);
        staging = temp();
        File wrapper = new File(staging, "Need For Speed Underground");
        assertTrue(wrapper.mkdirs());
        assertTrue(new File(wrapper, "speed.exe").createNewFile());   // lower case, as some copies ship it
        assertEquals(wrapper, GameImport.findGameRoot(staging, 0));
        assertNotNull(GameImport.child(wrapper, "Speed.exe"));
        GameImport.deleteTree(staging);
    }

    @Test public void twoLevelsDownIsStillFound_threeIsNot() throws Exception {
        File staging = temp();
        File deep = new File(staging, "a/b");
        assertTrue(deep.mkdirs());
        assertTrue(new File(deep, "Speed.exe").createNewFile());
        assertEquals(deep, GameImport.findGameRoot(staging, 0));
        GameImport.deleteTree(deep);
        File deeper = new File(staging, "x/y/z");
        assertTrue(deeper.mkdirs());
        assertTrue(new File(deeper, "Speed.exe").createNewFile());
        assertNull(GameImport.findGameRoot(staging, 0));
        GameImport.deleteTree(staging);
    }

    @Test public void theRequiredFoldersAreTheOnesTheRuntimeChecks() throws Exception {
        // android/app/src/main/cpp/game_data.h opens these at boot; read the header so the lists cannot drift apart.
        File header = new File(System.getProperty("user.dir"), "src/main/cpp/game_data.h");
        assertTrue("game_data.h next to the Java sources", header.isFile());
        String text = new String(Files.readAllBytes(header.toPath()), StandardCharsets.UTF_8);
        int start = text.indexOf("for (const char* name : {");
        assertTrue(start > 0);
        String list = text.substring(start, text.indexOf("}", start));
        List<String> runtime = new ArrayList<>();
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"([A-Z]+)\"").matcher(list);
        while (m.find()) runtime.add(m.group(1));
        List<String> ours = new ArrayList<>();
        for (String folder : GameImport.REQUIRED_FOLDERS) ours.add(folder.toUpperCase(Locale.ROOT));
        java.util.Collections.sort(runtime);
        java.util.Collections.sort(ours);
        assertEquals(runtime, ours);
        assertEquals(sha256(STOCK_EXE).length(), GameImport.SPEED_EXE_SHA256.length());
    }
}
