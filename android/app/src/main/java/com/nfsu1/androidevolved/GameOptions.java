package com.nfsu1.androidevolved;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.util.Locale;

/**
 * Launcher options. GameActivity reads them when the game process starts and hands them to the native
 * runtime before the guest boots; changing one takes effect on the next PLAY.
 *
 * Defaults adapt to the device (see {@link #defaultValue}): a phone with plenty of memory renders at the
 * panel's native size and filters textures at 16x; anti-aliasing stays off until the player turns it on,
 * because it changes how the game's back buffer is created and is best confirmed on the actual GPU.
 * Labels live in string resources (English default, Spanish in values-es).
 */
final class GameOptions {
    static final class Option {
        final String key;
        final int titleRes;
        final String[] values;
        final int[] labelRes;
        Option(String key, int titleRes, String[] values, int[] labelRes) {
            if (values.length != labelRes.length) throw new IllegalArgumentException(key);
            this.key = key; this.titleRes = titleRes; this.values = values; this.labelRes = labelRes;
        }
        String title(Context context) { return context.getString(titleRes); }
        String[] labels(Context context) {
            String[] result = new String[labelRes.length];
            for (int i = 0; i < result.length; i++) result[i] = context.getString(labelRes[i]);
            return result;
        }
        String label(Context context, String value) {
            for (int i = 0; i < values.length; i++) if (values[i].equals(value)) return context.getString(labelRes[i]);
            return value;
        }
        boolean allows(String value) {
            for (String allowed : values) if (allowed.equals(value)) return true;
            return false;
        }
    }

    static final String RESOLUTION = "resolution", FPS = "fps", WIDESCREEN = "widescreen", LATENCY = "latency",
            TEXTURES = "textures", MINIMAP = "minimap", LANGUAGE = "language", GAMEPAD = "gamepad",
            ANISOTROPY = "anisotropy", ANTIALIAS = "antialias", OVERLAY = "overlay", GFXLOG = "gfxlog", WRONGWAY = "wrongway", CPU = "cpu",
            GAMEGFX = "gamegfx", READS = "reads";

    static final Option[] ALL = {
            new Option(RESOLUTION, R.string.opt_resolution,
                    new String[] {"100", "75", "50", "720"},
                    new int[] {R.string.res_native, R.string.res_75, R.string.res_50, R.string.res_720}),
            new Option(FPS, R.string.opt_fps,
                    new String[] {"30", "60", "90", "120", "0"},
                    new int[] {R.string.fps_30, R.string.fps_60, R.string.fps_90, R.string.fps_120, R.string.fps_unlimited}),
            new Option(ANISOTROPY, R.string.opt_anisotropy,
                    new String[] {"0", "8", "16"},
                    new int[] {R.string.anisotropy_off, R.string.anisotropy_8, R.string.anisotropy_16}),
            new Option(ANTIALIAS, R.string.opt_antialias,
                    new String[] {"0", "2", "4"},
                    new int[] {R.string.antialias_off, R.string.antialias_2, R.string.antialias_4}),
            // The game's own graphics settings (profile .cfg): its 2003 auto-detect lands the port on a low tier
            // with reflections, light streaks and motion blur off; "max" writes the top tier before every launch.
            new Option(GAMEGFX, R.string.opt_gamegfx,
                    new String[] {"max", "game"},
                    new int[] {R.string.gamegfx_max, R.string.gamegfx_game}),
            new Option(WIDESCREEN, R.string.opt_widescreen,
                    new String[] {"on", "off"},
                    new int[] {R.string.widescreen_on, R.string.widescreen_off}),
            // Free roam (mod pack): the HUD's wrong-way warning and countdown make no sense off the circuit.
            new Option(WRONGWAY, R.string.opt_wrongway,
                    new String[] {"on", "off"},
                    new int[] {R.string.wrongway_on, R.string.wrongway_off}),
            new Option(MINIMAP, R.string.opt_minimap,
                    new String[] {"top", "bottom"},
                    new int[] {R.string.minimap_top, R.string.minimap_bottom}),
            // The game's one busy thread on the fastest core with a performance-hint session (host_perf.h);
            // "system" leaves placement to the scheduler, which parked it on a mid core in build 2.
            new Option(CPU, R.string.opt_cpu,
                    new String[] {"fast", "system"},
                    new int[] {R.string.cpu_fast, R.string.cpu_system}),
            // Reads of 64 KB and more (loader, music/stream blocks) let the other guest threads run while the kernel
            // copies; "atomic" holds the game for every read as builds 1-3 did (guest_files.inc).
            new Option(READS, R.string.opt_reads,
                    new String[] {"overlapped", "atomic"},
                    new int[] {R.string.reads_overlapped, R.string.reads_atomic}),
            new Option(LATENCY, R.string.opt_latency,
                    new String[] {"2", "1"},
                    new int[] {R.string.latency_performance, R.string.latency_low}),
            new Option(TEXTURES, R.string.opt_textures,
                    new String[] {"auto", "cpu"},
                    new int[] {R.string.textures_auto, R.string.textures_cpu}),
            // As keyboard: RT accelerates, LT brakes, A handbrake, B nitrous, the
            // layout players expect. The game's own joystick defaults put
            // accelerate/brake on B/A (issue #45).
            new Option(GAMEPAD, R.string.opt_gamepad,
                    new String[] {"keys", "directinput"},
                    new int[] {R.string.gamepad_keys, R.string.gamepad_directinput}),
            // Performance overlay: a compact line (FPS, p99 frame time, thermal state, battery temperature)
            // top-right, or that plus DXVK's HUD (frame-time graph, GPU load, pipelines) top-left.
            new Option(OVERLAY, R.string.opt_overlay,
                    new String[] {"off", "compact", "full"},
                    new int[] {R.string.overlay_off, R.string.overlay_compact, R.string.overlay_full}),
            // DXVK_LOG_LEVEL for the session log: debug lists every pipeline compile and resource event.
            new Option(GFXLOG, R.string.opt_gfxlog,
                    new String[] {"info", "debug"},
                    new int[] {R.string.gfxlog_info, R.string.gfxlog_debug}),
    };

    private static final String PREFS = "nfsu1_game";
    /** Devices with at least this much RAM get the native-resolution default. */
    static final long HIGH_TIER_MEMORY = 7L * 1024 * 1024 * 1024 + 512L * 1024 * 1024;
    /** {label in its own language, guest language name, LANGUAGES/<file>.bin}. */
    static final String[][] LANGUAGES = {
        {"English", "English", "English"}, {"Español", "Spanish", "Spanish"},
        {"Français", "French", "French"}, {"Deutsch", "German", "German"},
        {"Italiano", "Italian", "Italian"}, {"Nederlands", "Dutch", "Dutch"},
        {"Svenska", "Swedish", "Swedish"}, {"한국어", "Korean", "Korean"},
        {"中文", "Chinese", "Chinese"}
    };

    private GameOptions() {}

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static Option find(String key) {
        for (Option o : ALL) if (o.key.equals(key)) return o;
        throw new IllegalArgumentException(key);
    }

    /** Total RAM of the device; the only device property the defaults depend on. */
    static long totalMemory(Context context) {
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return 0;
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(info);
        return info.totalMem;
    }

    static boolean highTier(Context context) { return totalMemory(context) >= HIGH_TIER_MEMORY; }

    /** The default for an option on a device with the given RAM (pure, for unit tests). */
    static String defaultValue(String key, long totalMemory) {
        boolean high = totalMemory >= HIGH_TIER_MEMORY;
        switch (key) {
            case RESOLUTION: return high ? "100" : "75";
            case FPS: return "120";
            case ANISOTROPY: return "16";
            case ANTIALIAS: return "0";
            case WIDESCREEN: return "on";
            case MINIMAP: return "top";
            case LATENCY: return "2";
            case TEXTURES: return "auto";
            case GAMEPAD: return "keys";
            case OVERLAY: return "off";
            case WRONGWAY: return "on";
            case GAMEGFX: return "max";
            case READS: return "overlapped";
            case CPU: return "fast";
            case GFXLOG: return "info";
            default: throw new IllegalArgumentException(key);
        }
    }

    static String defaultValue(Context context, String key) { return defaultValue(key, totalMemory(context)); }

    static String get(Context context, String key) {
        Option o = find(key);
        String fallback = defaultValue(context, key);
        String value = prefs(context).getString(key, fallback);
        return o.allows(value) ? value : fallback;
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    static int intValue(Context context, String key) { return Integer.parseInt(get(context, key)); }

    /** DXVK_HUD for the full overlay: FPS, frame-time graph, draw calls, pipelines, memory, GPU load, compiler. */
    static final String DXVK_HUD_FULL = "fps,frametimes,drawcalls,pipelines,memory,gpuload,compiler,scale=2.5";

    static final String ONLINE_SERVER = "online_server", DEFAULT_ONLINE_SERVER = "eahub.eu";

    /** EA Nation replacement server: the game's *.ea.com hosts resolve to this name or IPv4 address. */
    static String onlineServer(Context context) {
        String value = prefs(context).getString(ONLINE_SERVER, DEFAULT_ONLINE_SERVER);
        return validServer(value) ? value : DEFAULT_ONLINE_SERVER;
    }

    static boolean validServer(String value) {
        return value != null && value.length() <= 253 && value.matches("[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)*");
    }

    /** The game's language (LANGUAGES/<file>.bin) chosen in the launcher, or the best default for the device. */
    static String language(Context context, File gameRoot) {
        String saved = prefs(context).getString(LANGUAGE, "");
        if (!saved.isEmpty()) return saved;
        return defaultLanguage(Locale.getDefault(), gameRoot);
    }

    /** The device's language when the game ships it, else English, else whatever the copy has (pure, for unit tests). */
    static String defaultLanguage(Locale locale, File gameRoot) {
        String code = locale == null ? "" : locale.getLanguage();
        String wanted = "English";
        switch (code) {
            case "es": wanted = "Spanish"; break;
            case "fr": wanted = "French"; break;
            case "de": wanted = "German"; break;
            case "it": wanted = "Italian"; break;
            case "nl": wanted = "Dutch"; break;
            case "sv": wanted = "Swedish"; break;
            case "ko": wanted = "Korean"; break;
            case "zh": wanted = "Chinese"; break;
            default: break;
        }
        if (gameRoot == null) return wanted;
        if (hasLanguage(gameRoot, wanted)) return wanted;
        if (hasLanguage(gameRoot, "English")) return "English";
        for (String[] language : LANGUAGES) if (hasLanguage(gameRoot, language[2])) return language[1];
        return wanted;
    }

    static boolean hasLanguage(File gameRoot, String file) {
        return new File(gameRoot, "LANGUAGES/" + file + ".bin").isFile() || new File(gameRoot, "Languages/" + file + ".bin").isFile();
    }

    /** Render size for the selected resolution on a display of width × height (landscape). */
    static int[] renderSize(Context context, int width, int height) {
        return renderSize(get(context, RESOLUTION), width, height);
    }

    static int[] renderSize(String value, int width, int height) {
        if (value.equals("720")) return new int[] {1280, 720};
        float scale = Integer.parseInt(value) / 100f;
        return new int[] {Math.round(width * scale / 2f) * 2, Math.round(height * scale / 2f) * 2};
    }
}
