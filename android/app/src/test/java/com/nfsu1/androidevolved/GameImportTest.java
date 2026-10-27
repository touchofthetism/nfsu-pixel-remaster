package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.nio.file.Files;
import org.junit.Test;

/** The parts of the importer that need no Android: locating the game root inside what a ZIP unpacked to. */
public class GameImportTest {
    private static File temp() throws Exception { return Files.createTempDirectory("import").toFile(); }

    @Test public void gameRootAtTheTop() throws Exception {
        File staging = temp();
        assertTrue(new File(staging, "Speed.exe").createNewFile());
        assertEquals(staging, GameImport.findGameRoot(staging, 0));
        GameImport.deleteTree(staging);
    }

    @Test public void gameRootUnderAWrapperFolder() throws Exception {
        File staging = temp();
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
        File deeper = new File(staging, "x/y/z");
        assertTrue(deeper.mkdirs());
        GameImport.deleteTree(deep);
        assertTrue(new File(deeper, "Speed.exe").createNewFile());
        assertNull(GameImport.findGameRoot(staging, 0));
        GameImport.deleteTree(staging);
    }

    @Test public void childLookupIsCaseInsensitive() throws Exception {
        File staging = temp();
        assertTrue(new File(staging, "CARS").mkdirs());
        assertNotNull(GameImport.child(staging, "Cars"));
        assertNull(GameImport.child(staging, "Tracks"));
        GameImport.deleteTree(staging);
        assertTrue(!staging.exists());
    }

    @Test public void theRequiredFoldersAreTheOnesTheRuntimeChecks() {
        // android/app/src/main/cpp/game_data.h opens CARS, FRONTEND, GLOBAL, LANGUAGES, MOVIES, SOUND, TRACKS.
        assertEquals(7, GameImport.REQUIRED_FOLDERS.length);
        for (String folder : GameImport.REQUIRED_FOLDERS) {
            boolean listed = false;
            for (String known : GameImport.GAME_FOLDERS) listed |= known.equals(folder);
            assertTrue(folder, listed);
        }
    }
}
