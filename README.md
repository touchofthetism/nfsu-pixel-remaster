# Need for Speed Underground — Remastered for the Pixel

A personal fork of [codepdbh/nfsu1-recompiled](https://github.com/codepdbh/nfsu1-recompiled): the 2003 PC game
statically recompiled (`Speed.exe` 1.4 → C → ARM64) and rendered through DXVK native 1.9.2b on Vulkan, tuned
for a **Pixel 10 Pro XL** and built entirely on GitHub Actions. The game stays the classic game; the phone
gets what a remaster would: native 2992×1344, 120 FPS, correct widescreen HUD, 16× anisotropic textures,
optional MSAA, an English launcher, one-tap game import and one-tap diagnostics.

Toolchain pins (`ci/pins.env`) are read by Gradle and CI alike; the build fails rather than drift.

Upstream's README (build on Windows, the lift pipeline, credits) is kept in [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md).
Install and test notes for the phone: [`docs/PIXEL.md`](docs/PIXEL.md).

## How a build happens

Two repositories: this one holds the code and runs checks; a **private companion repository** holds the
game inputs and builds/publishes the APK (game data never sits next to public code).

```
this repo: push to main ──► check.yml  (minutes)   Java compile + unit tests, host C++ syntax with the NDK, host tests

private repo: gamedata.yml (once) ──► fetches the owner's game copy, verifies Speed.exe 1.4,
                                        publishes lift-inputs.zip + nfsu-gamedata.zip as private release assets
private repo: push to BUILD ───────► build.yml  (~1–2 h)
      1. check out this repo at the commit named in BUILD
      2. clone the pinned siblings (ci/pins.env): nfsu2-recompiled, pcrecomp @35548b8 + 14 patches, dxvk-native
      3. ci/lift.sh: seeds → catalog → code seeds → catalog → lift Speed.exe + the .abk sound banks to C
      4. prepare DXVK + generate its shaders; Gradle assembleRelease (ThinLTO, ccache)
      5. publish release vX.Y.Z-build.N there, with the signed ARM64 APK and the unstripped symbols
```

Nothing game-derived is ever committed anywhere; `work/` and `src/recomp/gen*/` stay local to a runner.

## What this fork changes

| Area | Change |
|---|---|
| Launcher | English UI with Spanish kept as `values-es`; game language follows the phone unless chosen in the launcher; **Import game files (ZIP)** (verifies `Speed.exe` by size and SHA-256, swaps folders in atomically); **Share diagnostics** (ZIP via the share sheet, includes the DXVK state cache); update check that says plainly when releases are private, release notes bundled in the APK |
| Defaults | Native resolution on phones reporting ≥ 7.5 GiB RAM (the 8 GB class and up; 75 % otherwise), 120 FPS, widescreen on, 16× anisotropic |
| Graphics | **Texture filtering** → `dxvk.conf` (`d3d9.samplerAnisotropy`) handed to DXVK through `DXVK_CONFIG_FILE`; **Anti-aliasing** → validated MSAA on the back buffer at `CreateDevice`/`Reset` (`android/app/src/main/cpp/graphics_quality.h`, `d3d9_bridge.cpp`) |
| Telemetry | Per-session log file (logcat follower on the game process) and session record: frame-time histogram/percentiles/hitches at Present (`frame_telemetry.h`), DXVK pipeline-compile counters and info logs (`ci/dxvk_extra.py` on the prepared DXVK), audio underruns, D3D9 call profile, 2 s device samples (thermal, battery, memory, clocks, refresh rate) — `GameTelemetry.java`; readable `SessionSummary`; **Last session** card; **Performance overlay** (compact text or DXVK HUD); diagnostics ZIP with logs, records, summaries, caches, `dxvk.conf`, `Save/` |
| Caches | Bundled DXVK state cache + driver pipeline cache from the target phone (`assets/graphics-cache`), merged into the device cache before PLAY (`ShaderCacheSeed.java`, format-level merge by entry hash) |
| Game graphics | **Game graphics** option (`GameConfig.java`): the game's top performance tier written into every profile's `.cfg` before launch (its auto‑detect had the port on a low tier with reflections, streaks and blur off); values and maxima read from Speed.exe's own settings table |
| Saves | **Back up saves / Restore saves** (`SaveBackup.java`): ZIP of the profile folder (`nfsu1/NFS Underground`) to Download/NFSU1 and an atomic restore |
| Mod packs | **Install mod pack (ZIP)** / **Mod packs** (`ModPacks.java`): game‑relative files over nfsu1 with originals kept per pack, registry, reverse‑order removal; `tools/nfstex` builds the HD interface and cars packs (texture packs parsed and rebuilt byte‑identically, Real‑ESRGAN upscale, DXT3/DXT1 mip chains, every JDLZ blob checked against an exact emulation of the game's decompressor before packing) and the private `hdpack.yml` publishes them (current: interface v6, cars v7) |
| PC fixes | `src/runtime/misc_fixes.c`: ThirteenAG's 44.1 kHz audio (the game opens its mixer at 22.05 kHz at performance tiers 0–1) and AeroWidescreen's HD Cursor Support (the front‑end arrow is sized from its texture, so the HD 64×64 cursor is drawn at 16×16). ThirteenAG's bMalloc/bFree lock is deliberately not ported: the race it fixes needs two threads inside the allocation‑list update at once, and here guest threads switch only at loop back edges, imports and waits, none of which lie inside it (reasoning at the top of the file) |
| Frame rate | `src/runtime/framerate.c` + `config/hooks.txt`: the game paces itself to 60 FPS with a busy‑wait on QueryPerformanceCounter and clamps its time step to 1/60; its limiter now targets the **FPS limit** option (Unlimited = the 120 Hz panel) and ThirteenAG's high‑frame‑rate corrections (time‑step floor, tick carry‑over, audio update batching, particle step, steering snap) are ported as hooks, so 120 FPS runs at the right speed |
| Performance | `host_perf.cpp`: render guest thread on the fastest cluster, nice −10, ADPF performance‑hint session fed with each frame's CPU work; bridge fast path (device handle cache, redundant render/sampler/texture‑stage states answered locally); **Loader reads** option (`guest_files.inc`: reads of 64 KB and more hand the guest machine to the other guest threads while the kernel copies, so stream and loader reads no longer stall the render thread); **CPU placement** and **Wrong‑way warning** options |
| Native | English runtime messages; `nativeGraphicsQuality`, `nativeGraphicsHud`, `nativeTelemetryJson` JNI entries |
| Packaging | `applicationId com.nfsu1.remastered` (installs beside the upstream app), stable signing key, versionCode grows with the CI run |
| CI | `.github/workflows/check.yml` here; `build.yml` + `gamedata.yml` in the private companion; `ci/` scripts; host unit tests for the quality policy and frame telemetry; JUnit tests for options, versions, the importer, the cache merge, save backups and session summaries |

## Profile-guided optimisation (PGO) round
The lifted game code is the frame's biggest cost, and it is branchy enough that a profile helps the compiler more than flags do.
1. Private `BUILD`: `pgo=generate` → an instrumented APK (`…-pgo` versionName) that writes `files/pgo/game-*.profraw` on pause/exit.
2. Play a long session (front end, several races, a drift event), then **Share diagnostics**: the ZIP carries `pgo/*.profraw`.
3. `tools/pgo/merge_profile.sh -o nfsu1.profdata <diagnostics ZIP>...` (the pinned NDK's `llvm-profdata`; several ZIPs merge into one profile and the script prints the hottest lifted functions), commit it as `pgo/nfsu1.profdata` in the private repo, set `pgo=use`.
Profiles stay valid while the lift inputs and scripts are unchanged (the function set is the same); a changed lift just loses coverage with a warning, never correctness.

## Working on it

Pins live in `ci/pins.env`. Fast feedback without a device:

```bash
bash ci/setup_siblings.sh      # once: siblings + prepared DXVK (needs glslangValidator, spirv-val)
bash ci/syntax_check.sh        # host C++ against the NDK, same flags as the app
bash ci/host_tests.sh          # tests/*.cpp
cd android && ./gradlew :app:testReleaseUnitTest
```

A full local build follows upstream's README with `ci/lift.sh <dir with Speed.exe and Sound/>` in place of the
hand-run lift commands, then `./gradlew assembleRelease -PbuildNumber=0`.

## Credits

Everything that makes this run is upstream's work: the **nfsu1-recompiled** / **nfsu2-recompiled** projects
(static recompilation, Android runtime, DXVK Android recipe, widescreen port), **pcrecomp**, **DXVK native**,
**libadrenotools**, and ThirteenAG's WidescreenFix patterns. This fork only adds polish for one phone.
Need for Speed and Underground are trademarks of Electronic Arts; this is an unofficial, non-commercial,
personal build. No game data is distributed.
