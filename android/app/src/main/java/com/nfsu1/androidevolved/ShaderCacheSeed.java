package com.nfsu1.androidevolved;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Seeds the device's graphics caches from the ones bundled in the APK (assets/graphics-cache), collected
 * from earlier sessions on the same phone model through the diagnostics report.
 *
 * Two files live in cache/dxvk-v2 (see GraphicsCache and main.cpp):
 *  - {@code app_process64.dxvk-cache}: DXVK's state cache, driver independent. DXVK reads it at start and
 *    compiles every listed pipeline as soon as the game creates its shaders, before the first frame that
 *    needs them, instead of stalling mid-race the first time each effect appears. Its format (version 8+,
 *    dxvk_state_cache.cpp) is simple enough to merge here: a 12-byte header, then entries of a 4-byte
 *    header (stage mask in the low 8 bits, data size in the high 24), a 20-byte SHA-1 of the data and the
 *    data. Entries are deduplicated by that hash, so merging is a union that keeps the device's own entries.
 *  - {@code vulkan_pipelines.bin}: the driver's own pipeline cache. Only useful on the same GPU and driver
 *    (the loader checks vendor, device and pipelineCacheUUID and otherwise starts empty), and binary blobs
 *    cannot be merged without a Vulkan device, so it is copied only when the device has none yet.
 *
 * Runs in the launcher, right before the game process starts (never while it runs). Each bundled version
 * is merged once; a reset cache (GraphicsCache) is seeded again on the next start.
 */
final class ShaderCacheSeed {
    static final String ASSET_DIR = "graphics-cache";
    static final String STATE_CACHE = "app_process64.dxvk-cache", PIPELINE_CACHE = "vulkan_pipelines.bin";
    private static final String PREFS = "nfsu1_cache_seed";
    static final int HEADER_BYTES = 12, ENTRY_HEADER_BYTES = 4, HASH_BYTES = 20;
    private static final byte[] MAGIC = {'D', 'X', 'V', 'K'};

    /** A parsed state cache: version and the raw bytes of each valid entry, keyed by its hash. */
    static final class StateCache {
        final int version;
        final List<byte[]> entries = new ArrayList<>();
        final Set<String> hashes = new HashSet<>();
        private StateCache(int version) { this.version = version; }

        /** Reads what DXVK would read: stops at the first truncated or corrupt entry. */
        static StateCache parse(byte[] data) {
            if (data == null || data.length < HEADER_BYTES) return null;
            for (int i = 0; i < 4; i++) if (data[i] != MAGIC[i]) return null;
            ByteBuffer buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            StateCache cache = new StateCache(buffer.getInt(4));
            int offset = HEADER_BYTES;
            MessageDigest sha1;
            try { sha1 = MessageDigest.getInstance("SHA-1"); } catch (NoSuchAlgorithmException error) { throw new IllegalStateException(error); }
            while (offset + ENTRY_HEADER_BYTES + HASH_BYTES <= data.length) {
                int header = buffer.getInt(offset);
                int size = header >>> 8;
                int end = offset + ENTRY_HEADER_BYTES + HASH_BYTES + size;
                if (end > data.length) break;
                sha1.reset();
                sha1.update(data, offset + ENTRY_HEADER_BYTES + HASH_BYTES, size);
                byte[] digest = sha1.digest();
                boolean valid = true;
                for (int i = 0; i < HASH_BYTES; i++) if (digest[i] != data[offset + ENTRY_HEADER_BYTES + i]) { valid = false; break; }
                if (!valid) break;
                byte[] entry = new byte[end - offset];
                System.arraycopy(data, offset, entry, 0, entry.length);
                if (cache.hashes.add(hex(digest))) cache.entries.add(entry);
                offset = end;
            }
            return cache;
        }

        /** Adds the other cache's entries this one lacks; returns how many were added. Versions must match. */
        int merge(StateCache other) {
            if (other == null || other.version != version) return 0;
            int added = 0;
            for (byte[] entry : other.entries) {
                String key = hashOf(entry);
                if (hashes.add(key)) { entries.add(entry); added++; }
            }
            return added;
        }

        byte[] toBytes() {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            ByteBuffer header = ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.LITTLE_ENDIAN);
            header.put(MAGIC).putInt(version).putInt(0);
            out.write(header.array(), 0, HEADER_BYTES);
            for (byte[] entry : entries) out.write(entry, 0, entry.length);
            return out.toByteArray();
        }

        private static String hashOf(byte[] entry) {
            StringBuilder text = new StringBuilder();
            for (int i = 0; i < HASH_BYTES; i++) text.append(String.format("%02x", entry[ENTRY_HEADER_BYTES + i]));
            return text.toString();
        }

        private static String hex(byte[] digest) {
            StringBuilder text = new StringBuilder();
            for (byte b : digest) text.append(String.format("%02x", b));
            return text.toString();
        }
    }

    /** Merges the bundled state cache into the device one; returns the merged bytes, or null when nothing changes. */
    static byte[] mergedStateCache(byte[] device, byte[] bundled) {
        StateCache seed = StateCache.parse(bundled);
        if (seed == null || seed.entries.isEmpty()) return null;
        StateCache current = StateCache.parse(device);
        if (current == null) return seed.toBytes();            // absent or not a DXVK cache: start from the seed
        if (current.version != seed.version) return null;      // a different DXVK: leave the device's file alone
        return current.merge(seed) > 0 ? current.toBytes() : null;
    }

    /** What happened, for the launcher log; never throws for a missing asset (a build may ship none). */
    static String seed(Context context) {
        File directory = new File(context.getCacheDir(), GraphicsCache.DIRECTORY);
        if (!directory.isDirectory() && !directory.mkdirs()) return "cache directory unavailable";
        StringBuilder result = new StringBuilder();
        try {
            byte[] bundled = asset(context, STATE_CACHE);
            if (bundled != null) {
                String key = "state-" + sha256(bundled);
                File target = new File(directory, STATE_CACHE);
                SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
                if (!prefs.getBoolean(key, false) || !target.isFile()) {
                    byte[] device = target.isFile() ? read(target) : null;
                    byte[] merged = mergedStateCache(device, bundled);
                    if (merged != null) { writeAtomically(target, merged); result.append("state cache seeded (").append(merged.length).append(" bytes); "); }
                    else result.append("state cache already current; ");
                    prefs.edit().putBoolean(key, true).apply();
                } else result.append("state cache seeded earlier; ");
            }
            byte[] pipelines = asset(context, PIPELINE_CACHE);
            File blob = new File(directory, PIPELINE_CACHE);
            if (pipelines != null && !blob.exists() && !new File(directory, PIPELINE_CACHE + ".loading").exists()) {
                writeAtomically(blob, pipelines);
                result.append("driver pipeline cache seeded (").append(pipelines.length).append(" bytes)");
            } else if (pipelines != null) result.append("driver pipeline cache present");
        } catch (IOException error) {
            result.append("seeding failed: ").append(error);
        }
        return result.length() == 0 ? "no bundled graphics cache" : result.toString().trim();
    }

    private static byte[] asset(Context context, String name) throws IOException {
        try (InputStream input = context.getAssets().open(ASSET_DIR + "/" + name)) { return readAll(input); }
        catch (java.io.FileNotFoundException missing) { return null; }
    }

    static byte[] read(File file) throws IOException {
        try (InputStream input = new FileInputStream(file)) { return readAll(input); }
    }

    private static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[65536];
        for (int n; (n = input.read(buffer)) != -1;) out.write(buffer, 0, n);
        return out.toByteArray();
    }

    static void writeAtomically(File target, byte[] data) throws IOException {
        File temp = new File(target.getParentFile(), target.getName() + ".seed-tmp");
        try (OutputStream output = new FileOutputStream(temp)) { output.write(data); }
        if (!temp.renameTo(target)) { temp.delete(); throw new IOException("could not replace " + target.getName()); }
    }

    private static String sha256(byte[] data) {
        try {
            StringBuilder text = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) text.append(String.format("%02x", b));
            return text.toString();
        } catch (NoSuchAlgorithmException error) { throw new IllegalStateException(error); }
    }
}
