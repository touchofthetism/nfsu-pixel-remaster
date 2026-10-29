#pragma once
// Keeps host_perf's thread placement where it was put (host_perf.cpp; tested in tests/placement_keeper.cpp).
//
// A placement does not stay put by itself. Build 10's 00:34 session lost it nine minutes in: from then on the
// render thread ran on cpu2-7 instead of cpu7 alone and DXVK's threads reached cpu7, for the remaining eleven
// minutes (the next session kept it for all 28). Android rewrites a thread's affinity when it moves the
// process between cpusets, and the kernel widens the mask of a thread whose only cpu stops; nothing tells the
// app. The keeper remembers each placed thread's mask and nice value, and the profiler's sampler asks it
// once a second to put back whatever was replaced.
//
// A mask counts as kept while the thread's current mask lies within it: the kernel reports only the cpus that
// are active, so a placed cpu paused for heat is missing from the report without the placement being undone.
// A thread is followed by its id and its name together, so an id the system hands to a new thread after the
// placed one exits is never touched; a placed thread that is gone is dropped. Build 15: a thread whose name changed
// is still the same thread while its start time (the kernel's, /proc/self/task/<tid>/stat) is, and a name that
// cannot be read for a moment drops nothing. A 23-minute session of build 14 stopped following the render thread,
// which went on presenting, after a 5-minute pause, and with it the placement. Build 15's first session showed why:
// on the first resume after a long pause the render thread is renamed from the process's name to "Thread-5" (a
// system library attaches it to the Java VM while the swap chain is made again), and the name check took that for
// another thread. Every drop is now reported, with its reason.
#include <sched.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class PlacementKeeper {
public:
    // The system calls, replaceable in tests. Each returns 0 or an errno value (ESRCH: no such thread).
    struct System {
        std::function<int(int32_t tid, std::string* name)> getName;   // this process's threads only
        std::function<int(int32_t tid, uint64_t* start)> getStart;    // when it started (clock ticks since boot)
        std::function<int(int32_t tid, cpu_set_t* mask)> getAffinity;
        std::function<int(int32_t tid, const cpu_set_t& mask)> setAffinity;
        std::function<int(int32_t tid, int* nice)> getNice;
        std::function<int(int32_t tid, int nice)> setNice;
    };

    struct Thread {
        std::string label;        // for the log: "render", "dxvk-cs", ...
        int32_t tid = 0;
        std::string name;         // the thread's own name when placed (/proc/self/task/<tid>/comm)
        uint64_t start = 0;       // its start time when placed; 0: unknown (a new name then means another thread)
        bool keepMask = false;
        cpu_set_t mask{};
        bool keepNice = false;
        int nice = 0;
        bool failing = false;     // the last attempt to put it back failed (reported once until one works)
    };

    // What one check put back, or could not, or stopped following: one log line each.
    struct Change {
        std::string label, what;
        bool restored = false;    // an info line: put back, or followed under a new name; otherwise a warning
        bool dropped = false;     // no longer followed
    };

    explicit PlacementKeeper(System system) : system_(std::move(system)) {}

    void add(Thread thread) { threads_.push_back(std::move(thread)); }
    void clear() { threads_.clear(); }
    size_t size() const { return threads_.size(); }
    bool tracks(int32_t tid) const {
        for (const Thread& thread : threads_) if (thread.tid == tid) return true;
        return false;
    }

    // A placed thread's mask from now on (host_perf moved it on purpose: core_choice.h). False if not placed.
    bool setMask(int32_t tid, const cpu_set_t& mask) {
        for (Thread& thread : threads_)
            if (thread.tid == tid && thread.keepMask) { thread.mask = mask; thread.failing = false; return true; }
        return false;
    }

    // One pass over the placed threads. A failure is reported the first time; the next success is reported.
    std::vector<Change> check(unsigned cpus) {
        std::vector<Change> changes;
        for (size_t index = 0; index < threads_.size();) {
            Thread& thread = threads_[index];
            std::string name, ended;
            int read = system_.getName(thread.tid, &name);
            if (read == ESRCH) ended = "the thread ended";
            else if (read != 0) { ++index; continue; }   // unreadable for now: looked at again next time
            else if (name != thread.name) {
                uint64_t start = 0;
                if (thread.start && system_.getStart && system_.getStart(thread.tid, &start) == 0 && start == thread.start) {
                    changes.push_back({thread.label, "renamed \"" + thread.name + "\" to \"" + name + "\" (the same thread: its start time), still followed", true});
                    thread.name = name;
                } else ended = "its id now names another thread (\"" + name + "\")";
            }
            bool gone = !ended.empty();
            bool attempted = false, ok = true;
            std::string what;
            if (!gone && thread.keepMask) {
                cpu_set_t now;
                CPU_ZERO(&now);
                int error = system_.getAffinity(thread.tid, &now);
                if (error == ESRCH) { gone = true; ended = "the thread ended"; }
                else if (error == 0 && (CPU_COUNT(&now) == 0 || !within(now, thread.mask))) {
                    attempted = true;
                    int set = system_.setAffinity(thread.tid, thread.mask);
                    if (set == ESRCH) { gone = true; ended = "the thread ended"; }
                    else {
                        what = "affinity " + describe(now, cpus) + (set == 0 ? ", put back to " + describe(thread.mask, cpus)
                                : ", could not put back " + describe(thread.mask, cpus) + " (" + std::strerror(set) + ")");
                        ok = set == 0;
                    }
                }
            }
            if (!gone && thread.keepNice) {
                int now = 0;
                int error = system_.getNice(thread.tid, &now);
                if (error == ESRCH) { gone = true; ended = "the thread ended"; }
                else if (error == 0 && now != thread.nice) {
                    attempted = true;
                    int set = system_.setNice(thread.tid, thread.nice);
                    if (set == ESRCH) { gone = true; ended = "the thread ended"; }
                    else {
                        what += (what.empty() ? "" : "; ") + std::string("nice ") + std::to_string(now)
                              + (set == 0 ? ", put back to " + std::to_string(thread.nice)
                                          : ", could not put back " + std::to_string(thread.nice) + " (" + std::strerror(set) + ")");
                        ok = ok && set == 0;
                    }
                }
            }
            if (gone) {
                changes.push_back({thread.label, "no longer followed: " + ended, false, true});
                threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(index));
                ++dropped_;
                continue;
            }
            if (attempted) {
                if (ok) {
                    ++restored_;
                    thread.failing = false;
                    changes.push_back({thread.label, what, true});
                } else {
                    ++failed_;
                    if (!thread.failing) changes.push_back({thread.label, what, false});
                    thread.failing = true;
                }
            }
            ++index;
        }
        return changes;
    }

    uint64_t restored() const { return restored_; }   // checks that put a thread's placement back
    uint64_t failed() const { return failed_; }       // checks that found it changed and could not
    uint64_t dropped() const { return dropped_; }     // placed threads no longer followed

    // Every cpu set in `inner` is also set in `outer`.
    static bool within(const cpu_set_t& inner, const cpu_set_t& outer) {
        for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &inner) && !CPU_ISSET(cpu, &outer)) return false;
        return true;
    }

    // "cpu 7", "cpu 0-6", "cpu 0-1,3,5-7", "no cpu" (the first `cpus` cpus).
    static std::string describe(const cpu_set_t& mask, unsigned cpus) {
        std::string out;
        for (unsigned cpu = 0; cpu < cpus && cpu < CPU_SETSIZE;) {
            if (!CPU_ISSET(cpu, &mask)) { ++cpu; continue; }
            unsigned last = cpu;
            while (last + 1 < cpus && last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, &mask)) ++last;
            if (!out.empty()) out += ",";
            out += std::to_string(cpu);
            if (last > cpu) out += "-" + std::to_string(last);
            cpu = last + 1;
        }
        return out.empty() ? "no cpu" : "cpu " + out;
    }

private:
    System system_;
    std::vector<Thread> threads_;
    uint64_t restored_ = 0, failed_ = 0, dropped_ = 0;
};
