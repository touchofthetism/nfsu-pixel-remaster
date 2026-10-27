package com.nfsu1.androidevolved;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

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

    /** Every profile's .cfg under the game folder, raised to the maximum; returns the files that changed. */
    static List<File> applyMaximum(File root) throws IOException {
        List<File> changed = new ArrayList<>();
        File folder = SaveBackup.saveFolder(root);
        File[] files = folder == null ? null : folder.listFiles();
        if (files == null) return changed;
        for (File file : files) {
            if (!file.isFile() || !file.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".cfg")) continue;
            if (file.length() > 64 * 1024) continue;                           // not a settings file
            byte[] bytes = new byte[(int) file.length()];
            try (FileInputStream in = new FileInputStream(file)) {
                int read = 0;
                while (read < bytes.length) {
                    int n = in.read(bytes, read, bytes.length - read);
                    if (n < 0) break;
                    read += n;
                }
            }
            String text = new String(bytes, StandardCharsets.ISO_8859_1);
            if (!text.contains("ENVMAP") && !text.contains("ROADREFLECTION")) continue;   // some other .cfg
            String updated = applyMaximum(text);
            if (updated.equals(text)) continue;
            File temp = new File(file.getParentFile(), file.getName() + ".tmp");
            try (FileOutputStream outStream = new FileOutputStream(temp)) {
                outStream.write(updated.getBytes(StandardCharsets.ISO_8859_1));
            }
            if (!temp.renameTo(file)) {
                temp.delete();
                throw new IOException("could not replace " + file.getName());
            }
            changed.add(file);
        }
        return changed;
    }
}
