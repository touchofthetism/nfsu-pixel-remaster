// host_perf.cpp's placement end to end (Linux; the Android-only parts fall back as on a phone without them).
// A fake cpu directory gives the clusters, capacities and caps, and real threads named as the game's are placed:
//  - on this machine's own cpus with real affinity calls (any machine with two cpus or more);
//  - on the Pixel 10 Pro XL's eight cpus (cpu0-1, cpu2-4, cpu5-6, cpu7), simulated with the caps of 2026-10-28.
// Checks the clusters read from related_cpus and cpu_capacity (and the fallbacks without them), the adoption, the
// move to a faster cluster when the big core's cap falls (and between the mid clusters), DXVK's threads moving
// out of its way, a mask Android replaces put back to the new cluster, the way home, "Biggest core only" (no
// moves), "System scheduler" (no placement), hidden caps (no moves) and the JSON the overlay and summary read.
// Build 15: the performance hint's target (three quarters of the frame period), the render thread followed under
// a new name, and followed again from its own next frame when the keeper loses it anyway (moves then go on).
// Build 16: the command stream on a cluster of its own (the fastest besides the render thread's, at once when the
// render thread takes it); with a simulated clock, schedstat and time_in_state, the render thread leaving an X4
// that gives it 77 % of its cap and coming home when the thermal status changes; Pixel's PMU limit nodes read;
// time_in_state found stale; the JSON's clocks, threads and shares.
#include "../android/app/src/main/cpp/host_perf.cpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <thread>

namespace fs = std::filesystem;

static void require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::exit(1); }
}

namespace {
struct FakeCluster { std::vector<int> cpus; unsigned capacity, maxKhz; };

fs::path root;
std::vector<FakeCluster> layout;

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << text << "\n";
    require(bool(file), "write " + path.string());
}
void writeCap(const FakeCluster& cluster, unsigned khz) {
    for (int cpu : cluster.cpus) writeFile(root / ("cpu" + std::to_string(cpu)) / "cpufreq/scaling_max_freq", std::to_string(khz));
}
// A fresh cpu directory for `clusters`, every cluster at its top clock.
void writeTree(std::vector<FakeCluster> clusters) {
    if (!root.empty()) fs::remove_all(root);
    char pattern[] = "/tmp/host_perf_XXXXXX";
    require(mkdtemp(pattern) != nullptr, "mkdtemp");
    root = pattern;
    layout = std::move(clusters);
    for (const auto& cluster : layout) {
        std::string related;
        for (int cpu : cluster.cpus) related += std::to_string(cpu) + " ";
        for (int cpu : cluster.cpus) {
            fs::path dir = root / ("cpu" + std::to_string(cpu));
            writeFile(dir / "cpu_capacity", std::to_string(cluster.capacity));
            writeFile(dir / "cpufreq/cpuinfo_max_freq", std::to_string(cluster.maxKhz));
            writeFile(dir / "cpufreq/related_cpus", related);   // the kernel's format: "2 3 4 "
        }
        writeCap(cluster, cluster.maxKhz);
    }
    host_perf::cpuSysfs = root.string();
}

std::vector<int> affinity(int32_t tid) {
    cpu_set_t set; CPU_ZERO(&set);
    require(host_perf::os.getAffinity(tid, &set) == 0, "getAffinity");
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
    return cpus;
}
std::string list(const std::vector<int>& cpus) { return CoreChoice::describe(cpus); }
std::vector<int> range(int first, int last) { std::vector<int> out; for (int cpu = first; cpu <= last; ++cpu) out.push_back(cpu); return out; }
std::vector<int> concat(std::initializer_list<std::vector<int>> parts) {
    std::vector<int> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    std::sort(out.begin(), out.end());
    return out;
}
bool has(const std::string& json, const std::string& part) { return json.find(part) != std::string::npos; }
// Checks until the render thread is on `cpus` (at most `limit`); how many it took, or -1.
int checksUntil(int32_t render, const std::vector<int>& cpus, int limit) {
    for (int checks = 0; checks <= limit; ++checks) {
        if (affinity(render) == cpus) return checks;
        if (checks < limit) host_perf::maintainPlacement();
    }
    return -1;
}

// A thread with a name of its own that runs `start`, then whatever run() hands it, until the test ends it.
class NamedThread {
public:
    NamedThread(const char* name, std::function<void()> start) {
        thread_ = std::thread([this, name, start] {
            pthread_setname_np(pthread_self(), name);
            tid_ = int32_t(syscall(SYS_gettid));
            if (start) start();
            std::unique_lock<std::mutex> lock(mutex_);
            ready_ = true;
            changed_.notify_all();
            for (;;) {
                changed_.wait(lock, [this] { return stop_ || task_; });
                if (stop_) return;
                auto task = std::move(task_);
                task_ = nullptr;
                lock.unlock();
                task();
                lock.lock();
                done_ = true;
                changed_.notify_all();
            }
        });
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return ready_; });
    }
    ~NamedThread() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        changed_.notify_all();
        thread_.join();
    }
    int32_t tid() const { return tid_; }
    // Runs `task` on this thread and waits for it.
    void run(std::function<void()> task) {
        std::unique_lock<std::mutex> lock(mutex_);
        task_ = std::move(task);
        done_ = false;
        changed_.notify_all();
        changed_.wait(lock, [this] { return done_; });
    }
private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::function<void()> task_;
    bool ready_ = false, stop_ = false, done_ = false;
    int32_t tid_ = 0;
};
void adoptFastest() { host_perf::adoptRenderThread(host_perf::Placement::FastestCores, 60); }

// This machine's cpus as clusters, with real affinity calls. n = 2: a mid and a big core; n = 3: little, mid,
// big; n >= 4: little, two mid clusters, big.
void realMachine() {
    long count = host_perf::os.cpus();
    std::vector<int> usable = affinity(0);
    bool contiguous = count >= 2 && usable.size() == size_t(count);
    for (size_t i = 0; contiguous && i < usable.size(); ++i) contiguous = usable[i] == int(i);
    if (!contiguous) { std::puts("host_perf: this machine skipped (needs cpus 0..n-1 all usable, n >= 2)"); return; }
    const int n = int(count), big = n - 1;
    const std::vector<int> all = range(0, big);
    if (n == 2) writeTree({{{0}, 700, 3000000}, {{1}, 1024, 3800000}});
    else if (n == 3) writeTree({{{0}, 300, 2000000}, {{1}, 700, 3000000}, {{2}, 1024, 3800000}});
    else writeTree({{{0}, 300, 2000000}, {{1}, 700, 3000000}, {range(2, big - 1), 700, 3000000}, {{big}, 1024, 3800000}});
    const FakeCluster home = layout.back();
    // Where the render thread goes when the big core is capped (ties: the higher cpus), and DXVK's threads then.
    const std::vector<int> away = layout[layout.size() - 2].cpus;
    const std::vector<int> helpersAway = n >= 4 ? concat({layout[0].cpus, layout[1].cpus})          // not the capped big core
                                       : n == 3 ? concat({layout[0].cpus, home.cpus})                // only the little core else
                                                : home.cpus;
    const std::vector<int> helpersHome = range(0, big - 1);
    // The command stream's own cluster (build 16): the fastest besides the render thread's (ties: the higher cpus).
    const std::vector<int> streamHome = n >= 4 ? range(2, big - 1) : n == 3 ? std::vector<int>{1} : std::vector<int>{0};
    const std::vector<int> streamAway = n >= 4 ? std::vector<int>{1} : home.cpus;   // the X4-like core when it is the only other

    // The clusters as the kernel describes them.
    {
        auto topology = host_perf::readTopology();
        require(topology.clusters.size() == layout.size(), "one cluster per cpufreq policy: " + topology.clusterSummary);
        for (size_t i = 0; i < layout.size(); ++i)
            require(topology.clusters[i].cpus == layout[i].cpus && topology.clusters[i].capacity == layout[i].capacity
                        && topology.clusters[i].maxKhz == layout[i].maxKhz, "cluster " + std::to_string(i) + ": " + topology.clusterSummary);
        require(topology.fastest == std::to_string(big), "fastest: " + topology.fastest);
        require(!has(topology.clusterSummary, "estimated"), topology.clusterSummary);
    }

    NamedThread dxvk("dxvk-cs", nullptr), submit("dxvk-submit", nullptr);

    // "Fastest core": adopted on the big core, DXVK's thread everywhere else.
    {
        NamedThread render("render-test", adoptFastest);
        require(affinity(render.tid()) == home.cpus, "render on the big core: " + list(affinity(render.tid())));
        require(affinity(submit.tid()) == helpersHome, "DXVK off it: " + list(affinity(submit.tid())));
        require(affinity(dxvk.tid()) == streamHome, "the command stream on " + list(streamHome) + ": " + list(affinity(dxvk.tid())));
        std::string json = host_perf::statusJson(false);
        require(has(json, "\"affinity\":\"render thread on cpu " + std::to_string(big) + " (follows the fastest cluster)\""), json);
        require(has(json, "\"renderCpus\":\"" + std::to_string(big) + "\"") && has(json, "\"coreMoves\":0"), json);

        // An uncapped phone: no move however long.
        for (int i = 0; i < 15; ++i) host_perf::maintainPlacement();
        require(affinity(render.tid()) == home.cpus, "uncapped: stays");

        // The big core capped to 1 GHz (speed 1024 * 1.0 / 3.8 = 269, times 1.30 for its work per clock: 350, against a
        // mid core's 700): away on the third check.
        writeCap(home, 1000000);
        host_perf::maintainPlacement();
        host_perf::maintainPlacement();
        require(affinity(render.tid()) == home.cpus, "not before three checks");
        host_perf::maintainPlacement();
        require(affinity(render.tid()) == away, "moved to " + list(away) + ": " + list(affinity(render.tid())));
        require(affinity(submit.tid()) == helpersAway, "DXVK moved to " + list(helpersAway) + ": " + list(affinity(submit.tid())));
        require(affinity(dxvk.tid()) == streamAway, "the command stream at once to " + list(streamAway) + ": " + list(affinity(dxvk.tid())));
        json = host_perf::statusJson(true);
        std::string awayList = list(away).substr(4), homeList = std::to_string(big);
        require(has(json, "\"renderCpus\":\"" + awayList + "\"") && has(json, "\"coreMoves\":1"), json);
        require(std::regex_search(json, std::regex("\"moves\":\\[\\[0,\"" + homeList + "\"\\],\\[[0-9]+,\"" + awayList + "\"\\]\\]")), json);
        require(has(json, "\"clusters\":[{\"cpus\":\"" + list(layout[0].cpus).substr(4) + "\"") && has(json, "\"capMinMhz\":1000"), json);
        require(!has(host_perf::statusJson(false), "\"moves\""), "the overlay's JSON carries no move list");

        // Android replaces the mask: put back to the new cluster, not the old one.
        cpu_set_t everything = host_perf::maskOf(all);
        require(host_perf::os.setAffinity(render.tid(), everything) == 0, "widen");
        host_perf::maintainPlacement();
        require(affinity(render.tid()) == away, "put back to " + list(away) + ": " + list(affinity(render.tid())));
        require(has(host_perf::statusJson(false), "\"placementRestored\":1"), host_perf::statusJson(false));

        // The cap lifts: home again (after five checks as fast, and ten since the move).
        writeCap(home, home.maxKhz);
        int checks = checksUntil(render.tid(), home.cpus, 20);
        require(checks >= 5, "home after " + std::to_string(checks) + " checks");
        require(affinity(submit.tid()) == helpersHome && affinity(dxvk.tid()) == streamHome, "DXVK back: " + list(affinity(submit.tid())));
        json = host_perf::statusJson(true);
        require(has(json, "\"coreMoves\":2") && has(json, "\"renderCpus\":\"" + homeList + "\""), json);
        host_perf::release();
    }

    // "Biggest core only": the same caps never move it.
    {
        NamedThread render("render-test", [] { host_perf::adoptRenderThread(host_perf::Placement::BigCore, 60); });
        writeCap(home, 1000000);
        for (int i = 0; i < 20; ++i) host_perf::maintainPlacement();
        require(affinity(render.tid()) == home.cpus, "fixed on the big core");
        std::string json = host_perf::statusJson(true);
        require(has(json, "(fixed)\"") && has(json, "\"renderCpus\":\"" + std::to_string(big) + "\"") && !has(json, "\"clusters\""), json);
        writeCap(home, home.maxKhz);
        host_perf::release();
    }

    // "System scheduler": nothing placed, nothing reported as placed.
    {
        NamedThread render("render-test", [] { host_perf::adoptRenderThread(host_perf::Placement::System, 60); });
        std::string json = host_perf::statusJson(true);
        require(has(json, "\"affinity\":\"system placement\"") && !has(json, "renderCpus") && !has(json, "\"clusters\""), json);
        host_perf::release();
    }

    // A phone that hides its caps keeps the big core.
    {
        for (int cpu : all) fs::remove(root / ("cpu" + std::to_string(cpu)) / "cpufreq/scaling_max_freq");
        NamedThread render("render-test", adoptFastest);
        for (int i = 0; i < 20; ++i) host_perf::maintainPlacement();
        require(affinity(render.tid()) == home.cpus, "unreadable caps: stays");
        host_perf::release();
    }

    // Without cpu_capacity the capacities are estimated from the clocks (the big cores 25 % more work per clock);
    // without related_cpus the cpus sharing a top clock form one cluster.
    {
        for (int cpu : all) fs::remove(root / ("cpu" + std::to_string(cpu)) / "cpu_capacity");
        auto topology = host_perf::readTopology();
        require(has(topology.clusterSummary, "estimated") && topology.clusters.back().capacity == 1024, topology.clusterSummary);
        require(topology.clusters[topology.clusters.size() - 2].capacity == unsigned(1024.0 * 3000000 / 3800000 * 0.8), topology.clusterSummary);
        for (int cpu : all) fs::remove(root / ("cpu" + std::to_string(cpu)) / "cpufreq/related_cpus");
        topology = host_perf::readTopology();
        size_t expected = n >= 4 ? 3 : layout.size();   // the two mid clusters share 3.0 GHz
        require(topology.clusters.size() == expected && topology.fastest == std::to_string(big), topology.clusterSummary);
    }
    std::printf("host_perf: this machine's %d cpus ok\n", n);
}

// The Pixel 10 Pro XL: eight cpus simulated (the affinity calls kept in a table, refused like the kernel's when
// no cpu of the mask exists), the clusters and capacities as its kernel gives them, the caps of 2026-10-28.
struct Simulated {
    std::mutex mutex;
    std::map<int32_t, cpu_set_t> masks;
} simulated;

void pixel() {
    host_perf::os.cpus = [] { return 8L; };
    host_perf::os.getAffinity = [](int32_t tid, cpu_set_t* mask) {
        std::lock_guard<std::mutex> lock(simulated.mutex);
        auto it = simulated.masks.find(tid);
        *mask = it == simulated.masks.end() ? host_perf::maskOf(range(0, 7)) : it->second;
        return 0;
    };
    host_perf::os.setAffinity = [](int32_t tid, const cpu_set_t& mask) {
        cpu_set_t usable = host_perf::maskOf({});
        for (int cpu = 0; cpu < 8; ++cpu) if (CPU_ISSET(cpu, &mask)) CPU_SET(cpu, &usable);
        if (CPU_COUNT(&usable) == 0) return EINVAL;
        std::lock_guard<std::mutex> lock(simulated.mutex);
        simulated.masks[tid] = usable;
        return 0;
    };
    writeTree({{{0, 1}, 207, 2246000}, {{2, 3, 4}, 824, 3052000}, {{5, 6}, 825, 3052000}, {{7}, 1024, 3782000}});
    const FakeCluster little = layout[0], midA = layout[1], midB = layout[2], x4 = layout[3];
    {
        auto topology = host_perf::readTopology();
        require(topology.clusterSummary == "cpu 0-1 (capacity 207), cpu 2-4 (capacity 824), cpu 5-6 (capacity 825), cpu 7 (capacity 1024)"
                    && topology.fastest == "7", topology.clusterSummary);
    }
    NamedThread cs("dxvk-cs", nullptr), submit("dxvk-submit", nullptr), queue("dxvk-queue", nullptr), recycle("dxvk-recycle", nullptr);
    const NamedThread* helpers[] = {&submit, &queue, &recycle};
    // DXVK's other threads on `cpus`, the command stream on `stream` (its own cluster since build 16).
    auto dxvkOn = [&](const std::vector<int>& cpus, const std::vector<int>& stream) {
        for (const NamedThread* thread : helpers) if (affinity(thread->tid()) != cpus) return false;
        return affinity(cs.tid()) == stream;
    };
    NamedThread render("Thread-7", adoptFastest);
    std::string json = host_perf::statusJson(false);
    require(affinity(render.tid()) == x4.cpus && dxvkOn(range(0, 6), midB.cpus), "adopted: render on cpu 7, DXVK on 0-6, the stream on 5-6");
    require(has(json, "\"dxvk\":\"4 of 4 DXVK threads off cpu 7, the command stream on cpu 5-6 (follows the fastest other cluster)"), json);

    // The performance hint's target: three quarters of the frame period, not under the 120 Hz panel's 8.33 ms.
    require(host_perf::targetNanos(60) == 12500000 && host_perf::targetNanos(30) == 25000000 && host_perf::targetNanos(45) == 16666666
                && host_perf::targetNanos(90) == 8333333 && host_perf::targetNanos(120) == 8333333 && host_perf::targetNanos(0) == 8333333,
            "hint targets");
    require(has(json, "\"targetFps\":60"), json);

    // The render thread takes a new name: the same thread (its start time), still followed, its placement kept.
    render.run([] { pthread_setname_np(pthread_self(), "GameThread"); });
    host_perf::maintainPlacement();
    require(host_perf::os.setAffinity(render.tid(), host_perf::maskOf(range(0, 7))) == 0, "widen");
    host_perf::maintainPlacement();
    json = host_perf::statusJson(false);
    require(affinity(render.tid()) == x4.cpus && has(json, "\"placementDropped\":0"), "renamed: still on cpu 7; " + json);

    // Lost anyway (here: its start time reads differently): no longer followed, its widened mask left alone, and
    // no move made; its next frame follows it again, and the keeper's next check puts it back on cpu 7.
    auto realStart = host_perf::os.threadStart;
    const int32_t renderTid = render.tid();
    host_perf::os.threadStart = [realStart, renderTid](int32_t tid, uint64_t* start) {
        int result = realStart(tid, start);
        if (tid == renderTid) *start += 1;
        return result;
    };
    render.run([] { pthread_setname_np(pthread_self(), "GameThread2"); });
    host_perf::maintainPlacement();
    host_perf::os.threadStart = realStart;
    require(host_perf::os.setAffinity(render.tid(), host_perf::maskOf(range(0, 7))) == 0, "widen");
    host_perf::maintainPlacement();
    json = host_perf::statusJson(false);
    require(affinity(render.tid()) == range(0, 7) && has(json, "\"placementDropped\":1") && has(json, "\"renderFollowedAgain\":0"),
            "lost: not put back; " + json);
    render.run([] { host_perf::reportFrame(std::chrono::milliseconds(10)); });
    host_perf::maintainPlacement();
    json = host_perf::statusJson(false);
    require(affinity(render.tid()) == x4.cpus && has(json, "\"renderFollowedAgain\":1"), "followed again and put back: " + json);

    // 05:40: the X4 allowed 1152 MHz (speed 312, 467 for its work per clock), both A725 clusters 2688 (726, 727):
    // to cpu 5-6 on the third check; DXVK's threads off it and off the X4, the command stream at once to cpu 2-4.
    writeCap(little, 2246000); writeCap(midA, 2688000); writeCap(midB, 2688000); writeCap(x4, 1152000);
    require(checksUntil(render.tid(), midB.cpus, 10) == 3, "to cpu 5-6 on the third check");
    require(dxvkOn(range(0, 4), midA.cpus), "DXVK on cpu 0-4, the stream on 2-4: " + list(affinity(cs.tid())));
    json = host_perf::statusJson(true);
    require(has(json, "\"renderCpus\":\"5-6\"") && has(json, "\"clusters\":[{\"cpus\":\"0-1\"") && has(json, "{\"cpus\":\"2-4\"")
                && has(json, "{\"cpus\":\"5-6\"") && has(json, "{\"cpus\":\"7\",\"capacity\":1024,\"maxMhz\":3782"), json);
    require(std::regex_search(json, std::regex("\"moves\":\\[\\[0,\"7\"\\],\\[[0-9]+,\"5-6\"\\]\\]")), json);

    // Android widens the render thread's mask: back to cpu 5-6.
    require(host_perf::os.setAffinity(render.tid(), host_perf::maskOf(range(0, 7))) == 0, "widen");
    host_perf::maintainPlacement();
    require(affinity(render.tid()) == midB.cpus, "put back to cpu 5-6");

    // cpu 5-6 capped harder (1536 MHz, speed 415) than cpu 2-4 (726): over to cpu 2-4 once ten checks have passed;
    // DXVK's threads then on the little cores and cpu 5-6.
    writeCap(midB, 1536000);
    require(checksUntil(render.tid(), midA.cpus, 20) > 0, "to cpu 2-4");
    require(dxvkOn(concat({little.cpus, midB.cpus}), x4.cpus), "DXVK on cpu 0-1,5-6, the stream on the X4: " + list(affinity(cs.tid())));

    // Everything uncapped: home to the X4, DXVK's threads back on cpu 0-6.
    writeCap(midA, 3052000); writeCap(midB, 3052000); writeCap(x4, 3782000);
    require(checksUntil(render.tid(), x4.cpus, 20) >= 5, "home to cpu 7");
    require(dxvkOn(range(0, 6), midB.cpus), "DXVK back on cpu 0-6, the stream on 5-6");
    json = host_perf::statusJson(true);
    require(has(json, "\"coreMoves\":3") && has(json, "\"renderCpus\":\"7\"") && has(json, "\"streamCpus\":\"5-6\",\"streamMoves\":3"), json);
    require(std::regex_search(json, std::regex("\"streamMoveList\":\\[\\[0,\"5-6\"\\],\\[[0-9]+,\"2-4\"\\],\\[[0-9]+,\"7\"\\],\\[[0-9]+,\"5-6\"\\]\\]")), json);
    require(std::regex_search(json, std::regex("\"moves\":\\[\\[0,\"7\"\\],\\[[0-9]+,\"5-6\"\\],\\[[0-9]+,\"2-4\"\\],\\[[0-9]+,\"7\"\\]\\]")), json);
    host_perf::release();
    std::puts("host_perf: Pixel 10 Pro XL simulated ok");
}
// Build 16 on the simulated Pixel, with a clock, schedstat, last cpus, the thermal status and time_in_state the
// test drives: one simulated second a check.
struct Driven {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::map<int32_t, std::pair<uint64_t, uint64_t>> sched;   // tid -> ns on a cpu, ns waiting for one
    std::map<int32_t, int> cpu;
    int thermal = 1;
    std::map<int, std::map<unsigned, uint64_t>> ticks;         // first cpu of a cluster -> kHz -> ticks of 10 ms
} driven;
void writeTimeInState(int firstCpu) {
    std::string text;
    for (const auto& [khz, ticks] : driven.ticks[firstCpu]) text += std::to_string(khz) + " " + std::to_string(ticks) + "\n";
    if (!text.empty()) text.pop_back();
    writeFile(root / ("cpu" + std::to_string(firstCpu)) / "cpufreq/stats/time_in_state", text);
}
// A second at these clocks: `mix` is {kHz, ticks} (100 ticks a second).
void runFor(int firstCpu, std::vector<std::pair<unsigned, uint64_t>> mix) {
    for (const auto& [khz, ticks] : mix) driven.ticks[firstCpu][khz] += ticks;
    writeTimeInState(firstCpu);
}
void pixelMeasured() {
    host_perf::os.schedstat = [](int32_t tid, uint64_t* run, uint64_t* wait) {
        auto it = driven.sched.find(tid);
        if (it == driven.sched.end()) return host_perf::threadSchedstat(tid, run, wait);
        *run = it->second.first; *wait = it->second.second; return 0;
    };
    host_perf::os.lastCpu = [](int32_t tid, int* cpu) {
        auto it = driven.cpu.find(tid);
        if (it == driven.cpu.end()) return host_perf::threadCpu(tid, cpu);
        *cpu = it->second; return 0;
    };
    host_perf::os.now = [] { return driven.now; };
    host_perf::os.thermalStatus = [] { return driven.thermal; };
    writeTree({{{0, 1}, 207, 2246000}, {{2, 3, 4}, 824, 3052000}, {{5, 6}, 825, 3052000}, {{7}, 1024, 3782000}});
    const FakeCluster little = layout[0], midA = layout[1], midB = layout[2], x4 = layout[3];
    // Pixel's PMU limit nodes for two clusters, and its switch.
    writeFile(root / "cpu7/cpufreq/sched_pixel/limit_frequency", "883000");
    writeFile(root / "cpu7/cpufreq/sched_pixel/spc_threshold", "62");
    writeFile(root / "cpu7/cpufreq/sched_pixel/lcpi_threshold", "0");
    writeFile(root / "cpu2/cpufreq/sched_pixel/limit_frequency", "1536000");
    writeFile(root / "cpu2/cpufreq/sched_pixel/spc_threshold", "59");
    host_perf::vendorSchedPoll = (root / "pmu_poll_enable").string();
    writeFile(root / "pmu_poll_enable", "1");
    // Every clock of the tables from the start, as the kernel lists them.
    for (int first : {0, 2, 5, 7}) {
        driven.ticks[first] = {};
        for (unsigned khz : {883000u, 1075000u, 1152000u, 1305000u, 1536000u, 2188000u, 2208000u, 2246000u, 3052000u, 3782000u}) driven.ticks[first][khz] = 0;
        writeTimeInState(first);
    }
    // The 22:25 session's hot caps: the X4 allowed 1305 MHz (529), cpu 2-4 2188 (591), cpu 5-6 1075 (291).
    writeCap(little, 2246000); writeCap(midA, 2188000); writeCap(midB, 1075000); writeCap(x4, 1305000);
    NamedThread cs("dxvk-cs", nullptr), submit("dxvk-submit", nullptr);
    NamedThread render("render-test", adoptFastest);
    std::string json = host_perf::statusJson(false);
    require(has(json, "\"pmu\":\"cpu 2-4: limit 1536 MHz spc_threshold 59; cpu 7: limit 883 MHz spc_threshold 62 lcpi_threshold 0; pmu_poll_enable 1\""), json);
    require(affinity(render.tid()) == x4.cpus && affinity(cs.tid()) == midB.cpus, "adopted: the stream on 5-6");
    // A second of play: the render thread on a cpu 95 % of it, the stream half of it; 50 frames against the 60 cap
    // (behind); the render thread's cluster at what Pixel's PMU limit lets it run at (the X4 72 % of the time at
    // 883 MHz, cpu 2-4 at 88 % of its cap), the others at their caps.
    auto second = [&](unsigned frames, bool limitedClocks) {
        driven.now += std::chrono::seconds(1);
        auto& r = driven.sched[render.tid()]; r.first += 950000000; r.second += 10000000;
        driven.sched[cs.tid()].first += 500000000;
        std::vector<int> on = affinity(render.tid());
        driven.cpu[render.tid()] = on.front();
        driven.cpu[cs.tid()] = affinity(cs.tid()).front();
        for (int i = 0; i < int(frames); ++i) host_perf::reportFrame(std::chrono::milliseconds(19));
        auto capOf = [&](int first) { std::ifstream f(root / ("cpu" + std::to_string(first)) / "cpufreq/scaling_max_freq"); unsigned k = 0; f >> k; return k; };
        for (int first : {0, 2, 5, 7}) {
            unsigned cap = capOf(first);
            bool hasRender = on.front() == first || (first == 2 && on.front() >= 2 && on.front() <= 4) || (first == 5 && on.front() >= 5 && on.front() <= 6);
            if (limitedClocks && hasRender && first == 7) runFor(first, {{883000, 72}, {cap, 28}});
            else if (limitedClocks && hasRender) runFor(first, {{1536000, 40}, {cap, 60}});
            else runFor(first, {{cap, 100}});
        }
        host_perf::maintainPlacement();
    };
    // The caps alone would keep it on the X4 (12 % short of the margin); measured, it leaves for cpu 2-4 on the
    // fifth check (the first two give the baselines), and the stream goes at once to the X4.
    int left = 0;
    for (int i = 1; i <= 10 && !left; ++i) { second(50, true); if (affinity(render.tid()) == midA.cpus) left = i; }
    require(left == 5, "measured: left the X4 on check " + std::to_string(left));
    require(affinity(cs.tid()) == x4.cpus && affinity(submit.tid()) == concat({little.cpus, midB.cpus}), "the stream on the X4, the rest on 0-1,5-6");
    for (int i = 0; i < 10; ++i) second(50, true);
    json = host_perf::statusJson(true);
    require(has(json, "\"clockSource\":\"cpufreq stats (time_in_state)\"") && has(json, "\"thermalStatus\":1"), json);
    require(std::regex_search(json, std::regex("\\{\"cpus\":\"7\",[^}]*\"share\":0\\.8[0-9]*,\"learned\":4,[^}]*\"runMhz\":1001,\"runCapMhz\":1305,\"busy\":95,\"measured\":4")),
            "the X4's share, its clock and cap while the render thread was there: " + json);
    require(std::regex_search(json, std::regex("\\{\"cpus\":\"2-4\",[^}]*\"share\":0\\.89[0-9]*,\"learned\":7,[^}]*\"runMhz\":1927,\"runCapMhz\":2188,\"busy\":95,\"measured\":10")),
            "cpu 2-4's: " + json);
    require(std::regex_search(json, std::regex("\"threadsNow\":\\{\"render\":\\[[234],95,1\\],\"dxvk-cs\":\\[7,50,0\\]")), "the last check's threads: " + json);
    require(std::regex_search(json, std::regex("\"clocksNow\":\\[2246,1927,1075,1305\\]")), "the last check's clocks: " + json);
    require(std::regex_search(json, std::regex("\\{\"name\":\"dxvk-cs\",\"spanS\":14\\.0,\"runS\":[0-9.]+,\"waitS\":0\\.0,\"clusterRunS\":\\[0\\.0,[0-9.]+,[0-9.]+,[0-9.]+\\]\\}")),
            "the stream's seconds per cluster: " + json);
    // The phone cools to thermal status 0 (the PMU limit off): the shares start over, and with the X4 allowed 2208
    // MHz (895 against cpu 2-4's 824) the render thread is home five checks later; the stream back on cpu 5-6.
    writeCap(midA, 3052000); writeCap(midB, 3052000); writeCap(x4, 2208000);
    driven.thermal = 0;
    int home = 0;
    for (int i = 1; i <= 12 && !home; ++i) { second(60, false); if (affinity(render.tid()) == x4.cpus) home = i; }
    json = host_perf::statusJson(false);
    require(home == 5 && has(json, "\"thermalResets\":1") && has(json, "\"thermalStatus\":0"), "home after the reset: " + std::to_string(home) + " " + json);
    require(affinity(cs.tid()) == midB.cpus, "the stream back on cpu 5-6");
    host_perf::release();

    // A governor that switches clocks past cpufreq's stats: time_in_state's time keeps going to one clock while
    // scaling_cur_freq shows three. After a minute the stats are dropped for scaling_cur_freq.
    {
        NamedThread again("render-test", adoptFastest);
        unsigned clocks[] = {1305000, 1152000, 883000};
        for (int i = 0; i < 61; ++i) {
            driven.now += std::chrono::seconds(1);
            for (int first : {2, 5, 7}) {
                writeFile(root / ("cpu" + std::to_string(first)) / "cpufreq/scaling_cur_freq", std::to_string(clocks[i % 3]));
                runFor(first, {{first == 7 ? 3782000u : 3052000u, 100}});
            }
            host_perf::maintainPlacement();
        }
        json = host_perf::statusJson(false);
        require(has(json, "\"clockSource\":\"scaling_cur_freq at each check\""), "stale stats dropped: " + json);
        host_perf::release();
    }
    host_perf::os.schedstat = host_perf::threadSchedstat;
    host_perf::os.lastCpu = host_perf::threadCpu;
    host_perf::os.now = [] { return std::chrono::steady_clock::now(); };
    host_perf::os.thermalStatus = host_perf::systemThermalStatus;
    std::puts("host_perf: build 16 measurements ok");
}
}  // namespace

int main() {
    realMachine();
    pixel();
    pixelMeasured();
    if (!root.empty()) fs::remove_all(root);
    std::puts("host_perf: ok");
    return 0;
}
