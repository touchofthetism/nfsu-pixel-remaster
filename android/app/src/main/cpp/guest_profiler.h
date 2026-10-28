#pragma once
// Where the guest machine's time goes, by sampling (guest_runtime.cpp runs the sampler thread).
//
// Every interval a host thread looks at the guest machine: which guest thread holds it (the render thread,
// the one that presents, or another guest thread such as sound or the file loader), the lifted function
// that thread is in (g_cur_func: the function itself, not its callees) and whether it is running lifted game
// code, a D3D9 call (the bridge and DXVK) or another host import. Nobody holding the machine is "idle": the
// render thread sleeping in the frame pacer, or every guest thread blocked.
//
// The counts say what limits a frame: lifted code in a handful of functions (optimise those), D3D9 calls
// (cut calls or their cost), or idle (the GPU or the pacer sets the pace). A short ring of recent samples
// explains a single slow frame: which functions filled those 140 ms.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class GuestProfile {
public:
    // What the machine holder was doing.
    enum Place : uint8_t { Guest = 0, D3D9 = 1, Import = 2 };
    // Who held the machine.
    enum Holder : uint8_t { Nobody = 0, Render = 1, Other = 2 };
    static constexpr size_t kRing = 1024;

    struct Sample { uint64_t ns; uint32_t func; uint8_t holder, place; };

    void add(uint64_t ns, Holder holder, uint32_t func, Place place) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++samples_;
        ring_[next_] = {ns, func, uint8_t(holder), uint8_t(place)};
        next_ = (next_ + 1) % kRing;
        if (filled_ < kRing) ++filled_;
        if (holder == Nobody) { ++idle_; return; }
        if (holder == Other) { ++other_; ++otherFuncs_[func]; return; }
        ++render_[place < 3 ? place : 0];
        ++renderFuncs_[func][place < 3 ? place : 0];
    }

    // The render thread's samples in [from, to) (steady-clock nanoseconds), the top `count` (function, place)
    // pairs first: "sub_004A1B20 guest 41%, sub_00585B40 D3D9 22%, other threads 12%, idle 3% (88 samples)".
    std::string explain(uint64_t from, uint64_t to, size_t count = 4) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unordered_map<uint64_t, unsigned> pairs;   // func << 8 | place
        unsigned total = 0, other = 0, idle = 0;
        for (size_t i = 0; i < filled_; ++i) {
            const Sample& s = ring_[i];
            if (s.ns < from || s.ns >= to) continue;
            ++total;
            if (s.holder == Nobody) ++idle;
            else if (s.holder == Other) ++other;
            else ++pairs[(uint64_t(s.func) << 8) | s.place];
        }
        if (!total) return "no samples";
        std::vector<std::pair<uint64_t, unsigned>> sorted(pairs.begin(), pairs.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::string out;
        char item[64];
        for (size_t i = 0; i < sorted.size() && i < count; ++i) {
            std::snprintf(item, sizeof item, "%ssub_%08X %s %u%%", out.empty() ? "" : ", ", unsigned(sorted[i].first >> 8),
                placeName(uint8_t(sorted[i].first & 0xff)), percent(sorted[i].second, total));
            out += item;
        }
        std::snprintf(item, sizeof item, "%sother threads %u%%, idle %u%% (%u samples)", out.empty() ? "" : ", ",
            percent(other, total), percent(idle, total), total);
        return out + item;
    }

    // {"intervalUs":..,"samples":..,"render":{"guest":..,"d3d9":..,"import":..},"other":..,"idle":..,
    //  "top":[["004A1B20",guest,d3d9,import],...],"otherTop":[["0052D0C0",n],...]}
    std::string json(unsigned intervalUs, size_t top = 40, size_t otherTop = 8) const {
        std::lock_guard<std::mutex> lock(mutex_);
        char text[256];
        std::snprintf(text, sizeof text, "{\"intervalUs\":%u,\"samples\":%llu,\"render\":{\"guest\":%llu,\"d3d9\":%llu,\"import\":%llu},"
            "\"other\":%llu,\"idle\":%llu,\"top\":[", intervalUs, ull(samples_), ull(render_[0]), ull(render_[1]), ull(render_[2]), ull(other_), ull(idle_));
        std::string out = text;
        std::vector<std::pair<uint32_t, std::array<uint64_t, 3>>> funcs(renderFuncs_.begin(), renderFuncs_.end());
        auto sum = [](const std::array<uint64_t, 3>& c) { return c[0] + c[1] + c[2]; };
        std::sort(funcs.begin(), funcs.end(), [&](const auto& a, const auto& b) { return sum(a.second) != sum(b.second) ? sum(a.second) > sum(b.second) : a.first < b.first; });
        for (size_t i = 0; i < funcs.size() && i < top; ++i) {
            std::snprintf(text, sizeof text, "%s[\"%08X\",%llu,%llu,%llu]", i ? "," : "", funcs[i].first,
                ull(funcs[i].second[0]), ull(funcs[i].second[1]), ull(funcs[i].second[2]));
            out += text;
        }
        out += "],\"otherTop\":[";
        std::vector<std::pair<uint32_t, uint64_t>> others(otherFuncs_.begin(), otherFuncs_.end());
        std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        for (size_t i = 0; i < others.size() && i < otherTop; ++i) {
            std::snprintf(text, sizeof text, "%s[\"%08X\",%llu]", i ? "," : "", others[i].first, ull(others[i].second));
            out += text;
        }
        return out + "]}";
    }

    uint64_t samples() const { std::lock_guard<std::mutex> lock(mutex_); return samples_; }

private:
    static unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
    static unsigned percent(unsigned part, unsigned total) { return total ? unsigned((part * 100u + total / 2) / total) : 0u; }
    static const char* placeName(uint8_t place) { return place == D3D9 ? "D3D9" : place == Import ? "import" : "guest"; }

    mutable std::mutex mutex_;
    uint64_t samples_ = 0, idle_ = 0, other_ = 0;
    std::array<uint64_t, 3> render_{};
    std::unordered_map<uint32_t, std::array<uint64_t, 3>> renderFuncs_;
    std::unordered_map<uint32_t, uint64_t> otherFuncs_;
    std::array<Sample, kRing> ring_{};
    size_t next_ = 0, filled_ = 0;
};
