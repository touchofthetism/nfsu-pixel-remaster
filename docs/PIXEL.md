# Pixel 10 Pro XL — install, test, report

## What the phone is
Tensor G5 with an Imagination **PowerVR** GPU (not Mali, not Adreno), 2992×1344 @ 120 Hz, 16 GB RAM. The port had not been reported on a PowerVR phone before this build, so the first session is a validation run as much as a game session. The DXVK recipe already enables PowerVR-specific fallbacks (sample-rate shading only when supported, CPU decoding of BC textures when the GPU lacks them). Custom Vulkan drivers (Turnip/PanVK) do not apply to this GPU: keep **Vulkan driver: System**.

## Install
1. On the phone, open the **private companion repository's Releases** page (signed in to GitHub) and download the newest `NFSU-Remastered-…-arm64.apk`. Open it; allow Chrome to install apps once.
2. Download `nfsu-gamedata.zip` from its **gamedata** release (≈1 GB) — or leave the game ZIP in Drive; the launcher can read it from there.
3. Open the app → **Allow access to Internal storage/nfsu1** → turn on *All files access* → back.
4. **Import game files (ZIP)** → pick the zip. Progress shows in MB; the launcher verifies `Speed.exe` is the stock 1.4 build (size and SHA-256 — modified or no-CD executables are refused, since the recompiled code only matches the original) and that the game folders are present, then swaps the folders in so a failure half way leaves the previous install intact.
5. **PLAY**.

Later builds install straight over the previous one (same signing key, higher versionCode); saves and settings stay.

## First-session checklist
- [ ] Launcher is in English; **Game language** lists the languages found in the copy.
- [ ] Intro movies play; the front end appears; profile creation works (keyboard button for text).
- [ ] A race starts and runs; steering/pedals respond; a Bluetooth pad works (RT gas, LT brake).
- [ ] Native resolution — the phone's own screen resolution, 2404×1080 or 2992×1344 (the summary's *Render* line shows the game back buffer at that size) — at the default **60 FPS** limit: smooth after the first minute (first-session shader compiles are normal). 120 is for a cool phone.
- [ ] HUD, mirror, minimap sit correctly in the 20:9 picture; no stretching.
- [ ] Cars show reflections; textures stay sharp at grazing angles (Texture filtering 16×).
- [ ] Turn **Anti-aliasing** to 4× MSAA, play one race: smoother edges, no visual breakage, framerate holds. If anything breaks, set it back to Off and say so in the report.
- [ ] Background the game and come back; rotate the phone; play 10 minutes — no crash.
- [ ] **Share diagnostics** → send the ZIP (Drive is fine).

## What the diagnostics ZIP holds
- `report.txt` — device, RAM, thermal state, display modes, options and their defaults, the game folder listing (unexpected entries listed one level deep), and the crash log: every crash Android still keeps, marked *THIS BUILD* or *older build* (Android keeps crashes across installs).
- `session-summary.txt` — every recorded session in readable lines: frames, average FPS, 1 % low, p50/p99/max, hitches and stalls, pipeline compiles, audio underruns, thermal/battery/memory/CPU ranges (each core's clock and the clock it was allowed), where the game thread ran and the frame rate it reached on each group of cores, refresh-rate changes, D3D9 calls per frame, long D3D9 calls, DXVK's queue, events. A session whose record was never closed says what ended it (swiped away, crashed, closed for memory). Read this first.
- `telemetry/session-*.json` — the full records those lines come from (histogram, per-second FPS and worst frame, 2 s device samples).
- `logs/game-*.log` — the complete log of each game process (NFSU1 runtime, DXVK at info level, crashes); `logs/tail-*.txt` — the log buffer as the launcher saw it right after each game.
- `logcat-now.txt`, `exits.txt`, native crash/ANR traces if any.
- `graphics-cache/` — DXVK state cache and the driver pipeline cache (the next build ships them as its seed).
- `dxvk.conf`, and `save/` — the small `nfsu1/Save` folder (options, career), skipped above 4 MB. No game data files are ever included.

## Mod packs
*Install mod pack (ZIP)* in the launcher takes a ZIP of game‑relative files (one wrapping folder is fine) and installs it over Internal storage/nfsu1 with every replaced file kept under `nfsu1/.nfsu-modpacks/<id>/`; *Mod packs* lists what is installed and removes a pack (and everything installed after it, so originals always come back). Packs stack in install order. Programs, scripts and Speed.exe inside a ZIP are never written. The HD packs from the private `hdpack-v*` releases install this way (v10: interface and cars; v11: the city's roads, buildings and scenery in all 20 track streams, the 31 race tables that place them, and the skies — about 1 GB installed, the stock files kept for removal); so does the Free Roam Mod Beta 2.0 ZIP from nfsmods (set **Wrong‑way warning** to Off for free roam).

## The data loop
1. Play normally (career and quick race; a long session is better than a short one). Leave the overlay off unless you want to watch it; it does not change what is recorded.
2. Back in the launcher the **Last session** card shows the numbers; tap it for the full summary.
3. **Share diagnostics** → Drive. The next build is tuned from that ZIP and seeds its shader cache from it.

## Reading the log quickly
- `GPU … ; API …` — the Vulkan device DXVK ended up on.
- `D3D9 backbuffer WxH CreateDevice result=00000000` — the device came up at the chosen internal resolution.
- `Anti-aliasing: Nx MSAA requested, Mx usable` — what the GPU accepted.
- `Graphics quality: anisotropy 16 (…/dxvk.conf)` — texture filtering forced through DXVK.
- `CPU DXT fallback texture` — the GPU lacks BC formats; textures are decoded on the CPU (expected on some PowerVR drivers; costs load time, not frame time).
- `D3D9 CreateCubeTexture … status=` — reflection cubemaps; a failing status explains missing car reflections.
- `E DXVK` lines — DXVK errors; the first one after `CreateDevice` is usually the story.
- `CPU topology: … clusters: cpu 0-1 (capacity …), …` — the groups of cores sharing a clock and the kernel's capacity for each (1024 = the X4 at its top clock): what the placement works from. `(estimated)` means the kernel didn't say.
- `Render thread to cpu 5-6, allowed … MHz (speed …), from cpu 7 allowed … MHz (speed …); DXVK threads on …` — the game thread moved because the X4's allowed clock fell (or came back). No such line in a long session means the caps never favoured a move, or the phone doesn't show them: the summary's *allowed* clocks tell which.
