// tools/testhost/main_linux.cpp: the recompiled game on Linux x86-64, for trying features end to end before a
// build goes to the phone (README.md). The runtime is the phone's (guest_runtime.cpp, the D3D9 bridge, the
// lifted game); DXVK presents into an X11 test window (NFS_D3D9_TEST_DISPLAY: the bridge opens it on $DISPLAY,
// Xvfb in a container) and a timeline script presses keys and takes screenshots.
//
//   nfsu-testhost --game DIR [--generated DIR] [--freeroam DIR] [--out DIR] [--script FILE] [--seconds N]
//                 [--garage BITS] [--size WxH] [--fps N]
//
// Script lines (seconds from the start of the guest, '#' comments):
//   12.5 key ENTER tap        press and release a key (a DIK name or a hex scancode, 0x1c): held 100 ms and until
//                             the game has read the press (a slow frame would otherwise see both at once)
//   13   key UP down          hold, ...  14 key UP up
//   20   shot menu            screenshot of the display to <out>/menu.png
//   25   log some text        a marker in the log
//   30   peek race 0x777CC8 2 dwords of guest memory to the log; [0x736174]+0x6C reads through a pointer
//   40   wait 60 PSA.mad      hold the script until the runtime logs a line with this text (searched after the
//                             previous wait's line), at most 60 s; every later event moves by the time it took
//   90   quit
//   include other.txt        another script's events (a path relative to this script), at their own times
#include "guest_runtime.h"
#include "guest_boot_worker.h"
#include "runtime_log.h"
#include "d3d9_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Event {
    double at = 0;
    enum Kind { Key, Shot, Log, Peek, Wait, Quit } kind = Log;
    unsigned count = 1;
    double timeout = 0;
    unsigned scan = 0;
    int press = 0;   // 1 down, 0 up, 2 tap
    std::string text;
};

const std::map<std::string, unsigned> kScancodes = {
    {"ESCAPE", 0x01}, {"ESC", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06},
    {"BACKSPACE", 0x0E}, {"TAB", 0x0F}, {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14},
    {"ENTER", 0x1C}, {"RETURN", 0x1C}, {"LCTRL", 0x1D}, {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20}, {"F", 0x21},
    {"LSHIFT", 0x2A}, {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E}, {"V", 0x2F}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32},
    {"RSHIFT", 0x36}, {"SPACE", 0x39}, {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F},
    {"NUMPADENTER", 0x9C}, {"RCTRL", 0x9D}, {"HOME", 0xC7}, {"UP", 0xC8}, {"PGUP", 0xC9}, {"LEFT", 0xCB},
    {"RIGHT", 0xCD}, {"END", 0xCF}, {"DOWN", 0xD0}, {"PGDN", 0xD1}, {"INSERT", 0xD2}, {"DELETE", 0xD3},
};

std::string upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

std::vector<Event> readScript(const std::string& path, int depth = 0) {
    std::vector<Event> events;
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "[testhost] cannot read script %s\n", path.c_str()); std::exit(2); }
    std::string line;
    unsigned number = 0;
    while (std::getline(in, line)) {
        ++number;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        Event event;
        std::string kind;
        {
            std::istringstream first(line);
            std::string word, other;
            if (first >> word && word == "include" && first >> other) {
                if (depth > 8) { std::fprintf(stderr, "[testhost] %s:%u: includes nest too deep\n", path.c_str(), number); std::exit(2); }
                const auto slash = path.find_last_of('/');
                auto included = readScript(other[0] == '/' || slash == std::string::npos ? other : path.substr(0, slash + 1) + other, depth + 1);
                events.insert(events.end(), included.begin(), included.end());
                continue;
            }
        }
        if (!(words >> event.at >> kind)) continue;
        kind = upper(kind);
        if (kind == "KEY") {
            std::string key, press;
            words >> key >> press;
            auto named = kScancodes.find(upper(key));
            event.scan = named != kScancodes.end() ? named->second : unsigned(std::strtoul(key.c_str(), nullptr, 0));
            press = upper(press);
            event.press = press == "DOWN" ? 1 : press == "UP" ? 0 : 2;
            if (!event.scan || event.scan > 255) { std::fprintf(stderr, "[testhost] %s:%u: unknown key %s\n", path.c_str(), number, key.c_str()); std::exit(2); }
            event.kind = Event::Key;
        } else if (kind == "SHOT") {
            words >> event.text;
            event.kind = Event::Shot;
        } else if (kind == "LOG") {
            std::getline(words, event.text);
            event.kind = Event::Log;
        } else if (kind == "PEEK") {
            std::string label, address;
            words >> label >> address;
            if (!(words >> event.count)) event.count = 1;
            event.text = label + " " + address;
            event.kind = Event::Peek;
        } else if (kind == "WAIT") {
            words >> event.timeout;
            std::getline(words, event.text);
            const auto first = event.text.find_first_not_of(' ');
            event.text = first == std::string::npos ? "" : event.text.substr(first);
            if (event.text.empty() || !(event.timeout > 0)) {
                std::fprintf(stderr, "[testhost] %s:%u: wait needs a timeout and a text\n", path.c_str(), number);
                std::exit(2);
            }
            event.kind = Event::Wait;
        } else if (kind == "QUIT") {
            event.kind = Event::Quit;
        } else {
            std::fprintf(stderr, "[testhost] %s:%u: unknown event %s\n", path.c_str(), number, kind.c_str());
            std::exit(2);
        }
        events.push_back(event);
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.at < b.at; });
    return events;
}

// The runtime's log lines, for script waits (runtime_log.h runtimeLogObserver; any thread).
std::mutex logMutex;
std::vector<std::string> logLines;
void observeLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex);
    logLines.push_back(line);
}
// The first line from `from` on that contains `text`: its index + 1, or 0.
size_t findLogLine(size_t from, const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (size_t i = from; i < logLines.size(); ++i) if (logLines[i].find(text) != std::string::npos) return i + 1;
    return 0;
}

// "0x777CC8", or "[0x736174]+0x6C": the dword at 0x736174, plus 0x6C.
bool peekAddress(const std::string& text, uint32_t& va) {
    if (text.empty()) return false;
    if (text[0] != '[') { va = uint32_t(std::strtoul(text.c_str(), nullptr, 0)); return true; }
    const auto close = text.find(']');
    if (close == std::string::npos) return false;
    uint32_t pointer = uint32_t(std::strtoul(text.substr(1, close - 1).c_str(), nullptr, 0)), base = 0;
    if (!guestPeek(pointer, &base, 4)) return false;
    va = base + (close + 1 < text.size() ? uint32_t(std::strtoul(text.substr(close + 1).c_str(), nullptr, 0)) : 0u);
    return true;
}

void screenshot(const std::string& out, const std::string& name) {
    const char* display = std::getenv("DISPLAY");
    std::string path = out + "/" + name + ".png";
    // ImageMagick reads the X display (Xvfb); the game's window is the test window at the origin.
    std::string command = "import -display '" + std::string(display ? display : ":0") + "' -window root '" + path + "' 2>/dev/null";
    std::fflush(stdout);   // system() forks: a buffered line would be written by the child too
    std::fflush(stderr);
    int result = std::system(command.c_str());
    std::fprintf(stderr, "[testhost] shot %s %s\n", path.c_str(), result == 0 ? "saved" : "FAILED");
}

}  // namespace

int main(int argc, char** argv) {
    std::string game, generated, freeRoam, out = ".", script;
    double seconds = 60;
    unsigned width = 1280, height = 720, fps = 60;
    uint32_t garage = 0x1FF;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", arg.c_str()); std::exit(64); } return argv[++i]; };
        if (arg == "--game") game = next();
        else if (arg == "--generated") generated = next();
        else if (arg == "--freeroam") freeRoam = next();
        else if (arg == "--out") out = next();
        else if (arg == "--script") script = next();
        else if (arg == "--seconds") seconds = std::atof(next().c_str());
        else if (arg == "--garage") garage = uint32_t(std::strtoul(next().c_str(), nullptr, 0));
        else if (arg == "--fps") fps = unsigned(std::atoi(next().c_str()));
        else if (arg == "--size") { auto size = next(); std::sscanf(size.c_str(), "%ux%u", &width, &height); }
        else { std::fprintf(stderr, "unknown argument %s\n", arg.c_str()); return 64; }
    }
    if (game.empty()) { std::fprintf(stderr, "usage: nfsu-testhost --game DIR [--script FILE] [--out DIR] [--seconds N]\n"); return 64; }
    if (!std::getenv("DISPLAY")) { std::fprintf(stderr, "[testhost] no DISPLAY: run under Xvfb (tools/testhost/run.sh)\n"); return 64; }
    {
        std::error_code error;   // screenshots, the DXVK log and telemetry.json go there
        std::filesystem::create_directories(out, error);
        if (error) { std::fprintf(stderr, "[testhost] cannot create %s: %s\n", out.c_str(), error.message().c_str()); return 73; }
    }
    setenv("NFS_D3D9_TEST_DISPLAY", "1", 1);
    // DXVK's state cache with the generated data (it outlives a run, as the phone's does), not wherever the run
    // started; read when the device is made. Its log's level and folder are read by a static logger before main
    // runs, so run.sh sets those (DXVK_LOG_LEVEL, DXVK_LOG_PATH = --out).
    if (!std::getenv("DXVK_STATE_CACHE_PATH")) setenv("DXVK_STATE_CACHE_PATH", (generated.empty() ? out : generated).c_str(), 1);
    std::vector<Event> events = script.empty() ? std::vector<Event>{} : readScript(script);

    // Options the launcher sets before PLAY (GameActivity), with the phone's defaults.
    setGuestDisplaySize(width, height);
    setGuestResolution(width, height);
    setGuestFrameLimit(fps);
    setGuestWidescreen(true);
    setGuestGarage(garage);
    setGuestEffectStates(true);
    if (!generated.empty()) setGuestGeneratedDir(generated);
    if (!freeRoam.empty()) setGuestFreeRoamDir(freeRoam);
    setGuestLanguage("English");

    runtimeLogObserver = observeLog;
    GuestBootWorker worker;
    worker.start(game + "/Speed.exe");
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    struct Release { double earliest; uint64_t press; unsigned scan; };   // a tap's key: up once the game read it
    std::vector<Release> releases;
    size_t next = 0;
    std::string status;
    // A wait holds the script until the runtime logs a line with its text (searched from just after the line the
    // previous wait found); the events after it move by however long that took.
    const Event* waiting = nullptr;
    double waitUntil = 0;
    size_t logFrom = 0;
    while (elapsed() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (d3d9RequestsSurface()) acknowledgeD3D9Surface(true);
        double now = elapsed();
        auto current = worker.status();
        if (current != status) { status = current; std::fprintf(stderr, "[testhost] %.1f s: %s\n", now, status.c_str()); }
        for (auto it = releases.begin(); it != releases.end();) {
            const bool read = guestKeysDelivered() >= it->press || now > it->earliest + 5;   // 5 s: the game stopped reading
            if (it->earliest <= now && read) { setGuestKey(it->scan, false); it = releases.erase(it); } else ++it;
        }
        bool quit = false;
        if (waiting) {
            const size_t found = findLogLine(logFrom, waiting->text);
            if (found || now > waitUntil) {
                const double late = now - waiting->at;
                for (size_t i = next; i < events.size(); ++i) events[i].at += late;
                std::fprintf(stderr, "[testhost] %.1f s: %s \"%s\" (%.1f s after the script's time)\n", now,
                             found ? "waited for" : "gave up waiting for", waiting->text.c_str(), late);
                if (found) logFrom = found;
                waiting = nullptr;
            }
        }
        while (!waiting && next < events.size() && events[next].at <= now) {
            const Event& event = events[next++];
            switch (event.kind) {
            case Event::Key:
                if (event.press == 2) { setGuestKey(event.scan, true); releases.push_back({now + 0.1, guestKeySequence(), event.scan}); }
                else setGuestKey(event.scan, event.press == 1);
                std::fprintf(stderr, "[testhost] %.1f s: key 0x%02x %s\n", now, event.scan, event.press == 2 ? "tap" : event.press ? "down" : "up");
                break;
            case Event::Shot: screenshot(out, event.text); break;
            case Event::Log: std::fprintf(stderr, "[testhost] %.1f s: --%s\n", now, event.text.c_str()); break;
            case Event::Peek: {
                const auto space = event.text.find(' ');
                uint32_t va = 0;
                std::string line = "[testhost] " + std::to_string(now).substr(0, std::to_string(now).find('.') + 2) + " s: peek " + event.text + " =";
                if (!peekAddress(event.text.substr(space + 1), va)) line += " (no such pointer)";
                else {
                    char word[16];
                    std::snprintf(word, sizeof word, " @%08x:", va);
                    line += word;
                    for (unsigned i = 0; i < event.count && i < 64; ++i) {
                        uint32_t value = 0;
                        if (!guestPeek(va + 4 * i, &value, 4)) { line += " ?"; continue; }
                        std::snprintf(word, sizeof word, " %08x", value);
                        line += word;
                    }
                }
                std::fprintf(stderr, "%s\n", line.c_str());
                break;
            }
            case Event::Wait: waiting = &event; waitUntil = now + event.timeout; break;
            case Event::Quit: quit = true; break;
            }
        }
        if (quit) break;
        if (status.rfind("Running", 0) != 0 && status != "CRT boot pending" && now > 1) {
            // The worker returned: the guest stopped (its status says why).
            break;
        }
    }
    std::fprintf(stderr, "[testhost] stopping after %.1f s; guest status: %s\n", elapsed(), worker.status().c_str());
    std::string telemetry = guestTelemetryJson(false);
    std::ofstream(out + "/telemetry.json") << telemetry;
    std::fflush(stderr);
    // The guest thread may be inside a long call; the worker's destructor asks it to stop and joins.
    std::_Exit(0);
}
