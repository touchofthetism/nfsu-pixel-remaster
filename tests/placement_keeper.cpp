// The placement keeper (android/app/src/main/cpp/placement_keeper.h) against a simulated system: a mask
// Android replaced is put back and logged once; a placed cpu that is merely inactive is left alone; a restore
// the kernel refuses (the only placed cpu paused) is reported once, retried, and its success reported; nice
// values come back too; a thread that ended, or whose id now names another thread, is dropped untouched.
#include "../android/app/src/main/cpp/placement_keeper.h"

#include <cstdio>
#include <cstdlib>
#include <map>

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

namespace {
struct FakeThread { std::string name; cpu_set_t mask; int nice; };
std::map<int32_t, FakeThread> threads;
cpu_set_t active;          // cpus the kernel will schedule on (a paused one is missing)
unsigned setCalls = 0, niceCalls = 0;

cpu_set_t cpus(std::initializer_list<int> list) {
    cpu_set_t set; CPU_ZERO(&set);
    for (int cpu : list) CPU_SET(cpu, &set);
    return set;
}

PlacementKeeper::System fakeSystem() {
    PlacementKeeper::System system;
    system.getName = [](int32_t tid, std::string* name) {
        auto it = threads.find(tid);
        if (it == threads.end()) return ESRCH;
        *name = it->second.name;
        return 0;
    };
    // Like sched_getaffinity: the thread's mask limited to the active cpus.
    system.getAffinity = [](int32_t tid, cpu_set_t* mask) {
        auto it = threads.find(tid);
        if (it == threads.end()) return ESRCH;
        CPU_AND(mask, &it->second.mask, &active);
        return 0;
    };
    // Like sched_setaffinity: refused when no cpu of the new mask is active.
    system.setAffinity = [](int32_t tid, const cpu_set_t& mask) {
        ++setCalls;
        auto it = threads.find(tid);
        if (it == threads.end()) return ESRCH;
        cpu_set_t usable; CPU_AND(&usable, &mask, &active);
        if (CPU_COUNT(&usable) == 0) return EINVAL;
        it->second.mask = mask;
        return 0;
    };
    system.getNice = [](int32_t tid, int* nice) {
        auto it = threads.find(tid);
        if (it == threads.end()) return ESRCH;
        *nice = it->second.nice;
        return 0;
    };
    system.setNice = [](int32_t tid, int nice) {
        ++niceCalls;
        auto it = threads.find(tid);
        if (it == threads.end()) return ESRCH;
        it->second.nice = nice;
        return 0;
    };
    return system;
}
}  // namespace

int main() {
    const unsigned count = 8;
    active = cpus({0, 1, 2, 3, 4, 5, 6, 7});
    const cpu_set_t big = cpus({7}), rest = cpus({0, 1, 2, 3, 4, 5, 6}), all = cpus({0, 1, 2, 3, 4, 5, 6, 7});

    require(PlacementKeeper::describe(big, count) == "cpu 7", "describe one cpu");
    require(PlacementKeeper::describe(rest, count) == "cpu 0-6", "describe a range");
    require(PlacementKeeper::describe(cpus({0, 1, 3, 5, 6, 7}), count) == "cpu 0-1,3,5-7", "describe ranges and singles");
    require(PlacementKeeper::describe(cpus({}), count) == "no cpu", "describe nothing");
    require(PlacementKeeper::within(big, all) && !PlacementKeeper::within(all, big), "within");

    threads[100] = {"Thread-7", big, -10};
    threads[101] = {"dxvk-cs", rest, -10};
    threads[102] = {"dxvk-queue", rest, 0};
    PlacementKeeper keeper(fakeSystem());
    PlacementKeeper::Thread render; render.label = "render"; render.tid = 100; render.name = "Thread-7";
    render.keepMask = true; render.mask = big; render.keepNice = true; render.nice = -10;
    PlacementKeeper::Thread cs; cs.label = "dxvk-cs"; cs.tid = 101; cs.name = "dxvk-cs";
    cs.keepMask = true; cs.mask = rest; cs.keepNice = true; cs.nice = -10;
    PlacementKeeper::Thread queue; queue.label = "dxvk-queue"; queue.tid = 102; queue.name = "dxvk-queue";
    queue.keepMask = true; queue.mask = rest;
    keeper.add(render); keeper.add(cs); keeper.add(queue);

    // Nothing changed: no calls, nothing reported.
    require(keeper.check(count).empty() && setCalls == 0 && niceCalls == 0, "an intact placement is left alone");

    // The build-10 loss: every mask replaced by the whole cpuset.
    threads[100].mask = all; threads[101].mask = all; threads[102].mask = all;
    auto changes = keeper.check(count);
    require(changes.size() == 3 && keeper.restored() == 3, "three replaced masks put back");
    require(changes[0].label == "render" && changes[0].restored, "the render thread first");
    require(changes[0].what == "affinity cpu 0-7, put back to cpu 7", changes[0].what.c_str());
    require(CPU_EQUAL(&threads[100].mask, &big) && CPU_EQUAL(&threads[102].mask, &rest), "masks restored");
    require(keeper.check(count).empty(), "and then left alone");

    // cpu 6 paused: the queue thread's mask reads 0-5, which is within its placement. No action.
    active = cpus({0, 1, 2, 3, 4, 5, 7});
    unsigned before = setCalls;
    require(keeper.check(count).empty() && setCalls == before, "an inactive placed cpu is not a lost placement");

    // cpu 7 paused: the kernel widened the render thread's mask (no cpu of its own was left). Putting it back
    // is refused while cpu 7 is off: reported once, retried quietly, and the success reported.
    active = cpus({0, 1, 2, 3, 4, 5, 6});
    threads[100].mask = all;
    changes = keeper.check(count);
    require(changes.size() == 1 && !changes[0].restored && keeper.failed() == 1, "a refused restore is reported");
    require(changes[0].what.find("could not put back cpu 7") != std::string::npos, changes[0].what.c_str());
    require(keeper.check(count).empty() && keeper.failed() == 2, "and retried without another report");
    active = all;
    changes = keeper.check(count);
    require(changes.size() == 1 && changes[0].restored && CPU_EQUAL(&threads[100].mask, &big), "put back once cpu 7 returns");

    // A nice value reset.
    threads[101].nice = 0;
    changes = keeper.check(count);
    require(changes.size() == 1 && changes[0].label == "dxvk-cs" && changes[0].what == "nice 0, put back to -10" && threads[101].nice == -10,
            "nice put back");

    // The queue thread ends and its id goes to a new thread with another name: dropped, never touched.
    threads[102] = {"AsyncTask #3", all, 0};
    before = setCalls;
    require(keeper.check(count).empty() && setCalls == before && keeper.size() == 2 && keeper.dropped() == 1,
            "a reused thread id is not touched");
    require(CPU_EQUAL(&threads[102].mask, &all), "the new thread keeps its own mask");
    // The command stream ends.
    threads.erase(101);
    require(keeper.check(count).empty() && keeper.size() == 1 && keeper.dropped() == 2, "an ended thread is dropped");

    keeper.clear();
    require(keeper.size() == 0 && keeper.check(count).empty(), "cleared");
    std::puts("placement keeper ok");
    return 0;
}
