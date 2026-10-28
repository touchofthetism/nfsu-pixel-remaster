#include "../android/app/src/main/cpp/frame_telemetry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace std::chrono;
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
static bool near(double a,double b,double tolerance){return a>b-tolerance&&a<b+tolerance;}
static long long field(const std::string& json,const char* name){
    auto at=json.find(std::string("\"")+name+"\":");require(at!=std::string::npos,name);
    return std::atoll(json.c_str()+at+std::strlen(name)+3);
}
static double fieldD(const std::string& json,const char* name){
    auto at=json.find(std::string("\"")+name+"\":");require(at!=std::string::npos,name);
    return std::atof(json.c_str()+at+std::strlen(name)+3);
}
int main(){
    using Clock=FrameTelemetry::Clock;
    FrameTelemetry telemetry;
    auto time=Clock::time_point(seconds(1000));
    // 1200 frames at a steady 120 FPS, each Present taking 1 ms.
    for(int i=0;i<1200;++i){telemetry.present(time,milliseconds(1),120);time+=microseconds(8333);}
    auto s=telemetry.summary();
    require(s.frames==1199,"first Present opens the first interval");
    require(near(s.avgFps,120,.5),"average FPS from intervals");
    require(s.hitches==0&&s.stalls==0&&s.gaps==0,"a steady run has no hitches");
    require(near(s.presentMsAvg,1.0,.01),"present time averaged over calls");
    require(s.p99Ms<=8.5&&s.p50Ms<=8.5,"percentiles report the bucket bound");
    // One 120 ms stall (shader compile), one 40 ms hitch, then a 3 s pause the app did not announce.
    time-=microseconds(8333);  // back to the last Present
    telemetry.present(time+=milliseconds(120),milliseconds(1),120);
    telemetry.present(time+=milliseconds(40),milliseconds(1),120);
    telemetry.present(time+=seconds(3),milliseconds(1),120);
    s=telemetry.summary();
    require(s.frames==1201,"the pause is not a frame");
    require(s.hitches==2,"stall and hitch both exceed 2.5 frame periods (>25 ms floor)");
    require(s.stalls==1,"only the 120 ms frame is a stall");
    require(s.gaps==1,"a 3 s interval is a gap");
    require(near(s.maxMs,120,.01),"max frame time");
    require(s.spikes==2,"both slow frames are spikes against the 8.3 ms typical frame");
    // An announced pause: no interval is measured across it.
    telemetry.pause();
    telemetry.present(time+=seconds(30),milliseconds(1),120);
    telemetry.present(time+=microseconds(8333),milliseconds(1),120);
    s=telemetry.summary();
    require(s.gaps==1&&s.frames==1202,"pause() hides the background time");
    // A session that runs at a steady 33 FPS below a 120 cap: every frame is a "hitch" against the cap
    // (30 ms > the 25 ms floor) but none is a spike.
    FrameTelemetry slow33;auto t33=Clock::time_point(seconds(9));
    for(int i=0;i<600;++i){slow33.present(t33,milliseconds(1),120);t33+=milliseconds(30);}
    require(slow33.summary().hitches==599&&slow33.summary().spikes==0,"steady slow frames are not spikes");
    slow33.present(t33+=milliseconds(60),milliseconds(1),120);   // 90 ms after the last Present
    require(slow33.summary().spikes==1,"a 90 ms frame among 30 ms frames is a spike");
    // At 30 FPS the hitch threshold is 2.5 periods (83 ms): a 50 ms frame is normal there.
    FrameTelemetry slow;auto t2=Clock::time_point(seconds(5));
    slow.present(t2,milliseconds(2),30);slow.present(t2+=milliseconds(50),milliseconds(2),30);slow.present(t2+=milliseconds(90),milliseconds(2),30);
    require(slow.summary().hitches==1,"hitch threshold scales with the frame cap");
    // JSON: the full record and the live view.
    std::string json=telemetry.json();
    require(field(json,"frames")==1202,"json frames");
    require(field(json,"hitches")==2&&field(json,"stalls")==1&&field(json,"gaps")==1,"json counters");
    require(json.find("\"histogramUs\":[[")!=std::string::npos,"histogram present");
    require(json.find("\"perSecond\":[[0,")!=std::string::npos,"per-second series starts at second 0");
    require(json.back()=='}',"json closed");
    require(fieldD(json,"onePercentLowFps")>0,"1% low derived from p99");
    std::string live=telemetry.liveJson();
    require(live.find("\"recent\":{\"frames\":240,")!=std::string::npos,"live view covers the last 240 frames");
    require(fieldD(live,"fps")>0,"live fps");
    // Balanced braces/brackets in both documents.
    for(const std::string* doc:{&json,&live}){int depth=0;for(char c:*doc){if(c=='{'||c=='[')++depth;if(c=='}'||c==']')--depth;require(depth>=0,"nesting");}require(depth==0,"balanced json");}
    // Uncapped: 120 is assumed for the hitch threshold.
    FrameTelemetry uncapped;auto t3=Clock::time_point(seconds(7));
    uncapped.present(t3,milliseconds(1),0);uncapped.present(t3+=milliseconds(30),milliseconds(1),0);
    require(uncapped.summary().hitches==1&&uncapped.summary().targetFps==0,"uncapped hitch rule");
    // GPU busy per second from a GPU idle counter (DXVK's measure): a second with 250 ms idle is 75% busy.
    require(telemetry.summary().gpuBusyAvg<0,"no idle source: GPU busy not measured");
    static uint64_t fakeIdle=0;
    FrameTelemetry gpu;gpu.gpuIdleSource([]{return fakeIdle;});
    auto t4=Clock::time_point(seconds(50));
    for(int second=0;second<4;++second){
        for(int frame=0;frame<60;++frame){gpu.present(t4,milliseconds(1),60);t4+=microseconds(16667);}
        fakeIdle+=second==1?250000:second==2?900000:0;  // second 1: 75% busy, second 2: 10% busy, others 100%
    }
    std::string gj=gpu.json();
    require(gj.find("[1,60,16.7,75]")!=std::string::npos,"second 1 measured 75% busy");
    require(gj.find("[2,60,16.7,10]")!=std::string::npos,"second 2 measured 10% busy");
    require(gj.find("[3,60,16.7,-1]")!=std::string::npos||gj.find("[3,59,16.7,-1]")!=std::string::npos,"the open last second is not measured");
    require(near(gpu.summary().gpuBusyAvg,(100+75+10)/3.0,.6),"average over the measured seconds");
    // A pause closes the second without measuring it (the GPU idles in the background).
    gpu.pause();fakeIdle+=5000000;
    for(int frame=0;frame<130;++frame){gpu.present(t4+=seconds(frame?0:30),milliseconds(1),60);t4+=microseconds(16667);}
    require(gpu.summary().gpuBusyAvg>=0&&gpu.summary().gpuBusyAvg<=100,"pauses never produce a busy share outside 0..100");
    std::puts("Frame telemetry passed: steady 120 FPS, stall/hitch/gap classification, pause(), 30 FPS threshold, JSON shape, GPU busy per second");
}
