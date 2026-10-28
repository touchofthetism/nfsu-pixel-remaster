// host_perf.cpp's placement end to end on this machine (Linux; the Android-only parts fall back as on a phone
// without them): a fake cpu sysfs gives the clusters, capacities and caps; real threads named as the game's are
// placed with real sched_setaffinity calls. Checks the clusters read from related_cpus and cpu_capacity (and the
// fallbacks without them), the adoption, the move to a faster cluster when the big core's cap falls, DXVK's threads
// moving out of its way, a mask Android replaces put back to the new cluster, the way home, "Biggest core only"
// (no moves), "System scheduler" (no placement), and the JSON the overlay and the session summary read.
#include "../android/app/src/main/cpp/host_perf.cpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
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
void writeTree() {
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
}

std::vector<int> affinity(int32_t tid) {
    cpu_set_t set; CPU_ZERO(&set);
    require(sched_getaffinity(pid_t(tid), sizeof set, &set) == 0, "sched_getaffinity");
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
    return cpus;
}
std::string list(const std::vector<int>& cpus) { return CoreChoice::describe(cpus); }
std::vector<int> concat(std::initializer_list<std::vector<int>> parts) {
    std::vector<int> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    std::sort(out.begin(), out.end());
    return out;
}

// A thread with a name of its own that runs `start` and then waits until the test ends it.
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
            changed_.wait(lock, [this] { return stop_; });
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
private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false, stop_ = false;
    int32_t tid_ = 0;
};

bool has(const std::string& json, const std::string& part) { return json.find(part) != std::string::npos; }
}  // namespace

int main() {
    long count = sysconf(_SC_NPROCESSORS_CONF);
    std::vector<int> usable = affinity(0);
    bool contiguous = count >= 2 && usable.size() == size_t(count);
    for (size_t i = 0; contiguous && i < usable.size(); ++i) contiguous = usable[i] == int(i);
    if (!contiguous) { std::puts("host_perf: skipped (needs cpus 0..n-1 all usable, n >= 2)"); return 0; }
    const int n = int(count), big = n - 1;

    // n = 2: a mid and a big core; n = 3: little, mid, big; n >= 4: little, two mid clusters, big (the Pixel's shape).
    std::vector<int> all(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) all[size_t(i)] = i;
    if (n == 2) layout = {{{0}, 700, 3000000}, {{1}, 1024, 3800000}};
    else if (n == 3) layout = {{{0}, 300, 2000000}, {{1}, 700, 3000000}, {{2}, 1024, 3800000}};
    else {
        std::vector<int> midB;
        for (int cpu = 2; cpu < big; ++cpu) midB.push_back(cpu);
        layout = {{{0}, 300, 2000000}, {{1}, 700, 3000000}, {midB, 700, 3000000}, {{big}, 1024, 3800000}};
    }
    const FakeCluster& home = layout.back();
    // Where the render thread goes when the big core is capped (ties: the higher cpus), and DXVK's threads then.
    const std::vector<int> away = layout[layout.size() - 2].cpus;
    const std::vector<int> helpersAway = n >= 4 ? concat({layout[0].cpus, layout[1].cpus})          // not the capped big core
                                       : n == 3 ? concat({layout[0].cpus, home.cpus})                // only the little core else
                                                : home.cpus;
    std::vector<int> helpersHome(all.begin(), all.end() - 1);

    char pattern[] = "/tmp/host_perf_XXXXXX";
    require(mkdtemp(pattern) != nullptr, "mkdtemp");
    root = pattern;
    writeTree();
    host_perf::cpuSysfs = root.string();

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
        NamedThread render("render-test", [] { host_perf::adoptRenderThread(host_perf::Placement::FastestCores, 60); });
        require(affinity(render.tid()) == home.cpus, "render on the big core: " + list(affinity(render.tid())));
        require(affinity(dxvk.tid()) == helpersHome, "DXVK off it: " + list(affinity(dxvk.tid())));
        std::string json = host_perf::statusJson(false);
        require(has(json, "\"affinity\":\"render thread on cpu " + std::to_string(big) + " (follows the fastest cluster)\""), json);
        require(has(json, "\"renderCpus\":\"" + std::to_string(big) + "\"") && has(json, "\"coreMoves\":0"), json);

        // An uncapped phone: no move however long.
        for (int i = 0; i < 15; ++i) host_perf::maintainPlacement();
        require(affinity(render.tid()) == home.cpus, "uncapped: stays");

        // The big core capped to 1 GHz (speed 1024 * 1.0 / 3.8 = 269 against a mid core's 700): away on the third check.
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
        cpu_set_t everything; CPU_ZERO(&everything);
        for (int cpu : all) CPU_SET(cpu, &everything);
        require(sched_setaffinity(pid_t(render.tid()), sizeof everything, &everything) == 0, "widen");
        host_perf::maintainPlacement();
        require(affinity(render.tid()) == away, "put back to " + list(away) + ": " + list(affinity(render.tid())));
        require(has(host_perf::statusJson(false), "\"placementRestored\":1"), host_perf::statusJson(false));

        // The cap lifts: home again (after five checks as fast, and ten since the move).
        writeCap(home, home.maxKhz);
        int checks = 0;
        while (affinity(render.tid()) != home.cpus && checks < 20) { host_perf::maintainPlacement(); ++checks; }
        require(affinity(render.tid()) == home.cpus && checks >= 5, "home after " + std::to_string(checks) + " checks");
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
        NamedThread render("render-test", [] { host_perf::adoptRenderThread(host_perf::Placement::FastestCores, 60); });
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

    fs::remove_all(root);
    std::puts("host_perf: ok");
    return 0;
}
