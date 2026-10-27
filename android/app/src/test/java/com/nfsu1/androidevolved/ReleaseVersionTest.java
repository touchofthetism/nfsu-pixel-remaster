package com.nfsu1.androidevolved;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class ReleaseVersionTest {
    @Test public void numericNotTextual() {
        assertTrue(ReleaseVersion.newer("0.10.0", "0.5.4"));
        assertTrue(ReleaseVersion.newer("v1.0.1", "1.0.0"));
        assertFalse(ReleaseVersion.newer("1.0.0", "1.0.0"));
        assertFalse(ReleaseVersion.newer("0.9.9", "1.0.0"));
    }

    @Test public void ciBuildNumbersOrderWithinAVersion() {
        assertTrue(ReleaseVersion.newer("v1.0.0-build.12", "1.0.0-build.9"));
        assertTrue(ReleaseVersion.newer("v1.0.0-build.1", "1.0.0"));
        assertFalse(ReleaseVersion.newer("v1.0.0-build.3", "1.0.0-build.3"));
        assertTrue(ReleaseVersion.newer("v1.1.0", "1.0.0-build.99"));
        assertFalse(ReleaseVersion.newer("v1.0.0-build.99", "1.1.0"));
    }

    @Test public void garbageNeverCountsAsNewer() {
        assertFalse(ReleaseVersion.newer("gamedata", "1.0.0"));
        assertFalse(ReleaseVersion.newer("", "1.0.0"));
        assertFalse(ReleaseVersion.newer(null, "1.0.0"));
        assertFalse(ReleaseVersion.newer("v1.0.0-rc1", "1.0.0"));
        assertFalse(ReleaseVersion.newer("v2.0.0", "oops"));
    }
}
