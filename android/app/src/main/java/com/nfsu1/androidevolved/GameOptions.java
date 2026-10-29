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
            GAMEGFX = "gamegfx", READS = "reads", REFLECTIONS = "reflections", CLEANUP = "cleanup", EFFECTS = "effects",
            FREEROAM = "freeroam";

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
            // with reflections, light streaks and motion blur off. "max" raises a profile to the top tier the first
            // time the launcher sees it; what the player then changes in Display Settings stays (GameConfig).
            new Option(GAMEGFX, R.string.opt_gamegfx,
                    new String[] {"max", "game"},
                    new int[] {R.string.gamegfx_max, R.string.gamegfx_game}),
            // Reflection targets (reflections.c): the game draws the wet-road, car and mirror reflections at 320x240,
            // 128 and 256x64 and stretches them; "sharp" sizes them from the render height (996x448, 256, 896x224).
            new Option(REFLECTIONS, R.string.opt_reflections,
                    new String[] {"sharp", "original"},
                    new int[] {R.string.reflections_sharp, R.string.reflections_original}),
            new Option(WIDESCREEN, R.string.opt_widescreen,
                    new String[] {"on", "off"},
                    new int[] {R.string.widescreen_on, R.string.widescreen_off}),
            // Free roam (build 19, free_roam.h and freeroam.c): Olympic City and the two drift districts as Free Run
            // tracks, made from the Free Roam Mod Beta 2.0's track bundles the APK carries and the player's own game
            // files, with a map of the district on the minimap, no wrong way and resets to where the car was driving.
            new Option(FREEROAM, R.string.opt_freeroam,
                    new String[] {"on", "off"},
                    new int[] {R.string.freeroam_on, R.string.freeroam_off}),
            // The HUD's wrong-way warning and countdown everywhere (the free roam tracks never warn, whatever this says).
            new Option(WRONGWAY, R.string.opt_wrongway,
                    new String[] {"on", "off"},
                    new int[] {R.string.wrongway_on, R.string.wrongway_off}),
            new Option(MINIMAP, R.string.opt_minimap,
                    new String[] {"top", "bottom"},
                    new int[] {R.string.minimap_top, R.string.minimap_bottom}),
            // The game's one busy thread on the fastest core with a performance-hint session (host_perf.h). "fast"
            // follows the fastest cluster as the phone caps its clocks (build 12, core_choice.h: the X4 spent 60-84 %
            // of long sessions at 0.9-1.4 GHz while the A725s averaged 2.1-2.5); "big" keeps it on the biggest core
            // whatever its clock (builds 3-11); "system" leaves placement to the scheduler, which parked it on a
            // mid core in build 2.
            new Option(CPU, R.string.opt_cpu,
                    new String[] {"fast", "big", "system"},
                    new int[] {R.string.cpu_fast, R.string.cpu_big, R.string.cpu_system}),
            // Reads of 64 KB and more (loader, music/stream blocks) let the other guest threads run while the kernel
            // copies; "atomic" holds the game for every read as builds 1-3 did (guest_files.inc).
            new Option(READS, R.string.opt_reads,
                    new String[] {"overlapped", "atomic"},
                    new int[] {R.string.reads_overlapped, R.string.reads_atomic}),
            // DXVK resets each command list the GPU has finished. Done by its queue thread, that work held back
            // the signal the next frame waits for: 85 % of build 10's 20-224 ms freezes. "thread" moves it to a
            // thread of its own (ci/dxvk_extra.py); "queue" keeps DXVK's order, for comparison.
            new Option(CLEANUP, R.string.opt_cleanup,
                    new String[] {"thread", "queue"},
                    new int[] {R.string.cleanup_thread, R.string.cleanup_queue}),
            // The effects' render, sampler and texture stage states (build 17, effect_states.h): the host applies
            // them and skips the ones already set, instead of the game's D3DX code re-applying all of them on every
            // draw (43 % of the render thread's game time in build 16). "game" runs the game's own code, to compare.
            new Option(EFFECTS, R.string.opt_effects,
                    new String[] {"native", "game"},
                    new int[] {R.string.effects_native, R.string.effects_game}),
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
            // 60, the game's own pace: races are CPU-bound, and at 120 the big core runs flat out until the phone
            // warms and its clock is capped (build 8: 92 FPS falling to 45 within three minutes). A 60 cap lets
            // the pacer sleep and the governor run lower clocks, so the phone heats later.
            case FPS: return "60";
            case ANISOTROPY: return "16";
            case ANTIALIAS: return "0";
            case WIDESCREEN: return "on";
            case MINIMAP: return "top";
            case LATENCY: return "2";
            case TEXTURES: return "auto";
            case GAMEPAD: return "keys";
            case OVERLAY: return "off";
            case WRONGWAY: return "on";
            case FREEROAM: return "on";
            case GAMEGFX: return "max";
            case READS: return "overlapped";
            case REFLECTIONS: return "sharp";
            case CPU: return "fast";
            case CLEANUP: return "thread";
            case EFFECTS: return "native";
            case GFXLOG: return "info";
            default: throw new IllegalArgumentException(key);
        }
    }

    static String defaultValue(Context context, String key) { return defaultValue(key, totalMemory(context)); }

    /** The CPU placement option as native host_perf::Placement: 0 the system scheduler, 1 the fastest cluster as
     *  the phone caps clocks ("fast", the default, and anything unknown), 2 the biggest core whatever its clock. */
    static int cpuPlacement(String value) {
        return "system".equals(value) ? 0 : "big".equals(value) ? 2 : 1;
    }

    /**
     * The frame cap in force for the cap the player chose (0 = unlimited). While Battery Saver is on it holds
     * the panel at 60 Hz, and a higher cap only renders frames the screen never shows (build 9: 88 FPS on a
     * 60 Hz panel, a third of the work thrown away as heat, the rest unevenly spaced): the cap then follows
     * the panel's refresh rate until Battery Saver is off. Unlimited stays the player's explicit choice.
     */
    static int effectiveFrameLimit(int chosen, boolean powerSave, float refreshHz) {
        if (chosen <= 0 || !powerSave || !(refreshHz >= 30f)) return chosen;
        return Math.min(chosen, Math.round(refreshHz));
    }

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

    /**
     * Garage: customization freedoms, each a bit of the native NFS_GARAGE_* mask (src/runtime/nfs_garage.h, same
     * order). All on by default. None of them writes the save: Unlock everything is the game's own switch, set
     * in memory for the session; the parts installed with it are saved like any other. A rim size the game lacks
     * (allRimSizes) is saved by its part hash like the others; the game keeps getting it while a save wears one,
     * with the switch off too.
     *
     * The preference lists the features turned off, not those on, so a feature a later build adds starts on for
     * everyone, as all of them do on a first launch, and a feature turned off stays off.
     */
    static final String GARAGE_OFF = "garage_off";
    static final String[] GARAGE_KEYS = {
            "unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes", "morePaints",
            "allRimSizes"};
    static final int[] GARAGE_TITLES = {
            R.string.garage_unlock, R.string.garage_special_vinyls, R.string.garage_vinyl_layers, R.string.garage_hood_vinyls,
            R.string.garage_hood_decals, R.string.garage_all_decals, R.string.garage_race_brakes, R.string.garage_more_paints,
            R.string.garage_all_rim_sizes};
    static final int GARAGE_ALL = (1 << GARAGE_KEYS.length) - 1;

    /** The enabled Garage features as the native mask. */
    static int garage(Context context) { return garageMask(prefs(context).getString(GARAGE_OFF, null)); }

    static void setGarage(Context context, int mask) { prefs(context).edit().putString(GARAGE_OFF, garageOffValue(mask)).apply(); }

    /** The stored value (comma-separated keys of the features off) as the mask of those on; null (never chosen)
     *  or empty is all of them, and a key no build knows is ignored. */
    static int garageMask(String off) {
        int mask = GARAGE_ALL;
        if (off == null) return mask;
        for (String key : off.split(","))
            for (int i = 0; i < GARAGE_KEYS.length; i++) if (GARAGE_KEYS[i].equals(key.trim())) mask &= ~(1 << i);
        return mask;
    }

    /** The value stored for a mask: the keys of the features it leaves off. */
    static String garageOffValue(int mask) {
        StringBuilder value = new StringBuilder();
        for (int i = 0; i < GARAGE_KEYS.length; i++) {
            if ((mask & (1 << i)) != 0) continue;
            if (value.length() > 0) value.append(',');
            value.append(GARAGE_KEYS[i]);
        }
        return value.toString();
    }

    static int garageCount(int mask) { return Integer.bitCount(mask & GARAGE_ALL); }

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

    /**
     * The size the game renders at (its back buffer; the native runtime hands it to the game's video mode, which
     * on its own stops at 1280x1024): a share of the panel, or 720 lines at the panel's shape.
     */
    static int[] renderSize(String value, int width, int height) {
        if (value.equals("720")) return new int[] {Math.round(720f * width / height / 2f) * 2, 720};
        float scale = Integer.parseInt(value) / 100f;
        return new int[] {Math.round(width * scale / 2f) * 2, Math.round(height * scale / 2f) * 2};
    }
}
