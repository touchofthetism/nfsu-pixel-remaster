package com.nfsu1.androidevolved;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Reads a session record written by {@link GameTelemetry} into a few human-readable lines: what the
 * launcher's "Last session" card shows and what the diagnostics ZIP puts in session-summary.txt, so the
 * numbers can be read without parsing JSON. English on purpose: it is written for whoever tunes the build.
 */
final class SessionSummary {
    private static final String[] THERMAL = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    /** IDirect3DDevice9 vtable slots worth naming in a call profile (kind 2). */
    private static final Map<Integer, String> DEVICE_METHODS = new HashMap<>();
    static {
        int[] slots = {17, 23, 26, 27, 29, 34, 37, 39, 41, 42, 43, 44, 47, 57, 65, 67, 69, 75, 81, 82, 83, 84, 87, 89, 92, 94, 100, 104, 107, 109,
                3, 16, 18, 25, 28, 30, 31, 32, 35};
        String[] names = {"Present", "CreateTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateDepthStencilSurface", "StretchRect",
                "SetRenderTarget", "SetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform", "SetViewport", "SetRenderState",
                "SetTexture", "SetTextureStageState", "SetSamplerState", "SetScissorRect", "DrawPrimitive", "DrawIndexedPrimitive",
                "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "SetVertexDeclaration", "SetFVF", "SetVertexShader", "SetVertexShaderConstantF",
                "SetStreamSource", "SetIndices", "SetPixelShader", "SetPixelShaderConstantF",
                "TestCooperativeLevel", "Reset", "GetBackBuffer", "CreateCubeTexture", "CreateRenderTarget", "UpdateSurface", "UpdateTexture",
                "GetRenderTargetData", "ColorFill"};
        for (int i = 0; i < slots.length; i++) DEVICE_METHODS.put(slots[i], names[i]);
    }

    final String session, app;
    final long durationS, frames, hitches, stalls, gaps, spikes;
    final double avgFps, onePercentLowFps, p50Ms, p99Ms, maxMs, presentMsAvg;
    final long pipelineCompiles, pipelineOver10Ms;
    final double pipelineTotalMs, pipelineMaxMs;
    final int thermalMax, pssMaxMb;
    final double batteryStartC, batteryEndC, headroomMin;
    final boolean complete;
    final String report;

    private SessionSummary(JSONObject record) {
        session = record.optString("session", "?");
        app = record.optString("app", "?");
        durationS = record.optLong("durationS");
        complete = record.optBoolean("complete");
        JSONObject nat = record.optJSONObject("native");
        JSONObject frames = nat == null ? null : nat.optJSONObject("frames");
        JSONObject pipelines = nat == null ? null : nat.optJSONObject("pipelines");
        this.frames = frames == null ? 0 : frames.optLong("frames");
        hitches = frames == null ? 0 : frames.optLong("hitches");
        stalls = frames == null ? 0 : frames.optLong("stalls");
        gaps = frames == null ? 0 : frames.optLong("gaps");
        spikes = frames == null ? 0 : frames.optLong("spikes");
        avgFps = frames == null ? 0 : frames.optDouble("avgFps", 0);
        onePercentLowFps = frames == null ? 0 : frames.optDouble("onePercentLowFps", 0);
        p50Ms = frames == null ? 0 : frames.optDouble("p50Ms", 0);
        p99Ms = frames == null ? 0 : frames.optDouble("p99Ms", 0);
        maxMs = frames == null ? 0 : frames.optDouble("maxMs", 0);
        presentMsAvg = frames == null ? 0 : frames.optDouble("presentMsAvg", 0);
        pipelineCompiles = pipelines == null ? 0 : pipelines.optLong("graphics") + pipelines.optLong("compute");
        pipelineOver10Ms = pipelines == null ? 0 : pipelines.optLong("over10Ms");
        pipelineTotalMs = pipelines == null ? 0 : pipelines.optDouble("totalMs", 0);
        pipelineMaxMs = pipelines == null ? 0 : pipelines.optDouble("maxMs", 0);
        JSONArray samples = record.optJSONArray("samples");
        int thermal = 0, pss = 0;
        double battStart = Double.NaN, battEnd = Double.NaN, headroom = Double.NaN;
        if (samples != null) for (int i = 0; i < samples.length(); i++) {
            JSONObject s = samples.optJSONObject(i);
            if (s == null) continue;
            thermal = Math.max(thermal, s.optInt("thermal"));
            pss = Math.max(pss, s.optInt("pssMb"));
            if (s.has("battTemp")) { if (Double.isNaN(battStart)) battStart = s.optDouble("battTemp"); battEnd = s.optDouble("battTemp"); }
            if (s.has("headroom")) headroom = Double.isNaN(headroom) ? s.optDouble("headroom") : Math.min(headroom, s.optDouble("headroom"));
        }
        thermalMax = thermal; pssMaxMb = pss; batteryStartC = battStart; batteryEndC = battEnd; headroomMin = headroom;
        report = buildReport(record, nat, frames, pipelines, samples);
    }

    static SessionSummary parse(String json) throws JSONException { return new SessionSummary(new JSONObject(json)); }

    /** One line for the launcher card. */
    String shortLine() {
        if (frames == 0) return String.format(Locale.US, "%s · no frames recorded%s", duration(), complete ? "" : " · ended abruptly");
        return String.format(Locale.US, "%s · %.0f FPS avg · 1%% low %.0f · %d spikes · %s%s", duration(), avgFps, onePercentLowFps, spikes,
                THERMAL[Math.min(THERMAL.length - 1, Math.max(0, thermalMax))], complete ? "" : " · ended abruptly");
    }

    String duration() {
        long m = durationS / 60, s = durationS % 60;
        return m > 0 ? String.format(Locale.US, "%d min %02d s", m, s) : String.format(Locale.US, "%d s", s);
    }

    private String buildReport(JSONObject record, JSONObject nat, JSONObject frames, JSONObject pipelines, JSONArray samples) {
        StringBuilder out = new StringBuilder();
        out.append(String.format(Locale.US, "Session %s — %s — %s%s\n", session, app, duration(), complete ? " (complete)" : " (NOT complete: crash or kill)"));
        JSONObject options = record.optJSONObject("options");
        if (options != null) {
            List<String> keys = new ArrayList<>();
            for (java.util.Iterator<String> it = options.keys(); it.hasNext();) keys.add(it.next());
            Collections.sort(keys);
            StringBuilder line = new StringBuilder("  Options:");
            for (String key : keys) if (!key.equals("driverLoader")) line.append(' ').append(key).append('=').append(options.opt(key));
            out.append(line).append('\n');
        }
        JSONObject display = record.optJSONObject("display");
        if (display != null) out.append("  Display: ").append(display.optString("mode")).append(" of ").append(display.optJSONArray("modes"))
                .append(" preferred ").append(display.optDouble("preferredRefresh")).append(" Hz\n");
        if (frames == null) out.append("  No frame telemetry in this record.\n");
        else {
            out.append(String.format(Locale.US, "  Frames %d over %.0f s active: avg %.1f FPS, 1%% low %.0f FPS (p99 %.1f ms), p50 %.1f ms, p90 %.1f ms, p99.9 %.1f ms, max %.0f ms\n",
                    frames.optLong("frames"), frames.optDouble("activeSeconds"), avgFps, onePercentLowFps, p99Ms, p50Ms, frames.optDouble("p90Ms"),
                    frames.optDouble("p999Ms"), maxMs));
            out.append(String.format(Locale.US, "  Spikes %d (>2.5x the frames around them), hitches %d (>2.5 cap periods), stalls %d (>100 ms), gaps %d (>2 s); Present avg %.2f ms; frame cap %d\n",
                    frames.optLong("spikes"), hitches, stalls, gaps, presentMsAvg, frames.optInt("targetFps")));
            JSONArray histogram = frames.optJSONArray("histogramUs");
            if (histogram != null && histogram.length() > 0) {
                StringBuilder line = new StringBuilder("  Frame-time histogram (≤ms: frames):");
                for (int i = 0; i < histogram.length(); i++) {
                    JSONArray bucket = histogram.optJSONArray(i);
                    if (bucket == null) continue;
                    long bound = bucket.optLong(0);
                    line.append(' ').append(bound >= 0xFFFFFFFFL ? "more" : String.format(Locale.US, "%.1f", bound / 1000.0)).append(':').append(bucket.optLong(1));
                }
                out.append(line).append('\n');
            }
            JSONArray perSecond = frames.optJSONArray("perSecond");
            if (perSecond != null && perSecond.length() > 0) {
                List<double[]> worst = new ArrayList<>();
                int lowSeconds = 0, gpuMeasured = 0, gpuBound = 0, lowGpuBound = 0;
                for (int i = 0; i < perSecond.length(); i++) {
                    JSONArray second = perSecond.optJSONArray(i);
                    if (second == null) continue;
                    // [second, frames, worst ms, GPU busy % (-1 or absent: not measured)]
                    double gpu = second.length() > 3 ? second.optDouble(3, -1) : -1;
                    worst.add(new double[] {second.optDouble(0), second.optDouble(1), second.optDouble(2), gpu});
                    boolean low = second.optDouble(1) < 0.8 * Math.max(30, frames.optInt("targetFps") == 0 ? 120 : frames.optInt("targetFps")) && second.optDouble(1) > 0;
                    if (low) lowSeconds++;
                    if (gpu >= 0) { gpuMeasured++; if (gpu >= 90) { gpuBound++; if (low) lowGpuBound++; } }
                }
                worst.sort((a, b) -> Double.compare(b[2], a[2]));
                StringBuilder line = new StringBuilder(String.format(Locale.US, "  Seconds below 80%% of the cap: %d of %d. Worst seconds (t, fps, max ms):", lowSeconds, perSecond.length()));
                for (int i = 0; i < Math.min(8, worst.size()); i++)
                    line.append(String.format(Locale.US, " [%.0f: %.0f fps, %.0f ms%s]", worst.get(i)[0], worst.get(i)[1], worst.get(i)[2],
                            worst.get(i)[3] >= 0 ? String.format(Locale.US, ", GPU %.0f%%", worst.get(i)[3]) : ""));
                out.append(line).append('\n');
                // GPU busy >= 90% means the GPU set the pace that second; below it the CPU (game + bridge) did.
                if (gpuMeasured > 0) out.append(String.format(Locale.US, "  GPU busy: avg %.0f%%; %d of %d measured seconds at 90%% or more (%d of the slow ones)\n",
                        frames.optDouble("gpuBusyAvg", -1), gpuBound, gpuMeasured, lowGpuBound));
            }
        }
        if (pipelines != null) out.append(String.format(Locale.US, "  Pipelines compiled: %d graphics + %d compute, %.0f ms total, worst %.0f ms, %d over 10 ms\n",
                pipelines.optLong("graphics"), pipelines.optLong("compute"), pipelineTotalMs, pipelineMaxMs, pipelineOver10Ms));
        JSONObject audio = nat == null ? null : nat.optJSONObject("audio");
        if (audio != null) out.append(String.format(Locale.US, "  Audio: xruns %d, buffer %d frames (burst %d), %d Hz\n",
                audio.optInt("xruns"), audio.optInt("bufferFrames"), audio.optInt("burstFrames"), audio.optInt("rate")));
        JSONObject render = nat == null ? null : nat.optJSONObject("render");
        if (render != null) {
            // backBuffer: what the game really rendered (its D3D9 device); width/height: what the launcher asked for.
            org.json.JSONArray back = render.optJSONArray("backBuffer");
            String game = back != null && back.length() == 2 && back.optInt(0) > 0
                    ? String.format(Locale.US, " (game back buffer %dx%d)", back.optInt(0), back.optInt(1)) : "";
            out.append(String.format(Locale.US, "  Render %dx%d%s on display %s\n", render.optInt("width"), render.optInt("height"), game, render.optJSONArray("display")));
        }
        if (samples != null && samples.length() > 0) {
            double headroomMin = Double.NaN, battMin = Double.NaN, battMax = Double.NaN;
            int thermalMax = 0, thermalAt = -1, pssMax = 0, availMin = Integer.MAX_VALUE, cpuSum = 0, cpuCount = 0, lowMemory = 0;
            // Per-cpu clock average and maximum over the samples: the render thread's core is the one that
            // matters, and a session-wide maximum hid that it ran far below its cap.
            long[] clockSum = new long[16]; int[] clockMax = new int[16], clockSamples = new int[16]; int clockCpus = 0;
            Map<String, Integer> refresh = new HashMap<>();
            Integer levelStart = null, levelEnd = null; boolean charging = false;
            int powerSave = 0, powerSaveKnown = 0;
            for (int i = 0; i < samples.length(); i++) {
                JSONObject s = samples.optJSONObject(i);
                if (s == null) continue;
                if (s.optInt("thermal") > thermalMax) { thermalMax = s.optInt("thermal"); thermalAt = (int) s.optDouble("t"); }
                if (s.has("headroom")) headroomMin = Double.isNaN(headroomMin) ? s.optDouble("headroom") : Math.min(headroomMin, s.optDouble("headroom"));
                if (s.has("battTemp")) { double v = s.optDouble("battTemp"); battMin = Double.isNaN(battMin) ? v : Math.min(battMin, v); battMax = Double.isNaN(battMax) ? v : Math.max(battMax, v); }
                if (s.has("battLevel")) { if (levelStart == null) levelStart = s.optInt("battLevel"); levelEnd = s.optInt("battLevel"); }
                charging |= s.optBoolean("charging");
                if (s.has("powerSave")) { powerSaveKnown++; if (s.optBoolean("powerSave")) powerSave++; }
                pssMax = Math.max(pssMax, s.optInt("pssMb"));
                if (s.has("availMb")) availMin = Math.min(availMin, s.optInt("availMb"));
                if (s.optBoolean("lowMemory")) lowMemory++;
                if (s.has("cpuPct")) { cpuSum += s.optInt("cpuPct"); cpuCount++; }
                JSONArray mhz = s.optJSONArray("cpuMhz");
                if (mhz != null) for (int c = 0; c < Math.min(16, mhz.length()); c++) {
                    int value = mhz.optInt(c);
                    clockSum[c] += value; clockSamples[c]++; clockMax[c] = Math.max(clockMax[c], value);
                    clockCpus = Math.max(clockCpus, c + 1);
                }
                if (s.has("refresh")) { String hz = String.valueOf(Math.round(s.optDouble("refresh"))); refresh.put(hz, refresh.getOrDefault(hz, 0) + 1); }
            }
            out.append(String.format(Locale.US, "  Thermal: max %s (status %d) at %d s; headroom min %s; battery %s→%s°C (min %s, max %s), level %s→%s%%%s\n",
                    THERMAL[Math.min(THERMAL.length - 1, thermalMax)], thermalMax, thermalAt, fmt(headroomMin), fmt(batteryStartC), fmt(batteryEndC), fmt(battMin), fmt(battMax),
                    levelStart == null ? "?" : levelStart, levelEnd == null ? "?" : levelEnd, charging ? ", charging" : ""));
            out.append(String.format(Locale.US, "  Memory: game process PSS max %d MB; system free min %s MB; low-memory samples %d\n", pssMax,
                    availMin == Integer.MAX_VALUE ? "?" : String.valueOf(availMin), lowMemory));
            StringBuilder clocks = new StringBuilder();
            for (int c = 0; c < clockCpus; c++)
                if (clockSamples[c] > 0) clocks.append(String.format(Locale.US, " cpu%d %d/%d", c, clockSum[c] / clockSamples[c], clockMax[c]));
            out.append(String.format(Locale.US, "  CPU: game process %s%% of one core on average (cores=%d); clocks avg/max MHz:%s\n",
                    cpuCount == 0 ? "?" : String.valueOf(cpuSum / cpuCount), record.optInt("cores"), clocks.length() == 0 ? " ?" : clocks.toString()));
            if (!refresh.isEmpty()) {
                StringBuilder line = new StringBuilder("  Panel refresh (samples):");
                for (Map.Entry<String, Integer> e : refresh.entrySet()) line.append(' ').append(e.getKey()).append(" Hz×").append(e.getValue());
                if (powerSave > 0) line.append(String.format(Locale.US, "; Battery Saver on in %d of %d samples (it holds the panel at 60 Hz)", powerSave, powerSaveKnown));
                out.append(line).append('\n');
            }
        }
        JSONObject host = nat == null ? null : nat.optJSONObject("host");
        if (host != null) {
            // Where the render thread and DXVK's threads ran, and whether the performance-hint session took them.
            String dxvk = host.optString("dxvk", "");
            out.append(String.format(Locale.US, "  Threads: %s, %s%s; performance hints: %s (%d reports, %d failed)\n",
                    host.optString("affinity", "?"), host.optString("priority", "?"), dxvk.isEmpty() ? "" : "; " + dxvk,
                    host.optString("adpf", "?"), host.optLong("reports"), host.optLong("reportFailures")));
        }
        JSONObject calls = nat == null ? null : nat.optJSONObject("d3d9Calls");
        if (calls != null && frames != null && frames.optLong("frames") > 0) {
            List<Map.Entry<String, Long>> list = new ArrayList<>();
            for (java.util.Iterator<String> it = calls.keys(); it.hasNext();) { String key = it.next(); list.add(new java.util.AbstractMap.SimpleEntry<>(key, calls.optLong(key))); }
            list.sort((a, b) -> Long.compare(b.getValue(), a.getValue()));
            StringBuilder line = new StringBuilder("  D3D9 calls per frame:");
            for (int i = 0; i < Math.min(14, list.size()); i++)
                line.append(String.format(Locale.US, " %s %.0f", methodName(list.get(i).getKey()), list.get(i).getValue() / (double) frames.optLong("frames")));
            out.append(line).append('\n');
        }
        JSONObject methodNs = nat == null ? null : nat.optJSONObject("d3d9Ns");
        if (methodNs != null && frames != null && frames.optLong("frames") > 0) {
            // Bridge + DXVK time per frame by method: what a D3D9-side optimisation would buy.
            List<Map.Entry<String, Long>> list = new ArrayList<>();
            long totalNs = 0;
            for (java.util.Iterator<String> it = methodNs.keys(); it.hasNext();) { String key = it.next(); long ns = methodNs.optLong(key); totalNs += ns; list.add(new java.util.AbstractMap.SimpleEntry<>(key, ns)); }
            list.sort((a, b) -> Long.compare(b.getValue(), a.getValue()));
            double perFrame = 1e6 * frames.optLong("frames");
            StringBuilder line = new StringBuilder(String.format(Locale.US, "  D3D9 time per frame: %.2f ms;", totalNs / perFrame));
            for (int i = 0; i < Math.min(10, list.size()); i++)
                line.append(String.format(Locale.US, " %s %.2f", methodName(list.get(i).getKey()), list.get(i).getValue() / perFrame));
            // How many object lookups the bridge's cache answered (the rest went to its hash table).
            JSONArray lookups = nat.optJSONArray("d3d9Lookups");
            long cached = lookups == null ? 0 : lookups.optLong(0), table = lookups == null ? 0 : lookups.optLong(1);
            if (cached + table > 0)
                line.append(String.format(Locale.US, "; object lookups %.1f%% cached", 100.0 * cached / (cached + table)));
            out.append(line).append('\n');
        }
        JSONObject longCalls = nat == null ? null : nat.optJSONObject("longCalls");
        if (longCalls != null && longCalls.length() > 0) {
            // D3D9 calls the sampler saw last 20 ms or more (call_watch.h): count, average and longest per method.
            List<Map.Entry<String, JSONArray>> list = new ArrayList<>();
            for (java.util.Iterator<String> it = longCalls.keys(); it.hasNext();) {
                String key = it.next(); JSONArray v = longCalls.optJSONArray(key);
                if (v != null && v.length() >= 3) list.add(new java.util.AbstractMap.SimpleEntry<>(key, v));
            }
            list.sort((a, b) -> Double.compare(b.getValue().optDouble(1), a.getValue().optDouble(1)));
            StringBuilder line = new StringBuilder("  Long D3D9 calls (20 ms or more):");
            for (int i = 0; i < Math.min(6, list.size()); i++) {
                JSONArray v = list.get(i).getValue();
                long count = v.optLong(0);
                line.append(String.format(Locale.US, "%s %s %d (avg %.0f ms, longest %.0f)", i == 0 ? "" : ";", methodName(list.get(i).getKey()), count,
                        count > 0 ? v.optDouble(1) / count : 0, v.optDouble(2)));
            }
            out.append(line).append('\n');
        }
        JSONArray vkPresent = nat == null ? null : nat.optJSONArray("vkPresent");
        if (vkPresent != null && vkPresent.length() >= 4 && vkPresent.optLong(0) > 0) {
            // DXVK's presentImage (vkQueuePresentKHR on the submit thread): how long Android took to take each frame.
            out.append(String.format(Locale.US, "  vkQueuePresentKHR: %d presents, avg %.2f ms, longest %.1f ms, %d over 16 ms\n",
                    vkPresent.optLong(0), vkPresent.optDouble(1) / vkPresent.optLong(0), vkPresent.optDouble(2), vkPresent.optLong(3)));
        }
        JSONObject profile = nat == null ? null : nat.optJSONObject("profile");
        if (profile != null && profile.optLong("samples") > 0) {
            // Where the guest machine's time went (1 ms samples): the render thread in lifted game code, in D3D9
            // calls or other imports; other guest threads (sound, loader); nobody (pacer sleep, GPU waits).
            JSONObject renderThread = profile.optJSONObject("render");
            double all = profile.optLong("samples");
            double guest = renderThread == null ? 0 : renderThread.optLong("guest"), d3d9 = renderThread == null ? 0 : renderThread.optLong("d3d9"),
                    imports = renderThread == null ? 0 : renderThread.optLong("import");
            StringBuilder line = new StringBuilder(String.format(Locale.US,
                    "  Guest machine (%d samples): render thread game code %.0f%%, D3D9 %.0f%%, imports %.0f%%; other threads %.0f%%; idle %.0f%%. Top:",
                    profile.optLong("samples"), 100 * guest / all, 100 * d3d9 / all, 100 * imports / all,
                    100 * profile.optLong("other") / all, 100 * profile.optLong("idle") / all));
            JSONArray top = profile.optJSONArray("top");
            for (int i = 0; top != null && i < Math.min(12, top.length()); i++) {
                JSONArray f = top.optJSONArray(i);
                if (f == null) continue;
                long g = f.optLong(1), d = f.optLong(2), m = f.optLong(3);
                line.append(String.format(Locale.US, " sub_%s %.1f%%%s", f.optString(0), 100 * (g + d + m) / all,
                        d > g && d >= m ? " (D3D9)" : m > g && m > d ? " (import)" : ""));
            }
            out.append(line).append('\n');
        }
        JSONArray events = record.optJSONArray("events");
        if (events != null && events.length() > 0) {
            StringBuilder line = new StringBuilder("  Events:");
            for (int i = 0; i < Math.min(40, events.length()); i++) {
                JSONObject e = events.optJSONObject(i);
                if (e == null) continue;
                String detail = e.optString("detail", "");
                if (detail.length() > 60) detail = detail.substring(0, 57) + "...";
                line.append(String.format(Locale.US, " [%.0f s %s%s]", e.optDouble("t"), e.optString("event"), detail.isEmpty() ? "" : ": " + detail.replace('\n', ' ')));
            }
            out.append(line).append('\n');
        }
        return out.toString();
    }

    static String methodName(String key) {
        int slash = key.indexOf('/');
        if (slash < 0) return key;
        try {
            int kind = Integer.parseInt(key.substring(0, slash)), method = Integer.parseInt(key.substring(slash + 1));
            if (kind == 2 && DEVICE_METHODS.containsKey(method)) return DEVICE_METHODS.get(method);
            if (method == 0) return "QueryInterface(" + kind + ")";
            if (method == 1) return "AddRef(" + kind + ")";
            if (method == 2) return "Release(" + kind + ")";
        } catch (NumberFormatException ignored) { }
        return key;
    }

    private static String fmt(double value) { return Double.isNaN(value) ? "?" : String.format(Locale.US, "%.2f", value).replaceAll("\\.?0+$", ""); }
}
