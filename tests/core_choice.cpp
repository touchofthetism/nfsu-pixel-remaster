// The render thread's cluster choice (android/app/src/main/cpp/core_choice.h) with the Pixel 10 Pro XL's layout:
// A520 cpu0-1 (2.25 GHz), A725 cpu2-4 and cpu5-6 (3.05 GHz), X4 cpu7 (3.78 GHz), with the capacities the phone's
// kernel gives them (207, 824, 825, 1024: the same work per clock for the X4 and the A725) and the caps of
// 2026-10-28. Since build 15 the X4's speed counts 1.5 times the A725's work per clock (homeFactor 1.4976 here).
#include "../android/app/src/main/cpp/core_choice.h"

#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

namespace {
CoreChoice pixel() {
    return CoreChoice({{{0, 1}, 207, 2246000}, {{2, 3, 4}, 824, 3052000}, {{5, 6}, 825, 3052000}, {{7}, 1024, 3782000}});
}
bool near(double a, double b) { return a > b - 0.01 && a < b + 0.01; }
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
        // 1024/3782 against 825/3052 per kHz is 1.0016: the X4's speed is scaled by 1.5 / 1.0016.
        require(near(choice.homeFactor(), 1.4976) && near(choice.speed(3, 0), 1533.5) && near(choice.speed(2, 0), 825.0)
                    && near(choice.speed(3, 1152000), 467.1), "the X4 counts 1.5 times the A725's work per clock");
    }
    // Where the kernel already credits the home cluster with more per clock, less is added, and nothing past 1.5.
    {
        CoreChoice credits({{{0, 1, 2, 3}, 300, 2000000}, {{4, 5, 6}, 700, 2800000}, {{7}, 1024, 3300000}});
        require(near(credits.homeFactor(), 1.5 / ((1024.0 / 3300000) / (700.0 / 2800000))), "kernel ratio 1.24: 1.21 added");
        CoreChoice more({{{0, 1, 2, 3}, 300, 2000000}, {{4, 5, 6}, 500, 2800000}, {{7}, 1024, 3300000}});
        require(more.homeFactor() == 1.0, "kernel ratio 1.74: nothing added");
        CoreChoice alone({{{0, 1, 2, 3}, 300, 2000000}, {{7}, 1024, 3300000}});
        require(alone.homeFactor() == 1.0, "no eligible cluster to compare with: nothing added");
    }
    // Uncapped (or caps unreadable: 0): stays on the X4 for good.
    {
        CoreChoice choice = pixel();
        require(run(choice, caps(2246000, 3052000, 3052000, 3782000), 100) == 0 && choice.current() == 3, "uncapped stays home");
        require(run(choice, caps(0, 0, 0, 0), 100) == 0 && choice.current() == 3, "unknown caps stay home");
    }
    // The 05:40 session: X4 capped at 1.15 GHz (467), the mids allowed 2.69 GHz (726 and 727): 56 % faster. The
    // thread leaves after three checks, to the second mid cluster (the faster by one, as ties go there too), and
    // only once.
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
    // Build 14's moves of 2026-10-28 18:07 and 18:08 stay home now: the X4 allowed 2342 MHz (950) against cpu 2-4 at
    // 2937 (793), and 2208 (895) against 2688 (726).
    {
        CoreChoice choice = pixel();
        require(run(choice, caps(2246000, 2937000, 2937000, 2342000), 60) == 0 && run(choice, caps(2246000, 2688000, 2688000, 2208000), 60) == 0
                    && choice.current() == 3 && choice.moves() == 0, "today's moves are not made");
    }
    // The margin: another cluster must be 20 % faster. With the mids at their top clock (825) that is the X4 allowed
    // 1696 MHz or less.
    {
        CoreChoice choice = pixel();
        // X4 at 2.0 GHz (811; build 14 moved here, at 542 against 825): the mid is slower now.
        require(run(choice, caps(2246000, 3052000, 3052000, 2000000), 60) == 0 && choice.current() == 3, "2.0 GHz stays");
        // X4 at 1.8 GHz: 730; the mid 825 is 13 % faster: not enough.
        require(run(choice, caps(2246000, 3052000, 3052000, 1800000), 60) == 0 && choice.current() == 3, "13 % is not enough");
        // X4 at 1.6 GHz: 649; the mid is 27 % faster: moves.
        require(run(choice, caps(2246000, 3052000, 3052000, 1600000), 3) == 1 && choice.current() == 2, "27 % moves");
    }
    // A tie between the mid clusters goes to the higher cpus (away from the first, where Android's own work lands).
    {
        CoreChoice tie({{{0, 1}, 207, 2246000}, {{2, 3, 4}, 825, 3052000}, {{5, 6}, 825, 3052000}, {{7}, 1024, 3782000}});
        require(run(tie, caps(2246000, 2688000, 2688000, 1152000), 3) == 1 && tie.current() == 2, "a tie goes to cpu 5-6");
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
