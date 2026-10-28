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
    float target, minimum, wait;
    nfs_fr_plan(0, &target, &minimum, &wait);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60) && near(wait, 1.0 / 30), "0 keeps the game's own 1/60 pacing, floor and 1/30 wait threshold");
    nfs_fr_plan(60, &target, &minimum, &wait);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60) && near(wait, 1.0 / 30), "a 60 cap is the stock game");
    nfs_fr_plan(120, &target, &minimum, &wait);
    require(near(target, 1.0 / 120) && near(minimum, 1.0 / 120) && near(wait, 1.0 / 30), "120: the limiter waits for 1/120, frames may report 1/120, threshold stays 1/30");
    nfs_fr_plan(90, &target, &minimum, &wait);
    require(near(target, 1.0 / 90) && near(minimum, 1.0 / 90) && near(wait, 1.0 / 30), "90 likewise");
    nfs_fr_plan(30, &target, &minimum, &wait);
    require(near(target, 1.0 / 30) && near(minimum, 1.0 / 60), "below 60 the floor stays 1/60: the step is never shorter than the frame");
    require(near(wait, 2.0 / 30), "below 60 the wait threshold is 2/cap, as the mod writes (1/15 at 30)");
    nfs_fr_plan(45, &target, &minimum, &wait);
    require(near(wait, 2.0 / 45), "and 2/45 at 45");

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
    eax = 0x4500u;   // unordered: C0|C2|C3
    st0 = nfs_fr_min_step(NAN, 1.0f / 120, &eax);
    require(near(st0, 1.0 / 120) && (eax & 0x4100u) == 0, "a NaN step is replaced by the floor, as the mod's redirected compare would");

    // 0x585DB1: [esp] = whole 1/60 ticks this frame, eax = what the game is about to add.
    require(nfs_fr_ticks(0, 1) == 0, "a forced single tick for a frame shorter than 1/60 becomes none (the remainder carries)");
    require(nfs_fr_ticks(1, 1) == 1 && nfs_fr_ticks(3, 3) == 3, "real tick counts pass through");

    // EAXSound::Update (hook 0x51D24B): updates batched to at least 1/60, the frames between skipped.
    float acc = 0.0f, dt = 1.0f / 120;
    require(nfs_fr_should_update_audio(&acc, &dt) == 0, "the first of two 1/120 frames skips the update");
    dt = 1.0f / 120;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && near(dt, 1.0 / 60) && acc == 0.0f, "the second updates with the batched 1/60 and resets");
    dt = 1.0f / 60;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && near(dt, 1.0 / 60), "a 60 FPS frame updates with its own step");
    dt = 60.0f / 3600.0f;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && near(dt, 1.0 / 60), "a 60 FPS frame counted in the game's 1/3600 steps still updates (the mod's margin)");
    dt = 59.0f / 3600.0f;
    require(nfs_fr_should_update_audio(&acc, &dt) == 0, "one 1/3600 step short is held back");
    dt = 60.0f / 3600.0f;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && near(dt, 119.0 / 3600), "and passed on with the next frame");
    dt = 0.0f;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && dt == 0.0f, "paused (0) updates, step untouched");
    dt = -1.0f;
    require(nfs_fr_should_update_audio(&acc, &dt) == 1 && dt == -1.0f && acc == 0.0f, "negative steps pass through and add nothing");
    acc = 0.0f;
    int updates = 0;
    for (int frame = 0; frame < 3; frame++) { dt = 0.004f; updates += nfs_fr_should_update_audio(&acc, &dt); }
    dt = 0.005f;
    require(updates == 0 && nfs_fr_should_update_audio(&acc, &dt) == 1 && near(dt, 0.017), "any number of short frames batch into one update");
    acc = 0.0f; updates = 0;
    for (int frame = 0; frame < 1200; frame++) { dt = 1.0f / 120; updates += nfs_fr_should_update_audio(&acc, &dt); }
    require(updates == 600, "ten seconds at 120 FPS: exactly the 600 updates the original game made at 60");

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
