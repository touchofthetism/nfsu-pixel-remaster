package com.nfsu1.androidevolved;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Debug;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.view.Display;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Records one game session in the :game process so a report can show what the phone did while the
 * game ran, without anyone watching logcat:
 *
 *  - {@code files/logs/game-<session>.log}: this process' complete log (runtime, DXVK, crashes), written
 *    by a logcat child that follows the process. Android prunes the shared log buffer within minutes on a
 *    busy phone; the first report's logcat had already lost the whole game run.
 *  - {@code files/telemetry/session-<session>.json}: the frame-time record from the native runtime
 *    (frame_telemetry.h), DXVK pipeline compiles, audio underruns, the D3D9 call profile, plus a device
 *    sample every {@link #SAMPLE_MS}: thermal status and headroom, battery temperature, memory, CPU
 *    clocks, the panel's current refresh rate and this process' CPU use. Written every
 *    {@link #FLUSH_MS} and when the activity pauses or ends, so a crash still leaves the record up to
 *    its last half minute.
 *
 * Nothing leaves the phone by itself: Diagnostics bundles the latest files when the player shares a
 * report. Old sessions are pruned ({@link #KEEP_SESSIONS}).
 */
final class GameTelemetry {
    static final String LOG_DIR = "logs", SESSION_DIR = "telemetry";
    static final int KEEP_SESSIONS = 8;
    static final long SAMPLE_MS = 2000, FLUSH_MS = 30000, OVERLAY_MS = 500;
    private static final String[] THERMAL = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};

    interface NativeJson { String get(boolean live); }

    private final Activity activity;
    private final NativeJson nativeJson;
    private final String session;
    private final File logFile, sessionFile, sessionTemp;
    private final JSONObject record = new JSONObject();
    private final JSONArray samples = new JSONArray(), events = new JSONArray();
    private final HandlerThread thread = new HandlerThread("GameTelemetry", Process.THREAD_PRIORITY_BACKGROUND);
    private Handler handler;
    private java.lang.Process logcat;
    private final long startedAt = SystemClock.elapsedRealtime(), startedWall = System.currentTimeMillis();
    private long lastCpuMs = -1, lastCpuAt = -1, lastFlushAt;
    private boolean cpuFreqAvailable = true, stopped;
    private final int cores = Math.max(1, Runtime.getRuntime().availableProcessors());

    GameTelemetry(Activity activity, Intent launch, NativeJson nativeJson) {
        this.activity = activity;
        this.nativeJson = nativeJson;
        session = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File logs = new File(activity.getFilesDir(), LOG_DIR), sessions = new File(activity.getFilesDir(), SESSION_DIR);
        logs.mkdirs(); sessions.mkdirs();
        logFile = new File(logs, "game-" + session + ".log");
        sessionFile = new File(sessions, "session-" + session + ".json");
        sessionTemp = new File(sessions, "session-" + session + ".json.tmp");
        prune(logs, "game-", ".log", KEEP_SESSIONS);
        prune(sessions, "session-", ".json", KEEP_SESSIONS);
        try {
            record.put("session", session).put("app", ReleaseUpdates.installed(activity)).put("versionCode", BuildConfig.VERSION_CODE)
                    .put("start", startedWall).put("complete", false).put("device", Diagnostics.summary(activity).trim())
                    .put("cores", cores).put("options", extras(launch)).put("display", display());
        } catch (JSONException ignored) { }
    }

    String session() { return session; }

    /** Starts the log capture, the device sampler and the periodic flush. */
    void start() {
        startLogcat();
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(sampler);
        event("start", null);
    }

    /** A lifecycle or status event, timestamped relative to the session start. */
    void event(String name, String detail) {
        if (handler == null) { append(name, detail); return; }
        handler.post(() -> append(name, detail));
    }

    private void append(String name, String detail) {
        try {
            JSONObject event = new JSONObject().put("t", seconds()).put("event", name);
            if (detail != null) event.put("detail", detail);
            synchronized (events) { events.put(event); }
        } catch (JSONException ignored) { }
    }

    /** Writes the record soon on the telemetry thread (pause, background). */
    void flushAsync() { if (handler != null) handler.post(() -> flush(false)); }

    /** Final record (complete=true) and the end of the log capture; safe to call from the UI thread. */
    void stop() {
        synchronized (this) { if (stopped) return; stopped = true; }
        append("stop", null);
        flush(true);
        if (handler != null) handler.removeCallbacksAndMessages(null);
        thread.quitSafely();
        if (logcat != null) { logcat.destroy(); logcat = null; }
    }

    /** The latest sample as a short text for the in-game overlay (null until the first sample). */
    String overlayText(JSONObject live, boolean full) {
        JSONObject sample;
        synchronized (samples) { sample = samples.length() == 0 ? null : samples.optJSONObject(samples.length() - 1); }
        JSONObject frames = live == null ? null : live.optJSONObject("frames");
        JSONObject recent = frames == null ? null : frames.optJSONObject("recent");
        JSONObject pipelines = live == null ? null : live.optJSONObject("pipelines");
        StringBuilder text = new StringBuilder();
        if (recent != null) text.append(String.format(Locale.US, "%.0f FPS  %.1f ms p99", recent.optDouble("fps"), recent.optDouble("p99Ms")));
        if (sample != null) {
            if (text.length() > 0) text.append("  ·  ");
            text.append(THERMAL[Math.min(THERMAL.length - 1, Math.max(0, sample.optInt("thermal")))]);
            if (!Double.isNaN(sample.optDouble("battTemp"))) text.append(String.format(Locale.US, " %.0f°C", sample.optDouble("battTemp")));
        }
        if (!full || frames == null) return text.length() == 0 ? null : text.toString();
        text.append(String.format(Locale.US, "\n1%% low %.0f  avg %.0f  hitch %d  stall %d",
                frames.optDouble("onePercentLowFps"), frames.optDouble("avgFps"), frames.optLong("hitches"), frames.optLong("stalls")));
        if (pipelines != null) text.append(String.format(Locale.US, "\npipelines %d  worst %.0f ms  >10ms %d",
                pipelines.optLong("graphics") + pipelines.optLong("compute"), pipelines.optDouble("maxMs"), pipelines.optLong("over10Ms")));
        if (sample != null) {
            text.append('\n');
            if (sample.has("headroom")) text.append(String.format(Locale.US, "headroom %.2f  ", sample.optDouble("headroom")));
            text.append(String.format(Locale.US, "pss %d MB  cpu %d%%  %s Hz", sample.optInt("pssMb"), sample.optInt("cpuPct"),
                    sample.opt("refresh") == null ? "?" : String.valueOf(Math.round(sample.optDouble("refresh")))));
            JSONArray freq = sample.optJSONArray("cpuMhz");
            if (freq != null && freq.length() > 0) {
                int min = Integer.MAX_VALUE, max = 0;
                for (int i = 0; i < freq.length(); i++) { min = Math.min(min, freq.optInt(i)); max = Math.max(max, freq.optInt(i)); }
                text.append(String.format(Locale.US, "  clocks %d-%d MHz", min, max));
            }
        }
        return text.toString();
    }

    // ---- Sampling --------------------------------------------------------------------------------------

    private final Runnable sampler = new Runnable() {
        @Override public void run() {
            try { JSONObject sample = sample(); synchronized (samples) { samples.put(sample); } }
            catch (Exception ignored) { }
            long now = SystemClock.elapsedRealtime();
            if (now - lastFlushAt >= FLUSH_MS) flush(false);
            if (!stopped && handler != null) handler.postDelayed(this, SAMPLE_MS);
        }
    };

    private double seconds() { return (SystemClock.elapsedRealtime() - startedAt) / 1000.0; }

    JSONObject sample() throws JSONException {
        JSONObject sample = new JSONObject().put("t", Math.round(seconds() * 10) / 10.0);
        PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
        if (power != null) {
            sample.put("thermal", power.getCurrentThermalStatus());
            if (Build.VERSION.SDK_INT >= 30) {
                float headroom = power.getThermalHeadroom(0);
                if (!Float.isNaN(headroom)) sample.put("headroom", Math.round(headroom * 100) / 100.0);
            }
        }
        Intent battery = activity.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            int tenths = battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            if (tenths != Integer.MIN_VALUE) sample.put("battTemp", tenths / 10.0);
            int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (level >= 0 && scale > 0) sample.put("battLevel", level * 100 / scale);
            sample.put("charging", battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0);
        }
        Debug.MemoryInfo memory = new Debug.MemoryInfo();
        Debug.getMemoryInfo(memory);
        sample.put("pssMb", memory.getTotalPss() >> 10);
        ActivityManager manager = (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager != null) {
            ActivityManager.MemoryInfo system = new ActivityManager.MemoryInfo();
            manager.getMemoryInfo(system);
            sample.put("availMb", system.availMem >> 20).put("lowMemory", system.lowMemory);
        }
        long cpuMs = Process.getElapsedCpuTime(), at = SystemClock.elapsedRealtime();
        if (lastCpuMs >= 0 && at > lastCpuAt)
            sample.put("cpuPct", (int) Math.min(100 * cores, (cpuMs - lastCpuMs) * 100 / (at - lastCpuAt)));
        lastCpuMs = cpuMs; lastCpuAt = at;
        if (cpuFreqAvailable) {
            JSONArray mhz = new JSONArray();
            for (int cpu = 0; cpu < cores; cpu++) {
                Integer khz = readInt(new File("/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/scaling_cur_freq"));
                if (khz == null) { cpuFreqAvailable = false; break; }
                mhz.put(khz / 1000);
            }
            if (cpuFreqAvailable) sample.put("cpuMhz", mhz);
        }
        Display display = Build.VERSION.SDK_INT >= 30 ? activity.getDisplay() : activity.getWindowManager().getDefaultDisplay();
        if (display != null) sample.put("refresh", Math.round(display.getRefreshRate() * 10) / 10.0);
        return sample;
    }

    private static Integer readInt(File file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line = reader.readLine();
            return line == null ? null : Integer.parseInt(line.trim());
        } catch (IOException | NumberFormatException | SecurityException error) { return null; }
    }

    private JSONObject display() throws JSONException {
        JSONObject info = new JSONObject();
        Display display = Build.VERSION.SDK_INT >= 30 ? activity.getDisplay() : activity.getWindowManager().getDefaultDisplay();
        if (display == null) return info;
        Display.Mode current = display.getMode();
        info.put("mode", current.getPhysicalWidth() + "x" + current.getPhysicalHeight() + "@" + Math.round(current.getRefreshRate() * 10) / 10.0);
        JSONArray modes = new JSONArray();
        for (Display.Mode mode : display.getSupportedModes())
            modes.put(mode.getPhysicalWidth() + "x" + mode.getPhysicalHeight() + "@" + Math.round(mode.getRefreshRate() * 10) / 10.0);
        info.put("modes", modes).put("preferredRefresh", activity.getWindow().getAttributes().preferredRefreshRate);
        return info;
    }

    private static JSONObject extras(Intent launch) throws JSONException {
        JSONObject options = new JSONObject();
        if (launch == null || launch.getExtras() == null) return options;
        for (String key : launch.getExtras().keySet()) {
            Object value = launch.getExtras().get(key);
            if (value instanceof String[]) options.put(key, Arrays.toString((String[]) value));
            else if (value != null) options.put(key, value instanceof Number || value instanceof Boolean ? value : String.valueOf(value));
        }
        return options;
    }

    // ---- Persistence -----------------------------------------------------------------------------------

    /** Writes the record atomically (temp + rename). Synchronous; a few milliseconds. */
    void flush(boolean complete) {
        lastFlushAt = SystemClock.elapsedRealtime();
        String nativeText = null;
        try { nativeText = nativeJson.get(false); } catch (RuntimeException ignored) { }
        try {
            synchronized (record) {
                record.put("end", System.currentTimeMillis()).put("durationS", Math.round(seconds()));
                if (complete) record.put("complete", true);
                if (nativeText != null) {
                    try { record.put("native", new JSONObject(nativeText)); }
                    catch (JSONException malformed) { record.put("nativeRaw", nativeText); }
                }
                synchronized (samples) { record.put("samples", samples); }
                synchronized (events) { record.put("events", events); }
                try (Writer writer = new OutputStreamWriter(new FileOutputStream(sessionTemp), StandardCharsets.UTF_8)) {
                    writer.write(record.toString());
                }
                if (!sessionTemp.renameTo(sessionFile)) throw new IOException("rename");
            }
        } catch (IOException | JSONException error) {
            android.util.Log.w("NFSU1", "Session record not written: " + error);
        }
    }

    /**
     * logcat follows this process into a file (same uid, so no permission). It is a child process: when
     * the game process dies, whatever way, Android ends its process group, so no orphan stays behind.
     */
    private void startLogcat() {
        try {
            List<String> command = new ArrayList<>(Arrays.asList("logcat", "-v", "threadtime", "-b", "main,crash",
                    "--pid=" + Process.myPid(), "-f", logFile.getAbsolutePath()));
            logcat = new ProcessBuilder(command).redirectErrorStream(true).start();
            android.util.Log.i("NFSU1", "Session " + session + ": log to " + logFile.getName());
        } catch (IOException | RuntimeException error) {
            android.util.Log.w("NFSU1", "Log capture unavailable: " + error);
            logcat = null;
        }
    }

    /** Keeps the newest {@code keep} files of a kind (and this session's). */
    static void prune(File directory, String prefix, String suffix, int keep) {
        File[] files = directory.listFiles((dir, name) -> name.startsWith(prefix) && name.endsWith(suffix));
        if (files == null || files.length < keep) return;
        Arrays.sort(files, Comparator.comparing(File::getName).reversed());
        for (int i = keep - 1; i < files.length; i++) files[i].delete();
    }

    /** Session files, newest first (for Diagnostics and the launcher's last-session card). */
    static List<File> sessionFiles(Context context) {
        File[] files = new File(context.getFilesDir(), SESSION_DIR).listFiles((dir, name) -> name.startsWith("session-") && name.endsWith(".json"));
        List<File> list = files == null ? new ArrayList<>() : new ArrayList<>(Arrays.asList(files));
        list.sort(Comparator.comparing(File::getName).reversed());
        return list;
    }

    static List<File> logFiles(Context context) {
        File[] files = new File(context.getFilesDir(), LOG_DIR).listFiles((dir, name) -> name.endsWith(".log") || name.endsWith(".txt"));
        List<File> list = files == null ? new ArrayList<>() : new ArrayList<>(Arrays.asList(files));
        list.sort(Comparator.comparing(File::getName).reversed());
        return list;
    }
}
