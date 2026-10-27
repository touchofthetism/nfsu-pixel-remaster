package com.nfsu1.androidevolved;

/**
 * Compare release tags numerically (0.10.0 > 0.5.4), never by text or publication date. This build's
 * tags carry the CI run as a fourth component: v1.0.0-build.12 > v1.0.0-build.9 > v1.0.0.
 */
final class ReleaseVersion {
    static int[] parse(String version) {
        if (version == null || !version.matches("v?[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}(-build\\.[0-9]{1,9})?")) return null;
        String[] parts = version.replaceFirst("^v", "").replace("-build.", ".").split("\\.");
        int[] result = new int[4];
        for (int i = 0; i < parts.length && i < 4; i++) result[i] = Integer.parseInt(parts[i]);
        return result;
    }
    static boolean newer(String remote, String installed) {
        int[] a = parse(remote), b = parse(installed);
        if (a == null || b == null) return false;
        for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i];
        return false;
    }
}
