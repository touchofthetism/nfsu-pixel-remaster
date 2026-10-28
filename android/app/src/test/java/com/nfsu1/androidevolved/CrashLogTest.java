package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.util.Calendar;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import org.junit.Test;

/** Crash-log entries labelled with their build (lines from build 10's diagnostics: Android kept build 2-6 crashes). */
public class CrashLogTest {
    private static final TimeZone DENVER = TimeZone.getTimeZone("America/Denver");
    private static final String LOG = String.join("\n",
            "--------- beginning of crash",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: FATAL EXCEPTION: main",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: Process: com.nfsu1.remastered:game, PID: 8288",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: java.lang.RuntimeException: Unable to destroy activity {com.nfsu1.remastered/com.nfsu1.androidevolved.GameActivity}",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: \tat android.app.ActivityThread.performDestroyActivity(ActivityThread.java:6720)",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: Caused by: java.util.ConcurrentModificationException",
            "10-27 05:41:47.224  8288  8288 E AndroidRuntime: \tat com.nfsu1.androidevolved.GameTelemetry.flush(GameTelemetry.java:275)",
            "10-27 06:46:59.972  2361  2535 F libc    : Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x6ed75bc010 in tid 2535 (remastered:game), pid 2361 (remastered:game)",
            "10-27 06:47:00.663  4994  4994 F DEBUG   : pid: 2361, ppid: 912, tid: 2535, name: remastered:game  >>> com.nfsu1.remastered:game <<<",
            "10-27 06:47:00.663  4994  4994 F DEBUG   :       #00 pc 0000000002cbacac  /data/app/~~a==/com.nfsu1.remastered-b==/lib/arm64/libnfsu1_android.so (BuildId: 1cf1bbeab713d4035227221ba0adfa2ed01fb822)",
            "10-27 06:47:00.663  4994  4994 F DEBUG   :       #01 pc 0000000000bb61fc  /data/app/~~a==/com.nfsu1.remastered-b==/lib/arm64/libnfsu1_android.so (BuildId: 1cf1bbeab713d4035227221ba0adfa2ed01fb822)",
            "--------- beginning of main",
            "10-28 01:23:52.939 30369 30411 E DXVK    : Failed to create surface",
            "10-28 01:30:00.000  9000  9001 F libc    : Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 9001 (remastered:game), pid 9000 (remastered:game)",
            "10-28 01:30:00.500  9100  9100 F DEBUG   :       #00 pc 000000000005a2b4  /apex/com.android.runtime/lib64/bionic/libc.so (abort+164) (BuildId: cb94e32e)",
            "10-28 01:30:00.500  9100  9100 F DEBUG   :       #04 pc 0000000000123456  /data/app/x/lib/arm64/libnfsu1_android.so (BuildId: f230bfd592ea5e11a5bf30df1afc83b44d5e1fc6)");

    private static Calendar at(int month, int day, int hour, int minute) {
        Calendar c = Calendar.getInstance(DENVER, Locale.US);
        c.clear();
        c.set(2026, month - 1, day, hour, minute, 0);
        return c;
    }

    @Test public void readsJavaAndNativeCrashes() {
        List<CrashLog.Entry> entries = CrashLog.parse(LOG, at(10, 28, 1, 45));
        assertEquals(3, entries.size());
        assertFalse(entries.get(0).nativeCrash);
        assertEquals("ConcurrentModificationException", entries.get(0).detail);   // the root cause, not the wrapper
        assertTrue(entries.get(1).nativeCrash);
        assertEquals("SIGSEGV (SEGV_ACCERR)", entries.get(1).detail);
        assertEquals(2361, entries.get(1).pid);
        assertEquals("1cf1bbeab713d4035227221ba0adfa2ed01fb822", entries.get(1).buildId);
        assertEquals("SIGABRT (SI_QUEUE)", entries.get(2).detail);
        assertEquals("our library's frame, not libc's", "f230bfd592ea5e11a5bf30df1afc83b44d5e1fc6", entries.get(2).buildId);
        assertEquals(at(10, 27, 6, 46).getTimeInMillis() + 59_972, entries.get(1).time);
    }

    @Test public void labelsEachCrashWithItsBuild() {
        long installed = at(10, 28, 0, 4).getTimeInMillis();
        List<CrashLog.Entry> entries = CrashLog.parse(LOG, at(10, 28, 1, 45));
        String text = CrashLog.summary(entries, installed, "f230bfd592ea5e11a5bf30df1afc83b44d5e1fc6", DENVER);
        assertTrue(text, text.startsWith("Crash log (Android keeps it across installs): 3 crashes, 1 from this build (installed 2026-10-28 00:04, native build ID f230bfd5):"));
        assertTrue(text, text.contains("  2026-10-27 05:41:47  Java ConcurrentModificationException  older build"));
        assertTrue(text, text.contains("  2026-10-27 06:46:59  native SIGSEGV (SEGV_ACCERR), build ID 1cf1bbea  older build"));
        assertTrue(text, text.contains("  2026-10-28 01:30:00  native SIGABRT (SI_QUEUE), build ID f230bfd5  THIS BUILD"));
        // A build ID decides over the clock: an old build's crash after the install time is still the old build's.
        CrashLog.Entry late = new CrashLog.Entry(installed + 1000, 1, true, "x");
        late.buildId = entries.get(1).buildId;
        assertFalse(CrashLog.fromThisBuild(late, installed, "f230bfd592ea5e11a5bf30df1afc83b44d5e1fc6"));
        // A shortened ID matches its full form.
        assertTrue(CrashLog.fromThisBuild(entries.get(2), 0, "F230BFD5"));
        // Without IDs, the install time decides.
        assertTrue(CrashLog.fromThisBuild(new CrashLog.Entry(installed + 1000, 1, false, ""), installed, ""));
        assertEquals("Crash log: empty (this build installed 2026-10-28 00:04).\n",
                CrashLog.summary(CrashLog.parse("nothing here", at(10, 28, 1, 0)), installed, "", DENVER));
    }

    @Test public void aDateAheadOfNowIsLastYears() {
        List<CrashLog.Entry> entries = CrashLog.parse(
                "12-31 23:59:00.000  1  2 F libc    : Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 2 (x), pid 1 (x)",
                at(1, 2, 10, 0));
        Calendar c = Calendar.getInstance(DENVER, Locale.US);
        c.setTimeInMillis(entries.get(0).time);
        assertEquals(2025, c.get(Calendar.YEAR));
    }
}
