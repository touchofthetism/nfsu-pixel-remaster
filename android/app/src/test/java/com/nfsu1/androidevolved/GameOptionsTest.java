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
        assertEquals("60", GameOptions.defaultValue(GameOptions.FPS, 16 * GB));
        assertEquals("on", GameOptions.defaultValue(GameOptions.WIDESCREEN, 16 * GB));
        assertEquals("overlapped", GameOptions.defaultValue(GameOptions.READS, 16 * GB));   // build 7: reads hand the machine over again
        assertEquals("thread", GameOptions.defaultValue(GameOptions.CLEANUP, 16 * GB));     // build 11: DXVK's resets off its queue thread
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
        // 720 lines at the panel's 20:9, as the native side widens it (1344 * 2992/1344 -> 720 * 2.226 = 1602.9 -> 1602).
        assertArrayEquals(new int[] {1602, 720}, GameOptions.renderSize("720", 2992, 1344));
        assertArrayEquals(new int[] {1280, 720}, GameOptions.renderSize("720", 1920, 1080));
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

    @Test public void cpuPlacementFollowsTheFastestClusterByDefault() {
        // Build 12: "fast" follows the fastest cluster as the phone caps clocks; "big" keeps builds 3-11's pin.
        assertEquals("fast", GameOptions.defaultValue(GameOptions.CPU, 16 * GB));
        GameOptions.Option cpu = null;
        for (GameOptions.Option option : GameOptions.ALL) if (option.key.equals(GameOptions.CPU)) cpu = option;
        assertTrue(cpu != null);
        assertArrayEquals(new String[] {"fast", "big", "system"}, cpu.values);
        // The codes are host_perf::Placement: System 0, FastestCores 1, BigCore 2.
        assertEquals(1, GameOptions.cpuPlacement("fast"));
        assertEquals(2, GameOptions.cpuPlacement("big"));
        assertEquals(0, GameOptions.cpuPlacement("system"));
        assertEquals(1, GameOptions.cpuPlacement(null));        // no saved choice: the default
        assertEquals(1, GameOptions.cpuPlacement("fastest"));   // a value from no build: the default
    }

    @Test public void onlineServerNamesAreValidated() {
        assertTrue(GameOptions.validServer("eahub.eu"));
        assertTrue(GameOptions.validServer("192.168.1.20"));
        assertTrue(!GameOptions.validServer("http://x"));
        assertTrue(!GameOptions.validServer(""));
        assertTrue(!GameOptions.validServer(null));
    }

    @Test public void garageFeaturesAreStoredAsThoseTurnedOff() {
        // Build 17: seven switches, the bits of the native NFS_GARAGE_* mask in the same order (tests/garage.cpp
        // checks the native side's keys against the same list).
        assertArrayEquals(new String[] {"unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes"},
                GameOptions.GARAGE_KEYS);
        assertEquals(GameOptions.GARAGE_KEYS.length, GameOptions.GARAGE_TITLES.length);
        assertEquals(0x7F, GameOptions.GARAGE_ALL);
        // Never chosen, or nothing turned off: all on.
        assertEquals(0x7F, GameOptions.garageMask(null));
        assertEquals(0x7F, GameOptions.garageMask(""));
        assertEquals("", GameOptions.garageOffValue(0x7F));
        // Turned off: that bit clear; a key no build knows (or a later build's feature this one lacks) is ignored.
        assertEquals(0x7F & ~(1 << 6), GameOptions.garageMask("raceBrakes"));
        assertEquals(0x7F & ~1 & ~(1 << 3), GameOptions.garageMask("unlock, hoodVinyls"));
        assertEquals(0x7F & ~(1 << 2), GameOptions.garageMask("vinylLayers,rimSizes"));
        assertEquals("unlock,hoodVinyls", GameOptions.garageOffValue(0x7F & ~1 & ~(1 << 3)));
        // Every mask survives the round trip, all off included.
        for (int mask = 0; mask <= 0x7F; mask++) assertEquals(mask, GameOptions.garageMask(GameOptions.garageOffValue(mask)));
        assertEquals(String.join(",", GameOptions.GARAGE_KEYS), GameOptions.garageOffValue(0));
        assertEquals(3, GameOptions.garageCount(0b1010100));
        assertEquals(7, GameOptions.garageCount(-1));
    }

    @Test public void effectStatesAreTheHostsByDefault() {
        assertEquals("native", GameOptions.defaultValue(GameOptions.EFFECTS, 16 * GB));
        assertArrayEquals(new String[] {"native", "game"}, GameOptions.find(GameOptions.EFFECTS).values);
    }

    @Test public void batterySaverHoldsTheCapToThePanel() {
        assertEquals(60, GameOptions.effectiveFrameLimit(120, true, 60f));
        assertEquals(60, GameOptions.effectiveFrameLimit(90, true, 60.0001f));
        assertEquals(30, GameOptions.effectiveFrameLimit(30, true, 60f));
        assertEquals(120, GameOptions.effectiveFrameLimit(120, false, 60f));
        assertEquals(0, GameOptions.effectiveFrameLimit(0, true, 60f));
        assertEquals(120, GameOptions.effectiveFrameLimit(120, true, Float.NaN));
        assertEquals(120, GameOptions.effectiveFrameLimit(120, true, 0f));
    }
}
