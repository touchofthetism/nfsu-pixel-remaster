package com.nfsu1.androidevolved;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import javax.net.ssl.HttpsURLConnection;

/**
 * Optional GitHub updates from the repository this build was made from (BuildConfig.RELEASE_REPO).
 * No APK is installed or downloaded in the background. A private repository answers 404 to the
 * anonymous API, which counts as "no release to offer", so automatic checks stay silent.
 */
final class ReleaseUpdates {
    static final String REPO = "https://github.com/" + BuildConfig.RELEASE_REPO + "/";
    private static final String API = "https://api.github.com/repos/" + BuildConfig.RELEASE_REPO + "/releases/latest";
    static final class Result {
        final String tag, url, error;
        /** True when the repository answered 401/403/404 to the anonymous API: its releases are not visible from here. */
        final boolean hidden;
        Result(String tag, String url, String error) { this(tag, url, error, false); }
        Result(String tag, String url, String error, boolean hidden) { this.tag = tag; this.url = url; this.error = error; this.hidden = hidden; }
    }
    static SharedPreferences prefs(Context context) { return context.getSharedPreferences("nfsu1_updates", 0); }
    static String installed(Context context) {
        try { return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (Exception ignored) { return "0.0.0"; }
    }
    static String releaseTagUrl(String version) { return REPO + "releases/tag/v" + Uri.encode(version); }

    static void check(Context context, boolean force, Consumer<Result> callback) {
        Context app = context.getApplicationContext();
        Handler main = new Handler(Looper.getMainLooper());
        Thread worker = new Thread(() -> {
            Result result;
            try { result = fetch(app, force); }
            catch (Exception error) { result = new Result(null, null, app.getString(R.string.updates_unreachable)); }
            Result value = result;
            main.post(() -> callback.accept(value));
        }, "GitHubReleaseCheck");
        worker.setDaemon(true); worker.start();
    }
    /**
     * The version whose "what's new" notice the player has not closed yet, or null. An update is
     * detected when versionCode grew since the last launch; a fresh install only records the
     * version, and the first launch of a build that predates this record counts as an update when
     * Android says the package was updated after its install. The notice stays pending until
     * {@link #whatsNewSeen} (so a rotation that recreates the launcher shows it again).
     */
    static String pendingWhatsNew(Context context) {
        android.content.pm.PackageInfo info;
        try { info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0); }
        catch (Exception ignored) { return null; }
        long current = info.getLongVersionCode();
        SharedPreferences prefs = prefs(context);
        long previous = prefs.getLong("seen_version_code", 0);
        if (previous != current) {
            boolean updated = previous != 0 ? current > previous : info.lastUpdateTime > info.firstInstallTime;
            SharedPreferences.Editor edit = prefs.edit().putLong("seen_version_code", current);
            if (updated) edit.putString("whats_new", installed(context)); else edit.remove("whats_new");
            edit.apply();
        }
        String pending = prefs.getString("whats_new", "");
        return pending.isEmpty() ? null : pending;
    }

    static void whatsNewSeen(Context context) { prefs(context).edit().remove("whats_new").apply(); }

    /** Release notes of a version (GitHub release v{version}) as plain text, or null if unavailable. */
    static void releaseNotes(Context context, String version, Consumer<String> callback) {
        Handler main = new Handler(Looper.getMainLooper());
        Thread worker = new Thread(() -> {
            String notes = null;
            try {
                HttpsURLConnection connection = (HttpsURLConnection) new URL(
                        "https://api.github.com/repos/" + BuildConfig.RELEASE_REPO + "/releases/tags/v" + Uri.encode(version)).openConnection();
                connection.setConnectTimeout(6000); connection.setReadTimeout(6000);
                connection.setRequestProperty("Accept", "application/vnd.github+json");
                connection.setRequestProperty("User-Agent", "NFSU-Remastered/" + version);
                try {
                    if (connection.getResponseCode() == 200) {
                        ByteArrayOutputStream body = new ByteArrayOutputStream();
                        try (InputStream input = connection.getInputStream()) {
                            byte[] buffer = new byte[8192]; int n;
                            while ((n = input.read(buffer)) != -1 && body.size() < 512 * 1024) body.write(buffer, 0, n);
                        }
                        notes = plainText(new JSONObject(new String(body.toByteArray(), StandardCharsets.UTF_8)).optString("body"));
                    }
                } finally { connection.disconnect(); }
            } catch (Exception ignored) { }
            String value = notes;
            main.post(() -> callback.accept(value));
        }, "GitHubReleaseNotes");
        worker.setDaemon(true); worker.start();
    }

    /** The release notes bundled into this APK at build time (docs/RELEASE_NOTES.md), or null. */
    static String bundledNotes(Context context) {
        try (InputStream input = context.getResources().openRawResource(R.raw.release_notes)) {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192]; int n;
            while ((n = input.read(buffer)) != -1) body.write(buffer, 0, n);
            return plainText(new String(body.toByteArray(), StandardCharsets.UTF_8));
        } catch (Exception ignored) { return null; }
    }

    /** In the app, skip the download/install preamble of the release page: start at the changes heading. */
    static String whatsNewSection(String notes) {
        if (notes == null) return null;
        for (String heading : new String[] {"What's new", "What’s new", "Changes", "Novedades"}) {
            int start = notes.indexOf(heading);
            if (start > 0) return notes.substring(start);
        }
        return notes;
    }

    /** Markdown of a release body to readable dialog text. */
    static String plainText(String markdown) {
        if (markdown == null || markdown.trim().isEmpty()) return null;
        return markdown.replace("\r", "")
                .replaceAll("(?m)^#{1,6}\\s*", "")
                .replaceAll("(?m)^>\\s?", "")
                .replaceAll("(?m)^(\\s*)[-*]\\s+", "$1• ")
                .replace("**", "").replace("`", "")
                .replaceAll("\\[([^\\]]+)\\]\\([^)]+\\)", "$1")
                .replaceAll("\n{3,}", "\n\n").trim();
    }

    private static Result fetch(Context context, boolean force) throws Exception {
        SharedPreferences prefs = prefs(context);
        long age = System.currentTimeMillis() - prefs.getLong("last_check", 0);
        if (!force && age >= 0 && age < TimeUnit.HOURS.toMillis(6)) {
            String tag = prefs.getString("latest_tag", "");
            String url = prefs.getString("latest_url", REPO + "releases/latest");
            return new Result(tag, url.startsWith(REPO + "releases/") ? url : REPO + "releases/latest", null, prefs.getBoolean("hidden", false));
        }
        HttpsURLConnection connection = (HttpsURLConnection) new URL(API).openConnection();
        connection.setConnectTimeout(6000); connection.setReadTimeout(6000);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "NFSU-Remastered/" + installed(context));
        connection.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
        try {
            int code = connection.getResponseCode();
            if (code == 404 || code == 403 || code == 401) {
                // Private repository (or no release yet): nothing visible from here, not a connection problem.
                prefs.edit().putLong("last_check", System.currentTimeMillis())
                        .putString("latest_tag", "").putString("latest_url", REPO + "releases").putBoolean("hidden", true).apply();
                return new Result("", REPO + "releases", null, true);
            }
            prefs.edit().putBoolean("hidden", false).apply();
            if (code != 200) throw new IOException("GitHub answered " + code);
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream input = connection.getInputStream()) {
                byte[] buffer = new byte[8192]; int n;
                while ((n = input.read(buffer)) != -1) {
                    if (body.size() + n > 512 * 1024) throw new IOException("Response too large");
                    body.write(buffer, 0, n);
                }
            }
            JSONObject release = new JSONObject(new String(body.toByteArray(), StandardCharsets.UTF_8));
            String tag = release.getString("tag_name");
            if (release.optBoolean("draft") || release.optBoolean("prerelease")) tag = "";
            String url = REPO + "releases/tag/" + Uri.encode(tag);
            // Prefer this release's ARM64 APK. Source ZIPs and links outside our repository are ignored.
            JSONArray assets = release.optJSONArray("assets");
            if (assets != null) for (int i = 0; i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                String name = asset.optString("name").toLowerCase(java.util.Locale.ROOT);
                String download = asset.optString("browser_download_url");
                if (name.endsWith(".apk") && download.startsWith(REPO + "releases/download/" + Uri.encode(tag) + "/")) {
                    url = download;
                    if (name.contains("arm64")) break;
                }
            }
            prefs.edit().putLong("last_check", System.currentTimeMillis())
                    .putString("latest_tag", tag).putString("latest_url", url).apply();
            return new Result(tag, url, null);
        } finally { connection.disconnect(); }
    }
}
