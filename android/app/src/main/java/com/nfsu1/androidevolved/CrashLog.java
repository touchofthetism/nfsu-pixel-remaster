package com.nfsu1.androidevolved;

import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The crashes in this app's log, each labelled with the build it came from. Android keeps its crash buffer
 * across installs, so a diagnostics report carries crashes from builds long replaced: an outside analysis of
 * build 10 read build 2's ConcurrentModificationException and build 3 and 6's SIGSEGVs as build 10 being
 * unstable. A native crash's backtrace names libnfsu1_android.so's build ID, which settles it; any crash
 * older than the current install is from an older build either way.
 */
final class CrashLog {
    static final class Entry {
        final long time;          // when, from the log line (local time)
        final int pid;
        final boolean nativeCrash;
        String detail;            // the exception class, or the signal and code
        String buildId = "";      // libnfsu1_android.so's build ID from the backtrace (native crashes)

        Entry(long time, int pid, boolean nativeCrash, String detail) {
            this.time = time; this.pid = pid; this.nativeCrash = nativeCrash; this.detail = detail;
        }
    }

    // logcat -v threadtime: "10-27 06:46:59.972  2361  2535 F libc    : message"
    private static final Pattern LINE = Pattern.compile("^(\\d\\d)-(\\d\\d) (\\d\\d):(\\d\\d):(\\d\\d)\\.(\\d{3})\\s+(\\d+)\\s+\\d+\\s+\\w\\s+(\\S+)\\s*: ?(.*)$");
    private static final Pattern SIGNAL = Pattern.compile("^Fatal signal \\d+ \\((\\w+)\\), code -?\\d+ \\((\\w+)\\).*?pid (\\d+)");
    private static final Pattern BUILD_ID = Pattern.compile("libnfsu1_android\\.so.*\\(BuildId: ([0-9a-f]{8,})\\)");
    private static final Pattern CAUSE = Pattern.compile("^(?:Caused by: )?([a-zA-Z_$][\\w$]*(?:\\.[a-zA-Z_$][\\w$]*)+(?:Exception|Error))(?::.*)?$");

    private CrashLog() {}

    /** The crashes in a logcat dump; `now` fixes the year logcat leaves out (a date ahead of it is last year's). */
    static List<Entry> parse(String log, Calendar now) {
        List<Entry> entries = new ArrayList<>();
        Entry java = null;   // the Java crash being read (its exception lines follow the FATAL EXCEPTION line)
        boolean javaHasCause = false;
        for (String line : log.split("\n")) {
            Matcher m = LINE.matcher(line);
            if (!m.matches()) continue;
            String tag = m.group(8), message = m.group(9).trim();
            int pid = Integer.parseInt(m.group(7));
            if (tag.equals("AndroidRuntime")) {
                if (message.startsWith("FATAL EXCEPTION")) {
                    java = new Entry(time(m, now), pid, false, "");
                    javaHasCause = false;
                    entries.add(java);
                } else if (java != null && pid == java.pid) {
                    Matcher cause = CAUSE.matcher(message);
                    // The last "Caused by" is the root cause; without one, the first exception line.
                    if (cause.matches() && (message.startsWith("Caused by: ") || !javaHasCause && java.detail.isEmpty())) {
                        java.detail = simpleName(cause.group(1));
                        javaHasCause |= message.startsWith("Caused by: ");
                    }
                }
            } else if (tag.equals("libc")) {
                Matcher signal = SIGNAL.matcher(message);
                if (signal.find())
                    entries.add(new Entry(time(m, now), Integer.parseInt(signal.group(3)), true, signal.group(1) + " (" + signal.group(2) + ")"));
            } else if (tag.equals("DEBUG")) {
                Matcher id = BUILD_ID.matcher(message);
                if (id.find()) {
                    // The tombstone follows its "Fatal signal" line: the newest native entry without an ID yet.
                    for (int i = entries.size() - 1; i >= 0; i--) {
                        Entry e = entries.get(i);
                        if (!e.nativeCrash) continue;
                        if (e.buildId.isEmpty()) e.buildId = id.group(1);
                        break;
                    }
                }
            }
        }
        return entries;
    }

    /** True when the crash is from the build installed now: its build ID decides, else the install time. */
    static boolean fromThisBuild(Entry entry, long installedAt, String currentBuildId) {
        if (entry.buildId.length() >= 8 && currentBuildId != null && currentBuildId.length() >= 8) {
            String a = entry.buildId.toLowerCase(Locale.ROOT), b = currentBuildId.toLowerCase(Locale.ROOT);
            return a.startsWith(b) || b.startsWith(a);   // either may be shortened
        }
        return entry.time >= installedAt;
    }

    /** The report.txt section: every crash with the build it belongs to (times shown in `zone`). */
    static String summary(List<Entry> entries, long installedAt, String currentBuildId, TimeZone zone) {
        Calendar installed = Calendar.getInstance(zone, Locale.US);
        installed.setTimeInMillis(installedAt);
        String since = String.format(Locale.US, "installed %tF %<tR", installed);
        if (entries.isEmpty()) return "Crash log: empty (this build " + since + ").\n";
        int current = 0;
        for (Entry entry : entries) if (fromThisBuild(entry, installedAt, currentBuildId)) current++;
        StringBuilder text = new StringBuilder(String.format(Locale.US,
                "Crash log (Android keeps it across installs): %d crash%s, %s from this build (%s%s):\n",
                entries.size(), entries.size() == 1 ? "" : "es", current == 0 ? "none" : String.valueOf(current), since,
                currentBuildId == null || currentBuildId.isEmpty() ? "" : ", native build ID " + shortId(currentBuildId)));
        Calendar at = Calendar.getInstance(zone, Locale.US);
        for (Entry entry : entries) {
            at.setTimeInMillis(entry.time);
            text.append(String.format(Locale.US, "  %tF %<tT  %s %s%s  %s\n", at,
                    entry.nativeCrash ? "native" : "Java", entry.detail.isEmpty() ? "crash" : entry.detail,
                    entry.buildId.isEmpty() ? "" : ", build ID " + shortId(entry.buildId),
                    fromThisBuild(entry, installedAt, currentBuildId) ? "THIS BUILD" : "older build"));
        }
        return text.toString();
    }

    private static String shortId(String id) { return id.length() > 8 ? id.substring(0, 8) : id; }

    private static String simpleName(String className) {
        int dot = className.lastIndexOf('.');
        return dot < 0 ? className : className.substring(dot + 1);
    }

    private static long time(Matcher m, Calendar now) {
        Calendar at = (Calendar) now.clone();
        at.set(Calendar.MONTH, Integer.parseInt(m.group(1)) - 1);
        at.set(Calendar.DAY_OF_MONTH, Integer.parseInt(m.group(2)));
        at.set(Calendar.HOUR_OF_DAY, Integer.parseInt(m.group(3)));
        at.set(Calendar.MINUTE, Integer.parseInt(m.group(4)));
        at.set(Calendar.SECOND, Integer.parseInt(m.group(5)));
        at.set(Calendar.MILLISECOND, Integer.parseInt(m.group(6)));
        if (at.getTimeInMillis() > now.getTimeInMillis() + 86_400_000L) at.add(Calendar.YEAR, -1);
        return at.getTimeInMillis();
    }
}
