package com.nfsu1.androidevolved;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** When the APK's free roam bundles are copied out (FreeRoamData.needsCopy). */
public class FreeRoamDataTest {
    private static final String[] BUNDLED = {"TRACKB1000.lzc", "TRACKB1300.lzc", "TRACKB1400.lzc"};

    @Test public void copiedOnceAnApkPutsThemThere() {
        assertTrue("nothing copied yet", FreeRoamData.needsCopy(null, "190 1700000000", null, BUNDLED));
        assertTrue("an empty folder", FreeRoamData.needsCopy(null, "190 1700000000", new String[0], BUNDLED));
        String[] present = {".apk", "TRACKB1000.lzc", "TRACKB1300.lzc", "TRACKB1400.lzc"};
        assertFalse("the same APK, all there", FreeRoamData.needsCopy("190 1700000000", "190 1700000000", present, BUNDLED));
    }

    @Test public void copiedAgainWhenTheApkChangesOrAFileIsGone() {
        String[] present = {".apk", "TRACKB1000.lzc", "TRACKB1300.lzc", "TRACKB1400.lzc"};
        assertTrue("an update", FreeRoamData.needsCopy("190 1700000000", "191 1700000500", present, BUNDLED));
        assertTrue("a reinstall of the same version", FreeRoamData.needsCopy("190 1700000000", "190 1700000900", present, BUNDLED));
        String[] partial = {".apk", "TRACKB1000.lzc", "TRACKB1300.lzc.tmp"};
        assertTrue("an interrupted copy", FreeRoamData.needsCopy("190 1700000000", "190 1700000000", partial, BUNDLED));
    }
}
