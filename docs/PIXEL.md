# Pixel 10 Pro XL — install, test, report

## What the phone is
Tensor G5 with an Imagination **PowerVR** GPU (not Mali, not Adreno), 2992×1344 @ 120 Hz, 16 GB RAM. The port had not been reported on a PowerVR phone before this build, so the first session is a validation run as much as a game session. The DXVK recipe already enables PowerVR-specific fallbacks (sample-rate shading only when supported, CPU decoding of BC textures when the GPU lacks them). Custom Vulkan drivers (Turnip/PanVK) do not apply to this GPU: keep **Vulkan driver: System**.

## Install
1. On the phone, open the **private companion repository's Releases** page (signed in to GitHub) and download the newest `NFSU-Remastered-…-arm64.apk`. Open it; allow Chrome to install apps once.
2. Download `nfsu-gamedata.zip` from its **gamedata** release (≈1 GB) — or leave the game ZIP in Drive; the launcher can read it from there.
3. Open the app → **Allow access to Internal storage/nfsu1** → turn on *All files access* → back.
4. **Import game files (ZIP)** → pick the zip. Progress shows in MB; the launcher verifies `Speed.exe` is the 1.4 build and that the game folders are present.
5. **PLAY**.

Later builds install straight over the previous one (same signing key, higher versionCode); saves and settings stay.

## First-session checklist
- [ ] Launcher is in English; **Game language** lists the languages found in the copy.
- [ ] Intro movies play; the front end appears; profile creation works (keyboard button for text).
- [ ] A race starts and runs; steering/pedals respond; a Bluetooth pad works (RT gas, LT brake).
- [ ] Native resolution at **120 FPS**: smooth after the first minute (first-session shader compiles are normal).
- [ ] HUD, mirror, minimap sit correctly in the 20:9 picture; no stretching.
- [ ] Cars show reflections; textures stay sharp at grazing angles (Texture filtering 16×).
- [ ] Turn **Anti-aliasing** to 4× MSAA, play one race: smoother edges, no visual breakage, framerate holds. If anything breaks, set it back to Off and say so in the report.
- [ ] Background the game and come back; rotate the phone; play 10 minutes — no crash.
- [ ] **Share diagnostics** → send the ZIP (Drive is fine).

## What the diagnostics ZIP holds
`report.txt` (device, RAM, options and their defaults, game folder listing), `logcat.txt` (this app's log: NFSU1 runtime, DXVK warnings/errors, crashes), `exits.txt` plus native crash/ANR traces if any, and `graphics-cache/` (DXVK state cache — lets the next build precompile shaders so the first session doesn't stutter). No game files or saves are included.

## Reading the log quickly
- `GPU … ; API …` — the Vulkan device DXVK ended up on.
- `D3D9 backbuffer WxH CreateDevice result=00000000` — the device came up at the chosen internal resolution.
- `Anti-aliasing: Nx MSAA requested, Mx usable` — what the GPU accepted.
- `Graphics quality: anisotropy 16 (…/dxvk.conf)` — texture filtering forced through DXVK.
- `CPU DXT fallback texture` — the GPU lacks BC formats; textures are decoded on the CPU (expected on some PowerVR drivers; costs load time, not frame time).
- `D3D9 CreateCubeTexture … status=` — reflection cubemaps; a failing status explains missing car reflections.
- `E DXVK` lines — DXVK errors; the first one after `CreateDevice` is usually the story.
