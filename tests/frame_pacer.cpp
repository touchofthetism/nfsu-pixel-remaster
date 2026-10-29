#include "../android/app/src/main/cpp/frame_pacer.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace std::chrono;
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    using Clock=FramePacer::Clock;
    auto origin=Clock::time_point(seconds(100));
    for(unsigned limit:{30u,60u,90u,120u}){
        FramePacer pacer;auto time=origin;auto period=nanoseconds(1000000000ull/limit);
        require(pacer.deadline(time,limit)==time,"first frame must not sleep");
        for(unsigned frame=1;frame<=1000;++frame){
            auto ready=time+period/3;auto target=pacer.deadline(ready,limit);
            require(target==time+period,"fast frames obey the selected limit");time=target;
        }
        for(unsigned frame=0;frame<1000;++frame){
            time+=period+milliseconds(7);
            require(pacer.deadline(time,limit)==time,"CPU-limited frames must not gain periodic extra sleeps");
        }
        time+=seconds(5);
        require(pacer.deadline(time,limit)==time,"resume must not add a full frame of delay");
        require(pacer.deadline(time+milliseconds(1),0)==time+milliseconds(1),"uncapped mode never sleeps");
        require(pacer.deadline(time+milliseconds(2),limit)==time+milliseconds(2),"enabling a cap starts a fresh schedule");
        require(pacer.deadline(time+milliseconds(3),limit==30?60:30)==time+milliseconds(3),"changing caps starts a fresh schedule");
    }
    // Build 15: the sleep comes before each Present (guest_runtime.cpp, paceFrame). With work that varies from
    // frame to frame (6-14 ms under a 60 cap) the Presents land exactly a period apart; in build 14's order (the
    // sleep after the Present, then the work, then the next Present) each lands a frame's work after its deadline,
    // and the gaps between them vary as much as the work does.
    {
        const unsigned limit = 60;
        const auto period = nanoseconds(1000000000ull / limit);
        FramePacer before, after;
        auto tb = origin, ta = origin;
        std::vector<Clock::time_point> presentsBefore, presentsAfter;
        unsigned seed = 1;
        for (unsigned frame = 0; frame < 600; ++frame) {
            seed = seed * 1103515245u + 12345u;
            auto work = milliseconds(6) + microseconds((seed >> 8) % 8000);
            tb += work; tb = std::max(tb, before.deadline(tb, limit)); presentsBefore.push_back(tb);   // work, sleep, Present
            ta = std::max(ta, after.deadline(ta, limit)); ta += work; presentsAfter.push_back(ta);     // sleep, work, Present
        }
        nanoseconds shortest = hours(1), longest{};
        for (size_t i = 2; i < presentsBefore.size(); ++i)
            require(presentsBefore[i] - presentsBefore[i - 1] == period, "paced before the Present: every gap is the period");
        for (size_t i = 2; i < presentsAfter.size(); ++i) {
            auto gap = presentsAfter[i] - presentsAfter[i - 1];
            shortest = std::min(shortest, gap); longest = std::max(longest, gap);
        }
        require(longest - shortest > milliseconds(12), "paced after it: the gaps vary with the work");
    }
    std::puts("Frame pacing passed: 30/60/90/120, 8000 fast/slow frames, pause, uncapped and cap changes, Presents a period apart");
}
