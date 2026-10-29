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
                "Spikes 4 (>2.5x the frames around them), hitches 7 (frames over 25 ms), stalls 2 (>100 ms), gaps 1 (>2 s); Present avg 2.10 ms; frame cap 120",
                "Frame-time histogram (≤ms: frames): 8.5:50000 9.0:11000 250.0:2",
                "Seconds below 80% of the cap: 1 of 3. Worst seconds (t, fps, max ms): [120: 61 fps, 182 ms]",
                "Pipelines compiled: 212 graphics + 3 compute, 1830 ms total, worst 96 ms, 41 over 10 ms",
                "Audio: xruns 0, buffer 1920 frames (burst 192), 48000 Hz",
                "Thermal: max light (status 1) at 480 s; headroom min 0.4; battery 32.1→39.4°C (min 32.1, max 39.4), level 80→74%",
                "Memory: game process PSS max 1012 MB; system free min 2900 MB; low-memory samples 0",
                "CPU: game process 55% of one core on average (cores=8); clocks avg/max MHz: cpu0 1700/1700 cpu1 2600/2600",
                "Panel refresh (samples):",
                "D3D9 calls per frame: SetRenderState 1208 DrawIndexedPrimitive 408 SetTexture 376 Present 1 Release(5) 0",
                "Events: [0 s start] [9 s status: Running the game] [300 s pause]"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        assertTrue(report.contains("120 Hz×1") && report.contains("60 Hz×1"));
    }

    @Test public void lostSoundOutputAndDroppedPlacement() throws Exception {
        // Build 15: two streams lost and replaced, 0.18 s without sound; one placed thread dropped, the render thread
        // followed again.
        String record = RECORD.replace("\"audio\":{\"xruns\":0,\"bufferFrames\":1920,\"burstFrames\":192,\"rate\":48000}",
                        "\"audio\":{\"xruns\":3,\"bufferFrames\":1920,\"burstFrames\":96,\"rate\":48000,\"lost\":2,\"replaced\":2,\"silentMs\":184}")
                .replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7 (follows the fastest cluster)\",\"priority\":\"nice -10\",\"dxvk\":\"4 of 4 DXVK threads off cpu 7\","
                        + "\"adpf\":\"session with 5 threads, target 12.50 ms\",\"targetFps\":60,\"reports\":100,\"reportFailures\":0,\"placementRestored\":3,\"placementFailed\":0,"
                        + "\"placementDropped\":1,\"renderFollowedAgain\":1},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "Audio: xruns 3, buffer 1920 frames (burst 96), 48000 Hz; output lost 2 times, replaced 2, 0.2 s without sound",
                "performance hints: session with 5 threads, target 12.50 ms (100 reports, 0 failed); placement replaced and put back 3 times; "
                        + "1 placed thread no longer followed (the render thread followed again 1 time)"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        // Records before build 15, and sessions that lost nothing, read as before.
        String old = SessionSummary.parse(RECORD).report;
        assertTrue(old.contains("Audio: xruns 0, buffer 1920 frames (burst 192), 48000 Hz\n") && !old.contains("no longer followed"));
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

    @Test public void threadsAndPerCpuClocks() throws Exception {
        // Two samples: the render core (cpu1 here) averages half its peak.
        String record = RECORD.replace("\"cpuMhz\":[1700,2600]}", "\"cpuMhz\":[1700,2600]},{\"t\":4,\"cpuMhz\":[1300,600]}")
                .replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7\",\"priority\":\"nice -10\","
                        + "\"dxvk\":\"3 of 3 DXVK threads off cpu 7; command stream nice -10 (1 of 1)\",\"adpf\":\"session with 4 threads, target 16.67 ms\","
                        + "\"targetFps\":60,\"reports\":105930,\"reportFailures\":0},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "clocks avg/max MHz: cpu0 1500/1700 cpu1 1600/2600",
                "Threads: render thread on cpu 7, nice -10; 3 of 3 DXVK threads off cpu 7; command stream nice -10 (1 of 1); "
                        + "performance hints: session with 4 threads, target 16.67 ms (105930 reports, 0 failed)"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        // Build 8 records: no "dxvk" field.
        String old = RECORD.replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7\",\"priority\":\"nice -10\","
                + "\"adpf\":\"session with 1 threads, target 16.67 ms\",\"targetFps\":60,\"reports\":5,\"reportFailures\":0},\"d3d9Calls\":{");
        assertTrue(SessionSummary.parse(old).report.contains(
                "Threads: render thread on cpu 7, nice -10; performance hints: session with 1 threads, target 16.67 ms (5 reports, 0 failed)"));
    }

    @Test public void longCallsPresentTimingAndBatterySaver() throws Exception {
        String record = RECORD.replace("\"d3d9Calls\":{", "\"longCalls\":{\"2/17\":[18,1580.4,112.0],\"2/42\":[2,61.0,33.0]},"
                        + "\"vkPresent\":[42787,17114.8,95.0,12],\"d3d9Calls\":{")
                .replace("\"refresh\":60.0,", "\"refresh\":60.0,\"powerSave\":true,").replace("\"refresh\":120.0,", "\"refresh\":120.0,\"powerSave\":false,");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "Long D3D9 calls (20 ms or more): Present 18 (avg 88 ms, longest 112); EndScene 2 (avg 31 ms, longest 33)",
                "vkQueuePresentKHR: 42787 presents, avg 0.40 ms, longest 95.0 ms, 12 over 16 ms",
                "Battery Saver on in 1 of 2 samples (it holds the panel at 60 Hz)"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        assertEquals("TestCooperativeLevel", SessionSummary.methodName("2/3"));
    }

    @Test public void incompleteAndEmptyRecordsAreReadable() throws Exception {
        SessionSummary crashed = SessionSummary.parse("{\"session\":\"x\",\"app\":\"1\",\"durationS\":42,\"complete\":false}");
        assertEquals("42 s · no frames recorded · ended abruptly", crashed.shortLine());
        assertTrue(crashed.report, crashed.report.contains("(not closed: no exit record says why)"));
        assertTrue(crashed.report.contains("No frame telemetry"));
        assertEquals("DrawIndexedPrimitive", SessionSummary.methodName("2/82"));
        assertEquals("Release(3)", SessionSummary.methodName("3/2"));
        assertEquals("SwapChain Present", SessionSummary.methodName("13/3"));
        assertEquals("Texture LockRect", SessionSummary.methodName("3/19"));
        assertEquals("IndexBuffer Unlock", SessionSummary.methodName("7/12"));
        assertEquals("9/9", SessionSummary.methodName("9/9"));
    }

    @Test public void whatEndedAnUnclosedSession() throws Exception {
        // Build 10's 00:55 session: last written at a pause, then swiped away from recent apps a second later.
        String record = "{\"session\":\"20261028-005529\",\"app\":\"1.0.0-build.10\",\"start\":1000000,\"end\":2706000,\"durationS\":1706,\"complete\":false}";
        java.util.List<SessionSummary.Exit> exits = java.util.Arrays.asList(
                new SessionSummary.Exit(2707000, "com.nfsu1.remastered", 10, 0, "[REMOVE TASK] remove task"),          // the launcher process
                new SessionSummary.Exit(2707100, "com.nfsu1.remastered:game", 10, 0, "[REMOVE TASK] remove task"),
                new SessionSummary.Exit(900000, "com.nfsu1.remastered:game", 5, 11, ""),                                 // before this session
                new SessionSummary.Exit(9000000, "com.nfsu1.remastered:game", 5, 11, ""));                               // after the next began
        SessionSummary swiped = SessionSummary.parse(record, exits, 5000000);
        assertEquals("swiped away from recent apps", swiped.endReason);
        assertTrue(swiped.report, swiped.report.startsWith("Session 20261028-005529 — 1.0.0-build.10 — 28 min 26 s (ended: swiped away from recent apps)"));
        assertTrue(swiped.shortLine(), swiped.shortLine().endsWith("no frames recorded · swiped away from recent apps"));
        // The same record crashing instead.
        SessionSummary crashed = SessionSummary.parse(record, java.util.Collections.singletonList(
                new SessionSummary.Exit(2710000, "com.nfsu1.remastered:game", 5, 11, "")), 0);
        assertTrue(crashed.report, crashed.report.contains("(ended: crashed (native, signal 11))"));
        // A complete session is complete whatever exit followed (the launcher ends the game process itself).
        SessionSummary complete = SessionSummary.parse(record.replace("\"complete\":false", "\"complete\":true"), exits, 0);
        assertTrue(complete.report.contains("(complete)") && complete.endReason == null);
        assertEquals("Android closed it to free memory", SessionSummary.describeExit(3, 0, ""));
        assertEquals("stopped by the player", SessionSummary.describeExit(10, 0, "user request"));
        assertEquals("ended by Android (exit reason 13)", SessionSummary.describeExit(13, 0, ""));
    }

    @Test public void dxvkQueueAndPlacement() throws Exception {
        // Build 11: DXVK's command list processing, and a placement Android replaced twice.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"dxvkQueue\":{\"cleanupThread\":true,\"submits\":[183702,36740.4,31.0],"
                        + "\"resets\":[183702,165331.8,12.40,340,2],\"resetStepsMs\":[[110221.2,9.80],[36740.4,6.10],[9185.1,1.20],[9185.1,0.90]],"
                        + "\"fenceWaits\":[183702,385774.2],\"commandLists\":[41,9],\"descriptorPools\":[12,0]},"
                        + "\"host\":{\"affinity\":\"render thread on cpu 7\",\"priority\":\"nice -10\",\"dxvk\":\"4 of 4 DXVK threads off cpu 7\","
                        + "\"adpf\":\"session with 5 threads, target 8.33 ms\",\"targetFps\":0,\"reports\":9,\"reportFailures\":0,"
                        + "\"placementRestored\":2,\"placementFailed\":1},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "DXVK queue: cleanup on its own thread; vkQueueSubmit 183702 (3 per frame, avg 0.20 ms, longest 31.0 ms); "
                        + "list resets 183702 (avg 0.90 ms, longest 12.4 ms, 340 over 4 ms, 2 on the queue thread), "
                        + "avg/longest ms: command pools 0.60/9.8, resources 0.20/6.1, descriptor pools 0.05/1.2, buffers and queries 0.05/0.9; "
                        + "fence waits avg 2.10 ms; command lists created 41, freed 9; descriptor pools created 12, freed 0",
                "(9 reports, 0 failed); placement replaced and put back 2 times (1 checks could not)"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        String held = RECORD.replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7\",\"priority\":\"nice -10\","
                + "\"adpf\":\"x\",\"reports\":1,\"reportFailures\":0,\"placementRestored\":0,\"placementFailed\":0},\"d3d9Calls\":{");
        assertTrue(SessionSummary.parse(held).report.contains("(1 reports, 0 failed); placement held all session"));
    }

    @Test public void locksThatWaitedForTheGpu() throws Exception {
        // Build 13: D3D9 Locks of a buffer or texture the GPU still had, counted and timed in DXVK.
        String queue = "\"dxvkQueue\":{\"cleanupThread\":true,\"submits\":[10,2.0,0.5],\"resets\":[10,3.0,1.0,0,0],"
                + "\"fenceWaits\":[10,5.0],\"commandLists\":[4,0],\"descriptorPools\":[1,0]";
        String waited = SessionSummary.parse(RECORD.replace("\"d3d9Calls\":{", queue + ",\"resourceWaits\":[24,604.0,87.0]},\"d3d9Calls\":{")).report;
        assertTrue(waited, waited.contains("; descriptor pools created 1, freed 0; Locks that waited for the GPU 24 (604.0 ms in all, longest 87.0 ms)\n"));
        String none = SessionSummary.parse(RECORD.replace("\"d3d9Calls\":{", queue + ",\"resourceWaits\":[0,0.0,0.00]},\"d3d9Calls\":{")).report;
        assertTrue(none, none.contains("; descriptor pools created 1, freed 0; no Lock waited for the GPU\n"));
        String older = SessionSummary.parse(RECORD.replace("\"d3d9Calls\":{", queue + "},\"d3d9Calls\":{")).report;
        assertTrue(older, older.contains("; descriptor pools created 1, freed 0\n"));
    }

    @Test public void measuredClocksCommandStreamAndThreadTime() throws Exception {
        // Build 16: the clock the render thread ran at on each cluster, the command stream's own cluster, the threads'
        // time on a cpu, where the clocks came from and Pixel's PMU limit, and the thermal resets.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7 (follows the fastest cluster)\",\"priority\":\"nice -10\","
                        + "\"dxvk\":\"4 of 4 DXVK threads off cpu 7, the command stream on cpu 5-6 (follows the fastest other cluster)\",\"adpf\":\"x\",\"reports\":1,"
                        + "\"reportFailures\":0,\"placementRestored\":0,\"placementFailed\":0,\"clockSource\":\"cpufreq stats (time_in_state)\","
                        + "\"pmu\":\"not readable (Permission denied)\",\"thermalStatus\":1,\"thermalResets\":2,\"renderCpus\":\"2-4\",\"coreMoves\":3,"
                        + "\"clusters\":[{\"cpus\":\"0-1\",\"seconds\":0,\"streamSeconds\":0},"
                        + "{\"cpus\":\"2-4\",\"seconds\":300,\"streamSeconds\":40,\"runMhz\":1927,\"runCapMhz\":2188,\"busy\":95},"
                        + "{\"cpus\":\"5-6\",\"seconds\":0,\"streamSeconds\":60},"
                        + "{\"cpus\":\"7\",\"seconds\":100,\"streamSeconds\":300,\"runMhz\":1001,\"runCapMhz\":1305,\"busy\":93}],"
                        + "\"streamCpus\":\"7\",\"streamMoves\":4,\"moves\":[[0,\"7\"],[100,\"2-4\"]],"
                        + "\"threads\":[{\"name\":\"render\",\"spanS\":400,\"runS\":376,\"waitS\":4,\"clusterRunS\":[0,282,0,94]},"
                        + "{\"name\":\"dxvk-cs\",\"spanS\":400,\"runS\":220,\"waitS\":12,\"clusterRunS\":[0,22,33,165]},"
                        + "{\"name\":\"dxvk-submit\",\"spanS\":400,\"runS\":84,\"waitS\":1,\"clusterRunS\":[40,0,44,0]}]},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "  Render thread: cpu 2-4 75% at ",
                ", ran at 1927 of 2188 MHz (88%); cpu 7 25% at ",
                ", ran at 1001 of 1305 MHz (77%) (moved 3 times; the thermal status changed 2 times, starting the measured clocks over)\n",
                "  Command stream: cpu 2-4 10%, cpu 5-6 15%, cpu 7 75% (moved 4 times)\n",
                "  Thread time on a cpu (waiting for one): render 94% (1%), dxvk-cs 55% (3%), dxvk-submit 21%\n",
                "  Clocks measured from cpufreq stats (time_in_state); PMU frequency limit (sched_pixel): not readable (Permission denied)\n"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        // Records before build 16 have none of these lines.
        String old = SessionSummary.parse(RECORD).report;
        assertTrue(!old.contains("Command stream:") && !old.contains("Thread time") && !old.contains("Clocks measured"));
    }

    @Test public void renderThreadFollowsTheFastestCluster() throws Exception {
        // Build 12: the clock each cpu was allowed (two samples), and where the render thread ran as the X4 was capped.
        String record = RECORD.replace("\"cpuMhz\":[1700,2600]}", "\"cpuMhz\":[1700,2600],\"cpuCapMhz\":[2246,3782]},"
                        + "{\"t\":4,\"cpuMhz\":[1300,600],\"cpuCapMhz\":[2246,1152]}")
                .replace("\"d3d9Calls\":{", "\"host\":{\"affinity\":\"render thread on cpu 7 (follows the fastest cluster)\",\"priority\":\"nice -10\","
                        + "\"adpf\":\"x\",\"reports\":1,\"reportFailures\":0,\"placementRestored\":0,\"placementFailed\":0,\"renderCpus\":\"5-6\","
                        + "\"coreMoves\":9,\"clusters\":[{\"cpus\":\"0-1\",\"capacity\":280,\"maxMhz\":2246,\"seconds\":0,\"capMinMhz\":2246,\"capAvgMhz\":2246},"
                        + "{\"cpus\":\"2-4\",\"capacity\":760,\"maxMhz\":3052,\"seconds\":0,\"capMinMhz\":2688,\"capAvgMhz\":2950},"
                        + "{\"cpus\":\"5-6\",\"capacity\":760,\"maxMhz\":3052,\"seconds\":310,\"capMinMhz\":2688,\"capAvgMhz\":2967},"
                        + "{\"cpus\":\"7\",\"capacity\":1024,\"maxMhz\":3782,\"seconds\":190,\"capMinMhz\":533,\"capAvgMhz\":1402}],"
                        // From the first frame on the X4, to cpu 5-6 at 120 s (the RECORD's seconds 0 and 120-121 are then split 1:2).
                        + "\"moves\":[[0,\"7\"],[120,\"5-6\"]]},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        for (String expected : new String[] {
                "clocks avg/max MHz: cpu0 1500/1700 cpu1 1600/2600; allowed avg/min MHz: cpu0 2246/2246 cpu1 2467/1152",
                "Threads: render thread on cpu 7 (follows the fastest cluster), nice -10;",
                "  Render thread: cpu 5-6 62% at 79 FPS, cpu 7 38% at 118 FPS (moved 9 times)\n"}) {
            assertTrue("missing: " + expected + "\n" + report, report.contains(expected));
        }
        org.json.JSONArray perSecond = new org.json.JSONArray("[[0,40],[1,44],[2,90],[3,96],[5,100]]");
        String host = "{\"coreMoves\":2,\"clusters\":[{\"cpus\":\"5-6\",\"seconds\":2},{\"cpus\":\"7\",\"seconds\":4}],";
        // Back home at 5 s: 7 at 40-44 FPS until 2 s, 5-6 at 90-96, 7 again at 100.
        assertEquals("  Render thread: cpu 5-6 33% at 93 FPS, cpu 7 67% at 61 FPS (moved 2 times)\n", SessionSummary.coresLine(new org.json.JSONObject(
                host + "\"moves\":[[0,\"7\"],[2,\"5-6\"],[5,\"7\"]]}"), perSecond));
        // A move list that ran out: no rates rather than wrong ones; none in an older record either.
        assertEquals("  Render thread: cpu 5-6 33%, cpu 7 67% (moved 2 times)\n", SessionSummary.coresLine(new org.json.JSONObject(
                host + "\"moves\":[[0,\"7\"],[2,\"5-6\"]],\"movesComplete\":false}"), perSecond));
        assertEquals("  Render thread: cpu 5-6 33%, cpu 7 67% (moved 2 times)\n", SessionSummary.coresLine(new org.json.JSONObject(
                host + "\"x\":0}"), perSecond));
        // Moved once; no time yet; and records before build 12 (no clusters) have no such line.
        assertEquals("  Render thread: cpu 7 100% (moved 1 time)\n", SessionSummary.coresLine(new org.json.JSONObject(
                "{\"coreMoves\":1,\"clusters\":[{\"cpus\":\"7\",\"seconds\":12}]}"), null));
        assertEquals("  Render thread: no time playing yet (moved 0 times)\n", SessionSummary.coresLine(new org.json.JSONObject(
                "{\"coreMoves\":0,\"clusters\":[{\"cpus\":\"7\",\"seconds\":0}]}"), perSecond));
        assertEquals(null, SessionSummary.coresLine(new org.json.JSONObject("{\"affinity\":\"render thread on cpu 7\"}"), perSecond));
        assertTrue(!SessionSummary.parse(RECORD).report.contains("Render thread:"));
        assertTrue(!SessionSummary.parse(RECORD).report.contains("allowed avg/min"));
    }

    @Test public void garageAndEffectStatesAreReported() throws Exception {
        // Build 17: which Garage switches were on, which of the game's decisions they changed, the unlock byte.
        // A build-17 record names seven switches: all seven on, and more paints (build 18) not called off.
        String seven = "\"unlock\":0,\"specialVinyls\":17,\"vinylLayers\":0,\"hoodVinyls\":4,\"hoodDecals\":0,\"allDecals\":0,\"raceBrakes\":0";
        assertEquals("  Garage: all 7 on (unlock byte set); changed: special vinyls 17, hood vinyls 4\n", SessionSummary.garageLine(new org.json.JSONObject(
                "{\"features\":127,\"changes\":{" + seven + "},\"unknownSites\":0,\"unlockByte\":1}")));
        // Build 18 says how many switches it has.
        assertEquals("  Garage: all 8 on (unlock byte set); changed: special vinyls 17, hood vinyls 4, more paints 3\n",
                SessionSummary.garageLine(new org.json.JSONObject("{\"features\":255,\"switches\":8,\"changes\":{" + seven
                        + ",\"morePaints\":3},\"unlockByte\":1}")));
        assertEquals("  Garage: on: unlock everything, special vinyls, free vinyl layers, hood vinyls, hood decals, all decals, race brakes;"
                + " off: more paints (unlock byte set); changed: special vinyls 17, hood vinyls 4\n", SessionSummary.garageLine(new org.json.JSONObject(
                "{\"features\":127,\"switches\":8,\"changes\":{" + seven + ",\"morePaints\":0},\"unlockByte\":1}")));
        // Build 18 with every rim size; a later build's switch this one doesn't know is left out, not misnamed.
        assertEquals("  Garage: all 9 on; changed: every rim size 40\n",
                SessionSummary.garageLine(new org.json.JSONObject("{\"features\":511,\"switches\":9,\"changes\":{\"allRimSizes\":40}}")));
        assertEquals("  Garage: all 9 on; no game decision changed\n",
                SessionSummary.garageLine(new org.json.JSONObject("{\"features\":1023,\"switches\":10}")));
        // 66: special vinyls (bit 1) and race brakes (bit 6); nothing in the record says how many: every switch counts.
        assertEquals("  Garage: on: special vinyls, race brakes; off: unlock everything, free vinyl layers, hood vinyls, hood decals, all decals,"
                + " more paints, every rim size; no game decision changed\n", SessionSummary.garageLine(new org.json.JSONObject("{\"features\":66,\"changes\":{},\"unknownSites\":0}")));
        assertEquals("  Garage: all off (unlock byte clear)\n", SessionSummary.garageLine(new org.json.JSONObject("{\"features\":0,\"unlockByte\":0}")));
        // A jump the lifter wrapped with no rule in the table (config/branches.txt and nfs_garage.h out of step).
        assertEquals("  Garage: all 9 on; no game decision changed; 3 jumps reached with no rule\n",
                SessionSummary.garageLine(new org.json.JSONObject("{\"features\":511,\"unknownSites\":3}")));
        assertEquals(null, SessionSummary.garageLine(null));
        assertEquals(null, SessionSummary.garageLine(new org.json.JSONObject("{}")));

        // The effects' states: per frame, the host's share, the share of those already set, the value updates.
        org.json.JSONObject effects = new org.json.JSONObject("{\"host\":true,\"applyCalls\":1200000,\"applyHandled\":1116000,"
                + "\"evaluateCalls\":900000,\"evaluateHandled\":873000,\"kept\":1071360,\"sent\":44640}");
        assertEquals("  Effect states (host): 20,000 applied a frame, 93% by the host (96% of those already set, skipped), the rest by the game's"
                + " D3DX code; value updates 97% answered at once\n", SessionSummary.effectStatesLine(effects, 60));
        assertEquals("  Effect states (host): none applied\n", SessionSummary.effectStatesLine(new org.json.JSONObject("{\"host\":true,\"applyCalls\":0}"), 60));
        assertEquals("  Effect states: the game's own D3DX code (launcher option)\n",
                SessionSummary.effectStatesLine(new org.json.JSONObject("{\"host\":false,\"applyCalls\":5}"), 60));
        assertEquals(null, SessionSummary.effectStatesLine(null, 60));
        assertEquals(null, SessionSummary.effectStatesLine(new org.json.JSONObject("{}"), 60));

        // In the report, before the events; records before build 17 have neither line.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"garage\":{\"features\":255,\"switches\":8,\"changes\":{\"specialVinyls\":2},\"unknownSites\":0,\"unlockByte\":1},"
                + "\"effectStates\":{\"host\":true,\"applyCalls\":1224680000,\"applyHandled\":1224680000,\"evaluateCalls\":10,\"evaluateHandled\":10,"
                + "\"kept\":1175692800,\"sent\":48987200},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        String garage = "  Garage: all 8 on (unlock byte set); changed: special vinyls 2\n";
        String states = "  Effect states (host): 20,000 applied a frame, 100% by the host (96% of those already set, skipped); value updates 100%"
                + " answered at once\n";
        assertTrue(report, report.contains(garage + states));
        assertTrue(report, report.indexOf(states) < report.indexOf("  Events:"));
        String old = SessionSummary.parse(RECORD).report;
        assertTrue(!old.contains("Garage:") && !old.contains("Effect states"));
    }

    @Test public void freeRoamIsReported() throws Exception {
        org.json.JSONObject played = new org.json.JSONObject("{\"on\":true,\"offered\":3,\"ms\":41,\"tracks\":["
                + "{\"number\":1000,\"offered\":true,\"cache\":\"hit\",\"ms\":0,\"mapSize\":2048,\"tiles\":21,\"quads\":57869},"
                + "{\"number\":1300,\"offered\":true,\"cache\":\"hit\",\"ms\":0,\"mapSize\":512,\"tiles\":4,\"quads\":9000},"
                + "{\"number\":1400,\"offered\":true,\"cache\":\"hit\",\"ms\":0,\"mapSize\":512,\"tiles\":4,\"quads\":8000}],"
                + "\"tracksAdded\":3,\"namesAdded\":6,\"races\":1,\"wrongWayCalls\":9000,\"wrongWayCleared\":3,\"updates\":9000,"
                + "\"crumbs\":640,\"resets\":5,\"resetsPlaced\":4,\"resetsLeft\":1}");
        assertEquals("  Free roam: 3 tracks in Free Run (made earlier, checked in 41 ms; maps 2048, 512, 512); 1 session, 640 breadcrumbs;"
                + " resets: 4 to a breadcrumb, 1 by the game; wrong way cleared 3 times\n", SessionSummary.freeRoamLine(played));
        org.json.JSONObject partly = new org.json.JSONObject("{\"on\":true,\"offered\":2,\"ms\":1830,\"tracks\":["
                + "{\"number\":1000,\"offered\":true,\"cache\":\"built\",\"ms\":1500,\"mapSize\":2048},"
                + "{\"number\":1300,\"offered\":false,\"cache\":\"failed\",\"error\":\"section 3 is not the same section\"},"
                + "{\"number\":1400,\"offered\":true,\"cache\":\"built\",\"mapSize\":0,\"mapError\":\"a streaming texture pack\"}],"
                + "\"races\":0}");
        assertEquals("  Free roam: 2 tracks in Free Run (made in 1830 ms; maps 2048); 1300 not offered (section 3 is not the same section);"
                + " 1400 without a map (a streaming texture pack)\n", SessionSummary.freeRoamLine(partly));
        assertEquals("  Free roam: no tracks (Global/GlobalB.lzc unreadable)\n", SessionSummary.freeRoamLine(new org.json.JSONObject(
                "{\"on\":true,\"offered\":0,\"error\":\"Global/GlobalB.lzc unreadable\",\"tracks\":[]}")));
        assertEquals(null, SessionSummary.freeRoamLine(new org.json.JSONObject("{\"on\":false,\"offered\":0,\"tracks\":[]}")));
        assertEquals(null, SessionSummary.freeRoamLine(null));
    }

    @Test public void filesAndLayerBlendAreReported() throws Exception {
        // Build 20: the file opens and the path lookups the game's file layer made, with the listings kept.
        org.json.JSONObject files = new org.json.JSONObject("{\"opens\":1271,\"openMs\":381.3,\"lookups\":6702,\"cached\":6641,"
                + "\"listings\":61,\"changes\":4,\"resolveMs\":52.4}");
        assertEquals("  Files: 1,271 opened (0.3 ms each); 6,702 path lookups, 99% kept; 61 folder listings, 4 folder changes; 52 ms resolving\n",
                SessionSummary.filesLine(files));
        assertEquals("  Files: 0 opened (0.0 ms each); 0 path lookups, 0% kept; 0 folder listings, 1 folder change; 0 ms resolving\n",
                SessionSummary.filesLine(new org.json.JSONObject("{\"opens\":0,\"lookups\":0,\"changes\":1}")));
        assertEquals(null, SessionSummary.filesLine(null));
        assertEquals(null, SessionSummary.filesLine(new org.json.JSONObject("{}")));

        // The layer blend of the car textures: all by the host, some left to the game's code, the test host's verify mode.
        assertEquals("  Layer blend (car textures): 1,234,567 pixels, all by the host\n", SessionSummary.layerBlendLine(
                new org.json.JSONObject("{\"calls\":1234567,\"handled\":1234567,\"verified\":0,\"mismatches\":0}")));
        assertEquals("  Layer blend (car textures): 1,000 pixels, 990 by the host, 10 by the game's code\n", SessionSummary.layerBlendLine(
                new org.json.JSONObject("{\"calls\":1000,\"handled\":990}")));
        assertEquals("  Layer blend (car textures): 500 pixels, all by the host; 500 checked against the game's code, 1 difference\n",
                SessionSummary.layerBlendLine(new org.json.JSONObject("{\"calls\":500,\"handled\":500,\"verified\":500,\"mismatches\":1}")));
        assertEquals(null, SessionSummary.layerBlendLine(new org.json.JSONObject("{\"calls\":0,\"handled\":0}")));
        assertEquals(null, SessionSummary.layerBlendLine(null));

        // In the report, after the effect states and before the events; an older record has neither line.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"layerBlend\":{\"calls\":20,\"handled\":20,\"verified\":0,\"mismatches\":0},"
                + "\"files\":{\"opens\":3,\"openMs\":0.9,\"lookups\":10,\"cached\":9,\"listings\":2,\"changes\":0,\"resolveMs\":1.2},\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        String lines = "  Layer blend (car textures): 20 pixels, all by the host\n"
                + "  Files: 3 opened (0.3 ms each); 10 path lookups, 90% kept; 2 folder listings, 0 folder changes; 1 ms resolving\n";
        assertTrue(report, report.contains(lines));
        assertTrue(report, report.indexOf(lines) < report.indexOf("  Events:"));
        String old = SessionSummary.parse(RECORD).report;
        assertTrue(!old.contains("Files:") && !old.contains("Layer blend"));
    }

    @Test public void rimSizesAreReported() throws Exception {
        // Build 18: the wheel pack served with every rim size on, from the app's folder, and what the game side did.
        org.json.JSONObject on = new org.json.JSONObject("{\"on\":true,\"served\":true,\"reason\":\"on\",\"cache\":\"hit\",\"ms\":3,"
                + "\"parts\":492,\"solids\":1476,\"bytes\":41762352,\"lookups\":12345,\"later\":37,\"shared\":120,\"sorted\":6,\"opened\":4}");
        assertEquals("  Rim sizes: 492 added, made earlier, checked in 3 ms; parts lookups by the host 12,345 (37 found an added size);"
                + " rim browser: 120 thumbnails shared, 6 lists in size order, 4 opened on the car's rim\n", SessionSummary.rimSizesLine(on));
        // Made at this boot.
        assertEquals("  Rim sizes: 492 added, made in 412 ms; parts lookups by the host 0 (0 found an added size); rim browser: 0 thumbnails"
                + " shared, 0 lists in size order, 0 opened on the car's rim\n", SessionSummary.rimSizesLine(new org.json.JSONObject(
                "{\"on\":true,\"served\":true,\"reason\":\"on\",\"cache\":\"built\",\"ms\":412,\"parts\":492}")));
        // Off, kept for a save that wears an added size: no browser figures.
        assertEquals("  Rim sizes: off, the wheel pack kept for a save that wears one of its sizes; 492 added, made earlier, checked in 2 ms;"
                + " parts lookups by the host 900 (1 found an added size)\n", SessionSummary.rimSizesLine(new org.json.JSONObject(
                "{\"on\":false,\"served\":true,\"reason\":\"save\",\"cache\":\"hit\",\"ms\":2,\"parts\":492,\"lookups\":900,\"later\":1}")));
        // On but not served: why.
        assertEquals("  Rim sizes: the wheel pack was not served (the wheel file has more than one solid list); the rim browser offered the"
                + " sizes the game has\n", SessionSummary.rimSizesLine(new org.json.JSONObject(
                "{\"on\":true,\"served\":false,\"cache\":\"failed\",\"error\":\"the wheel file has more than one solid list\"}")));
        // Off and nothing served, or a record before build 18: no line.
        assertEquals(null, SessionSummary.rimSizesLine(new org.json.JSONObject("{\"on\":false,\"served\":false,\"reason\":\"off\"}")));
        assertEquals(null, SessionSummary.rimSizesLine(null));
        assertEquals(null, SessionSummary.rimSizesLine(new org.json.JSONObject("{}")));
        // In the report, right after the Garage line.
        String record = RECORD.replace("\"d3d9Calls\":{", "\"garage\":{\"features\":511,\"switches\":9,\"changes\":{},\"unknownSites\":0},"
                + "\"rimSizes\":" + on + ",\"d3d9Calls\":{");
        String report = SessionSummary.parse(record).report;
        assertTrue(report, report.contains("  Garage: all 9 on; no game decision changed\n  Rim sizes: 492 added"));
    }

    @Test public void overlayShowsTheRenderCores() throws Exception {
        org.json.JSONObject sample = new org.json.JSONObject("{\"cpuCapMhz\":[2246,2246,2688,2688,2688,2688,2688,1152]}");
        assertEquals("render cpu 5-6 (≤2688 MHz)  moved 3", GameTelemetry.renderLine(
                new org.json.JSONObject("{\"renderCpus\":\"5-6\",\"coreMoves\":3}"), sample));
        // Build 16: the clock it ran at over the last check, and the command stream's cores.
        assertEquals("render cpu 7 (883 of ≤1152 MHz)  stream cpu 2-4  moved 3", GameTelemetry.renderLine(new org.json.JSONObject(
                "{\"renderCpus\":\"7\",\"coreMoves\":3,\"streamCpus\":\"2-4\",\"clusters\":[{\"cpus\":\"0-1\"},{\"cpus\":\"2-4\"},{\"cpus\":\"5-6\"},"
                        + "{\"cpus\":\"7\"}],\"clocksNow\":[2246,2188,1075,883]}"), sample));
        // "big": pinned without moves; no sample yet; caps unreadable on this phone.
        assertEquals("render cpu 7 (≤1152 MHz)", GameTelemetry.renderLine(new org.json.JSONObject("{\"renderCpus\":\"7\"}"), sample));
        assertEquals("render cpu 7", GameTelemetry.renderLine(new org.json.JSONObject("{\"renderCpus\":\"7\"}"), null));
        assertEquals("render cpu 9", GameTelemetry.renderLine(new org.json.JSONObject("{\"renderCpus\":\"9\"}"), sample));
        // "system": not pinned, no line.
        assertEquals(null, GameTelemetry.renderLine(new org.json.JSONObject("{\"affinity\":\"system placement\"}"), sample));
        assertEquals(null, GameTelemetry.renderLine(null, sample));
        assertEquals(5, GameTelemetry.firstCpu("5-6"));
        assertEquals(0, GameTelemetry.firstCpu("0,2"));
        assertEquals(-1, GameTelemetry.firstCpu(""));
        assertEquals(-1, GameTelemetry.firstCpu("x"));
    }
}
