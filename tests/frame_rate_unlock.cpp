// The frame-pacing decisions of src/runtime/framerate.c (nfs_framerate.h), the port of the
// FrameRate block of ThirteenAG's NFSUnderground.WidescreenFix: what each hook leaves in the
// guest's registers, stack and ST0, without a guest.
#include "../src/runtime/nfs_framerate.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-7; }
int main() {
    float target, minimum;
    nfs_fr_plan(0, &target, &minimum);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60), "0 keeps the game's own 1/60 pacing and floor");
    nfs_fr_plan(60, &target, &minimum);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60), "a 60 cap is the stock game");
    nfs_fr_plan(120, &target, &minimum);
    require(near(target, 1.0 / 120) && near(minimum, 1.0 / 120), "120: the limiter waits for 1/120 and frames may report 1/120");
    nfs_fr_plan(90, &target, &minimum);
    require(near(target, 1.0 / 90) && near(minimum, 1.0 / 90), "90 likewise");
    nfs_fr_plan(30, &target, &minimum);
    require(near(target, 1.0 / 30) && near(minimum, 1.0 / 60), "below 60 the floor stays 1/60: the step is never shorter than the frame");

    // 0x585CB5: ST0 = the frame's step, ax = fnstsw after fcom [1/60]; C0 (bit 8) set means "below".
    uint32_t eax = 0x0000'0100u | 0xDEAD'0000u;
    double st0 = nfs_fr_min_step(1.0 / 120, 1.0f / 120, &eax);
    require(near(st0, 1.0 / 120), "a 1/120 frame keeps its 1/120 step with a 1/120 floor");
    require((eax & 0x4100u) == 0 && (eax & 0xFFFF0000u) == 0xDEAD0000u, "C0 and C3 cleared so the game's clamp to 1/60 is skipped; the rest of eax untouched");
    eax = 0x0100u;
    st0 = nfs_fr_min_step(0.004, 1.0f / 120, &eax);
    require(near(st0, 1.0 / 120), "a shorter frame is raised to the floor");
    eax = 0x4000u;   // C3: equal to 1/60
    st0 = nfs_fr_min_step(1.0 / 60, 1.0f / 60, &eax);
    require(near(st0, 1.0 / 60) && (eax & 0x4100u) == 0, "at 60 the result equals the game's own clamp");
    eax = 0;
    st0 = nfs_fr_min_step(0.05, 1.0f / 60, &eax);
    require(near(st0, 0.05), "longer frames pass through");

    // 0x585DB1: [esp] = whole 1/60 ticks this frame, eax = what the game is about to add.
    require(nfs_fr_ticks(0, 1) == 0, "a forced single tick for a frame shorter than 1/60 becomes none (the remainder carries)");
    require(nfs_fr_ticks(1, 1) == 1 && nfs_fr_ticks(3, 3) == 3, "real tick counts pass through");

    // 0x4479D3: EAXSound::Update(dt) batched to at least 1/60.
    float acc = 0.0f;
    require(nfs_fr_audio_step(&acc, 1.0f / 120) == 0.0f, "the first of two 1/120 frames passes 0 (the game's paused value)");
    require(near(nfs_fr_audio_step(&acc, 1.0f / 120), 1.0 / 60) && acc == 0.0f, "the second passes the batched 1/60 and resets");
    require(near(nfs_fr_audio_step(&acc, 1.0f / 60), 1.0 / 60), "a 60 FPS frame passes through unchanged");
    require(near(nfs_fr_audio_step(&acc, 60.0f / 3600.0f), 1.0 / 60), "a 60 FPS frame counted in the game's 1/3600 steps still updates (the mod's margin)");
    require(nfs_fr_audio_step(&acc, 59.0f / 3600.0f) == 0.0f && near(nfs_fr_audio_step(&acc, 60.0f / 3600.0f), 119.0 / 3600), "one step short batches with the next frame");
    require(nfs_fr_audio_step(&acc, 0.0f) == 0.0f && nfs_fr_audio_step(&acc, -1.0f) == -1.0f, "dt <= 0 passes through");
    acc = 0.0f;
    nfs_fr_audio_step(&acc, 0.004f); nfs_fr_audio_step(&acc, 0.004f); nfs_fr_audio_step(&acc, 0.004f);
    require(near(nfs_fr_audio_step(&acc, 0.005f), 0.017), "any number of short frames batch up");

    // 0x40A74D: particle step from the frame's real time, clamped like the mod.
    require(near(nfs_fr_particle_step(1.0f / 120), 1.0 / 120), "120 FPS particles step 1/120");
    require(near(nfs_fr_particle_step(0.2f), 0.05), "a hitch is clamped to 50 ms");
    require(nfs_fr_particle_step(0.0f) == 0.0f && nfs_fr_particle_step(-1.0f) == 0.0f && nfs_fr_particle_step(NAN) == 0.0f, "nothing below zero");

    // 0x460A3A: `test ah,5; jp skip` must jump: ah & 5 == 0 has even parity.
    require(nfs_fr_skip_steering_snap(0x0000'0100u) == 0 && nfs_fr_skip_steering_snap(0x1234'0500u) == 0x1234'0000u, "C0 and C2 cleared, nothing else");
    require(__builtin_parity((nfs_fr_skip_steering_snap(0x0100u) >> 8) & 5) == 0, "even parity: the jp is taken and the snap skipped");

    std::puts("frame_rate_unlock: ok");
    return 0;
}
