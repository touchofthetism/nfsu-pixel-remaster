# Linux test host

The phone's runtime — `guest_runtime.cpp`, the D3D9 bridge, DXVK, the recompiled game — built for x86-64 Linux, so a
feature can be played end to end on the build machine before an APK goes to the phone. DXVK presents into an X11
window (Xvfb in a container, Mesa's software Vulkan driver lavapipe), sound is mixed and pulled in real time by a
stand-in for AAudio (`tests/fake_aaudio.h`), and a timeline script presses keys and takes screenshots. Nothing here
ships in the APK.

It runs the owner's game folder exactly as the phone does: the same served files (every rim size's wheel pack, free
roam's tracks and their SELECT LOCATION pictures), the same hooks and fast paths, the same telemetry (`telemetry.json` at the end). Expect 8–30 FPS on
lavapipe with two cores: the game's clock is real time, so menus and races behave as on the phone, only choppier.

## Build

```sh
ci/lift.sh <dir with Speed.exe and Sound/>     # once: the recompiled game in src/recomp/gen (gitignored)
cmake -S tools/testhost -B build-testhost -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build-testhost nfsu-testhost           # the lifted units take a while at -O1 (two compile at a time)
```

Needs `../pcrecomp` and `../nfsu2-recompiled` (with the prepared DXVK source) as for the APK (`ci/setup_siblings.sh`),
and Xvfb, ImageMagick's `import` and Mesa's Vulkan drivers (`mesa-vulkan-drivers`).

After a change to `config/hooks.txt`, `config/fastpaths.txt` or `config/branches.txt`, lift again into a scratch folder
and copy only the files that changed, so ninja rebuilds what it must:
`python3 scripts/run_lift.py lift work/Speed.analysis.exe work/catalog.json work/gen_new` (about 80 s).

## Run

```sh
tools/testhost/run.sh build-testhost --game ~/testgame/stock --generated /tmp/gen --out /tmp/run \
    --script tools/testhost/scripts/free_roam_list.txt --seconds 90 [--freeroam DIR] [--garage BITS] [--size 1280x720] [--fps 60]
```

- `--game`: a game folder with `Speed.exe` 1.4 (a hard-linked copy is fine: nothing writes to the game's files except
  saves and options). An HD pack is tried by extracting it over another hard-linked copy, each file unlinked first.
- `--generated`: the folder for data the host makes (the phone's `files/generated`).
- `--freeroam`: a folder with the Free Roam Mod Beta 2.0's `TRACKB1000/1300/1400.lzc` (the phone's `files/freeroam`).
- `run.sh` starts Xvfb on a free display, picks lavapipe, and raises `vm.max_map_count`: lavapipe maps memory for every
  descriptor set, DXVK keeps thousands of them, and past Linux's default limit (65,530 mappings) a failed mapping
  crashes lavapipe inside `vkAllocateDescriptorSets` (seen at the first dialog that blurs the menu). Without root it
  warns instead.
- DXVK's own log (warnings) goes to the `--out` folder, its state cache to `--generated` (kept between runs, as
  the phone keeps its).
- `TESTHOST_WRAP="gdb -batch -x cmds.txt --args"` runs it under a debugger.
- `NFS_VERIFY_FASTPATHS=1` runs the game's own code beside a fast path that computes a result by itself (the car
  textures' layer blend, `src/runtime/nfs_blend.h`): both on every call, the game's state kept, each difference
  counted (`layerBlend` in `telemetry.json`) and the first ones logged (`Layer blend differs from the game's`).

## Scripts

One event a line, at seconds from the start (`#` starts a comment):

```
12.5 key ENTER tap      press, then release once the game has read the press (DIK name or 0x1c)
13   key UP down        hold ...
20   key UP up          ... and release
21   shot menu          <out>/menu.png
25   log some text      a marker in the log
30   peek race 0x777CC8 2   two dwords of guest memory to the log; [0x736174]+0x6C reads through a pointer
40   wait 60 PSA.mad        hold the script until the runtime logs a line with this text (at most 60 s); the
                        events after it move by the time that took
90   quit
include other.txt       another script's events, at their own times
```

`scripts/to_free_run.txt` boots to Quick Race › Free Run's track list on the test game's profile (the intro movies,
the title and the profile each take an ENTER; the main menu opens on QUICK RACE). Each step waits for the file the
game opens for its screen (the runtime logs every file the game opens), so the script follows the game however fast
it loads: with the HD packs the front end takes many seconds longer on lavapipe. The others build on it:
`free_roam_list.txt` goes to the list's first entry (the list opens on the last track raced and does not wrap, so
LEFT until it stops) and right through the eight circuits to the three free roam tracks, with a screenshot of each
one's SELECT LOCATION picture; `free_roam_city_select.txt` (and the `industrial`/`parkade` ones) picks a free roam
track the same way and accepts its options; `free_roam_city_drive.txt` (and the others) drives it: the start, half
a minute with two turns, a reset with `R`, and on again.

The driving keys are the game's defaults: arrows to drive, `R` resets the car, `ESC` pauses. Taps wait for the game
to read the press (a slow frame would otherwise see press and release together and act on neither), so menus
follow at any frame rate.

`peek` reads guest memory into the log, for finding out what the game holds: `[0x736174]+0x6C` is the current race's
track number, `0x777CC8` the race type a track list is for. In free roam the runtime adds a line a second
(`[freeroam] t=… pos … vel … fwd … up … heading … crumbs …`) and one per reset.
