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
- [ ] Background the game and come back (the sound returns at once, even after minutes away); rotate the phone; play 10 minutes — no crash.
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
*Install mod pack (ZIP)* in the launcher takes a ZIP of game‑relative files (one wrapping folder is fine) and installs it over Internal storage/nfsu1 with every replaced file kept under `nfsu1/.nfsu-modpacks/<id>/`; *Mod packs* lists what is installed and removes a pack (and everything installed after it, so originals always come back). Packs stack in install order. Programs, scripts and Speed.exe inside a ZIP are never written. The HD packs from the private `hdpack-v*` releases install this way (v10: interface and cars; v11: the city's roads, buildings and scenery in all 20 track streams, the 31 race tables that place them, and the skies — about 1 GB installed, the stock files kept for removal; v12: Plajer's HD User Interface 1.8 and HD Decals 1.3 laid over both, about 310 MB more, needs build 14 or newer, which the launcher checks). Free roam is built in since build 19 (Quick Race › Free Run, the three tracks after the eight circuits), so the Free Roam Mod Beta 2.0 ZIP is no longer needed; installed as a pack, its files are not used while **Free roam** is on.

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
- `Render thread to cpu 5-6, allowed … MHz (speed …), from cpu 7 allowed … MHz (speed …); DXVK threads on …` — the game thread moved because the X4's allowed clock fell (or came back). Since build 15 the X4's speed counts 1.5 times its kernel figure per clock, so a move needs the other cores allowed 1.8 times the X4's clock: on the Pixel only deep throttling moves it. No such line in a long session means the caps never favoured a move, or the phone doesn't show them: the summary's *allowed* clocks tell which.
- `Placement of render: renamed … still followed`, `… no longer followed: …`, `… followed again` — the keeper's view of a placed thread: a new name (the same thread), a drop with its reason, and the game thread followed again from its own next frame (build 15).
- `Render thread to cpu 2-4: allowed 2188 MHz, ran at 100 % of it (speed 591); from cpu 7: allowed 1305 MHz, ran at 76 % of it (speed 404)` — a move (build 16): each group's allowed clock, the share of it it gave the game thread as measured, and the speed that makes. `Command stream to …, which the render thread took` — DXVK's command stream following the fastest other group.
- `PMU frequency limit (sched_pixel): …` — what of Pixel's memory‑stall frequency limit the app can read (each group's `limit_frequency`, `spc_threshold`, `lcpi_threshold`, and `pmu_poll_enable`), or `not readable (…)`. `Thermal status 1 -> 0: …` — the measured shares starting over. `Clock measurement: … time_in_state does not follow its clock` — the kernel's time per clock found stale for a group, which is then measured by its momentary clock.
- `Sound output lost (…): …` then `Sound output replaced: a new stream after … ms without sound` — Android took the sound stream (in the background, or headphones in or out) and a new one plays. `Sound output: no stream (…), attempt …` — an open that failed and when the next try is due; the game's sound buffers keep moving meanwhile. The summary's *Audio* line counts them.
- `Garage: on: everything unlocked, special vinyls listed, …; off: none; unlock-everything byte 1` — the Garage switches the session started with (build 17), and the game's own unlock byte after boot. The summary's *Garage* line counts the game decisions each switch changed; `… jumps reached with no rule` there means `config/branches.txt` and `nfs_garage.h` disagree.
- `Rim sizes: the wheel pack (492 parts, 1476 solids added, 41762352 bytes) served for Cars/WHEELS/GEOMETRY.BIN, made earlier, checked in 3 ms` — every rim size on (build 18), the pack from the app's folder; `made in … ms` the first time, or after the game's wheel or GlobalB files change. `; every rim size is off, kept for the save …` when a save wears an added size. `Rim sizes: the wheel pack is not served (…)` says why (a modded wheel file of another layout, say); the rim browser then offers the sizes the game has. The summary's *Rim sizes* line counts the lookups the host answered and those that found an added size, and in the rim browser the thumbnails shared, the lists sorted and the browsers opened on the car's rim.
- `Free roam: Olympic City (1000) in Free Run, made earlier, checked in 0 ms; map 2048x2048 over 3265 m from 16 race maps and 57869 road polygons` — one line per free roam track (build 19): offered, its files made at this start (`made in … ms`, the first time or after the game's files change) or found from an earlier one, and its map. `… not offered: …` says why a track is missing from Free Run (a base race's section table that differs from the mod's in more than where the sections lie, say); `no map (…): the minimap stays off` when the map could not be made. The summary's *Free roam* line counts the free roam sessions, breadcrumbs and resets (to a breadcrumb, or left to the game when there was none yet). `Free roam: SELECT LOCATION pictures for 3 tracks (4 textures added) served for Global/DYNTEX.BIN, …` follows them: the menu's map of each free roam track (the game's dynamic texture pack with theirs added); `no SELECT LOCATION pictures (…)` when that pack could not be read, and only their picture there stays black.
- `Effect states: render, sampler and texture stage states of the game's effects applied by the host (already set ones skipped)` — build 17's default (*Effect states: Native*); `applied by the game's own D3DX code` with *Game*. At the end, `Effect states applied by the host this session: … already set, … sent to DXVK`. The summary's *Effect states* line gives the states a frame and the share the host took.
- `Slow frame 102 ms: sub_00674898 guest 34%, sub_005A8640 guest 26%, …` — a frame of 80 ms or more and what the render thread did during it (the guest function, `D3D9` for a call into DXVK, `import` for a host import such as a file open since build 20), the share of time other guest threads held the machine and the time nobody did. `Hitch while driving 41 ms: …` (build 20) — the same for frames from 34 ms while a race has the driving controls (at most one a second): the short hitches of racing.
- The summary's *Files* line (build 20): the files the game opened and their average time, the path lookups its file layer made and the share answered from the folder listings kept (`guest_paths.h`), the listings read and the folder changes that made them be read again. On the phone's shared storage a listing is a round trip to Android's media daemon; nearly every lookup should be kept. *Layer blend (car textures)*: the pixels of car textures composited by the host (all of them, normally).
- `ADPF: session with 5 threads, target 12.50 ms` — the performance hint aims at three quarters of the frame period (60 FPS cap: 12.5 ms), so frames end with time to spare.
