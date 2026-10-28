package com.nfsu1.androidevolved;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * The game's own graphics settings, kept per profile in {@code NFS Underground/<profile>.cfg} as
 * {@code KEY: value} lines (CR LF). On a profile's first run the 2003 auto-detect scores the "hardware"
 * it sees through the port and lands on performance tier 2, which switches off motion blur and light
 * streaks and leaves the rest at whatever the tier allows; a tier-0 result (the first profiles made here)
 * turns off car and world reflections, the wet-road reflection, fog, headlights and depth of field too.
 * The GPU on the phone is idle, so the port can hand every profile the game's top tier: these are the
 * values Speed.exe initialises its settings with before the auto-detect lowers them, and the per-setting
 * maxima the game clamps a loaded .cfg against (tier 3: ENVMAP 3, SHADOW 2, ROADREFLECTION 2, WorldLOD 3,
 * CarLOD 1, every effect on). Resolution, colour depth and sound lines are left alone.
 *
 * The top tier is a starting point, not a rule: Light Trails, Light Glow and Motion Blur change the look
 * and players choose them in the game's Display Settings. So the launcher raises a profile only the first
 * time it sees it (a new profile carries the auto-detect's tier) and remembers the graphics values it left;
 * when a later launch finds different values, the player changed them in the game and they stay.
 */
final class GameConfig {
    /** Tier-3 values in the order the game writes them; the file's own order is kept when rewriting. */
    static final String[][] MAXIMUM = {
            {"PERFORMANCE", "3"}, {"ENVMAP", "3"}, {"WORLDENVMAP", "1"}, {"SHADOW", "2"}, {"HEADLIGHTS", "1"},
            {"CARLIGHTING", "1"}, {"DAMAGE", "1"}, {"CROWD", "1"}, {"ROADREFLECTION", "2"}, {"FOG", "1"},
            {"MOTIONBLUR", "1"}, {"LIGHTSTREAKS", "1"}, {"LIGHTGLOW", "1"}, {"ANIMATEDTEXTURE", "1"},
            {"PARTICLESYSTEMS", "1"}, {"DEPTHOFFIELD", "1"}, {"WorldLOD", "3"}, {"CarLOD", "1"}};

    private GameConfig() {}

    /** The .cfg text with the graphics settings raised to the maximum; byte-identical when already there. */
    static String applyMaximum(String text) {
        Map<String, String> wanted = new LinkedHashMap<>();
        for (String[] pair : MAXIMUM) wanted.put(pair[0], pair[1]);
        StringBuilder out = new StringBuilder(text.length() + 64);
        int lastKeyLineEnd = -1;      // position in `out` just after the last KEY: value line (and its line end)
        int pos = 0;
        while (pos < text.length()) {
            int eol = text.indexOf('\n', pos);
            String line = eol < 0 ? text.substring(pos) : text.substring(pos, eol + 1);
            pos = eol < 0 ? text.length() : eol + 1;
            String body = line.endsWith("\r\n") ? line.substring(0, line.length() - 2)
                    : line.endsWith("\n") ? line.substring(0, line.length() - 1) : line;
            String ending = line.substring(body.length());
            int colon = body.indexOf(':');
            if (colon > 0) {
                String key = body.substring(0, colon).trim();
                String value = wanted.remove(key);
                if (value != null) body = key + ": " + value;
                out.append(body).append(ending);
                lastKeyLineEnd = out.length();
            } else {
                out.append(line);
            }
        }
        if (!wanted.isEmpty()) {
            // A key the file lacks goes after the last setting, in the file's own line ending.
            String ending = text.contains("\r\n") ? "\r\n" : "\n";
            StringBuilder added = new StringBuilder();
            for (Map.Entry<String, String> entry : wanted.entrySet()) added.append(entry.getKey()).append(": ").append(entry.getValue()).append(ending);
            if (lastKeyLineEnd < 0) out.append(added);
            else {
                if (lastKeyLineEnd == out.length() && out.charAt(out.length() - 1) != '\n') {
                    out.append(ending);                   // the last setting had no line end: give it one
                    lastKeyLineEnd = out.length();
                }
                out.insert(lastKeyLineEnd, added);
            }
        }
        return out.toString();
    }

    /**
     * The graphics settings of a .cfg text in {@link #MAXIMUM} order as {@code KEY=value;...}: what the
     * player can change in Display Settings. Lines the game also keeps there (resolution, colour depth,
     * sound) are not part of it, so saving those never reads as a graphics change.
     */
    static String graphicsFingerprint(String text) {
        Map<String, String> found = new java.util.HashMap<>();
        for (String line : text.split("\n", -1)) {
            int colon = line.indexOf(':');
            if (colon <= 0) continue;
            found.put(line.substring(0, colon).trim(), line.substring(colon + 1).trim());
        }
        StringBuilder out = new StringBuilder();
        for (String[] pair : MAXIMUM) {
            String value = found.get(pair[0]);
            if (value == null) continue;
            if (out.length() > 0) out.append(';');
            out.append(pair[0]).append('=').append(value);
        }
        return out.toString();
    }

    /** What {@link #applyMaximum(File, Map)} did, and the values to compare against at the next launch. */
    static final class Outcome {
        /** Profiles seen for the first time and written at the top tier. */
        final List<File> raised = new ArrayList<>();
        /** Profiles whose graphics changed in the game since the last launch: left as the player set them. */
        final List<File> kept = new ArrayList<>();
        /** Lower-case file name -> {@link #graphicsFingerprint} now on disk. Profiles that are gone drop out. */
        final Map<String, String> seen = new TreeMap<>();
    }

    /**
     * "Game graphics: Maximum". A profile the launcher has not seen yet is raised to the top tier; one it has
     * seen keeps whatever values it has now, so changes made in the game's Display Settings stick.
     *
     * @param seen lower-case file name -> fingerprint recorded by the previous launch (empty: raise every profile)
     */
    static Outcome applyMaximum(File root, Map<String, String> seen) throws IOException {
        Outcome outcome = new Outcome();
        for (File file : profileConfigs(root)) {
            String text = read(file);
            String key = file.getName().toLowerCase(Locale.ROOT);
            String before = seen.get(key);
            if (before != null) {
                String now = graphicsFingerprint(text);
                if (!before.equals(now)) outcome.kept.add(file);
                outcome.seen.put(key, now);
                continue;
            }
            String updated = applyMaximum(text);
            if (!updated.equals(text)) {
                write(file, updated);
                outcome.raised.add(file);
            }
            outcome.seen.put(key, graphicsFingerprint(updated));
        }
        return outcome;
    }

    /** The fingerprint of every profile as it is on disk, without changing anything (after a save restore). */
    static Map<String, String> observe(File root) throws IOException {
        Map<String, String> seen = new TreeMap<>();
        for (File file : profileConfigs(root)) seen.put(file.getName().toLowerCase(Locale.ROOT), graphicsFingerprint(read(file)));
        return seen;
    }

    /** Every profile's graphics .cfg under the game folder (other .cfg files and oversized ones are skipped). */
    static List<File> profileConfigs(File root) throws IOException {
        List<File> configs = new ArrayList<>();
        File folder = SaveBackup.saveFolder(root);
        File[] files = folder == null ? null : folder.listFiles();
        if (files == null) return configs;
        java.util.Arrays.sort(files);
        for (File file : files) {
            if (!file.isFile() || !file.getName().toLowerCase(Locale.ROOT).endsWith(".cfg")) continue;
            if (file.length() > 64 * 1024) continue;                           // not a settings file
            String text = read(file);
            if (!text.contains("ENVMAP") && !text.contains("ROADREFLECTION")) continue;   // some other .cfg
            configs.add(file);
        }
        return configs;
    }

    private static String read(File file) throws IOException {
        byte[] bytes = new byte[(int) file.length()];
        try (FileInputStream in = new FileInputStream(file)) {
            int read = 0;
            while (read < bytes.length) {
                int n = in.read(bytes, read, bytes.length - read);
                if (n < 0) break;
                read += n;
            }
            return new String(bytes, 0, read, StandardCharsets.ISO_8859_1);
        }
    }

    private static void write(File file, String text) throws IOException {
        File temp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (FileOutputStream outStream = new FileOutputStream(temp)) {
            outStream.write(text.getBytes(StandardCharsets.ISO_8859_1));
        }
        if (!temp.renameTo(file)) {
            temp.delete();
            throw new IOException("could not replace " + file.getName());
        }
    }
}
