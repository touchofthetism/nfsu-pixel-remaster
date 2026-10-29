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

    NamedThread dxvk("dxvk-cs", nullptr);

    // "Fastest core": adopted on the big core, DXVK's thread everywhere else.
    {
        NamedThread render("render-test", adoptFastest);
        require(affinity(render.tid()) == home.cpus, "render on the big core: " + list(affinity(render.tid())));
        require(affinity(dxvk.tid()) == helpersHome, "DXVK off it: " + list(affinity(dxvk.tid())));
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
        require(affinity(dxvk.tid()) == helpersAway, "DXVK moved to " + list(helpersAway) + ": " + list(affinity(dxvk.tid())));
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
        require(affinity(dxvk.tid()) == helpersHome, "DXVK back: " + list(affinity(dxvk.tid())));
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
    const NamedThread* dxvk[] = {&cs, &submit, &queue, &recycle};
    auto dxvkOn = [&](const std::vector<int>& cpus) {
        for (const NamedThread* thread : dxvk) if (affinity(thread->tid()) != cpus) return false;
        return true;
    };
    NamedThread render("Thread-7", adoptFastest);
    std::string json = host_perf::statusJson(false);
    require(affinity(render.tid()) == x4.cpus && dxvkOn(range(0, 6)), "adopted: render on cpu 7, DXVK on 0-6");
    require(has(json, "\"dxvk\":\"4 of 4 DXVK threads off cpu 7"), json);

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
    // to cpu 5-6 on the third check; DXVK's threads off it and off the X4.
    writeCap(little, 2246000); writeCap(midA, 2688000); writeCap(midB, 2688000); writeCap(x4, 1152000);
    require(checksUntil(render.tid(), midB.cpus, 10) == 3, "to cpu 5-6 on the third check");
    require(dxvkOn(range(0, 4)), "DXVK on cpu 0-4: " + list(affinity(cs.tid())));
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
    require(dxvkOn(concat({little.cpus, midB.cpus})), "DXVK on cpu 0-1,5-6: " + list(affinity(cs.tid())));

    // Everything uncapped: home to the X4, DXVK's threads back on cpu 0-6.
    writeCap(midA, 3052000); writeCap(midB, 3052000); writeCap(x4, 3782000);
    require(checksUntil(render.tid(), x4.cpus, 20) >= 5, "home to cpu 7");
    require(dxvkOn(range(0, 6)), "DXVK back on cpu 0-6");
    json = host_perf::statusJson(true);
    require(has(json, "\"coreMoves\":3") && has(json, "\"renderCpus\":\"7\""), json);
    require(std::regex_search(json, std::regex("\"moves\":\\[\\[0,\"7\"\\],\\[[0-9]+,\"5-6\"\\],\\[[0-9]+,\"2-4\"\\],\\[[0-9]+,\"7\"\\]\\]")), json);
    host_perf::release();
    std::puts("host_perf: Pixel 10 Pro XL simulated ok");
}
}  // namespace

int main() {
    realMachine();
    pixel();
    if (!root.empty()) fs::remove_all(root);
    std::puts("host_perf: ok");
    return 0;
}
