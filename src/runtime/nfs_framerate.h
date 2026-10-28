/* nfs_framerate.h - the game's own frame pacing on a high-refresh display.
 *
 * Speed.exe paces itself: PrepareRealTimestep (0x585B40) busy-waits on
 * QueryPerformanceCounter until a fixed 1/60 s has passed (a 3-minute build-6
 * session made 13.3 billion of those calls), then clamps the frame's time step
 * to at least 1/60 s, and the tick counter adds at least one 1/60 tick per
 * frame. Lifting the cap alone therefore ran the game fast. ThirteenAG's
 * NFSUnderground.WidescreenFix (WidescreenFixesPack, MIT; Misc.ixx, namespace
 * FrameRate) carries the complete, long-shipped correction, on by default at
 * the monitor's refresh rate: seven small changes. framerate.c ports them as
 * host hooks (config/hooks.txt) and two data writes; the decisions each hook
 * makes live here, header-only, so tests/frame_rate_unlock.cpp can exercise
 * them without a guest.
 */
#ifndef NFS_FRAMERATE_H
#define NFS_FRAMERATE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Guest addresses in Speed.exe 1.4 (found by the mod's byte patterns). */
#define NFS_FR_TARGET_FRAME_TIME   0x006F0890u   /* float, 1/60: `fld` at 0x585B83 */
#define NFS_FR_WAIT_THRESHOLD      0x006CCDECu   /* float, 1/30: `fcomp` at 0x585BBB gates the spin */
#define NFS_FR_REAL_TIME_ELAPSED   0x0073AD38u   /* float, the frame's time step (1/3600 s steps) */
#define NFS_FR_HOOK_MIN_STEP       0x00585CB5u   /* `test ah,41h` after `fcom [1/60]`: min time step clamp */
#define NFS_FR_HOOK_TICK           0x00585DB1u   /* `mov ecx,[tickCount]` after the forced `mov eax,1` */
#define NFS_FR_HOOK_AUDIO          0x0051D24Bu   /* EAXSound::Update: `cmp eax,ebx` on the sound-on flag, dt at [esp+0Ch] */
#define NFS_FR_HOOK_PARTICLES      0x0040A74Du   /* `call` after `push 1/60; push system`: particle step */
#define NFS_FR_HOOK_STEERING       0x00460A3Au   /* `test ah,5` after `fcomp [1.0]`: steering snap */

/* Pacing for a launcher cap: the limiter's target frame time, the smallest
 * time step a frame may report and the spin's wait threshold (the mod's
 * fTargetFrameTime / fMinFrameTime / WaitThreshold). fps 0 means the original
 * game (1/60, 1/60, 1/30). Above 60 the step follows the frame; at or below 60
 * the game keeps its 1/60 floor exactly as shipped. The threshold is
 * max(2 x target, 1/30): the stock 1/30 at every cap of 60 and above, 2/cap
 * below (the busy-wait is skipped when the time left exceeds it). */
static inline void nfs_fr_plan(unsigned fps, float *target, float *min_step, float *wait_threshold) {
    float sixty = 1.0f / 60.0f, thirty = 1.0f / 30.0f;
    if (fps == 0) { *target = sixty; *min_step = sixty; *wait_threshold = thirty; return; }
    *target = 1.0f / (float)fps;
    *min_step = *target < sixty ? *target : sixty;
    *wait_threshold = 2.0f * *target > thirty ? 2.0f * *target : thirty;
}

/* 0x585CB5: ST0 holds the step being floored (the carried remainder
 * [0x734570] plus the frame's elapsed time; the 3/60 ceiling was already
 * computed and stored to [esp+0Ch]), ax the fcom status against 1/60 (C0 =
 * ah bit 0 "below", C3 = bit 6 "equal"; `test ah,41h; je` skips the clamp
 * when both are clear). Returns the value to leave in ST0 and clears C0/C3 in
 * *eax so the game's own clamp to 1/60 never runs: the clamp is ours now, to
 * min_step. A NaN is replaced by the floor, as the mod's redirected fcom
 * (unordered sets C0|C2|C3) would. */
static inline double nfs_fr_min_step(double step, float min_step, uint32_t *eax) {
    *eax &= ~0x4100u;
    return !(step >= (double)min_step) ? (double)min_step : step;
}

/* 0x585DB1: the tick accumulator (0x585D70) computed `ticks` whole 1/60 ticks
 * for this frame (kept at [esp]) and, when that was 0, forced eax to 1. The
 * remainder was already stored for the next frame, so a forced tick is
 * returned as 0: at 120 FPS the ticks alternate 0,1,0,1 and tick-driven logic
 * runs at its 60 Hz. Any real count passes through. */
static inline uint32_t nfs_fr_ticks(uint32_t ticks, uint32_t eax) {
    return ticks == 0 ? 0u : eax;
}

/* EAXSound::Update(dt) (0x51D240, called only from the main loop at 0x4479D3)
 * was only ever called at up to 60 FPS. Above 60 the frames are batched so
 * every update covers at least 1/60 s, as in the original game; the frames in
 * between skip the update entirely (a small margin keeps the game's 1/3600-s
 * rounding from skipping one at exactly 60 FPS). This is the mod's
 * ShouldUpdateAudio verbatim: 0 = skip, else *dt is the batched step. The
 * hook at 0x51D24B skips by zeroing eax, the sound-on flag the function has
 * just loaded, so it leaves through its own `pop edi; pop ebx; ret 4` - the
 * stack effect of the mod's `ret 4` at the call site. dt <= 0 (paused) is
 * passed through untouched, as by the mod. */
static inline int nfs_fr_should_update_audio(float *accumulated, float *dt) {
    if (*dt <= 0.0f) return 1;
    *accumulated += *dt;
    if (*accumulated < 1.0f / 60.0f - 1.0f / 7200.0f) return 0;
    *dt = *accumulated;
    *accumulated = 0.0f;
    return 1;
}

/* 0x40A74D: the particle systems were stepped by a fixed 1/60 every frame;
 * they take the frame's real time step, clamped like the mod does. */
static inline float nfs_fr_particle_step(float real_time_elapsed) {
    if (!(real_time_elapsed > 0.0f)) return 0.0f;
    return real_time_elapsed > 0.05f ? 0.05f : real_time_elapsed;
}

/* 0x460A3A: `fcomp [1.0]; fnstsw ax; test ah,5; jp skip` snaps a steering
 * angle below 1 degree to zero; a frame's change is smaller than that at high
 * frame rates, so the wheels stuck centred. Clearing C0 and C2 gives
 * `test ah,5` an even parity (PF = 1) and the jp always skips the snap, the
 * mod's `jp -> jmp`. */
static inline uint32_t nfs_fr_skip_steering_snap(uint32_t eax) {
    return eax & ~0x0500u;
}

#ifdef __cplusplus
}
#endif
#endif
