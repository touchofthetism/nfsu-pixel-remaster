package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** A session record (GameTelemetry) summarised for the launcher card and session-summary.txt. */
public class SessionSummaryTest {
    static final String RECORD = "{\"session\":\"20261027-030000\",\"app\":\"1.0.0-build.2\",\"versionCode\":1002,\"start\":1,\"end\":2,"
            + "\"durationS\":754,\"complete\":true,\"cores\":8,\"options\":{\"renderWidth\":2992,\"renderHeight\":1344,\"frameCap\":120,\"antialias\":0},"
            + "\"display\":{\"mode\":\"2992x1344@120.0\",\"modes\":[\"2992x1344@60.0\",\"2992x1344@120.0\"],\"preferredRefresh\":120},"
            + "\"native\":{\"frames\":{\"frames\":61234,\"activeSeconds\":512.0,\"avgFps\":119.6,\"p50Ms\":8.5,\"p90Ms\":9.0,\"p99Ms\":11.9,\"p999Ms\":40.0,"
            + "\"maxMs\":182.0,\"onePercentLowFps\":84.0,\"hitches\":7,\"stalls\":2,\"gaps\":1,\"spikes\":4,\"presentMsAvg\":2.1,\"targetFps\":120,"
            + "\"histogramUs\":[[8500,50000],[9000,11000],[250000,2]],\"perSecond\":[[0,118,9.1],[120,61,182.0],[121,97,40.0]]},"
            + "\"pipelines\":{\"graphics\":212,\"compute\":3,\"totalMs\":1830.0,\"maxMs\":96.0,\"over10Ms\":41},"
            + "\"audio\":{\"xruns\":0,\"bufferFrames\":1920,\"burstFrames\":192,\"rate\":48000},\"render\":{\"width\":2992,\"height\":1344,\"display\":[2992,1344]},"
            + "\"d3d9Calls\":{\"2/82\":25000000,\"2/65\":23000000,\"2/57\":74000000,\"2/17\":61234,\"5/2\":1000}},"
            + "\"samples\":[{\"t\":2,\"thermal\":0,\"headroom\":0.4,\"battTemp\":32.1,\"battLevel\":80,\"charging\":false,\"pssMb\":700,\"availMb\":4000,\"refresh\":120.0,\"cpuMhz\":[1700,2600]},"
            + "{\"t\":480,\"thermal\":1,\"headroom\":0.71,\"battTemp\":39.4,\"battLevel\":74,\"charging\":false,\"pssMb\":1012,\"availMb\":2900,\"refresh\":60.0,\"cpuPct\":55,\"lowMemory\":false}],"
            + "\"events\":[{\"t\":0,\"event\":\"start\"},{\"t\":9,\"event\":\"status\",\"detail\":\"Running the game\"},{\"t\":300,\"event\":\"pause\"}]}";

    @Test public void shortLineForTheLauncherCard() throws Exception {
        SessionSummary summary = SessionSummary.parse(RECORD);
        assertEquals("12 min 34 s · 120 FPS avg · 1% low 84 · 4 spikes · light", summary.shortLine());
        assertEquals(61234, summary.frames);
        assertEquals(215, summary.pipelineCompiles);
        assertEquals(1, summary.thermalMax);
        assertEquals(1012, summary.pssMaxMb);
        assertEquals(32.1, summary.batteryStartC, 1e-9);
        assertEquals(39.4, summary.batteryEndC, 1e-9);
        assertEquals(0.4, summary.headroomMin, 1e-9);
    }

    @Test public void reportCarriesEverySection() throws Exception {
        String report = SessionSummary.parse(RECORD).report;
        for (String expected : new String[] {
                "Session 20261027-030000 — 1.0.0-build.2 — 12 min 34 s (complete)",
                "Options: antialias=0 frameCap=120 renderHeight=1344 renderWidth=2992",
                "Display: 2992x1344@120.0",
                "Frames 61234 over 512 s active: avg 119.6 FPS, 1% low 84 FPS (p99 11.9 ms)",
                "Spikes 4 (>2.5x the frames around them), hitches 7 (>2.5 cap periods), stalls 2 (>100 ms), gaps 1 (>2 s); Present avg 2.10 ms; frame cap 120",
                "Frame-time histogram (≤ms: frames): 8.5:50000 9.0:11000 250.0:2",
                "Seconds below 80% of the cap: 1 of 3. Worst seconds (t, fps, max ms): [120: 61 fps, 182 ms]",
                "Pipelines compiled: 212 graphics + 3 compute, 1830 ms total, worst 96 ms, 41 over 10 ms",
                "Audio: xruns 0, buffer 1920 frames (burst 192), 48000 Hz",
                "Thermal: max light (status 1) at 480 s; headroom min 0.4; battery 32.1→39.4°C (min 32.1, max 39.4), level 80→74%",
                "Memory: game process PSS max 1012 MB; system free min 2900 MB; low-memory samples 0",
                "CPU: game process 55% of one core on average (cores=8); clocks max 2600 MHz",
                "Panel refresh (samples):",
                "D3D9 calls per frame: SetRenderState 1208 DrawIndexedPrimitive 408 SetTexture 376 Present 1 Release(5) 0",
                "Events: [0 s start] [9 s status: Running the game] [300 s pause]"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        assertTrue(report.contains("120 Hz×1") && report.contains("60 Hz×1"));
    }

    @Test public void gpuBusyAndTheRealBackBuffer() throws Exception {
        // Build 9 records: each second carries the GPU busy share, the render block the game's real back buffer.
        String record = RECORD.replace("\"perSecond\":[[0,118,9.1],[120,61,182.0],[121,97,40.0]]",
                        "\"perSecond\":[[0,118,9.1,55],[120,61,182.0,97],[121,97,40.0,-1]],\"gpuBusyAvg\":76.0")
                .replace("\"render\":{\"width\":2992,\"height\":1344,\"display\":[2992,1344]}",
                        "\"render\":{\"width\":2992,\"height\":1344,\"display\":[2992,1344],\"backBuffer\":[2992,1344]}");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "Worst seconds (t, fps, max ms): [120: 61 fps, 182 ms, GPU 97%] [121: 97 fps, 40 ms] [0: 118 fps, 9 ms, GPU 55%]",
                "GPU busy: avg 76%; 1 of 2 measured seconds at 90% or more (1 of the slow ones)",
                "Render 2992x1344 (game back buffer 2992x1344) on display [2992,1344]"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        // Older records (no GPU column, no back buffer) read as before.
        String old = SessionSummary.parse(RECORD).report;
        assertTrue(old.contains("Render 2992x1344 on display [2992,1344]") && !old.contains("GPU busy"));
    }

    @Test public void guestProfileAndD3D9Time() throws Exception {
        // 61234 frames; DrawIndexedPrimitive 122.468 s in all = 2.00 ms a frame, SetTexture 0.50 ms.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"d3d9Ns\":{\"2/82\":122468000000,\"2/65\":30617000000},\"d3d9Lookups\":[9750,250],"
                + "\"profile\":{\"intervalUs\":1000,\"samples\":1000,\"render\":{\"guest\":550,\"d3d9\":300,\"import\":20},\"other\":80,\"idle\":50,"
                + "\"top\":[[\"004A1B20\",120,0,0],[\"0040B9C0\",10,90,0]],\"otherTop\":[[\"0052D0C0\",60]]},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "D3D9 time per frame: 2.50 ms; DrawIndexedPrimitive 2.00 SetTexture 0.50; object lookups 97.5% cached",
                "Guest machine (1000 samples): render thread game code 55%, D3D9 30%, imports 2%; other threads 8%; idle 5%. Top: sub_004A1B20 12.0% sub_0040B9C0 10.0% (D3D9)"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
    }

    @Test public void incompleteAndEmptyRecordsAreReadable() throws Exception {
        SessionSummary crashed = SessionSummary.parse("{\"session\":\"x\",\"app\":\"1\",\"durationS\":42,\"complete\":false}");
        assertEquals("42 s · no frames recorded · ended abruptly", crashed.shortLine());
        assertTrue(crashed.report.contains("NOT complete"));
        assertTrue(crashed.report.contains("No frame telemetry"));
        assertEquals("DrawIndexedPrimitive", SessionSummary.methodName("2/82"));
        assertEquals("Release(3)", SessionSummary.methodName("3/2"));
        assertEquals("13/3", SessionSummary.methodName("13/3"));
    }
}
