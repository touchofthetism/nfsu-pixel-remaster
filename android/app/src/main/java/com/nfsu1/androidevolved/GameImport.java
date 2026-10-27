package com.nfsu1.androidevolved;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.StatFs;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Imports the game folder from a ZIP the player picks (Drive, Downloads, any document provider) into
 * Internal storage/nfsu1, so nobody has to shuffle folders in a file manager.
 *
 * The ZIP is streamed once into a staging folder next to the destination, the folder holding Speed.exe
 * is located (the game root may sit at the ZIP root or under one wrapper folder), the executable is
 * checked against the 1.4 build the port targets, and only then are the game's folders moved into
 * place with renames (instant, same filesystem). Existing folders of the same name are replaced; other
 * files in nfsu1 (saves, settings) are untouched. Cancel at any point leaves the destination as it was.
 *
 * Bounded like DriverArchive: no absolute or parent-relative paths, an entry count cap and a size cap.
 */
final class GameImport {
    interface Progress { void update(long doneBytes, long totalBytes, String currentEntry); }

    static final class Failure extends IOException {
        final int messageRes;
        final Object[] args;
        Failure(int messageRes, Object... args) { super("import failure " + messageRes); this.messageRes = messageRes; this.args = args; }
        String message(Context context) { return context.getString(messageRes, args); }
    }

    static final class Result {
        final int files;
        final long bytes;
        Result(int files, long bytes) { this.files = files; this.bytes = bytes; }
    }

    /** Speed.exe 1.4 (android/app/src/main/cpp/game_data.h, scripts/abk_module.py). */
    static final long SPEED_EXE_SIZE = 3178496;
    private static final int MAX_ENTRIES = 20000;
    private static final long MAX_BYTES = 8L * 1024 * 1024 * 1024;
    private static final int MAX_DEPTH = 24;
    /** The game folders the launcher and the runtime expect; everything else in the ZIP is ignored. */
    static final String[] GAME_FOLDERS = {"Cars", "FrontEnd", "Global", "Languages", "Movies", "Sound", "SNDSTREAMS", "SpeechStreams", "Tracks", "fx"};
    static final String[] REQUIRED_FOLDERS = {"Cars", "FrontEnd", "Global", "Languages", "Movies", "Sound", "Tracks"};

    private GameImport() {}

    /** Size the provider reports for the picked document, or -1 when unknown. */
    static long documentSize(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (column >= 0 && !cursor.isNull(column)) return cursor.getLong(column);
            }
        } catch (RuntimeException ignored) { }
        return -1;
    }

    static Result run(Context context, Uri zip, File destination, AtomicBoolean cancelled, Progress progress) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        long zipSize = documentSize(resolver, zip);
        if (!destination.isDirectory() && !destination.mkdirs()) throw new Failure(R.string.import_err_access);
        // Deflated game data expands roughly 1.4x; leave headroom for the rename-in-place step.
        if (zipSize > 0) {
            long free = new StatFs(destination.getAbsolutePath()).getAvailableBytes();
            long needed = zipSize * 3 / 2 + 128L * 1024 * 1024;
            if (free < needed) throw new Failure(R.string.import_err_space, needed / 1048576, free / 1048576);
        }
        File staging = new File(destination, ".import-" + System.currentTimeMillis());
        if (!staging.mkdirs()) throw new Failure(R.string.import_err_access);
        try {
            Result unpacked = unpack(resolver, zip, zipSize, staging, cancelled, progress);
            File root = findGameRoot(staging, 0);
            if (root == null) {
                if (containsArchive(staging, 0)) throw new Failure(R.string.import_err_nested_archive);
                throw new Failure(R.string.import_err_no_exe);
            }
            File exe = child(root, "Speed.exe");
            if (exe == null || exe.length() != SPEED_EXE_SIZE)
                throw new Failure(R.string.import_err_bad_exe, exe == null ? 0 : exe.length());
            List<String> missing = new ArrayList<>();
            for (String folder : REQUIRED_FOLDERS) if (child(root, folder) == null) missing.add(folder);
            if (!missing.isEmpty()) throw new Failure(R.string.import_err_missing_folders, String.join(", ", missing));
            if (cancelled.get()) throw new Failure(R.string.import_cancelled);
            moveIntoPlace(root, destination);
            return unpacked;
        } finally {
            deleteTree(staging);
        }
    }

    private static Result unpack(ContentResolver resolver, Uri zip, long zipSize, File staging, AtomicBoolean cancelled,
                                 Progress progress) throws IOException {
        String prefix = staging.getCanonicalPath() + File.separator;
        InputStream raw = resolver.openInputStream(zip);
        if (raw == null) throw new Failure(R.string.import_err_open);
        CountingInputStream counting = new CountingInputStream(raw);
        int files = 0;
        long bytes = 0;
        byte[] buffer = new byte[1 << 16];
        try (ZipInputStream input = new ZipInputStream(counting)) {
            ZipEntry entry;
            while ((entry = input.getNextEntry()) != null) {
                if (cancelled.get()) throw new Failure(R.string.import_cancelled);
                if (++files > MAX_ENTRIES) throw new Failure(R.string.import_err_entry, "too many entries");
                String name = entry.getName();
                if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":") || name.contains("../")
                        || name.startsWith("..") || name.split("/").length > MAX_DEPTH)
                    throw new Failure(R.string.import_err_entry, name);
                File file = new File(staging, name);
                if (!file.getCanonicalPath().startsWith(prefix)) throw new Failure(R.string.import_err_entry, name);
                if (entry.isDirectory()) {
                    if (!file.isDirectory() && !file.mkdirs()) throw new IOException("mkdir " + name);
                    continue;
                }
                File parent = file.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent);
                progress.update(counting.count, zipSize, name);
                try (OutputStream output = new FileOutputStream(file)) {
                    int n;
                    while ((n = input.read(buffer)) != -1) {
                        bytes += n;
                        if (bytes > MAX_BYTES) throw new Failure(R.string.import_err_entry, "archive too large");
                        output.write(buffer, 0, n);
                        if ((bytes & ((1 << 22) - 1)) < n) {   // every ~4 MB
                            if (cancelled.get()) throw new Failure(R.string.import_cancelled);
                            progress.update(counting.count, zipSize, name);
                        }
                    }
                }
            }
        }
        progress.update(zipSize > 0 ? zipSize : counting.count, zipSize, "");
        return new Result(files, bytes);
    }

    /** The folder holding Speed.exe: the staging root or one of its subfolders, at most two levels down. */
    static File findGameRoot(File directory, int depth) {
        if (child(directory, "Speed.exe") != null) return directory;
        if (depth >= 2) return null;
        File[] children = directory.listFiles(File::isDirectory);
        if (children == null) return null;
        for (File sub : children) {
            File found = findGameRoot(sub, depth + 1);
            if (found != null) return found;
        }
        return null;
    }

    private static boolean containsArchive(File directory, int depth) {
        File[] children = directory.listFiles();
        if (children == null || depth > 2) return false;
        for (File file : children) {
            String lower = file.getName().toLowerCase(Locale.ROOT);
            if (file.isFile() && (lower.endsWith(".7z") || lower.endsWith(".rar") || lower.endsWith(".iso") || lower.endsWith(".bin") && file.length() > 100L * 1024 * 1024)) return true;
            if (file.isDirectory() && containsArchive(file, depth + 1)) return true;
        }
        return false;
    }

    /** A child by name, case-insensitively (the ZIP may spell folders CARS or Cars). */
    static File child(File directory, String name) {
        File exact = new File(directory, name);
        if (exact.exists()) return exact;
        File[] children = directory.listFiles();
        if (children != null) for (File file : children) if (file.getName().equalsIgnoreCase(name)) return file;
        return null;
    }

    private static void moveIntoPlace(File root, File destination) throws IOException {
        List<String> names = new ArrayList<>();
        names.add("Speed.exe");
        for (String folder : GAME_FOLDERS) names.add(folder);
        for (String name : names) {
            File source = child(root, name);
            if (source == null) continue;
            File target = new File(destination, source.getName());
            File existing = child(destination, source.getName());
            if (existing != null) deleteTree(existing);
            if (!source.renameTo(target)) {
                // Different filesystem or a lingering handle: fall back to a copy.
                copyTree(source, target);
                deleteTree(source);
            }
        }
    }

    private static void copyTree(File source, File target) throws IOException {
        if (source.isDirectory()) {
            if (!target.isDirectory() && !target.mkdirs()) throw new IOException("mkdir " + target);
            File[] children = source.listFiles();
            if (children != null) for (File child : children) copyTree(child, new File(target, child.getName()));
            return;
        }
        try (InputStream in = new java.io.FileInputStream(source); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
        }
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    private static final class CountingInputStream extends FilterInputStream {
        long count;
        CountingInputStream(InputStream in) { super(in); }
        @Override public int read() throws IOException { int b = in.read(); if (b >= 0) count++; return b; }
        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            int n = in.read(buffer, offset, length);
            if (n > 0) count += n;
            return n;
        }
        @Override public long skip(long n) throws IOException { long skipped = in.skip(n); count += skipped; return skipped; }
    }
}
