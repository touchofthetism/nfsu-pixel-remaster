// The frame-pacing decisions of src/runtime/framerate.c (nfs_framerate.h), the port of the
// FrameRate block of ThirteenAG's NFSUnderground.WidescreenFix: what each hook leaves in the
// guest's registers, stack and ST0, without a guest.
#include "../src/runtime/nfs_framerate.h"
#include "../android/app/src/main/cpp/frame_pacer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-7; }
int main() {
    float target, minimum, wait;
    nfs_fr_plan(0, 0, &target, &minimum, &wait);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60) && near(wait, 1.0 / 30), "0 keeps the game's own 1/60 pacing, floor and 1/30 wait threshold");
    nfs_fr_plan(60, 0, &target, &minimum, &wait);
    require(near(target, 1.0 / 60) && near(minimum, 1.0 / 60) && near(wait, 1.0 / 30), "a 60 cap is the stock game");
    nfs_fr_plan(120, 0, &target, &minimum, &wait);
    require(near(target, 1.0 / 120) && near(minimum, 1.0 / 120) && near(wait, 1.0 / 30), "120: the limiter waits for 1/120, frames may report 1/120, threshold stays 1/30");
    nfs_fr_plan(90, 0, &target, &minimum, &wait);
    require(near(target, 1.0 / 90) && near(minimum, 1.0 / 90) && near(wait, 1.0 / 30), "90 likewise");
    nfs_fr_plan(30, 0, &target, &minimum, &wait);
    require(near(target, 1.0 / 30) && near(minimum, 1.0 / 60), "below 60 the floor stays 1/60: the step is never shorter than the frame");
    require(near(wait, 2.0 / 30), "below 60 the wait threshold is 2/cap, as the mod writes (1/15 at 30)");
    nfs_fr_plan(45, 0, &target, &minimum, &wait);
    require(near(wait, 2.0 / 45), "and 2/45 at 45");
    // Host paced: the same target and floor, the game's own spin off at every cap.
    for (unsigned fps : {30u, 45u, 60u, 90u, 120u}) {
        float paced_target, paced_minimum, paced_wait, own_target, own_minimum, own_wait;
        nfs_fr_plan(fps, 1, &paced_target, &paced_minimum, &paced_wait);
        nfs_fr_plan(fps, 0, &own_target, &own_minimum, &own_wait);
        require(paced_target == own_target && paced_minimum == own_minimum, "pacing by the host keeps the target and the floor");
        require(paced_wait == 0.0f, "pacing by the host switches the game's spin off");
        double reported = 0;
        require(nfs_fr_game_wait(paced_target * 0.25, paced_target, paced_wait, &reported) == 0.0 && near(reported, paced_target * 0.25),
                "with the threshold at 0 an early frame is not spun and reports its real time");
    }
    float stock_target, stock_minimum, stock_wait;
    nfs_fr_plan(0, 1, &stock_target, &stock_minimum, &stock_wait);
    require(near(stock_wait, 1.0 / 30), "fps 0 (the game's own pacing) keeps its threshold either way");
    {   // The game's wait itself, as lifted at 0x585B98..0x585C4C.
        double reported = 0;
        require(near(nfs_fr_game_wait(0.010, 1.0f / 60, 1.0f / 30, &reported), 1.0 / 60 - 0.010) && near(reported, 1.0 / 60), "an early frame spins to the target and reports it");
        require(nfs_fr_game_wait(0.020, 1.0f / 60, 1.0f / 30, &reported) == 0.0 && near(reported, 0.020), "a late frame does not spin");
    }
    // The two limiters in series (build 9): the runtime's pacer sleeps after each Present to its next
    // deadline, the game spins at the top of the next frame until `target` has passed since the end of its
    // previous wait. A menu frame takes 4 ms of work and the game's bookkeeping between its two clock reads
    // 20 us. With the game's spin on it grows until it has eaten the pacer's sleep; with it off the pacer
    // sleeps the whole slack and every frame still reports a sixtieth (the floor) and takes one period.
    for (int paced = 0; paced <= 1; ++paced) {
        float t_target, t_floor, t_wait;
        nfs_fr_plan(60, paced, &t_target, &t_floor, &t_wait);
        FramePacer pacer;
        using clock = FramePacer::Clock;
        clock::time_point now{}, waitEnd{};
        double spun = 0, slept = 0, game = 0;
        const double work = 0.004, bookkeeping = 20e-6;
        const int frames = 6000;
        for (int n = 0; n < frames; ++n) {
            double elapsed = std::chrono::duration<double>(now - waitEnd).count(), reported = 0;
            double spin = nfs_fr_game_wait(elapsed, t_target, t_wait, &reported);
            if (n >= frames / 2) { spun += spin; game += reported < t_floor ? t_floor : reported; }
            now += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(spin + bookkeeping));
            waitEnd = now;
            now += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(work));
            auto deadline = pacer.deadline(now, 60);
            if (deadline > now) { if (n >= frames / 2) slept += std::chrono::duration<double>(deadline - now).count(); now = deadline; }
        }
        double span = (frames / 2) / 60.0;
        if (!paced) require(spun / span > 0.6 && slept / span < 0.1, "with the game's spin on, it ends up spinning through the pacer's slack");
        else require(spun == 0.0 && slept / span > 0.7, "with the spin off the pacer sleeps the slack");
        if (paced) require(std::fabs(game / span - 1.0) < 0.005, "and game time keeps step with real time");
    }

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
