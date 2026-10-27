package com.nfsu1.androidevolved;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Backup and restore of the game's profile folder ({@code nfsu1/Save}: career saves, options, records).
 * A backup is a plain ZIP of that folder saved to Download/NFSU1 and offered to the share sheet (Drive,
 * email). Restoring replaces the folder atomically: the ZIP is checked and unpacked beside it first, the
 * current folder is renamed away, the new one moved into place, and the old one deleted only at the end
 * (any failure puts the old folder back). Both refuse to run while the game process is alive.
 */
final class SaveBackup {
    /** The game writes profiles to "My Documents\NFS Underground"; the port maps My Documents to the game folder. */
    static final String FOLDER = "NFS Underground";
    static final String LEGACY_FOLDER = "Save";
    static final long MAX_BYTES = 256L * 1024 * 1024;
    static final int MAX_ENTRIES = 2000;

    static final class Result {
        final int files; final long bytes; final Uri uri; final String location;
        Result(int files, long bytes, Uri uri, String location) { this.files = files; this.bytes = bytes; this.uri = uri; this.location = location; }
    }

    /** The profile folder whatever its case on disk (NFS Underground, else a legacy Save), or the canonical path when none exists yet. */
    static File saveFolder(File root) {
        File[] children = root.listFiles();
        if (children != null) for (File child : children) if (child.isDirectory() && child.getName().equalsIgnoreCase(FOLDER)) return child;
        if (children != null) for (File child : children) if (child.isDirectory() && child.getName().equalsIgnoreCase(LEGACY_FOLDER)) return child;
        return new File(root, FOLDER);
    }

    /** Writes the ZIP to Download/NFSU1 through MediaStore (no storage permission needed for our own files). */
    static Result backup(Context context, File root) throws IOException {
        File save = saveFolder(root);
        List<File> files = new ArrayList<>();
        collect(save, files, 0);
        if (files.isEmpty()) throw new IOException(context.getString(R.string.saves_none));
        String name = "NFSU-saves-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".zip";
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        values.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS + "/NFSU1");
        ContentResolver resolver = context.getContentResolver();
        Uri target = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (target == null) throw new IOException(context.getString(R.string.report_err_downloads_create));
        long bytes;
        try (OutputStream output = resolver.openOutputStream(target, "w")) {
            if (output == null) throw new IOException(context.getString(R.string.report_err_downloads_write));
            bytes = writeZip(files, save, output);
        } catch (IOException error) {
            resolver.delete(target, null, null);
            throw error;
        }
        return new Result(files.size(), bytes, target, "Download/NFSU1/" + name);
    }

    /** ZIP of the folder's files with paths relative to it; returns the bytes archived. */
    static long writeZip(List<File> files, File base, OutputStream output) throws IOException {
        long total = 0;
        String prefix = base.getAbsolutePath();
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            byte[] buffer = new byte[65536];
            for (File file : files) {
                String relative = file.getAbsolutePath().substring(prefix.length() + 1).replace(File.separatorChar, '/');
                ZipEntry entry = new ZipEntry(relative);
                entry.setTime(file.lastModified());
                zip.putNextEntry(entry);
                try (InputStream input = new FileInputStream(file)) {
                    for (int n; (n = input.read(buffer)) != -1;) { zip.write(buffer, 0, n); total += n; }
                }
                zip.closeEntry();
            }
        }
        return total;
    }

    /**
     * Replaces the Save folder with the ZIP's contents. Entries may be rooted at the ZIP's top level or
     * under a single "Save/" folder (a backup of the whole nfsu1 folder is refused: that is a game import).
     */
    static Result restore(Context context, InputStream zipStream, File root) throws IOException {
        File save = saveFolder(root);
        File staging = new File(root, ".save-restore-" + System.nanoTime());
        if (!staging.mkdirs()) throw new IOException(context.getString(R.string.saves_err_staging));
        int files = 0; long bytes = 0;
        try {
            try (ZipInputStream zip = new ZipInputStream(zipStream)) {
                byte[] buffer = new byte[65536];
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                    String name = entryName(entry.getName());
                    if (name == null) throw new IOException(context.getString(R.string.saves_err_entry, entry.getName()));
                    if (entry.isDirectory() || name.isEmpty()) continue;
                    if (++files > MAX_ENTRIES) throw new IOException(context.getString(R.string.saves_err_too_many));
                    File target = new File(staging, name);
                    File parent = target.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException(context.getString(R.string.saves_err_staging));
                    try (OutputStream output = new FileOutputStream(target)) {
                        for (int n; (n = zip.read(buffer)) != -1;) {
                            output.write(buffer, 0, n);
                            if ((bytes += n) > MAX_BYTES) throw new IOException(context.getString(R.string.saves_err_too_large));
                        }
                    }
                }
            }
            if (files == 0) throw new IOException(context.getString(R.string.saves_err_empty));
            File previous = new File(root, ".save-previous-" + System.nanoTime());
            boolean hadPrevious = save.isDirectory();
            if (hadPrevious && !save.renameTo(previous)) throw new IOException(context.getString(R.string.saves_err_replace));
            if (!staging.renameTo(save)) {
                if (hadPrevious) previous.renameTo(save);
                throw new IOException(context.getString(R.string.saves_err_replace));
            }
            if (hadPrevious) deleteTree(previous);
            return new Result(files, bytes, null, save.getName());
        } finally {
            if (staging.exists()) deleteTree(staging);
        }
    }

    /**
     * Normalises a ZIP entry to a path inside Save: no absolute paths, drive letters or parent references;
     * a leading "Save/" (any case) is dropped. Returns null for an entry that must not be written.
     */
    static String entryName(String raw) {
        String name = raw.replace('\\', '/');
        while (name.startsWith("/")) name = name.substring(1);
        if (name.contains(":") || name.contains("\0")) return null;
        for (String part : name.split("/")) if (part.equals("..") || part.equals(".")) return null;
        for (String folder : new String[] {FOLDER, LEGACY_FOLDER})
            if (name.toLowerCase(Locale.ROOT).startsWith(folder.toLowerCase(Locale.ROOT) + "/")) { name = name.substring(folder.length() + 1); break; }
        // Anything else nested under a top-level folder of the game (Cars/..., Tracks/...) is not a save.
        String top = name.contains("/") ? name.substring(0, name.indexOf('/')).toLowerCase(Locale.ROOT) : "";
        for (String folder : new String[] {"cars", "frontend", "global", "languages", "movies", "sound", "tracks", "fx", "sndstreams", "speechstreams"})
            if (top.equals(folder)) return null;
        return name;
    }

    static void collect(File directory, List<File> out, int depth) {
        File[] children = directory.listFiles();
        if (children == null || depth > 6) return;
        java.util.Arrays.sort(children);
        for (File child : children) {
            if (child.isDirectory()) collect(child, out, depth + 1);
            else if (child.isFile()) out.add(child);
        }
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
}
