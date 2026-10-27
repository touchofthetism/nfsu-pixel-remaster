package com.nfsu1.androidevolved;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Process;
import android.provider.MediaStore;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Crash detection and diagnostics (after the MW port). A report holds the device and launcher options,
 * the game process' exit records, the per-session logs and telemetry written by {@link GameTelemetry}
 * (with a readable session-summary.txt), the current log buffer, the DXVK graphics caches, the small
 * profile/save folder and dxvk.conf; never the game's data files. Nothing is uploaded automatically: the
 * ZIP is saved to Download/NFSU1 and handed to the system share sheet, so it reaches whoever maintains
 * this build through Drive, email or a chat.
 *
 * The report text is written in English regardless of the UI language: it is read by the maintainer.
 */
final class Diagnostics {
    private static final int LIMIT = 4 * 1024 * 1024;
    private static final long CACHE_LIMIT = 64L * 1024 * 1024, LOG_LIMIT = 8L * 1024 * 1024, SAVE_LIMIT = 4L * 1024 * 1024;
    private static final int LOGS_IN_REPORT = 6, SESSIONS_IN_REPORT = 8;
    private static final String PREFS = "nfsu1_diagnostics";
    /** Game-folder entries the port expects; anything else is listed one level deep in the report. */
    private static final String[] KNOWN_ENTRIES = {"cars", "frontend", "global", "languages", "movies", "sound", "tracks",
            "sndstreams", "speechstreams", "fx", "save", "speed.exe", "scripts", "binkw32.dll", "speed.cfg"};

    /** Where the report landed: the MediaStore document and a path the player recognises. */
    static final class Saved {
        final Uri uri;
        final String location;
        Saved(Uri uri, String location) { this.uri = uri; this.location = location; }
    }

    /** A crash of the :game process the player has not been asked about yet, or null. */
    static String unreportedCrash(Context context) {
        if (Build.VERSION.SDK_INT < 30) return null;
        SharedPreferences prefs = context.getSharedPreferences(PREFS, 0);
        long seen = prefs.getLong("crash_seen", 0);
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return null;
        try {
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 8)) {
                if (exit.getTimestamp() <= seen || !exit.getProcessName().endsWith(":game")) continue;
                int reason = exit.getReason();
                if (reason == ApplicationExitInfo.REASON_CRASH || reason == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || reason == ApplicationExitInfo.REASON_ANR) {
                    return context.getString(reason == ApplicationExitInfo.REASON_ANR ? R.string.crash_anr : R.string.crash_native)
                            + (exit.getDescription() == null ? "" : " (" + exit.getDescription() + ")");
                }
            }
        } catch (RuntimeException ignored) { }
        return null;
    }

    static void acknowledgeCrashes(Context context) {
        context.getSharedPreferences(PREFS, 0).edit().putLong("crash_seen", System.currentTimeMillis()).apply();
    }

    static String summary(Context context) {
        GpuDrivers.Driver driver = GpuDrivers.selected(context);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager != null) manager.getMemoryInfo(memory);
        return context.getString(R.string.app_name) + " " + ReleaseUpdates.installed(context)
                + " (" + BuildConfig.APPLICATION_ID + ", versionCode " + BuildConfig.VERSION_CODE + ")"
                + "\nAPK ABI: " + (Process.is64Bit() ? "arm64-v8a" : "armeabi-v7a")
                + "\nPhone: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")"
                + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ", " + Build.VERSION.SECURITY_PATCH + ")"
                + "\nSoC: " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE)
                + "\nRAM: " + (memory.totalMem >> 20) + " MB total, " + (memory.availMem >> 20) + " MB free"
                + "\nVulkan driver: " + (driver == null ? "system" : driver.name) + "\n";
    }

    static File createReport(Context context) throws Exception {
        File directory = new File(context.getCacheDir(), "reports");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException(context.getString(R.string.report_err_create));
        File[] old = directory.listFiles();
        if (old != null) for (File file : old)
            if (file.isFile() && file.lastModified() < System.currentTimeMillis() - TimeUnit.DAYS.toMillis(7)) file.delete();
        String date = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File report = new File(directory, "NFSU-diagnostics-" + date + ".zip");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(report))) {
            StringBuilder info = new StringBuilder(summary(context));
            info.append("Date: ").append(new Date()).append("\nSupported ABIs: ").append(Arrays.toString(Build.SUPPORTED_ABIS))
                    .append("\nUI locale: ").append(Locale.getDefault()).append("\n\nOptions:\n");
            for (GameOptions.Option option : GameOptions.ALL)
                info.append(option.key).append('=').append(GameOptions.get(context, option.key))
                    .append(" (default ").append(GameOptions.defaultValue(context, option.key)).append(")\n");
            info.append("language=").append(GameOptions.language(context, LauncherActivity.gameRoot())).append('\n');
            info.append("online_server=").append(GameOptions.onlineServer(context)).append('\n');
            info.append(deviceState(context));
            info.append("\nGame folder (Internal storage/nfsu1):\n").append(gameFolderListing(LauncherActivity.gameRoot()));
            List<File> sessions = GameTelemetry.sessionFiles(context), logs = GameTelemetry.logFiles(context);
            info.append("\nSessions recorded: ").append(sessions.size()).append(", logs: ").append(logs.size()).append('\n');
            add(zip, "report.txt", info.toString().getBytes(StandardCharsets.UTF_8));
            add(zip, "session-summary.txt", sessionSummaries(sessions).getBytes(StandardCharsets.UTF_8));
            add(zip, "logcat-now.txt", logcat(0));
            ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            if (Build.VERSION.SDK_INT >= 30 && manager != null) addExitInfo(zip, manager, context);
            for (int i = 0; i < Math.min(LOGS_IN_REPORT, logs.size()); i++) addFile(zip, "logs/" + logs.get(i).getName(), logs.get(i), LOG_LIMIT);
            for (int i = 0; i < Math.min(SESSIONS_IN_REPORT, sessions.size()); i++) addFile(zip, "telemetry/" + sessions.get(i).getName(), sessions.get(i), LIMIT);
            File config = new File(context.getFilesDir(), "dxvk.conf");
            if (config.isFile()) addFile(zip, "dxvk.conf", config, 65536);
            addGraphicsCache(zip, context);
            addSaveFolder(zip, LauncherActivity.gameRoot());
        } catch (Exception error) {
            report.delete();
            throw error;
        }
        return report;
    }

    /** Names and sizes of the top level of the game folder; unexpected entries are listed one level deep. */
    static String gameFolderListing(File root) {
        File[] children = root.listFiles();
        if (children == null) return "(not readable)\n";
        StringBuilder text = new StringBuilder();
        Arrays.sort(children);
        for (File child : children) {
            boolean known = Arrays.asList(KNOWN_ENTRIES).contains(child.getName().toLowerCase(Locale.ROOT));
            if (child.isDirectory()) {
                String[] inside = child.list();
                text.append(child.getName()).append("/  ").append(inside == null ? "?" : inside.length).append(" entries").append(known ? "" : "  (unexpected)").append('\n');
                if (!known && inside != null) {
                    Arrays.sort(inside);
                    for (int i = 0; i < Math.min(inside.length, 24); i++) text.append("    ").append(inside[i]).append('\n');
                    if (inside.length > 24) text.append("    ...\n");
                }
            } else {
                text.append(child.getName()).append("  ").append(child.length()).append(" bytes").append(known ? "" : "  (unexpected)").append('\n');
            }
        }
        return text.toString();
    }

    /** Thermal state, display modes and the current refresh rate as the launcher sees them. */
    private static String deviceState(Context context) {
        StringBuilder text = new StringBuilder();
        try {
            android.os.PowerManager power = (android.os.PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (power != null) {
                text.append("Thermal status now: ").append(power.getCurrentThermalStatus());
                if (Build.VERSION.SDK_INT >= 30) text.append(", headroom ").append(power.getThermalHeadroom(0));
                text.append('\n');
            }
            android.view.Display display = Build.VERSION.SDK_INT >= 30 ? context.getDisplay()
                    : ((android.view.WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay();
            if (display != null) {
                android.view.Display.Mode mode = display.getMode();
                text.append("Display: ").append(mode.getPhysicalWidth()).append('x').append(mode.getPhysicalHeight()).append(" @ ")
                        .append(mode.getRefreshRate()).append(" Hz; modes:");
                for (android.view.Display.Mode m : display.getSupportedModes())
                    text.append(' ').append(m.getPhysicalWidth()).append('x').append(m.getPhysicalHeight()).append('@').append(Math.round(m.getRefreshRate()));
                text.append('\n');
            }
        } catch (RuntimeException error) { text.append("Device state unavailable: ").append(error).append('\n'); }
        text.append("Graphics log level: ").append(GameOptions.get(context, GameOptions.GFXLOG)).append('\n');
        return text.toString();
    }

    /** session-summary.txt: every recorded session, newest first, in readable lines. */
    static String sessionSummaries(List<File> sessions) {
        StringBuilder text = new StringBuilder("NFS Underground Remastered — session summaries (newest first)\n\n");
        if (sessions.isEmpty()) text.append("No session recorded yet: start the game once with this build.\n");
        for (File file : sessions) {
            try { text.append(SessionSummary.parse(readText(file, LIMIT)).report).append('\n'); }
            catch (Exception error) { text.append("Session ").append(file.getName()).append(": unreadable (").append(error).append(")\n\n"); }
        }
        return text.toString();
    }

    static String readText(File file, long limit) throws IOException {
        try (InputStream input = new FileInputStream(file)) { return new String(readLimited(input, (int) Math.min(limit, Integer.MAX_VALUE)), StandardCharsets.UTF_8); }
    }

    /** Adds a file; one larger than the limit contributes its last {@code limit} bytes (the recent end of a log). */
    private static void addFile(ZipOutputStream zip, String name, File file, long limit) throws IOException {
        if (!file.isFile()) return;
        long skip = Math.max(0, file.length() - limit);
        try (InputStream input = new FileInputStream(file)) {
            if (skip > 0) { long skipped = 0; while (skipped < skip) { long n = input.skip(skip - skipped); if (n <= 0) break; skipped += n; } }
            zip.putNextEntry(new ZipEntry(skip > 0 ? name + ".tail" : name));
            byte[] buffer = new byte[65536];
            for (int n; (n = input.read(buffer)) != -1;) zip.write(buffer, 0, n);
            zip.closeEntry();
        }
    }

    /**
     * The profile folder (options .opt/.cfg, career saves): small, and the only way to reproduce a report's
     * in-game detail settings or a crash on a particular career. Skipped when larger than SAVE_LIMIT.
     */
    private static void addSaveFolder(ZipOutputStream zip, File root) throws IOException {
        File save = null;
        File[] children = root.listFiles();
        if (children != null) for (File child : children) if (child.isDirectory() && child.getName().equalsIgnoreCase("save")) save = child;
        if (save == null) return;
        List<File> files = new ArrayList<>();
        long total = collect(save, files, 0);
        if (total > SAVE_LIMIT || files.size() > 200) { add(zip, "save/SKIPPED.txt", ("Save folder is " + total + " bytes in " + files.size() + " files; not included.\n").getBytes(StandardCharsets.UTF_8)); return; }
        String base = save.getAbsolutePath();
        for (File file : files) addFile(zip, "save/" + file.getAbsolutePath().substring(base.length() + 1).replace(File.separatorChar, '/'), file, SAVE_LIMIT);
    }

    private static long collect(File directory, List<File> out, int depth) {
        File[] children = directory.listFiles();
        long total = 0;
        if (children == null || depth > 4) return 0;
        for (File child : children) {
            if (child.isDirectory()) total += collect(child, out, depth + 1);
            else if (child.isFile()) { out.add(child); total += child.length(); }
        }
        return total;
    }

    /**
     * The current log buffer of this uid, saved under files/logs: the launcher calls it when it regains the
     * screen after a game, so the last minutes of the game process survive even when its own capture
     * could not start. Keeps the newest four.
     */
    static File captureLogTail(Context context) throws IOException {
        File logs = new File(context.getFilesDir(), GameTelemetry.LOG_DIR);
        logs.mkdirs();
        GameTelemetry.prune(logs, "tail-", ".txt", 4);
        File file = new File(logs, "tail-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".txt");
        byte[] data = logcat(0);
        try (OutputStream output = new FileOutputStream(file)) { output.write(data); }
        return file;
    }

    /**
     * The DXVK state cache (driver-independent pipeline state): a cache collected on one phone lets the
     * next build precompile shaders on first start instead of stuttering mid-race.
     */
    private static void addGraphicsCache(ZipOutputStream zip, Context context) throws IOException {
        File cache = new File(context.getCacheDir(), GraphicsCache.DIRECTORY);
        File[] files = cache.listFiles();
        if (files == null) return;
        for (File file : files) {
            if (!file.isFile() || file.length() > CACHE_LIMIT) continue;
            if (!file.getName().endsWith(".dxvk-cache") && !file.getName().endsWith(".bin")) continue;
            try (InputStream input = new FileInputStream(file)) {
                zip.putNextEntry(new ZipEntry("graphics-cache/" + file.getName()));
                byte[] buffer = new byte[65536];
                for (int n; (n = input.read(buffer)) != -1;) zip.write(buffer, 0, n);
                zip.closeEntry();
            }
        }
    }

    /** The last game lines (runtime, DXVK, crashes) of this app's log. */
    static String logTail(int lines) {
        try { return new String(logcat(lines, "NFSU1:I", "DXVK:W", "AndroidRuntime:E", "DEBUG:F", "libc:F", "*:S"), StandardCharsets.UTF_8); }
        catch (IOException error) { return error.toString(); }
    }

    /** Copies the report to Download/NFSU1 (MediaStore: an app needs no permission for its own files). */
    static Saved saveToDownloads(Context context, File report) throws IOException {
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, report.getName());
        values.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS + "/NFSU1");
        ContentResolver resolver = context.getContentResolver();
        Uri target = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (target == null) throw new IOException(context.getString(R.string.report_err_downloads_create));
        try (InputStream input = new FileInputStream(report);
             OutputStream output = resolver.openOutputStream(target, "w")) {
            if (output == null) throw new IOException(context.getString(R.string.report_err_downloads_write));
            byte[] buffer = new byte[65536];
            for (int n; (n = input.read(buffer)) != -1;) output.write(buffer, 0, n);
        } catch (IOException error) {
            resolver.delete(target, null, null);
            throw error;
        }
        return new Saved(target, "Download/NFSU1/" + report.getName());
    }

    /** The system share sheet for a saved report. */
    static Intent shareIntent(Context context, Uri report) {
        Intent send = new Intent(Intent.ACTION_SEND)
                .setType("application/zip")
                .putExtra(Intent.EXTRA_STREAM, report)
                .putExtra(Intent.EXTRA_SUBJECT, context.getString(R.string.app_name) + " " + ReleaseUpdates.installed(context) + " diagnostics")
                .putExtra(Intent.EXTRA_TEXT, summary(context))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return Intent.createChooser(send, context.getString(R.string.report_share_chooser));
    }

    private static void add(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    private static byte[] readLimited(InputStream input) throws IOException { return readLimited(input, LIMIT); }

    private static byte[] readLimited(InputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        while (output.size() < limit) {
            int count = input.read(buffer, 0, Math.min(buffer.length, limit - output.size()));
            if (count < 0) break;
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    /** This app's main and crash log buffers (all processes of our uid); lines = 0 for the whole buffer. */
    private static byte[] logcat(int lines, String... filters) throws IOException {
        java.lang.Process process = null;
        try {
            List<String> command = new java.util.ArrayList<>(Arrays.asList("logcat", "-d", "-b", "main", "-b", "crash",
                    "-v", "threadtime", "--uid=" + Process.myUid()));
            if (lines > 0) { command.add("-t"); command.add(String.valueOf(lines)); }
            command.addAll(Arrays.asList(filters));
            // Android 11's logcat has no --uid (the report came back empty); an app only reads
            // its own uid's log there anyway.
            if (Build.VERSION.SDK_INT < 31) command.remove("--uid=" + Process.myUid());
            process = new ProcessBuilder(command).redirectErrorStream(true).start();
            java.lang.Process active = process;
            Thread timeout = new Thread(() -> {
                try { if (!active.waitFor(5, TimeUnit.SECONDS)) active.destroyForcibly(); }
                catch (InterruptedException ignored) { active.destroyForcibly(); Thread.currentThread().interrupt(); }
            }, "ReportLogcatTimeout");
            timeout.setDaemon(true);
            timeout.start();
            try (InputStream input = process.getInputStream()) { return readLimited(input); }
        } finally { if (process != null) process.destroy(); }
    }

    private static void addExitInfo(ZipOutputStream zip, ActivityManager manager, Context context) throws IOException {
        StringBuilder info = new StringBuilder();
        try {
            boolean traceAdded = false;
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 5)) {
                info.append(new Date(exit.getTimestamp())).append(' ').append(exit.getProcessName())
                        .append(" reason=").append(exit.getReason()).append(" status=").append(exit.getStatus())
                        .append(" description=").append(exit.getDescription()).append('\n');
                if (!traceAdded && (exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || exit.getReason() == ApplicationExitInfo.REASON_ANR)) {
                    try (InputStream trace = exit.getTraceInputStream()) {
                        if (trace != null) {
                            add(zip, exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE ? "native-crash.pb" : "anr-trace.txt",
                                    readLimited(trace));
                            traceAdded = true;
                        }
                    } catch (IOException error) { info.append("Trace unavailable: ").append(error).append('\n'); }
                }
            }
        } catch (RuntimeException error) { info.append(error); }
        add(zip, "exits.txt", info.toString().getBytes(StandardCharsets.UTF_8));
    }
}
