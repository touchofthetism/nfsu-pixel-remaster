// The render thread's cluster choice (android/app/src/main/cpp/core_choice.h) with the Pixel 10 Pro XL's layout:
// A520 cpu0-1 (2.25 GHz), A725 cpu2-4 and cpu5-6 (3.05 GHz), X4 cpu7 (3.78 GHz), with the capacities the phone's
// kernel gives them (207, 824, 825, 1024: the same work per clock for the X4 and the A725) and the caps of
// 2026-10-28. Since build 15 the X4's speed counts 1.5 times the A725's work per clock (homeFactor 1.4976 here).
// Build 16: each cluster's share of its cap under the render thread's load (learned only while the thread is
// busy and behind its target, not right after a move, relaxing back to 1, reset with the thermal status), a
// replay shaped on build 15's 22:25 session (85 moves on the caps alone), and the command stream's own cluster.
#include "../android/app/src/main/cpp/core_choice.h"

#include <cmath>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::exit(1); }
}

namespace {
CoreChoice pixel(double returnFactor = CoreChoice::kReturnFactor) {
    return CoreChoice({{{0, 1}, 207, 2246000}, {{2, 3, 4}, 824, 3052000}, {{5, 6}, 825, 3052000}, {{7}, 1024, 3782000}}, returnFactor);
}
bool near(double a, double b) { return a > b - 0.01 && a < b + 0.01; }
std::vector<unsigned> caps(unsigned little, unsigned midA, unsigned midB, unsigned big) { return {little, midA, midB, big}; }
// Runs `n` checks with the same caps; returns how many of them asked for a move.
unsigned run(CoreChoice& choice, const std::vector<unsigned>& c, unsigned n) {
    unsigned moved = 0;
    for (unsigned i = 0; i < n; ++i) moved += choice.check(c) ? 1 : 0;
    return moved;
}
// What the render thread's cluster gave it: `clockKhz`, the thread on a cpu `busy` of the time, behind or not.
CoreChoice::Observation seen(unsigned clockKhz, double busy = 0.95, bool behind = true) {
    CoreChoice::Observation o;
    o.clockKhz = clockKhz; o.busy = busy; o.behind = behind;
    return o;
}
// The 22:25 session's hot stretch as it recorded it (tests/data/caps-20261028-222502.txt): each 2 s device
// sample's second and the clocks cpu 2-4, cpu 5-6 and cpu 7 were allowed, held until the next sample.
std::vector<std::vector<unsigned>> recordedCaps(unsigned* firstSecond) {
    std::string here = __FILE__;
    std::ifstream file(here.substr(0, here.find_last_of('/') + 1) + "data/caps-20261028-222502.txt");
    std::vector<std::vector<unsigned>> perSecond;
    std::string line;
    unsigned last = 0;
    std::vector<unsigned> held;
    while (std::getline(file, line)) {
        unsigned t, midA, midB, x4;
        if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%u %u %u %u", &t, &midA, &midB, &x4) != 4) continue;
        if (held.empty()) *firstSecond = last = t;
        for (; last < t; ++last) perSecond.push_back(held);
        held = caps(2246000, midA * 1000, midB * 1000, x4 * 1000);
    }
    if (!held.empty()) perSecond.push_back(held);
    return perSecond;
}
// The clock the render thread's cluster ran at there: the X4 at 883 MHz 72 % of the time and at its cap the rest
// (988 of 1323 MHz on average in the session), the A725s at 88 % of theirs.
unsigned hotClock(int cluster, const std::vector<unsigned>& c) {
    if (cluster == 3) return unsigned(0.72 * 883000 + 0.28 * c[3]);
    return unsigned(0.88 * c[size_t(cluster)]);
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
    // Build 16. The X4 allowed 1305 MHz (529), cpu 2-4 allowed 2188 (591): 12 % faster at the caps, not enough to
    // leave. Measured, the X4 gave the busy render thread 988 MHz (76 % of its cap: the PMU limit): its share
    // falls a quarter of the way a check, and on the fourth check the thread leaves for cpu 2-4.
    const auto limited = caps(2246000, 2188000, 1075000, 1305000);
    {
        CoreChoice choice = pixel();
        require(near(choice.speedAtCap(3, 1305000), 529.14) && near(choice.speed(1, 2188000), 590.73) && near(choice.speed(2, 1075000), 290.59),
                "the speeds at the caps");
        require(run(choice, limited, 50) == 0 && choice.current() == 3 && choice.share(3) == 1.0, "on the caps alone: stays");
        unsigned at = 0;
        for (unsigned i = 1; i <= 10 && !at; ++i) if (choice.check(limited, seen(988000))) at = i;
        require(at == 4 && choice.current() == 1 && choice.learned(3) == 4, "measured: leaves on the fourth check");
        require(choice.share(3) > 0.80 && choice.share(3) < 0.86 && choice.share(1) == 1.0, "the X4's share learned, cpu 2-4's not yet");
    }
    // Nothing is learned while the thread waits most of the check (busy under 75 %), keeps up with its target, or
    // its cluster's clock is unknown: that clock is then what the thread asked for.
    {
        CoreChoice choice = pixel();
        for (int i = 0; i < 50; ++i) {
            require(!choice.check(limited, seen(700000, 0.6, true)) && !choice.check(limited, seen(700000, 0.95, false))
                        && !choice.check(limited, seen(0)), "no move");
        }
        require(choice.current() == 3 && choice.share(3) == 1.0 && choice.learned(3) == 0, "nothing learned");
        // A clock above the cap (the cap fell during the check) counts as the cap: a share never passes 1.
        for (int i = 0; i < 10; ++i) choice.check(limited, seen(2000000));
        require(choice.share(3) == 1.0 && choice.learned(3) == 10, "never above 1");
    }
    // After a move the new cluster's first three checks do not count (its clock follows the new load), the fourth
    // does; the share it learns makes the cluster slower than its cap says.
    {
        CoreChoice choice = pixel();
        for (int i = 0; i < 10 && choice.current() == 3; ++i) choice.check(limited, seen(988000));
        require(choice.current() == 1, "on cpu 2-4");
        const auto mids = seen(unsigned(0.88 * 2188000));
        for (int i = 0; i < 3; ++i) choice.check(limited, mids);
        require(choice.learned(1) == 0, "three checks settle");
        choice.check(limited, mids);
        require(choice.learned(1) == 1 && near(choice.share(1), 0.97), "the fourth counts");
        for (int i = 0; i < 40; ++i) choice.check(limited, mids);
        require(choice.share(1) > 0.87 && choice.share(1) < 0.90 && choice.current() == 1, "cpu 2-4 at 88 % of its cap");
    }
    // A share relaxes back to 1 (five-minute time constant). With the X4 allowed 1305 MHz it would be 2 % faster
    // than what cpu 2-4 gives the thread even at its full cap, short of the 5 % it needs: it is not tried again.
    // Allowed 1766 MHz it is (716, 37 % more than cpu 2-4's 521), within five checks; there it gives the thread 883
    // MHz flat, and the thread leaves again once the X4's share says cpu 2-4 is 20 % faster.
    {
        CoreChoice choice = pixel();
        for (int i = 0; i < 10 && choice.current() == 3; ++i) choice.check(limited, seen(988000));
        const auto mids = seen(unsigned(0.88 * 2188000));
        unsigned back = 0;
        for (unsigned i = 1; i <= 1000 && !back; ++i) if (choice.check(limited, mids)) back = i;
        require(!back && choice.current() == 1 && choice.share(3) > 0.99, "not tried again: share " + std::to_string(choice.share(3)));
        const auto warmer = caps(2246000, 2188000, 1075000, 1766000);
        for (unsigned i = 1; i <= 10 && !back; ++i) if (choice.check(warmer, mids)) back = i;
        require(back == 5 && choice.current() == 3, "tried again after " + std::to_string(back) + " checks");
        unsigned left = 0;
        for (unsigned i = 1; i <= 30 && !left; ++i) if (choice.check(warmer, seen(883000))) left = i;
        require(left > CoreChoice::kDwellChecks && left < 15 && choice.current() == 1 && choice.share(3) < 0.62,
                "left again after " + std::to_string(left) + " checks, share " + std::to_string(choice.share(3)));
    }
    // The thermal status changes (the PMU limit is switched with it): every share starts over, and with the X4
    // allowed 2208 MHz (895, against cpu 2-4's 824 at its top clock) the thread goes home after five checks.
    {
        CoreChoice choice = pixel();
        for (int i = 0; i < 10 && choice.current() == 3; ++i) choice.check(limited, seen(700000));   // 54 % of its cap
        require(choice.current() == 1 && choice.share(3) < 0.75, "left the X4 at 54 %");
        for (int i = 0; i < 20; ++i) choice.check(limited, seen(unsigned(0.88 * 2188000)));
        const auto cooler = caps(2246000, 3052000, 3052000, 2208000);
        require(run(choice, cooler, 20) == 0 && choice.current() == 1, "the X4's learned share keeps it away");
        choice.resetShares();
        require(choice.share(1) == 1.0 && choice.share(3) == 1.0, "shares reset");
        unsigned at = 0;
        for (unsigned i = 1; i <= 10 && !at; ++i) if (choice.check(cooler)) at = i;
        require(at == 5 && choice.current() == 3, "home after five checks");
    }
    // The hot stretch replayed through the caps it recorded (29 minutes, one check a second). Build 15's rule (on
    // the caps alone, home once as fast) shuttles as the session did (79 moves here, 78 in the session, 59 % of the
    // time on the X4 against its 61 %); measured, the thread leaves the X4, stays on cpu 2-4 and tries the X4
    // again now and then, and the speed it gets (its cluster's at the clock it ran at, as the session measured
    // them: hotClock) is higher.
    {
        unsigned first = 0;
        auto recorded = recordedCaps(&first);
        require(recorded.size() > 1700 && first == 2400, "the recorded stretch: " + std::to_string(recorded.size()) + " s");
        CoreChoice old = pixel(1.0), now = pixel();   // build 15's rule: home once as fast, nothing measured
        unsigned oldOnX4 = 0, nowOnX4 = 0;
        double oldSpeed = 0, newSpeed = 0;
        auto got = [](const CoreChoice& c, const std::vector<unsigned>& k) {
            int at = c.current();
            return c.speedAtCap(size_t(at), hotClock(at, k));
        };
        for (const auto& k : recorded) {
            old.check(k);
            now.check(k, seen(hotClock(now.current(), k)));
            oldSpeed += got(old, k);
            newSpeed += got(now, k);
            oldOnX4 += old.current() == 3 ? 1 : 0;
            nowOnX4 += now.current() == 3 ? 1 : 0;
        }
        std::printf("core_choice: the 22:25 hot stretch (%zu s): caps alone %u moves, %u s on the X4; measured %u moves, %u s on the X4; "
                    "the thread gets %+.1f %%\n", recorded.size(), old.moves(), oldOnX4, now.moves(), nowOnX4, 100 * (newSpeed / oldSpeed - 1));
        require(old.moves() >= 70 && oldOnX4 * 2 > recorded.size(), "the caps alone shuttle: " + std::to_string(old.moves()));
        require(now.moves() <= 30 && nowOnX4 * 10 < recorded.size(),
                "measured: " + std::to_string(now.moves()) + " moves, " + std::to_string(nowOnX4) + " s on the X4");
        require(newSpeed > 1.08 * oldSpeed, "the thread gets 8 % more at least");
    }
    // DXVK's command stream: the fastest eligible cluster besides the render thread's, at its cap. At the top clocks
    // that is cpu 5-6 (825 against 824); with cpu 5-6 capped at 1075 MHz and cpu 2-4 at 2188 it moves to cpu 2-4 on
    // the third check; when the render thread takes cpu 2-4 it goes at once to the X4 (529 against 291), and once
    // the render thread is home it goes at once to the fastest mid cluster again.
    {
        CoreChoice choice = pixel();
        require(choice.stream() == 2 && choice.streamMoves() == 0, "the stream starts on cpu 5-6");
        require(run(choice, caps(2246000, 3052000, 3052000, 3782000), 20) == 0 && !choice.checkStream(caps(2246000, 3052000, 3052000, 3782000)),
                "uncapped: stays");
        unsigned at = 0;
        for (unsigned i = 1; i <= 10 && !at; ++i) { choice.check(limited); if (choice.checkStream(limited)) at = i; }
        require(at == 3 && choice.stream() == 1 && choice.current() == 3, "to cpu 2-4 on the third check");
        for (int i = 0; i < 10 && choice.current() == 3; ++i) { choice.check(limited, seen(988000)); if (choice.current() == 1) break; }
        require(choice.current() == 1, "the render thread takes cpu 2-4");
        require(choice.checkStream(limited) && choice.stream() == 3 && choice.streamMoves() == 2, "the stream at once to the X4");
        require(!choice.checkStream(limited) && choice.stream() == 3, "and stays");
        choice.resetShares();
        const auto cool = caps(2246000, 3052000, 3052000, 3782000);
        for (int i = 0; i < 20 && choice.current() != 3; ++i) choice.check(cool);
        require(choice.current() == 3 && choice.checkStream(cool) && choice.stream() == 2, "render home: the stream at once to cpu 5-6");
        // Its own moves wait ten checks after the last: cpu 5-6 capped hard at once.
        unsigned moved = 0;
        for (unsigned i = 1; i <= 20 && !moved; ++i) { choice.check(limited); if (choice.checkStream(limited)) moved = i; }
        require(moved == CoreChoice::kDwellChecks && choice.stream() == 1, "dwell: " + std::to_string(moved));
        // The stream goes by the caps, not by the shares the render thread's load measured: the X4 left at 38 % of
        // its cap (speed 529 at the cap, 203 at that share) still beats cpu 5-6's 291 for it.
        CoreChoice low = pixel();
        const auto midsLow = caps(2246000, 600000, 600000, 1305000);   // nothing to leave for while it learns
        for (int i = 0; i < 30; ++i) low.check(midsLow, seen(500000));
        for (int i = 0; i < 10 && low.current() == 3; ++i) low.check(limited, seen(500000));
        require(low.current() == 1 && low.share(3) < 0.40, "left the X4 at 38 %: share " + std::to_string(low.share(3)));
        unsigned took = 0;
        for (unsigned i = 1; i <= 5 && !took; ++i) if (low.checkStream(limited)) took = i;
        require(took == 3 && low.stream() == 3, "the stream still takes the X4, on the third check");
        // Its margin is the render thread's for leaving home: cpu 2-4 10 % faster than cpu 5-6 does not move it,
        // 25 % does.
        CoreChoice margin = pixel();
        require(run(margin, caps(2246000, 3052000, 2775000, 3782000), 1) == 0, "");
        for (int i = 0; i < 20; ++i) require(!margin.checkStream(caps(2246000, 3052000, 2775000, 3782000)), "10 % does not move the stream");
        unsigned moves25 = 0;
        for (int i = 0; i < 5; ++i) moves25 += margin.checkStream(caps(2246000, 3052000, 2440000, 3782000)) ? 1 : 0;
        require(moves25 == 1 && margin.stream() == 1, "25 % does");
        // A broken streak starts again.
        CoreChoice streak = pixel();
        streak.check(limited); streak.checkStream(limited);
        streak.check(limited); streak.checkStream(limited);
        streak.check(cool); streak.checkStream(cool);
        require(!(streak.check(limited), streak.checkStream(limited)) && !(streak.check(limited), streak.checkStream(limited))
                    && (streak.check(limited), streak.checkStream(limited)), "stream streak restarts");
    }
    // One mid cluster (little / mid / big): the stream on the mid cluster, on the big core while the render thread
    // has the mids. The big core and the little cores only: no cluster for it.
    {
        CoreChoice three({{{0, 1, 2, 3}, 300, 2000000}, {{4, 5, 6}, 700, 2800000}, {{7}, 1024, 3300000}});
        require(three.stream() == 1, "the stream on the mids");
        const auto hot = std::vector<unsigned>{2000000, 2800000, 900000};
        for (int i = 0; i < 10 && three.current() == 2; ++i) three.check(hot);
        require(three.current() == 1 && three.checkStream(hot) && three.stream() == 2, "render on the mids: the stream on the big core");
        CoreChoice two({{{0, 1, 2, 3}, 300, 2000000}, {{7}, 1024, 3300000}});
        require(two.stream() == -1 && !two.checkStream({2000000, 3300000}), "no eligible cluster for it");
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
