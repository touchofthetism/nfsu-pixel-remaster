## Install on the Pixel

1. Download the `NFSU-Remastered-…-arm64.apk` asset below on the phone and open it (allow installs from Chrome once).
2. Open **NFS Underground Remastered** → **Allow access to Internal storage/nfsu1** → confirm "All files access".
3. **Import game files (ZIP)** → pick `nfsu-gamedata.zip` (from the private *gamedata* release, downloaded on the phone, or straight from Drive). The launcher unpacks it into place and verifies `Speed.exe`.
4. **PLAY.** After a session, **Share diagnostics** sends the log ZIP wherever you like.

## What's new

### Build 4 — the game's own graphics at full tier, and the profile build
- **Game graphics: Maximum (default).** The game keeps its graphics settings per profile in `NFS Underground/<profile>.cfg`, and its 2003 auto‑detect scored the port's "hardware" into a low tier: car and world reflections, the wet‑road reflection, fog, headlights, depth of field, motion blur and light streaks were all off, car detail low. Before every launch the launcher now writes the game's top tier into every profile — the values Speed.exe initialises with before the auto‑detect lowers them (`PERFORMANCE 3, ENVMAP 3, SHADOW 2, ROADREFLECTION 2, WorldLOD 3, CarLOD 1`, every effect on); resolution, colour depth and sound lines are left alone. *As set in the game's own options* leaves the files untouched.
- Build 4 handed the guest machine over during reads of 64 KB and more (to stop loader and stream reads from stalling the frame); with it the sound library's service thread ran during the profile's car‑texture load and called through an unknown heap pointer on every launch (`ICALL unresolved 0x12c2ea1c from 0x652960`). Reads are atomic again as in build 3; unresolved calls now report the target bytes and the caller's registers so the next occurrence is identifiable.
- **Shader cache seed** from a 27‑minute build‑3 session (DXVK state cache and Vulkan pipeline cache), so a fresh install starts with every pipeline that session needed.
- **HD cars pack** (private `hdpack-v4`, installs on top of the interface pack): every `Cars/*/TEXTURES.BIN` — head‑ and brake‑light lenses, badges, grilles, wheels, interiors — small maps 4×, 256‑px ones 2×; textures the game stores uncompressed stay uncompressed (A8R8G8B8) so light‑glow alpha keeps its 8 bits.
- **HD interface pack v3**: streaming packs (HUDTEX, TSELECT) keep their per‑texture JDLZ blobs and the table the engine walks when unloading them; v1/v2 crashed on the first post‑race *Continue* for want of it.
- Build `…-pgo` of this version is instrumented (`NFS_GUEST_PGO=GENERATE`): it plays a little slower and writes `files/pgo/*.profraw` on pause and exit, shipped in *Share diagnostics*. The optimised build that follows is compiled from that profile.

### Build 3 — the fast build
- **The game thread runs on the big core.** Build 2's telemetry showed the race at 45–50 FPS with the game's single busy thread parked on a mid core at ~2.7 GHz while the Cortex‑X sat idle. The render guest thread is now pinned to the fastest cluster, raised in priority, and registered in an Android performance‑hint (ADPF) session with the frame cap as its target, so the governor keeps the clock up. **CPU placement** option: *Fastest core + performance hints* (default) or *System scheduler*.
- **Fewer D3D calls reach DXVK.** A race frame issues ~45,000 device calls; render, sampler and texture‑stage states that repeat the current value are now answered in the bridge, and the device handle is resolved without a hash lookup.
- **Mod packs.** *Install mod pack (ZIP)* puts game‑relative files (Global/, Tracks/, FrontEnd/, Cars/…) over Internal storage/nfsu1, keeps every replaced original, and *Mod packs* removes them again in reverse order. This is how the HD interface pack and community data mods such as the Free Roam Mod Beta 2.0 install; programs and Speed.exe inside a ZIP are ignored.
- **HD interface pack (v2).** Built by `tools/nfstex` from the game's own textures with Real‑ESRGAN: every gauge cluster 4× (128 → 512 px), the in‑game HUD and the front end 2–4×, the 159 track‑select previews 2× (512×256 → 1024×512), the front‑end world textures 2× up to 512 px — 1,015 textures, DXT3/DXT1 with full mip chains, bundles rewritten uncompressed under the names the engine opens first (`GLOBAL\INGAMEB.LZC`, `FRONTEND\FRONTB.LZC`). Magazine covers stay stock (164 MB at 2× for a screen seen once per win). Installs through *Install mod pack (ZIP)*; about 215 MB of bundle data resident in the front end (34 MB of it in races), which the engine's growable pools take from the port's 1 GB guest arena.
- **Wrong‑way warning** option (off for free roam): blanks the HUD's wrong‑way elements the way the Free Roam Mod's own patcher does.
- Saves: the game's profile folder is `nfsu1/NFS Underground` (not `Save`); backup, restore and the diagnostics report now use it.
- Telemetry: *spikes* (frames 2.5× slower than their neighbours) replace cap‑relative hitches on the Last session card, which were meaningless while the cap was out of reach; the session record carries the thread placement result.
- Fix: the game process crashed on exit in build 2 (the session recorder raced its own sampler); the record is now written under lock after the sampler stops.

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
