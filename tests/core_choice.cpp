// The render thread's cluster choice (android/app/src/main/cpp/core_choice.h) with the Pixel 10 Pro XL's layout:
// A520 cpu0-1 (2.25 GHz), A725 cpu2-4 and cpu5-6 (3.05 GHz), X4 cpu7 (3.78 GHz). The capacities are
// illustrative (the phone reports its own); what matters is their order and the caps from 2026-10-28.
#include "../android/app/src/main/cpp/core_choice.h"

#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

namespace {
CoreChoice pixel() {
    return CoreChoice({{{0, 1}, 280, 2246000}, {{2, 3, 4}, 760, 3052000}, {{5, 6}, 760, 3052000}, {{7}, 1024, 3782000}});
}
std::vector<unsigned> caps(unsigned little, unsigned midA, unsigned midB, unsigned big) { return {little, midA, midB, big}; }
// Runs `n` checks with the same caps; returns how many of them asked for a move.
unsigned run(CoreChoice& choice, const std::vector<unsigned>& c, unsigned n) {
    unsigned moved = 0;
    for (unsigned i = 0; i < n; ++i) moved += choice.check(c) ? 1 : 0;
    return moved;
}
}  // namespace

int main() {
    // Home is the biggest core; the little cores are never candidates.
    {
        CoreChoice choice = pixel();
        require(choice.home() == 3 && choice.current() == 3, "home is the X4");
        require(!choice.eligible(0) && choice.eligible(1) && choice.eligible(2), "little cores not eligible");
        require(CoreChoice::describe(choice.cluster(1).cpus) == "cpu 2-4" && CoreChoice::describe(choice.cluster(3).cpus) == "cpu 7",
                "cluster labels");
    }
    // Uncapped (or caps unreadable: 0): stays on the X4 for good.
    {
        CoreChoice choice = pixel();
        require(run(choice, caps(2246000, 3052000, 3052000, 3782000), 100) == 0 && choice.current() == 3, "uncapped stays home");
        require(run(choice, caps(0, 0, 0, 0), 100) == 0 && choice.current() == 3, "unknown caps stay home");
    }
    // The 05:40 session: X4 capped at 1.15 GHz, the mids allowed 2.69 GHz. The thread leaves after three checks,
    // to the second mid cluster on a tie, and only once.
    {
        CoreChoice choice = pixel();
        auto hot = caps(2246000, 2688000, 2688000, 1152000);
        require(!choice.check(hot) && !choice.check(hot), "not before three checks");
        require(choice.check(hot) && choice.current() == 2, "moves to cpu 5-6 on the third check");
        require(run(choice, hot, 50) == 0 && choice.current() == 2 && choice.moves() == 1, "stays there");
        // The X4's cap comes back: home again after five checks at least as fast.
        auto cool = caps(2246000, 2688000, 2688000, 3782000);
        unsigned moved = 0, at = 0;
        for (unsigned i = 1; i <= 10 && !moved; ++i) if (choice.check(cool)) { moved = 1; at = i; }
        require(moved && at == 5 && choice.current() == 3 && choice.moves() == 2, "returns home after five checks");
    }
    // A cap that is only a little lower does not move it: 20 % is needed.
    {
        CoreChoice choice = pixel();
        // X4 at 2.6 GHz: 1024*2.6/3.78 = 704; a mid at 3.05 GHz: 760. Faster, but by 8 %.
        require(run(choice, caps(2246000, 3052000, 3052000, 2600000), 60) == 0 && choice.current() == 3, "8 % is not enough");
        // X4 at 2.0 GHz: 542; the mid 760 is 40 % faster: moves.
        require(run(choice, caps(2246000, 3052000, 3052000, 2000000), 3) == 1 && choice.current() == 2, "40 % moves");
    }
    // A streak broken by one check starts again.
    {
        CoreChoice choice = pixel();
        auto hot = caps(2246000, 2688000, 2688000, 1152000), cool = caps(2246000, 2688000, 2688000, 3782000);
        choice.check(hot); choice.check(hot); choice.check(cool);
        require(!choice.check(hot) && !choice.check(hot) && choice.check(hot), "streak restarts");
    }
    // No shuttling: after a move, ten checks pass before the next one, whatever the caps do.
    {
        CoreChoice choice = pixel();
        auto hot = caps(2246000, 2688000, 2688000, 1152000), cool = caps(2246000, 2688000, 2688000, 3782000);
        run(choice, hot, 3);
        require(choice.current() == 2, "moved");
        unsigned moved = 0, at = 0;
        for (unsigned i = 1; i <= 20 && !moved; ++i) if (choice.check(cool)) { moved = 1; at = i; }
        require(moved && at == CoreChoice::kDwellChecks, "dwell holds the return until ten checks after the move");
    }
    // Between the mid clusters: the faster one, with the same 20 % margin.
    {
        CoreChoice choice = pixel();
        run(choice, caps(2246000, 2688000, 2688000, 1152000), 3);
        require(choice.current() == 2, "on cpu 5-6");
        run(choice, caps(2246000, 2688000, 1536000, 1152000), 20);
        require(choice.current() == 1, "to cpu 2-4 when cpu 5-6 is capped harder");
    }
    // DXVK's threads: off the render thread's cluster, and off a slow home cluster it left, while a mid cluster
    // remains for them.
    {
        CoreChoice choice = pixel();
        require(choice.helperCpus(3) == std::vector<int>({0, 1, 2, 3, 4, 5, 6}), "render on the X4: everything else (build 11)");
        require(choice.helperCpus(2) == std::vector<int>({0, 1, 2, 3, 4}), "render on cpu 5-6: not the X4 either");
        require(choice.helperCpus(1) == std::vector<int>({0, 1, 5, 6}), "render on cpu 2-4: cpu 5-6 and the little cores");
        require(choice.helperCpus(-1).empty() && choice.helperCpus(4).empty(), "no such cluster");
        // little / mid / big: with the render thread on the mids, the big core is the only other fast one.
        CoreChoice three({{{0, 1, 2, 3}, 300, 2000000}, {{4, 5, 6}, 700, 2800000}, {{7}, 1024, 3300000}});
        require(three.helperCpus(1) == std::vector<int>({0, 1, 2, 3, 7}), "one mid cluster: the big core stays allowed");
        require(three.helperCpus(2) == std::vector<int>({0, 1, 2, 3, 4, 5, 6}), "and the big core's complement at home");
    }
    // Wrong input is ignored.
    {
        CoreChoice choice = pixel();
        require(!choice.check({1, 2}) && choice.current() == 3, "cap count mismatch ignored");
        CoreChoice none;
        require(!none.check({}) && none.current() == -1, "no clusters: nothing to do");
        CoreChoice one({{{0, 1, 2, 3}, 1024, 2000000}});
        require(run(one, {500000}, 50) == 0 && one.current() == 0, "a single cluster never moves");
    }
    std::puts("core_choice: ok");
    return 0;
}
