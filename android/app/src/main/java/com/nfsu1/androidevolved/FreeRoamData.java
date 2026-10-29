package com.nfsu1.androidevolved;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Free roam's bundled data (build 19): the Free Roam Mod Beta 2.0's three track bundles (TRACKB1000/1300/1400.lzc,
 * by hockeyguy731, konigsseg and BojanV03), which the private build puts in the APK under assets/freeroam. The
 * native side serves the free roam tracks from them and the player's own game files (free_roam.h), so they are
 * copied out to files/freeroam, again whenever the installed APK changes; each file is written beside and renamed
 * over, the marker last, so an interrupted copy is redone.
 */
final class FreeRoamData {
    static final String ASSET_DIR = "freeroam", MARKER = ".apk";

    private FreeRoamData() {}

    /** The folder with the bundles, copied out first when needed; null when this APK carries none or the copy failed. */
    static File install(Context context) {
        AssetManager assets = context.getAssets();
        String[] listed;
        try { listed = assets.list(ASSET_DIR); } catch (IOException error) { return null; }
        if (listed == null || listed.length == 0) return null;
        File dir = new File(context.getFilesDir(), ASSET_DIR);
        String stamp = apkStamp(context);
        File marker = new File(dir, MARKER);
        if (!needsCopy(readText(marker), stamp, dir.list(), listed)) return dir;
        if (!dir.isDirectory() && !dir.mkdirs()) return null;
        if (marker.exists() && !marker.delete()) return null;
        byte[] buffer = new byte[1 << 16];
        for (String name : listed) {
            File target = new File(dir, name), partial = new File(dir, name + ".tmp");
            try (InputStream in = assets.open(ASSET_DIR + "/" + name); OutputStream out = new FileOutputStream(partial)) {
                for (int count; (count = in.read(buffer)) > 0; ) out.write(buffer, 0, count);
            } catch (IOException error) {
                partial.delete();
                return null;
            }
            if (!partial.renameTo(target)) { partial.delete(); return null; }
        }
        try (OutputStream out = new FileOutputStream(marker)) {
            out.write(stamp.getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) {
            return null;
        }
        return dir;
    }

    /** Whether the bundles must be copied: another APK than the one they came from, or one of them missing. */
    static boolean needsCopy(String storedStamp, String stamp, String[] present, String[] bundled) {
        if (storedStamp == null || !storedStamp.equals(stamp) || present == null) return true;
        for (String name : bundled) if (!Arrays.asList(present).contains(name)) return true;
        return false;
    }

    /** The installed APK, as its version and update time (an update or reinstall changes it). */
    static String apkStamp(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return info.getLongVersionCode() + " " + info.lastUpdateTime;
        } catch (Exception error) {
            return "unknown " + System.currentTimeMillis();   // copies every time rather than trusting an old copy
        }
    }

    private static String readText(File file) {
        if (!file.isFile() || file.length() > 4096) return null;
        byte[] data = new byte[(int) file.length()];
        try (FileInputStream in = new FileInputStream(file)) {
            int done = 0;
            while (done < data.length) { int count = in.read(data, done, data.length - done); if (count <= 0) return null; done += count; }
        } catch (IOException error) {
            return null;
        }
        return new String(data, StandardCharsets.UTF_8);
    }
}
