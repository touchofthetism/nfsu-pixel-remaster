## Install on the Pixel

1. Download the `NFSU-Remastered-…-arm64.apk` asset below on the phone and open it (allow installs from Chrome once).
2. Open **NFS Underground Remastered** → **Allow access to Internal storage/nfsu1** → confirm "All files access".
3. **Import game files (ZIP)** → pick `nfsu-gamedata.zip` (from the private *gamedata* release, downloaded on the phone, or straight from Drive). The launcher unpacks it into place and verifies `Speed.exe`.
4. **PLAY.** After a session, **Share diagnostics** sends the log ZIP wherever you like.

## What's new

### Build 2 — the measuring build
- **Session recorder.** Every game session now writes its own complete log (`files/logs`, a logcat follower on the game process: the shared buffer had already lost the first report's game run) and a telemetry record (`files/telemetry`): frame-time distribution and 1 % lows, hitches/stalls, time inside Present, DXVK pipeline compiles (count and worst case), audio underruns, the D3D9 call profile, and a device sample every 2 s — thermal status and headroom, battery temperature, memory, CPU clocks and the panel's live refresh rate. Flushed every 30 s, so a crash keeps everything up to its last half minute.
- **Last session** card in the launcher: duration, average FPS, 1 % low, hitches, peak thermal state; tap for the full readable summary.
- **Share diagnostics** now packs the session summaries, the logs, the telemetry records, the graphics caches, `dxvk.conf`, the device state and the small `nfsu1/Save` folder (never game data).
- **Performance overlay** option: *Compact* (FPS, p99 frame time, thermal state, battery °C, top-right) or *Full* (adds DXVK's HUD: frame-time graph, GPU load, pipelines, memory).
- **Graphics log detail** option: *Verbose* makes DXVK log every shader compile for a one-off deep look.
- DXVK's information log (adapter, driver version, state-cache entries, swapchain rebuilds) now reaches the log; it used to go to a stderr nobody could read.
- **Shader cache seeded from your phone**: the state cache and the driver pipeline cache from the first Pixel 10 Pro XL session ship in the APK and are merged into the device cache before PLAY, so pipelines seen before are compiled ahead of their first frame. Each diagnostics ZIP you send grows the next build's seed.
- **Back up saves / Restore saves**: `nfsu1/Save` to a ZIP in Download/NFSU1 (share it to Drive) and back, atomically.
- Original app icon and launcher mark.
- Fix: the report's `logcat` only covered the launcher process; the launcher also captures the log tail the moment it returns from the game.

### Build 1

- English launcher and in-game dialogs (Spanish kept as a locale); the game's text follows the phone's language.
- Pixel-class defaults: native resolution on phones with about 8 GB of RAM or more, 120 FPS cap, widescreen HUD on.
- **Texture filtering** option: 8×/16× anisotropic filtering across the whole game (default 16×).
- **Anti-aliasing** option: 2×/4× MSAA on the game's back buffer, validated against the GPU, off until you turn it on.
- **Import game files**: unpack the game ZIP directly from Drive/Downloads into Internal storage/nfsu1; `Speed.exe` is checked by size and SHA-256 and folders are swapped in atomically.
- **Share diagnostics**: one-tap log ZIP (log, device, options, exit records, graphics cache) through the share sheet.
- Signed with a stable key and a growing versionCode: every build installs over the previous one.
- Built from source on GitHub Actions: Speed.exe 1.4 → C → ARM64, DXVK native 1.9.2b on Vulkan.
