package com.nfsu1.androidevolved;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Data mods for the game folder ("mod packs"): a ZIP whose entries are game-relative files
 * (Global/GLOBALB.BUN, Tracks/TRACKB1000.lzc, FrontEnd/FrontB.lzc ...), optionally described by an
 * {@code nfsu-modpack.json} manifest, installed over Internal storage/nfsu1 with every replaced original
 * kept, so a pack can be removed again. This is how the HD texture packs built for this port and
 * community data mods such as the Free Roam Mod reach the phone: no Windows tools involved.
 *
 * Layout under {@code nfsu1/.nfsu-modpacks/}: {@code installed.json} (the registry, in install order) and one
 * folder per pack holding the files it displaced. Packs stack: a later pack that touches the same file
 * keeps the earlier pack's version, so removal has to run in reverse order; {@link #uninstall} removes a
 * pack together with every pack installed after it.
 *
 * Safety: entries are confined to the known game folders (a single wrapping folder such as
 * "NFSU Free Roam Beta 2.0/" is tolerated), executables, DLLs and scripts are skipped, and Speed.exe is
 * never replaced: the recompiled game is tied to the verified executable.
 */
final class ModPacks {
    static final String STORE = ".nfsu-modpacks", MANIFEST = "nfsu-modpack.json", REGISTRY = "installed.json";
    static final long MAX_BYTES = 3L * 1024 * 1024 * 1024;
    static final int MAX_ENTRIES = 20000;
    /** First path component a pack file must have (lower case). */
    static final Set<String> GAME_FOLDERS = new HashSet<>(Arrays.asList("cars", "frontend", "global", "languages", "movies",
            "sound", "tracks", "fx", "sndstreams", "speechstreams", "scripts"));
    private static final Set<String> SKIPPED_EXTENSIONS = new HashSet<>(Arrays.asList("exe", "dll", "asi", "bat", "cmd", "ps1",
            "ini", "url", "lnk", "end", "txt", "md", "pdf", "html", "htm", "jpg", "jpeg", "png", "gif", "db", "ds_store"));

    interface Progress { void update(long bytes, String entry); }

    static final class Pack {
        final String id, name, version, notes;
        final long installedAt;
        final List<String> files;          // game-relative, as installed
        final List<String> replaced;       // subset of files that displaced an existing file (backed up)
        Pack(String id, String name, String version, String notes, long installedAt, List<String> files, List<String> replaced) {
            this.id = id; this.name = name; this.version = version; this.notes = notes; this.installedAt = installedAt;
            this.files = files; this.replaced = replaced;
        }
        JSONObject toJson() throws JSONException {
            return new JSONObject().put("id", id).put("name", name).put("version", version).put("notes", notes)
                    .put("installedAt", installedAt).put("files", new JSONArray(files)).put("replaced", new JSONArray(replaced));
        }
        static Pack fromJson(JSONObject o) {
            List<String> files = new ArrayList<>(), replaced = new ArrayList<>();
            JSONArray f = o.optJSONArray("files"), r = o.optJSONArray("replaced");
            if (f != null) for (int i = 0; i < f.length(); i++) files.add(f.optString(i));
            if (r != null) for (int i = 0; i < r.length(); i++) replaced.add(r.optString(i));
            return new Pack(o.optString("id"), o.optString("name"), o.optString("version"), o.optString("notes"), o.optLong("installedAt"), files, replaced);
        }
    }

    // ---- Registry ----------------------------------------------------------------------------------------

    static File store(File root) { return new File(root, STORE); }

    static List<Pack> installed(File root) {
        List<Pack> packs = new ArrayList<>();
        File registry = new File(store(root), REGISTRY);
        if (!registry.isFile()) return packs;
        try {
            JSONArray array = new JSONArray(readText(registry));
            for (int i = 0; i < array.length(); i++) packs.add(Pack.fromJson(array.getJSONObject(i)));
        } catch (IOException | JSONException error) { /* unreadable registry: treat as empty, never guess */ }
        return packs;
    }

    private static void writeRegistry(File root, List<Pack> packs) throws IOException {
        try {
            JSONArray array = new JSONArray();
            for (Pack pack : packs) array.put(pack.toJson());
            File registry = new File(store(root), REGISTRY), temp = new File(store(root), REGISTRY + ".tmp");
            try (OutputStream out = new FileOutputStream(temp)) { out.write(array.toString(2).getBytes(StandardCharsets.UTF_8)); }
            if (!temp.renameTo(registry)) throw new IOException("could not write the mod pack registry");
        } catch (JSONException error) { throw new IOException(error); }
    }

    // ---- Install -----------------------------------------------------------------------------------------

    /**
     * Installs a pack. {@code sourceName} (the ZIP's display name) names the pack when it has no manifest.
     * The game process must not be running. Throws with a readable English reason; nothing is changed
     * in the game folder unless every file could be staged first.
     */
    static Pack install(InputStream zipStream, File root, String sourceName, Progress progress) throws IOException {
        return install(zipStream, root, sourceName, progress, BuildConfig.VERSION_CODE);
    }

    /**
     * As above, for an app of the given versionCode: a pack whose manifest asks for a newer app
     * ({@code "minVersionCode"}, e.g. pack v12's HD fonts need build 14's font fix) is refused before
     * anything in the game folder changes.
     */
    static Pack install(InputStream zipStream, File root, String sourceName, Progress progress, int appVersionCode) throws IOException {
        if (!root.isDirectory()) throw new IOException("game folder not found");
        File store = store(root);
        if (!store.isDirectory() && !store.mkdirs()) throw new IOException("could not create " + STORE);
        File staging = new File(store, ".staging-" + System.nanoTime());
        if (!staging.mkdirs()) throw new IOException("could not create a staging folder");
        List<String> stagedNames = new ArrayList<>();
        JSONObject manifest = null;
        long bytes = 0;
        try {
            // 1. Stage every acceptable entry.
            try (ZipInputStream zip = new ZipInputStream(zipStream)) {
                byte[] buffer = new byte[1 << 16];
                int entries = 0;
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                    if (entry.isDirectory()) continue;
                    if (++entries > MAX_ENTRIES) throw new IOException("too many files for a mod pack");
                    String raw = entry.getName().replace('\\', '/');
                    String base = raw.substring(raw.lastIndexOf('/') + 1);
                    if (base.equalsIgnoreCase(MANIFEST)) {
                        manifest = parseManifest(readAll(zip, 1 << 20));
                        continue;
                    }
                    String name = entryName(raw);
                    if (name == null) continue;
                    File target = new File(staging, name);
                    File parent = target.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not stage " + name);
                    try (OutputStream out = new FileOutputStream(target)) {
                        for (int n; (n = zip.read(buffer)) != -1;) {
                            out.write(buffer, 0, n);
                            if ((bytes += n) > MAX_BYTES) throw new IOException("the pack is larger than " + (MAX_BYTES >> 30) + " GB");
                        }
                    }
                    stagedNames.add(name);
                    if (progress != null) progress.update(bytes, name);
                }
            }
            if (stagedNames.isEmpty()) throw new IOException("no game files in this ZIP (expected Global/, Tracks/, FrontEnd/, Cars/ ... paths)");
            int needed = manifest != null ? manifest.optInt("minVersionCode", 0) : 0;
            if (needed > appVersionCode)
                throw new IOException(String.format(java.util.Locale.US, "this pack needs build %d of the app or newer (this is build %d): install the newer APK first",
                        needed - 1000, appVersionCode - 1000));
            // 2. Identity and registry.
            List<Pack> packs = installed(root);
            String name = manifest != null ? manifest.optString("name", "") : "";
            if (name.isEmpty()) name = cleanName(sourceName);
            String version = manifest != null ? manifest.optString("version", "") : "";
            String notes = manifest != null ? manifest.optString("notes", "") : "";
            String id = slug(name) + "-" + Long.toString(System.currentTimeMillis(), 36);
            File backup = new File(store, id);
            if (!backup.mkdirs()) throw new IOException("could not create the backup folder");
            // 3. Move into place, keeping what each file displaces.
            List<String> installedFiles = new ArrayList<>(), replaced = new ArrayList<>();
            for (String relative : stagedNames) {
                File destination = resolveExisting(root, relative);
                String actualRelative = relativePath(root, destination);
                if (destination.isFile()) {
                    File kept = new File(backup, actualRelative);
                    File keptParent = kept.getParentFile();
                    if (keptParent != null && !keptParent.isDirectory() && !keptParent.mkdirs()) throw new IOException("could not back up " + actualRelative);
                    if (!destination.renameTo(kept)) throw new IOException("could not move aside " + actualRelative);
                    replaced.add(actualRelative);
                }
                File destinationParent = destination.getParentFile();
                if (destinationParent != null && !destinationParent.isDirectory() && !destinationParent.mkdirs()) throw new IOException("could not create " + actualRelative);
                if (!new File(staging, relative).renameTo(destination)) {
                    // Put the original back before giving up on this file.
                    File kept = new File(backup, actualRelative);
                    if (kept.isFile()) kept.renameTo(destination);
                    throw new IOException("could not install " + actualRelative);
                }
                installedFiles.add(actualRelative);
            }
            Pack pack = new Pack(id, name, version, notes, System.currentTimeMillis(), installedFiles, replaced);
            packs.add(pack);
            writeRegistry(root, packs);
            return pack;
        } finally {
            deleteTree(staging);
        }
    }

    /** Removes a pack and every pack installed after it (they may sit on top of its files); returns what was removed. */
    static List<Pack> uninstall(File root, String id) throws IOException {
        List<Pack> packs = installed(root);
        int index = -1;
        for (int i = 0; i < packs.size(); i++) if (packs.get(i).id.equals(id)) index = i;
        if (index < 0) throw new IOException("pack not installed");
        // Every displaced original must still be there before anything is touched.
        for (int i = packs.size() - 1; i >= index; i--)
            for (String relative : packs.get(i).replaced)
                if (!new File(new File(store(root), packs.get(i).id), relative).isFile())
                    throw new IOException("backup of " + relative + " is missing (" + packs.get(i).name + "); import the game files again to restore it");
        List<Pack> removed = new ArrayList<>();
        for (int i = packs.size() - 1; i >= index; i--) {
            Pack pack = packs.get(i);
            File backup = new File(store(root), pack.id);
            for (String relative : pack.files) {
                File installedFile = new File(root, relative);
                File kept = new File(backup, relative);
                if (installedFile.exists() && !installedFile.delete()) throw new IOException("could not remove " + relative);
                if (kept.isFile() && !kept.renameTo(installedFile)) throw new IOException("could not restore " + relative);
            }
            deleteTree(backup);
            packs.remove(i);
            removed.add(pack);
        }
        writeRegistry(root, packs);
        return removed;
    }

    // ---- Names and paths ---------------------------------------------------------------------------------

    /**
     * A game-relative path for a ZIP entry, or null when the entry is not a game file: it must start with a
     * known game folder (after at most one wrapping folder), stay inside it, and not be an executable,
     * script, document or Speed.exe. Case is preserved; comparisons are case-insensitive.
     */
    static String entryName(String raw) {
        String name = raw.replace('\\', '/');
        while (name.startsWith("/")) name = name.substring(1);
        if (name.isEmpty() || name.contains(":") || name.indexOf('\0') >= 0) return null;
        List<String> parts = new ArrayList<>(Arrays.asList(name.split("/")));
        for (String part : parts) if (part.isEmpty() || part.equals(".") || part.equals("..")) return null;
        String base = parts.get(parts.size() - 1);
        if (base.startsWith(".") || base.equalsIgnoreCase("speed.exe")) return null;
        int dot = base.lastIndexOf('.');
        String extension = dot < 0 ? "" : base.substring(dot + 1).toLowerCase(Locale.ROOT);
        if (SKIPPED_EXTENSIONS.contains(extension)) return null;
        if (parts.size() >= 2 && parts.get(0).equalsIgnoreCase("__MACOSX")) return null;
        // Drop one wrapping folder ("NFSU Free Roam/Global/x" -> "Global/x").
        if (parts.size() >= 3 && !GAME_FOLDERS.contains(parts.get(0).toLowerCase(Locale.ROOT))
                && GAME_FOLDERS.contains(parts.get(1).toLowerCase(Locale.ROOT))) parts.remove(0);
        if (parts.size() < 2 || !GAME_FOLDERS.contains(parts.get(0).toLowerCase(Locale.ROOT))) return null;
        StringBuilder out = new StringBuilder();
        for (String part : parts) out.append(out.length() == 0 ? "" : "/").append(part);
        return out.toString();
    }

    /** The file the relative path means on this (case-insensitive) copy of the game: an existing entry whatever its case. */
    static File resolveExisting(File root, String relative) {
        File current = root;
        String[] parts = relative.split("/");
        for (int i = 0; i < parts.length; i++) {
            File exact = new File(current, parts[i]);
            if (exact.exists()) { current = exact; continue; }
            File match = null;
            String[] names = current.list();
            if (names != null) for (String candidate : names) if (candidate.equalsIgnoreCase(parts[i])) { match = new File(current, candidate); break; }
            current = match != null ? match : exact;
        }
        return current;
    }

    static String relativePath(File root, File file) {
        String base = root.getAbsolutePath(), path = file.getAbsolutePath();
        return path.startsWith(base) ? path.substring(base.length() + 1).replace(File.separatorChar, '/') : file.getName();
    }

    static String cleanName(String sourceName) {
        if (sourceName == null || sourceName.trim().isEmpty()) return "Mod pack";
        String name = sourceName.trim();
        int dot = name.lastIndexOf('.');
        if (dot > 0 && name.length() - dot <= 5) name = name.substring(0, dot);
        return name.replace('_', ' ').trim();
    }

    static String slug(String name) {
        String slug = name.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "-").replaceAll("(^-|-$)", "");
        if (slug.length() > 40) slug = slug.substring(0, 40);
        return slug.isEmpty() ? "pack" : slug;
    }

    private static JSONObject parseManifest(byte[] data) throws IOException {
        try { return new JSONObject(new String(data, StandardCharsets.UTF_8)); }
        catch (JSONException error) { throw new IOException("the pack's manifest is not valid JSON"); }
    }

    // ---- Files -------------------------------------------------------------------------------------------

    static String readText(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) { return new String(readAll(in, 4 << 20), StandardCharsets.UTF_8); }
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[1 << 16];
        for (int n; out.size() < limit && (n = in.read(buffer, 0, Math.min(buffer.length, limit - out.size()))) != -1;) out.write(buffer, 0, n);
        return out.toByteArray();
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
}
