/*
 * framerate.c - NFSU1 frame pacing at the launcher's cap (see nfs_framerate.h).
 *
 * A port of the FrameRate block of ThirteenAG's NFSUnderground.WidescreenFix
 * (WidescreenFixesPack, MIT; Misc.ixx). That mod patches x86 bytes; the game
 * here is lifted C, so each patch is a host hook (config/hooks.txt ->
 * RECOMP_HOOK -> recomp_hook, dispatched here from widescreen.c) at the
 * instruction after the one whose result it changes, plus one data write.
 *
 * Of the mod's six: the limiter's target is the data write (re-asserted once
 * per present, like the mod's ProtectedGameRef); the busy-wait threshold it
 * also sets stays at the game's 1/30 s, which is what the mod computes for
 * every cap of 60 and above; the other four are hooks. The mod's FPSLimit
 * defaults to the monitor's refresh rate, so this is its shipped behaviour.
 *
 * Shared by the Windows host and the Android runtime.
 */
#include <stdint.h>
#include <string.h>
#include "recomp_types.h"
#include "nfs_framerate.h"
#include "nfs_widescreen.h"

static unsigned s_fps;                      /* 0: the game's own 60 */
static float s_target = 1.0f / 60.0f;
static float s_min_step = 1.0f / 60.0f;
static float s_audio_time;                  /* EAXSound::Update batching */

static float fr_rdf(uint32_t va) { uint32_t v = MEM32(va); float f; memcpy(&f, &v, 4); return f; }
static void fr_wrf(uint32_t va, float f) { uint32_t v; memcpy(&v, &f, 4); MEM32(va) = v; }

void nfs_widescreen_set_frame_rate(unsigned fps) {
    s_fps = fps;
    nfs_fr_plan(fps, &s_target, &s_min_step);
    s_audio_time = 0.0f;
}

/* Once per present: PrepareRealTimestep reads its target from .data every
 * frame, and the game may re-initialise it on video-mode changes. */
void nfs_widescreen_apply_frame_rate(void) {
    if (!s_fps) return;
    if (fr_rdf(NFS_FR_TARGET_FRAME_TIME) != s_target) fr_wrf(NFS_FR_TARGET_FRAME_TIME, s_target);
}

/* The hooks: 1 when `va` was one of ours. Registers are the synced globals,
 * g_st[0] is ST(0). They are in effect at every cap, as in the mod. At 60 FPS
 * the step clamp, the tick rule and the audio batching give the game's own
 * results (a frame is never shorter than the 1/60 floor there); the particle
 * step becomes the frame's real time (about 1/60) and the steering snap is
 * skipped at any rate, the mod's two deliberate changes. */
int nfs_framerate_hook(uint32_t va) {
    switch (va) {
    case NFS_FR_HOOK_MIN_STEP:
        g_st[0] = nfs_fr_min_step(g_st[0], s_min_step, &g_eax);
        return 1;
    case NFS_FR_HOOK_TICK:
        g_eax = nfs_fr_ticks(MEM32(g_esp), g_eax);
        return 1;
    case NFS_FR_HOOK_AUDIO:
        fr_wrf(g_esp, nfs_fr_audio_step(&s_audio_time, fr_rdf(g_esp)));
        return 1;
    case NFS_FR_HOOK_PARTICLES:
        fr_wrf(g_esp + 4, nfs_fr_particle_step(fr_rdf(NFS_FR_REAL_TIME_ELAPSED)));
        return 1;
    case NFS_FR_HOOK_STEERING:
        g_eax = nfs_fr_skip_steering_snap(g_eax);
        return 1;
    default:
        return 0;
    }
}
