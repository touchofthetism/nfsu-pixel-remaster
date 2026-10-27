package com.nfsu1.androidevolved;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.nio.file.Files;
import java.util.Locale;
import org.junit.Test;

public class GameOptionsTest {
    private static final long GB = 1024L * 1024 * 1024;

    @Test public void highMemoryDevicesRenderNative() {
        assertEquals("100", GameOptions.defaultValue(GameOptions.RESOLUTION, 12 * GB));   // Pixel 10 Pro XL: 16 GB
        assertEquals("100", GameOptions.defaultValue(GameOptions.RESOLUTION, 8 * GB));
        assertEquals("75", GameOptions.defaultValue(GameOptions.RESOLUTION, 6 * GB));
        assertEquals("75", GameOptions.defaultValue(GameOptions.RESOLUTION, 0));
    }

    @Test public void qualityDefaultsAreConservativeWhereUnverified() {
        assertEquals("16", GameOptions.defaultValue(GameOptions.ANISOTROPY, 16 * GB));
        assertEquals("16", GameOptions.defaultValue(GameOptions.ANISOTROPY, 3 * GB));
        assertEquals("0", GameOptions.defaultValue(GameOptions.ANTIALIAS, 16 * GB));
        assertEquals("120", GameOptions.defaultValue(GameOptions.FPS, 16 * GB));
        assertEquals("on", GameOptions.defaultValue(GameOptions.WIDESCREEN, 16 * GB));
        assertEquals("overlapped", GameOptions.defaultValue(GameOptions.READS, 16 * GB));   // build 7: reads hand the machine over again
    }

    @Test public void everyOptionHasADefaultAmongItsValues() {
        for (GameOptions.Option option : GameOptions.ALL) {
            for (long memory : new long[] {0, 4 * GB, 16 * GB}) {
                String value = GameOptions.defaultValue(option.key, memory);
                assertTrue(option.key + " default " + value, option.allows(value));
            }
            assertEquals(option.key, option.values.length, option.labelRes.length);
        }
    }

    @Test public void renderSizeFollowsThePanel() {
        assertArrayEquals(new int[] {2992, 1344}, GameOptions.renderSize("100", 2992, 1344));
        assertArrayEquals(new int[] {2244, 1008}, GameOptions.renderSize("75", 2992, 1344));
        assertArrayEquals(new int[] {1496, 672}, GameOptions.renderSize("50", 2992, 1344));
        assertArrayEquals(new int[] {1280, 720}, GameOptions.renderSize("720", 2992, 1344));
        int[] odd = GameOptions.renderSize("75", 2001, 1001);
        assertEquals(0, odd[0] % 2);
        assertEquals(0, odd[1] % 2);
    }

    @Test public void languageFollowsTheDeviceWhenTheGameShipsIt() throws Exception {
        File root = Files.createTempDirectory("nfsu1").toFile();
        File languages = new File(root, "LANGUAGES");
        assertTrue(languages.mkdirs());
        for (String name : new String[] {"English", "Spanish", "German"}) assertTrue(new File(languages, name + ".bin").createNewFile());
        assertEquals("English", GameOptions.defaultLanguage(Locale.US, root));
        assertEquals("Spanish", GameOptions.defaultLanguage(new Locale("es", "MX"), root));
        assertEquals("German", GameOptions.defaultLanguage(Locale.GERMANY, root));
        assertEquals("English", GameOptions.defaultLanguage(Locale.FRANCE, root));      // French not shipped: English
        assertEquals("English", GameOptions.defaultLanguage(null, root));
        File spanishOnly = Files.createTempDirectory("nfsu1es").toFile();
        assertTrue(new File(spanishOnly, "LANGUAGES").mkdirs());
        assertTrue(new File(spanishOnly, "LANGUAGES/Spanish.bin").createNewFile());
        assertEquals("Spanish", GameOptions.defaultLanguage(Locale.US, spanishOnly));  // whatever the copy has
        assertEquals("French", GameOptions.defaultLanguage(Locale.FRANCE, null));      // no folder yet: the device's language
        GameImport.deleteTree(root);
        GameImport.deleteTree(spanishOnly);
    }

    @Test public void onlineServerNamesAreValidated() {
        assertTrue(GameOptions.validServer("eahub.eu"));
        assertTrue(GameOptions.validServer("192.168.1.20"));
        assertTrue(!GameOptions.validServer("http://x"));
        assertTrue(!GameOptions.validServer(""));
        assertTrue(!GameOptions.validServer(null));
    }
}
