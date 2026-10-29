#include "host_perf.h"
#include "placement_keeper.h"
#include "runtime_log.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace host_perf {
namespace {
// android/performance_hint.h (API 33) without a compile-time dependency: minSdk is 29.
struct APerformanceHintManager;
struct APerformanceHintSession;
using GetManagerFn=APerformanceHintManager*(*)();
using CreateSessionFn=APerformanceHintSession*(*)(APerformanceHintManager*,const int32_t*,size_t,int64_t);
using UpdateTargetFn=int(*)(APerformanceHintSession*,int64_t);
using ReportActualFn=int(*)(APerformanceHintSession*,int64_t);
using CloseSessionFn=void(*)(APerformanceHintSession*);
std::mutex mutex;
APerformanceHintSession* session=nullptr;
CloseSessionFn closeSession=nullptr;UpdateTargetFn updateTarget=nullptr;ReportActualFn reportActual=nullptr;
std::atomic<bool> adopted{false};
std::atomic<unsigned> currentTargetFps{120};
std::atomic<uint64_t> framesReported{0};     // frames reportFrame saw (build 16: the checks' frame rate)
std::string affinityResult="not applied",priorityResult="not applied",adpfResult="not applied",dxvkResult="not applied";
uint64_t reports=0,reportFailures=0;
std::vector<std::pair<std::string,int32_t>> tracked;
unsigned cpuCount=0;
unsigned placementLogs=0;
// Build 12: the render thread follows the fastest cluster (core_choice.h). Guarded by keeperMutex.
CoreChoice choice;
bool adaptive=false;
bool renderPlaced=false;                     // the render thread is pinned (to choice.current()'s cpus)
int32_t renderTid=0;
// What adoption applied to the render thread, so it can be followed again (followRenderAgain) when the keeper loses
// it while it still presents: its mask (the fixed one; adaptive placement uses choice.current()'s) and nice value.
bool renderPinned=false,renderNiced=false;
cpu_set_t renderFixedMask{};
std::atomic<bool> renderLost{false};          // the keeper stopped following the render thread
unsigned renderFollows=0;                     // times it was followed again
std::vector<int32_t> dxvkPlacedTids;         // DXVK threads placed off the render thread's cores (helperCpus)
std::vector<double> clusterSeconds;          // checks spent on each cluster (one a second)
std::vector<unsigned> capMinKhz;             // lowest allowed clock seen per cluster
std::vector<double> capSumKhz;               // for the average
uint64_t capChecks=0;
unsigned moveLogs=0;
// Each move with the seconds since adoption (the time base of the frame telemetry's per-second series, which
// starts at the same first Present), so the session summary can give each cluster's frame rate.
struct MoveRecord{uint32_t second;int cluster;};
constexpr size_t kMoveRecords=1000;          // ten seconds apart at least: hours of the busiest shuttling
std::vector<MoveRecord> moveRecord;
bool moveRecordFull=false;
std::chrono::steady_clock::time_point adoptedAt;
// Build 16: DXVK's command stream thread on a cluster of its own (CoreChoice::stream()), and its moves.
std::vector<int32_t> streamTids;
std::vector<double> streamSeconds;           // checks it spent on each cluster
std::vector<MoveRecord> streamRecord;
bool streamRecordFull=false;
unsigned streamLogs=0;
// Build 16: what each check measures. A cluster's clock over the check comes from cpufreq's time per clock
// (stats/time_in_state of its first cpu); where that is missing or stale, from scaling_cur_freq at the check.
struct ClusterClock{
    std::vector<std::pair<unsigned,uint64_t>> last;   // the previous time_in_state reading: kHz, ticks of 10 ms
    bool stats=false,stale=false;
    unsigned checks=0;bool statsMoved=false;std::vector<unsigned> curSeen;   // staleness: see measureClocks
};
std::vector<ClusterClock> clusterClocks;
std::vector<unsigned> clockNowKhz;           // each cluster's average clock over the last check (0 = not measured)
std::vector<unsigned> clusterOfCpu;          // cluster index of each cpu
// While the render thread was on a cluster: the clock it ran at and the cap, summed over the checks that
// measured them, and its share of the time on a cpu.
std::vector<double> renderClockSum,renderCapSum,renderBusySum;
std::vector<unsigned> renderClockChecks;
// Each tracked thread's schedstat (on a cpu, waiting for one) at the last check, the shares of the last check,
// and its time on a cpu over the session per cluster (by the cpu it last ran on at each check).
struct ThreadUse{
    std::string name;int32_t tid=0;
    uint64_t run=0,wait=0;bool have=false;
    int cpu=-1;double runShare=0,waitShare=0;
    double runSeconds=0,waitSeconds=0,spanSeconds=0;   // summed over the checks that measured it
    std::vector<double> clusterSeconds;
};
std::vector<ThreadUse> threadUse;            // the tracked threads (the render thread first), copied at adoption
std::chrono::steady_clock::time_point lastCheckAt;
bool checkedOnce=false;
uint64_t framesAtCheck=0;
int thermalStatus=-1;                        // AThermal's status at the last check (-1: unknown)
unsigned thermalResets=0;
std::string clockSource="not measured",pmuSummary="not read";
// A thread of this process by id: its name, or ESRCH once it is gone (or the id is another process's); any other
// error (out of file descriptors, say) is passed on as it is, and says nothing about the thread.
int threadName(int32_t tid,std::string* name){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/comm",int(tid));
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?ESRCH:errno;
    char text[64];ssize_t got=read(fd,text,sizeof text-1);int error=got<0?errno:0;close(fd);
    if(got<0)return error==ENOENT?ESRCH:error;
    while(got>0&&(text[got-1]=='\n'||text[got-1]=='\0'))--got;
    name->assign(text,size_t(got));return 0;
}
// When the thread started (field 22 of /proc/self/task/<tid>/stat, clock ticks since boot): with its id, which
// thread it is, whatever it is named.
int threadStart(int32_t tid,uint64_t* start){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/stat",int(tid));
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?ESRCH:errno;
    char text[1024];ssize_t got=read(fd,text,sizeof text-1);int error=got<0?errno:0;close(fd);
    if(got<0)return error==ENOENT?ESRCH:error;
    text[got]=0;
    const char* at=std::strrchr(text,')');   // the name, in parentheses, may hold spaces
    if(!at)return EINVAL;
    ++at;
    for(int field=3;field<=22;++field){        // field 3 (the state) follows the parenthesis
        while(*at==' ')++at;
        if(!*at)return EINVAL;
        if(field==22){char* end=nullptr;unsigned long long value=std::strtoull(at,&end,10);if(end==at)return EINVAL;*start=value;return 0;}
        while(*at&&*at!=' ')++at;
    }
    return EINVAL;
}
// /proc/self/task/<tid>/schedstat: nanoseconds on a cpu, then nanoseconds runnable but waiting for one.
int threadSchedstat(int32_t tid,uint64_t* run,uint64_t* wait){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/schedstat",int(tid));
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?ESRCH:errno;
    char text[128];ssize_t got=read(fd,text,sizeof text-1);int error=got<0?errno:0;close(fd);
    if(got<0)return error==ENOENT?ESRCH:error;
    text[got]=0;unsigned long long onCpu=0,waiting=0;
    if(std::sscanf(text,"%llu %llu",&onCpu,&waiting)!=2)return EINVAL;
    *run=onCpu;*wait=waiting;return 0;
}
// The cpu a thread last ran on (field 39 of /proc/self/task/<tid>/stat).
int threadCpu(int32_t tid,int* cpu){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/stat",int(tid));
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?ESRCH:errno;
    char text[1024];ssize_t got=read(fd,text,sizeof text-1);int error=got<0?errno:0;close(fd);
    if(got<0)return error==ENOENT?ESRCH:error;
    text[got]=0;
    const char* at=std::strrchr(text,')');
    if(!at)return EINVAL;
    ++at;
    for(int field=3;field<=39;++field){
        while(*at==' ')++at;
        if(!*at)return EINVAL;
        if(field==39){char* end=nullptr;long value=std::strtol(at,&end,10);if(end==at)return EINVAL;*cpu=int(value);return 0;}
        while(*at&&*at!=' ')++at;
    }
    return EINVAL;
}
// The thermal status (AThermal, API 30; -1 where unknown): the power HAL switches Pixel's PMU frequency limit with
// it, so the clusters' measured shares of their caps start over when it changes (core_choice.h).
int systemThermalStatus(){
#ifdef __ANDROID__
    using AcquireFn=void*(*)();using StatusFn=int(*)(void*);
    static StatusFn status=nullptr;static void* manager=nullptr;static bool tried=false;
    if(!tried){
        tried=true;
        void* android=dlopen("libandroid.so",RTLD_NOW|RTLD_NOLOAD);
        if(!android)android=dlopen("libandroid.so",RTLD_NOW);
        auto acquire=android?reinterpret_cast<AcquireFn>(dlsym(android,"AThermal_acquireManager")):nullptr;
        status=android?reinterpret_cast<StatusFn>(dlsym(android,"AThermal_getCurrentThermalStatus")):nullptr;
        manager=acquire&&status?acquire():nullptr;
    }
    return status&&manager?status(manager):-1;
#else
    return -1;
#endif
}
// The kernel's cpu directory, the cpu count, the affinity calls, the threads' start times, cpu use and last cpu,
// and the thermal status: replaceable by tests/host_perf.cpp, which runs this file against a fake cpu directory,
// with this machine's cpus or with the Pixel's eight simulated.
std::string cpuSysfs="/sys/devices/system/cpu";
std::string vendorSchedPoll="/proc/vendor_sched/pmu_poll_enable";   // Pixel's PMU limit switch (power HAL)
struct Os{
    std::function<long()> cpus=[]{return sysconf(_SC_NPROCESSORS_CONF);};
    std::function<int(int32_t,cpu_set_t*)> getAffinity=[](int32_t tid,cpu_set_t* mask){return sched_getaffinity(pid_t(tid),sizeof *mask,mask)==0?0:errno;};
    std::function<int(int32_t,const cpu_set_t&)> setAffinity=[](int32_t tid,const cpu_set_t& mask){return sched_setaffinity(pid_t(tid),sizeof mask,&mask)==0?0:errno;};
    std::function<int(int32_t,uint64_t*)> threadStart=host_perf::threadStart;
    std::function<int(int32_t,uint64_t*,uint64_t*)> schedstat=threadSchedstat;
    std::function<int(int32_t,int*)> lastCpu=threadCpu;
    std::function<int()> thermalStatus=systemThermalStatus;
    std::function<std::chrono::steady_clock::time_point()> now=[]{return std::chrono::steady_clock::now();};
} os;
PlacementKeeper::System systemCalls(){
    PlacementKeeper::System system;
    system.getName=threadName;
    system.getStart=[](int32_t tid,uint64_t* start){return os.threadStart(tid,start);};
    system.getAffinity=[](int32_t tid,cpu_set_t* mask){return os.getAffinity(tid,mask);};
    system.setAffinity=[](int32_t tid,const cpu_set_t& mask){return os.setAffinity(tid,mask);};
    system.getNice=[](int32_t tid,int* nice){errno=0;int value=getpriority(PRIO_PROCESS,id_t(tid));if(value==-1&&errno)return errno;*nice=value;return 0;};
    system.setNice=[](int32_t tid,int nice){return setpriority(PRIO_PROCESS,id_t(tid),nice)==0?0:errno;};
    return system;
}
// Its own lock: a check makes a dozen system calls, and `mutex` is taken by reportFrame on every frame.
// Order where both are held: mutex, then keeperMutex.
std::mutex keeperMutex;
PlacementKeeper keeper(systemCalls());
PlacementKeeper::Thread placed(const char* label,int32_t tid,const cpu_set_t* mask,bool keepNice,int nice){
    PlacementKeeper::Thread thread;thread.label=label;thread.tid=tid;threadName(tid,&thread.name);os.threadStart(tid,&thread.start);
    if(mask){thread.keepMask=true;thread.mask=*mask;}
    thread.keepNice=keepNice;thread.nice=nice;
    return thread;
}

unsigned readKhz(const std::string& path){
    std::ifstream file(path);unsigned value=0;if(!(file>>value))return 0;return value;
}

std::vector<int32_t> threadsNamed(const std::vector<const char*>& names){
    std::vector<int32_t> tids;
    DIR* tasks=opendir("/proc/self/task");if(!tasks)return tids;
    while(auto* entry=readdir(tasks)){
        if(entry->d_name[0]=='.')continue;
        std::string comm;{std::ifstream file(std::string("/proc/self/task/")+entry->d_name+"/comm");std::getline(file,comm);}
        for(auto* name:names)if(comm.rfind(name,0)==0){tids.push_back(int32_t(std::strtol(entry->d_name,nullptr,10)));break;}
    }
    closedir(tasks);return tids;
}
}  // namespace

namespace {
std::vector<int> readCpuList(const std::string& path){
    std::ifstream file(path);std::vector<int> cpus;int cpu;
    while(file>>cpu)cpus.push_back(cpu);
    return cpus;
}
cpu_set_t maskOf(const std::vector<int>& cpus){
    cpu_set_t set;CPU_ZERO(&set);
    for(int cpu:cpus)if(cpu>=0&&cpu<CPU_SETSIZE)CPU_SET(cpu,&set);
    return set;
}
// "5-6" for the JSON (CoreChoice::describe without its "cpu ").
std::string cpuList(const std::vector<int>& cpus){
    std::string text=CoreChoice::describe(cpus);
    return text.rfind("cpu ",0)==0?text.substr(4):"";
}
std::string cpuFile(unsigned cpu,const char* name){return cpuSysfs+"/cpu"+std::to_string(cpu)+"/"+name;}
// The clock each cluster may run at now (scaling_max_freq of its first cpu: the cap thermal and battery limits
// set); 0 where unreadable.
std::vector<unsigned> readCaps(){
    std::vector<unsigned> caps;caps.reserve(choice.size());
    for(size_t i=0;i<choice.size();++i){
        const auto& cpus=choice.cluster(i).cpus;
        caps.push_back(cpus.empty()?0:readKhz(cpuFile(unsigned(cpus.front()),"cpufreq/scaling_max_freq")));
    }
    return caps;
}
// cpufreq's time per clock for a cpu's cluster (stats/time_in_state: "kHz ticks" lines, ticks of 10 ms).
bool readTimeInState(unsigned cpu,std::vector<std::pair<unsigned,uint64_t>>* out){
    std::ifstream file(cpuFile(cpu,"cpufreq/stats/time_in_state"));
    if(!file)return false;
    out->clear();unsigned long long khz=0,ticks=0;
    while(file>>khz>>ticks)out->emplace_back(unsigned(khz),uint64_t(ticks));
    return !out->empty();
}
// The average clock between two readings (kHz; 0 when no time passed, or the table changed or was reset), and how
// many clocks the time went to.
unsigned averageKhz(const std::vector<std::pair<unsigned,uint64_t>>& before,const std::vector<std::pair<unsigned,uint64_t>>& after,unsigned* clocks){
    *clocks=0;
    if(before.size()!=after.size())return 0;
    double sum=0,ticks=0;
    for(size_t i=0;i<after.size();++i){
        if(after[i].first!=before[i].first||after[i].second<before[i].second){*clocks=0;return 0;}
        double d=double(after[i].second-before[i].second);
        if(d>0)++*clocks;
        sum+=d*after[i].first;ticks+=d;
    }
    return ticks>0?unsigned(sum/ticks+0.5):0;
}
// Each cluster's average clock since the last check (clockNowKhz): from time_in_state where it works, else the
// clock scaling_cur_freq shows at the check. A table whose time never moves between two clocks over the first
// minute while scaling_cur_freq shows three or more is stale (a governor that switches clocks past cpufreq's
// stats) and is no longer read.
void measureClocks(){
    unsigned withStats=0,eligible=0;
    for(size_t i=0;i<choice.size();++i){
        ClusterClock& c=clusterClocks[i];
        const auto& cpus=choice.cluster(i).cpus;
        if(cpus.empty()){clockNowKhz[i]=0;continue;}
        unsigned first=unsigned(cpus.front());
        unsigned cur=readKhz(cpuFile(first,"cpufreq/scaling_cur_freq"));
        if(cur&&c.curSeen.size()<8&&std::find(c.curSeen.begin(),c.curSeen.end(),cur)==c.curSeen.end())c.curSeen.push_back(cur);
        unsigned average=0;
        std::vector<std::pair<unsigned,uint64_t>> now;
        if(!c.stale&&readTimeInState(first,&now)){
            unsigned clocks=0;
            if(c.stats)average=averageKhz(c.last,now,&clocks);
            if(clocks>=2)c.statsMoved=true;
            c.last=std::move(now);c.stats=true;
            if(++c.checks>=60&&!c.statsMoved&&c.curSeen.size()>=3){
                c.stale=true;average=0;
                if(placementLogs<40){++placementLogs;
                    NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Clock measurement: %s's time_in_state does not follow its clock; its scaling_cur_freq instead",
                                    CoreChoice::describe(cpus).c_str());}
            }
        }
        clockNowKhz[i]=average?average:cur;
        if(choice.eligible(i)){++eligible;if(c.stats&&!c.stale)++withStats;}
    }
    char text[96];
    if(!eligible)clockSource="not measured";
    else if(withStats==eligible)clockSource="cpufreq stats (time_in_state)";
    else if(!withStats)clockSource="scaling_cur_freq at each check";
    else{std::snprintf(text,sizeof text,"time_in_state for %u of %u clusters, scaling_cur_freq for the rest",withStats,eligible);clockSource=text;}
}
// A small sysfs or proc value: true and the text, or false and errno.
bool readValue(const std::string& path,std::string* value,int* error){
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if(fd<0){*error=errno;return false;}
    char text[64];ssize_t got=read(fd,text,sizeof text-1);*error=got<0?errno:0;close(fd);
    if(got<0)return false;
    while(got>0&&(text[got-1]=='\n'||text[got-1]==' '))--got;
    value->assign(text,size_t(got));return true;
}
// Pixel's PMU frequency limit (sched_pixel, set by the power HAL; build 16): each eligible cluster's limit_frequency,
// spc_threshold and lcpi_threshold, and whether polling is on, where this app may read them.
std::string readPmuSummary(){
    std::string out;int firstError=0;bool any=false;
    for(size_t i=0;i<choice.size();++i){
        if(!choice.eligible(i)||choice.cluster(i).cpus.empty())continue;
        unsigned first=unsigned(choice.cluster(i).cpus.front());
        std::string limit,spc,lcpi;int error=0;
        bool haveLimit=readValue(cpuFile(first,"cpufreq/sched_pixel/limit_frequency"),&limit,&error);
        if(!haveLimit&&!firstError)firstError=error;
        bool haveSpc=readValue(cpuFile(first,"cpufreq/sched_pixel/spc_threshold"),&spc,&error);
        bool haveLcpi=readValue(cpuFile(first,"cpufreq/sched_pixel/lcpi_threshold"),&lcpi,&error);
        if(!haveLimit&&!haveSpc&&!haveLcpi)continue;
        any=true;
        out+=(out.empty()?"":"; ")+CoreChoice::describe(choice.cluster(i).cpus)+":";
        if(haveLimit){unsigned long khz=std::strtoul(limit.c_str(),nullptr,10);out+=" limit "+std::to_string(khz/1000)+" MHz";}
        if(haveSpc)out+=" spc_threshold "+spc;
        if(haveLcpi)out+=" lcpi_threshold "+lcpi;
    }
    std::string poll;int error=0;
    bool havePoll=readValue(vendorSchedPoll,&poll,&error);
    if(havePoll){any=true;out+=(out.empty()?"":"; ")+std::string("pmu_poll_enable ")+poll;}
    if(!any)return std::string("not readable (")+std::strerror(firstError?firstError:error?error:ENOENT)+")";
    return out;
}
}  // namespace

// The performance hint session's target work duration (build 15): three quarters of the frame period, so the
// governor clocks for the frame's work to end with time to spare, and never under the 120 Hz panel's own period
// (8.33 ms, also the target uncapped). Aimed at the period itself (builds 3-14), the governor settled where half the
// frames took longer: build 14 under a 60 cap ran 57 FPS on average, 60 in 46-59 % of its seconds, two frames in
// five at 16.7-20 ms, with the X4 at 1.9 GHz on average while it was allowed 3.78.
int64_t targetNanos(unsigned fps){
    constexpr int64_t panel=1000000000ll/120;
    return fps?std::max(int64_t(750000000ll/fps),panel):panel;   // 3/4 of 1e9/fps, without rounding it twice
}

CpuTopology readTopology(){
    CpuTopology topology;
    long count=os.cpus();topology.cpus=count>0?unsigned(count):0;
    std::vector<unsigned> khz(topology.cpus),capacity(topology.cpus);
    std::vector<std::vector<int>> related(topology.cpus);
    char buffer[112];
    bool capacities=true;
    for(unsigned cpu=0;cpu<topology.cpus;++cpu){
        khz[cpu]=readKhz(cpuFile(cpu,"cpufreq/cpuinfo_max_freq"));topology.maxKhz=std::max(topology.maxKhz,khz[cpu]);
        capacity[cpu]=readKhz(cpuFile(cpu,"cpu_capacity"));if(!capacity[cpu])capacities=false;
        related[cpu]=readCpuList(cpuFile(cpu,"cpufreq/related_cpus"));
        std::snprintf(buffer,sizeof buffer,"%scpu%u %u",cpu?", ":"",cpu,khz[cpu]/1000);topology.summary+=buffer;
    }
    // Clusters: cpus sharing a clock (related_cpus; cpus that do not say form one cluster per top clock).
    std::vector<bool> taken(topology.cpus,false);
    for(unsigned cpu=0;cpu<topology.cpus;++cpu){
        if(taken[cpu])continue;
        CoreChoice::Cluster cluster;
        for(unsigned other=cpu;other<topology.cpus;++other){
            bool same=!related[cpu].empty()?std::find(related[cpu].begin(),related[cpu].end(),int(other))!=related[cpu].end()
                                            :related[other].empty()&&khz[other]==khz[cpu];
            if(same&&!taken[other]){cluster.cpus.push_back(int(other));taken[other]=true;}
        }
        if(cluster.cpus.empty()){cluster.cpus.push_back(int(cpu));taken[cpu]=true;}
        cluster.maxKhz=khz[unsigned(cluster.cpus.front())];
        cluster.capacity=capacity[unsigned(cluster.cpus.front())];
        topology.clusters.push_back(cluster);
    }
    // Without cpu_capacity: the clock ratio, the fastest cluster's cores taken as 25 % more work per clock (the
    // Cortex-X over the A7xx cores of current phones).
    if(!capacities&&topology.maxKhz)
        for(auto& cluster:topology.clusters)
            cluster.capacity=unsigned(1024.0*cluster.maxKhz/topology.maxKhz*(cluster.maxKhz*20>=topology.maxKhz*19?1.0:0.8));
    CoreChoice clusters(topology.clusters);
    for(size_t i=0;i<topology.clusters.size();++i){
        std::snprintf(buffer,sizeof buffer,"%s%s (capacity %u%s)",i?", ":"",CoreChoice::describe(topology.clusters[i].cpus).c_str(),
                      topology.clusters[i].capacity,capacities?"":", estimated");
        topology.clusterSummary+=buffer;
    }
    // The fastest cluster: the biggest cores (one Cortex-X on current Tensor/Snapdragon).
    if(clusters.home()>=0)
        for(int cpu:topology.clusters[size_t(clusters.home())].cpus){
            std::snprintf(buffer,sizeof buffer,"%s%d",topology.fastest.empty()?"":",",cpu);topology.fastest+=buffer;
        }
    return topology;
}

void adoptRenderThread(Placement placement,unsigned targetFps){
    if(adopted.load(std::memory_order_acquire))return;   // every Present asks; only the first one works
    std::lock_guard<std::mutex> lock(mutex);
    if(adopted.exchange(true))return;
    currentTargetFps=targetFps;
    auto startedAt=os.now();
    pid_t tid=pid_t(syscall(SYS_gettid));
    CpuTopology topology=readTopology();
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","CPU topology: %s MHz; clusters: %s; fastest cluster: cpu %s",topology.summary.c_str(),
                    topology.clusterSummary.c_str(),topology.fastest.empty()?"?":topology.fastest.c_str());
    cpuCount=topology.cpus;
    // DXVK's threads (named since build 9, ci/dxvk_extra.py; before that the lookup found none): the command
    // stream, which does the Vulkan half of every draw on its own thread, then submission, the queue, and
    // (build 11) the cleanup thread that resets finished command lists. They exist once the device does,
    // before the first Present. Command stream first.
    std::vector<std::pair<const char*,int32_t>> dxvkThreads;
    for(const char* name:{"dxvk-cs","dxvk-submit","dxvk-queue","dxvk-recycle"})
        for(int32_t thread:threadsNamed({name}))dxvkThreads.emplace_back(name,thread);
    std::vector<int32_t> dxvk;for(const auto& thread:dxvkThreads)dxvk.push_back(thread.second);
    tracked.clear();tracked.emplace_back("render",int32_t(tid));
    for(const auto& thread:dxvkThreads)tracked.emplace_back(thread.first,thread.second);
    std::lock_guard<std::mutex> keeperLock(keeperMutex);
    keeper.clear();placementLogs=0;moveLogs=0;moveRecord.clear();moveRecordFull=false;adoptedAt=startedAt;
    choice=CoreChoice(topology.clusters);adaptive=false;renderPlaced=false;renderTid=int32_t(tid);dxvkPlacedTids.clear();
    renderPinned=renderNiced=false;CPU_ZERO(&renderFixedMask);renderLost=false;renderFollows=0;
    clusterSeconds.assign(choice.size(),0.0);capMinKhz.assign(choice.size(),0u);capSumKhz.assign(choice.size(),0.0);capChecks=0;
    // Build 16: the command stream's cluster and the measurements.
    streamTids.clear();streamSeconds.assign(choice.size(),0.0);streamRecord.clear();streamRecordFull=false;streamLogs=0;
    clusterClocks.assign(choice.size(),ClusterClock());clockNowKhz.assign(choice.size(),0u);
    renderClockSum.assign(choice.size(),0.0);renderCapSum.assign(choice.size(),0.0);renderBusySum.assign(choice.size(),0.0);
    renderClockChecks.assign(choice.size(),0u);
    clusterOfCpu.assign(topology.cpus,0u);
    for(size_t i=0;i<choice.size();++i)for(int cpu:choice.cluster(i).cpus)if(cpu>=0&&unsigned(cpu)<topology.cpus)clusterOfCpu[size_t(cpu)]=unsigned(i);
    threadUse.assign(tracked.size(),ThreadUse());
    for(size_t i=0;i<threadUse.size();++i){threadUse[i].name=tracked[i].first;threadUse[i].tid=tracked[i].second;threadUse[i].clusterSeconds.assign(choice.size(),0.0);}
    checkedOnce=false;thermalStatus=-1;thermalResets=0;clockSource="not measured";
    pmuSummary=readPmuSummary();
    cpu_set_t set,rest,streamMask;CPU_ZERO(&set);CPU_ZERO(&rest);CPU_ZERO(&streamMask);
    std::vector<bool> dxvkPlaced(dxvkThreads.size(),false),dxvkStream(dxvkThreads.size(),false);
    if((placement==Placement::FastestCores||placement==Placement::BigCore)&&!topology.fastest.empty()){
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu){
            std::string needle=std::to_string(cpu);std::string list=","+topology.fastest+",";
            if(list.find(","+needle+",")!=std::string::npos)CPU_SET(cpu,&set);
        }
        int error=os.setAffinity(int32_t(tid),set);
        renderPinned=error==0;renderFixedMask=set;
        if(renderPinned)affinityResult="render thread on cpu "+topology.fastest;
        else affinityResult=std::string("sched_setaffinity failed: ")+std::strerror(error);
        // Build 12: follows the fastest cluster as the phone caps clocks (two clusters to choose from at least).
        size_t candidates=0;for(size_t i=0;i<choice.size();++i)candidates+=choice.eligible(i)?1:0;
        adaptive=placement==Placement::FastestCores&&renderPinned&&candidates>1;
        // Build 16: with it, the command stream gets the fastest other cluster of its own (core_choice.h).
        bool streamOwn=adaptive&&choice.stream()>=0;
        if(streamOwn)streamMask=maskOf(choice.cluster(size_t(choice.stream())).cpus);
        // DXVK's threads stay off the render thread's cores. The performance hint boosts them too, and the
        // scheduler places a boosted thread on the biggest core that fits: the render thread's, where the
        // two would take turns. Build 8 races ran at 45-50 FPS under a 60 cap with that core idle 40 % of
        // the time, the render thread waiting on work outside it.
        unsigned others=0;
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu)if(!CPU_ISSET(cpu,&set)){CPU_SET(cpu,&rest);++others;}
        if(dxvk.empty())dxvkResult="DXVK threads not found";
        else if(!others)dxvkResult="DXVK threads: system placement (one cluster)";
        else{
            unsigned count=0,own=0;
            for(size_t i=0;i<dxvkThreads.size();++i){
                dxvkStream[i]=streamOwn&&std::strcmp(dxvkThreads[i].first,"dxvk-cs")==0;
                if(os.setAffinity(dxvkThreads[i].second,dxvkStream[i]?streamMask:rest)==0){dxvkPlaced[i]=true;++count;own+=dxvkStream[i]?1:0;}
            }
            char text[160];std::snprintf(text,sizeof text,"%u of %zu DXVK threads off cpu %s",count,dxvk.size(),topology.fastest.c_str());dxvkResult=text;
            if(own)dxvkResult+=", the command stream on "+CoreChoice::describe(choice.cluster(size_t(choice.stream())).cpus)+" (follows the fastest other cluster)";
        }
        for(size_t i=0;i<dxvkThreads.size();++i)if(dxvkPlaced[i])(dxvkStream[i]?streamTids:dxvkPlacedTids).push_back(dxvkThreads[i].second);
        renderPlaced=renderPinned&&choice.current()>=0;
        if(adaptive)moveRecord.push_back({0,choice.current()});
        if(!streamTids.empty())streamRecord.push_back({0,choice.stream()});
        if(renderPinned)affinityResult+=adaptive?" (follows the fastest cluster)":" (fixed)";
    }else{
        affinityResult=placement==Placement::System?"system placement":"no fastest cluster found";
        dxvkResult=dxvk.empty()?"DXVK threads not found":"DXVK threads: system placement";
    }
    // Apps may lower their own threads' nice value on Android (how THREAD_PRIORITY_URGENT_AUDIO works).
    bool renderNice=setpriority(PRIO_PROCESS,id_t(tid),-10)==0;
    renderNiced=renderNice;
    if(renderNice)priorityResult="nice -10";
    else priorityResult=std::string("setpriority failed: ")+std::strerror(errno);
    // The command stream is on every frame's critical path as much as the render thread is.
    unsigned raised=0,streams=0;std::vector<bool> dxvkNice(dxvkThreads.size(),false);
    for(size_t i=0;i<dxvkThreads.size();++i)if(std::strcmp(dxvkThreads[i].first,"dxvk-cs")==0){
        ++streams;if(setpriority(PRIO_PROCESS,id_t(dxvkThreads[i].second),-10)==0){dxvkNice[i]=true;++raised;}}
    if(streams){char text[64];std::snprintf(text,sizeof text,"; command stream nice -10 (%u of %u)",raised,streams);dxvkResult+=text;}
    // What maintainPlacement() keeps: whatever was applied above.
    if(renderPinned||renderNice)keeper.add(placed("render",int32_t(tid),renderPinned?&set:nullptr,renderNice,-10));
    for(size_t i=0;i<dxvkThreads.size();++i)if(dxvkPlaced[i]||dxvkNice[i])
        keeper.add(placed(dxvkThreads[i].first,dxvkThreads[i].second,dxvkPlaced[i]?(dxvkStream[i]?&streamMask:&rest):nullptr,dxvkNice[i],-10));
    // ADPF.
    void* android=dlopen("libandroid.so",RTLD_NOW|RTLD_NOLOAD);
    if(!android)android=dlopen("libandroid.so",RTLD_NOW);
    auto getManager=android?reinterpret_cast<GetManagerFn>(dlsym(android,"APerformanceHint_getManager")):nullptr;
    auto createSession=android?reinterpret_cast<CreateSessionFn>(dlsym(android,"APerformanceHint_createSession")):nullptr;
    updateTarget=android?reinterpret_cast<UpdateTargetFn>(dlsym(android,"APerformanceHint_updateTargetWorkDuration")):nullptr;
    reportActual=android?reinterpret_cast<ReportActualFn>(dlsym(android,"APerformanceHint_reportActualWorkDuration")):nullptr;
    closeSession=android?reinterpret_cast<CloseSessionFn>(dlsym(android,"APerformanceHint_closeSession")):nullptr;
    if(!getManager||!createSession||!updateTarget||!reportActual||!closeSession){adpfResult="performance hints unavailable on this Android";}
    else{
        std::vector<int32_t> tids=dxvk;
        tids.insert(tids.begin(),int32_t(tid));
        auto manager=getManager();
        session=manager?createSession(manager,tids.data(),tids.size(),targetNanos(targetFps)):nullptr;
        // A session the system will not take with DXVK's threads is still worth having for the render thread.
        if(!session&&manager&&tids.size()>1){tids.resize(1);session=createSession(manager,tids.data(),tids.size(),targetNanos(targetFps));}
        if(session){char text[96];std::snprintf(text,sizeof text,"session with %zu threads, target %.2f ms",tids.size(),targetNanos(targetFps)/1e6);adpfResult=text;}
        else adpfResult="session refused";
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Render thread placement: %s; priority: %s; %s; ADPF: %s",affinityResult.c_str(),priorityResult.c_str(),dxvkResult.c_str(),adpfResult.c_str());
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","PMU frequency limit (sched_pixel): %s",pmuSummary.c_str());
}

void setTargetFps(unsigned targetFps){
    std::lock_guard<std::mutex> lock(mutex);
    currentTargetFps=targetFps;
    if(session&&updateTarget)updateTarget(session,targetNanos(targetFps));
}

// On the render thread, when the keeper has lost it: followed again, by the thread itself, so there is no doubt
// which thread it is. Its placement (the current cluster with adaptive placement) and nice value are put back by
// the keeper's next check.
void followRenderAgain(){
    std::lock_guard<std::mutex> keeperLock(keeperMutex);
    if(!renderLost.exchange(false))return;
    int32_t tid=int32_t(syscall(SYS_gettid));
    if(keeper.tracks(tid))return;
    cpu_set_t mask=adaptive&&choice.current()>=0?maskOf(choice.cluster(size_t(choice.current())).cpus):renderFixedMask;
    keeper.add(placed("render",tid,renderPinned?&mask:nullptr,renderNiced,-10));
    std::string name="?";threadName(tid,&name);
    renderTid=tid;++renderFollows;
    if(placementLogs<40){++placementLogs;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Placement of render: followed again (thread %d \"%s\", still presenting)",int(tid),name.c_str());}
}

void reportFrame(std::chrono::nanoseconds work){
    if(!adopted.load(std::memory_order_relaxed))return;
    framesReported.fetch_add(1,std::memory_order_relaxed);
    if(renderLost.load(std::memory_order_relaxed))followRenderAgain();
    std::lock_guard<std::mutex> lock(mutex);
    if(!session||!reportActual)return;
    int64_t nanos=work.count();if(nanos<=0)return;
    ++reports;
    if(reportActual(session,nanos)!=0)++reportFailures;
}

namespace {
// Build 16, at each check: each cluster's clock, each tracked thread's time on a cpu and waiting for one, and what
// the render thread's cluster gave it (core_choice.h's Observation). Under keeperMutex.
CoreChoice::Observation measure(){
    CoreChoice::Observation seen;
    auto now=os.now();
    double seconds=checkedOnce?std::chrono::duration<double>(now-lastCheckAt).count():0;
    uint64_t frames=framesReported.load(std::memory_order_relaxed);
    measureClocks();
    double renderBusy=-1;
    for(size_t i=0;i<threadUse.size();++i){
        ThreadUse& use=threadUse[i];
        int32_t tid=i==0?renderTid:use.tid;   // the render thread as followed now
        uint64_t run=0,wait=0;int cpu=-1;
        bool have=os.schedstat(tid,&run,&wait)==0;
        if(os.lastCpu(tid,&cpu)!=0)cpu=-1;
        use.cpu=cpu;
        // Only intervals of a check a second count: the first after a pause spans the time in the background.
        if(have&&use.have&&seconds>0&&seconds<=3.0&&run>=use.run&&wait>=use.wait){
            double ran=(run-use.run)/1e9,waited=(wait-use.wait)/1e9;
            use.runShare=std::min(1.0,ran/seconds);use.waitShare=std::min(1.0,waited/seconds);
            use.runSeconds+=ran;use.waitSeconds+=waited;use.spanSeconds+=seconds;
            if(cpu>=0&&size_t(cpu)<clusterOfCpu.size()&&clusterOfCpu[size_t(cpu)]<use.clusterSeconds.size())use.clusterSeconds[clusterOfCpu[size_t(cpu)]]+=ran;
            if(i==0)renderBusy=use.runShare;
        }
        use.run=run;use.wait=wait;use.have=have;
    }
    // A check a second while playing: an interval far from that spans a pause or a stall and says nothing.
    if(checkedOnce&&seconds>=0.5&&seconds<=3.0&&renderBusy>=0){
        int at=choice.current();
        unsigned target=currentTargetFps.load(std::memory_order_relaxed);
        double fps=double(frames-framesAtCheck)/seconds;
        seen.clockKhz=at>=0&&size_t(at)<clockNowKhz.size()?clockNowKhz[size_t(at)]:0;
        seen.busy=renderBusy;
        seen.behind=fps<0.9*(target?target:120);
    }
    lastCheckAt=now;framesAtCheck=frames;checkedOnce=true;
    return seen;
}
// The thermal status changed: the clusters' shares start over (the PMU limit is switched with it).
void followThermalStatus(){
    int status=os.thermalStatus();
    if(status<0)return;
    if(thermalStatus>=0&&status!=thermalStatus&&adaptive){
        choice.resetShares();++thermalResets;
        if(placementLogs<40){++placementLogs;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Thermal status %d -> %d: the clusters' measured shares of their caps start over",thermalStatus,status);}
    }
    thermalStatus=status;
}
void recordMove(std::vector<MoveRecord>& record,bool& full,int cluster){
    if(record.size()<kMoveRecords){
        auto since=std::chrono::duration_cast<std::chrono::seconds>(os.now()-adoptedAt).count();
        record.push_back({uint32_t(since>0?since:0),cluster});
    }else full=true;
}
}  // namespace

void maintainPlacement(){
    if(!adopted.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(keeperMutex);
    CoreChoice::Observation seen=measure();
    followThermalStatus();
    // While the render thread was on a cluster: its clock, cap and busy share there (the session summary's).
    std::vector<unsigned> caps;
    if(adaptive||renderPlaced)caps=readCaps();
    if(renderPlaced&&keeper.tracks(renderTid)&&choice.current()>=0&&seen.clockKhz&&caps.size()==choice.size()){
        size_t at=size_t(choice.current());
        renderClockSum[at]+=seen.clockKhz;renderCapSum[at]+=choice.allowedKhz(at,caps[at]);renderBusySum[at]+=seen.busy;++renderClockChecks[at];
    }
    if(!keeper.size())return;
    // Moves wait while the render thread is not followed (until its next frame follows it again).
    if(adaptive&&keeper.tracks(renderTid)){
        int from=choice.current();
        if(from>=0)clusterSeconds[size_t(from)]+=1.0;
        if(choice.stream()>=0&&!streamTids.empty())streamSeconds[size_t(choice.stream())]+=1.0;
        ++capChecks;
        for(size_t i=0;i<caps.size();++i){
            unsigned cap=choice.allowedKhz(i,caps[i]);
            if(!capMinKhz[i]||cap<capMinKhz[i])capMinKhz[i]=cap;
            capSumKhz[i]+=cap;
        }
        // The clock each may run at (an unreadable cap counts as the top clock, as in core_choice.h).
        auto allowedMhz=[&](int i){return choice.allowedKhz(size_t(i),caps[size_t(i)])/1000;};
        if(choice.check(caps,seen)){
            int to=choice.current();
            cpu_set_t renderMask=maskOf(choice.cluster(size_t(to)).cpus),rest=maskOf(choice.helperCpus(to));
            // Through the keeper first: it still follows the render thread only while that thread lives under its
            // name, so an id the system has handed to another thread is never moved.
            int error=0;
            if(!keeper.setMask(renderTid,renderMask))error=ESRCH;
            else if((error=os.setAffinity(renderTid,renderMask))!=0)keeper.setMask(renderTid,maskOf(choice.cluster(size_t(from)).cpus));
            char text[320];
            if(error){
                choice.undo(from);
                std::snprintf(text,sizeof text,"Render thread stays on %s: moving it to %s failed (%s)",CoreChoice::describe(choice.cluster(size_t(from)).cpus).c_str(),
                              CoreChoice::describe(choice.cluster(size_t(to)).cpus).c_str(),std::strerror(error));
            }else{
                recordMove(moveRecord,moveRecordFull,to);
                for(int32_t thread:dxvkPlacedTids)if(keeper.setMask(thread,rest))os.setAffinity(thread,rest);
                std::snprintf(text,sizeof text,"Render thread to %s: allowed %u MHz, ran at %.0f %% of it (speed %.0f); from %s: allowed %u MHz, ran at %.0f %% of it (speed %.0f); DXVK threads on %s",
                              CoreChoice::describe(choice.cluster(size_t(to)).cpus).c_str(),allowedMhz(to),100*choice.share(size_t(to)),choice.speed(size_t(to),caps[size_t(to)]),
                              CoreChoice::describe(choice.cluster(size_t(from)).cpus).c_str(),allowedMhz(from),100*choice.share(size_t(from)),choice.speed(size_t(from),caps[size_t(from)]),
                              PlacementKeeper::describe(rest,cpuCount).c_str());
            }
            if(moveLogs<60){++moveLogs;NFS_RUNTIME_LOG(error?ANDROID_LOG_WARN:ANDROID_LOG_INFO,"NFSU1","%s",text);}
        }
        // The command stream's turn (after the render thread's: at once when that took its cluster).
        int streamFrom=choice.stream();
        bool taken=streamFrom>=0&&streamFrom==choice.current();
        if(!streamTids.empty()&&choice.checkStream(caps)){
            int to=choice.stream();
            cpu_set_t mask=maskOf(choice.cluster(size_t(to)).cpus);
            unsigned movedThreads=0;
            for(int32_t thread:streamTids)if(keeper.setMask(thread,mask)&&os.setAffinity(thread,mask)==0)++movedThreads;
            recordMove(streamRecord,streamRecordFull,to);
            if(streamLogs<60){++streamLogs;
                NFS_RUNTIME_LOG(movedThreads?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"NFSU1","Command stream to %s: allowed %u MHz (speed %.0f); from %s: allowed %u MHz (speed %.0f)%s%s",
                                CoreChoice::describe(choice.cluster(size_t(to)).cpus).c_str(),allowedMhz(to),choice.speedAtCap(size_t(to),caps[size_t(to)]),
                                streamFrom>=0?CoreChoice::describe(choice.cluster(size_t(streamFrom)).cpus).c_str():"?",streamFrom>=0?allowedMhz(streamFrom):0,
                                streamFrom>=0?choice.speedAtCap(size_t(streamFrom),caps[size_t(streamFrom)]):0.0,
                                taken?", which the render thread took":"",movedThreads?"":" (not applied: the thread is gone)");}
        }
    }
    for(const auto& change:keeper.check(cpuCount)){
        // The render thread lost while it presents is followed again from its next frame (reportFrame).
        if(change.dropped&&change.label=="render")renderLost=true;
        if(placementLogs>=40)continue;   // a session that keeps losing it is visible in the counts
        ++placementLogs;
        NFS_RUNTIME_LOG(change.restored?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"NFSU1","Placement of %s: %s",change.label.c_str(),change.what.c_str());
    }
}

void release(){
    std::lock_guard<std::mutex> lock(mutex);
    if(session&&closeSession)closeSession(session);
    session=nullptr;adopted=false;tracked.clear();
    std::lock_guard<std::mutex> keeperLock(keeperMutex);keeper.clear();threadUse.clear();streamTids.clear();
}

std::vector<std::pair<std::string,int32_t>> trackedThreads(){
    std::lock_guard<std::mutex> lock(mutex);
    return tracked;
}

namespace {
std::string jsonText(const std::string& text){
    std::string out;
    for(char c:text){
        if(c=='"'||c=='\\'){out+='\\';out+=c;}
        else if(static_cast<unsigned char>(c)<0x20)out+=' ';
        else out+=c;
    }
    return out;
}
}  // namespace

std::string statusJson(bool full){
    std::lock_guard<std::mutex> lock(mutex);
    std::lock_guard<std::mutex> keeperLock(keeperMutex);
    char text[720];
    std::snprintf(text,sizeof text,"{\"affinity\":\"%s\",\"priority\":\"%s\",\"dxvk\":\"%s\",\"adpf\":\"%s\",\"targetFps\":%u,\"reports\":%llu,\"reportFailures\":%llu,"
        "\"placementRestored\":%llu,\"placementFailed\":%llu,\"placementDropped\":%llu,\"renderFollowedAgain\":%u",
        affinityResult.c_str(),priorityResult.c_str(),dxvkResult.c_str(),adpfResult.c_str(),currentTargetFps.load(),static_cast<unsigned long long>(reports),static_cast<unsigned long long>(reportFailures),
        static_cast<unsigned long long>(keeper.restored()),static_cast<unsigned long long>(keeper.failed()),
        static_cast<unsigned long long>(keeper.dropped()),renderFollows);
    std::string json=text;
    // Build 16: where the clocks come from, what Pixel's PMU limit shows of itself, the thermal status and how often
    // it reset the shares; then the last check's clock per cluster and each tracked thread's [cpu, % on a cpu,
    // % waiting for one] (the device samples copy both).
    std::snprintf(text,sizeof text,",\"clockSource\":\"%s\",\"pmu\":\"%s\",\"thermalStatus\":%d,\"thermalResets\":%u",
                  jsonText(clockSource).c_str(),jsonText(pmuSummary).c_str(),thermalStatus,thermalResets);
    json+=text;
    json+=",\"clocksNow\":[";
    for(size_t i=0;i<clockNowKhz.size();++i)json+=(i?",":"")+std::to_string(clockNowKhz[i]/1000);
    json+="],\"threadsNow\":{";
    for(size_t i=0;i<threadUse.size();++i){
        std::snprintf(text,sizeof text,"%s\"%s\":[%d,%.0f,%.0f]",i?",":"",jsonText(threadUse[i].name).c_str(),threadUse[i].cpu,
                      100*threadUse[i].runShare,100*threadUse[i].waitShare);
        json+=text;
    }
    json+="}";
    // Build 12: the cpus the render thread is on now (the overlay shows them), and with FastestCores where it ran,
    // how often it moved and the clocks each cluster was allowed (min and average MHz, one check a second).
    // Build 16: each cluster's measured share of its cap, the checks that measured it, the clock the render thread
    // ran at there against the cap of those checks and its share of the time on a cpu, and the command stream's
    // seconds there.
    if(renderPlaced)json+=",\"renderCpus\":\""+cpuList(choice.cluster(size_t(choice.current())).cpus)+"\"";
    if(adaptive){
        json+=",\"coreMoves\":"+std::to_string(choice.moves())+",\"clusters\":[";
        for(size_t i=0;i<choice.size();++i){
            char entry[320];
            std::snprintf(entry,sizeof entry,"%s{\"cpus\":\"%s\",\"capacity\":%u,\"maxMhz\":%u,\"seconds\":%.0f,\"capMinMhz\":%u,\"capAvgMhz\":%.0f,"
                          "\"share\":%.3f,\"learned\":%u,\"streamSeconds\":%.0f",
                          i?",":"",cpuList(choice.cluster(i).cpus).c_str(),choice.cluster(i).capacity,choice.cluster(i).maxKhz/1000,
                          clusterSeconds[i],capMinKhz[i]/1000,capChecks?capSumKhz[i]/capChecks/1000.0:0.0,
                          choice.share(i),choice.learned(i),i<streamSeconds.size()?streamSeconds[i]:0.0);
            json+=entry;
            if(i<renderClockChecks.size()&&renderClockChecks[i]){
                double n=renderClockChecks[i];
                std::snprintf(entry,sizeof entry,",\"runMhz\":%.0f,\"runCapMhz\":%.0f,\"busy\":%.0f,\"measured\":%u",
                              renderClockSum[i]/n/1000.0,renderCapSum[i]/n/1000.0,100*renderBusySum[i]/n,renderClockChecks[i]);
                json+=entry;
            }
            json+="}";
        }
        json+="]";
        if(!streamTids.empty()&&choice.stream()>=0)
            json+=",\"streamCpus\":\""+cpuList(choice.cluster(size_t(choice.stream())).cpus)+"\",\"streamMoves\":"+std::to_string(choice.streamMoves());
        // [seconds since the first frame, cpus] from the start, the home cluster first; the session record only.
        if(full){
            json+=",\"moves\":[";
            for(size_t i=0;i<moveRecord.size();++i)
                json+=(i?",[":"[")+std::to_string(moveRecord[i].second)+",\""+cpuList(choice.cluster(size_t(moveRecord[i].cluster)).cpus)+"\"]";
            json+="]";
            if(moveRecordFull)json+=",\"movesComplete\":false";
            if(!streamRecord.empty()){
                json+=",\"streamMoveList\":[";
                for(size_t i=0;i<streamRecord.size();++i)
                    json+=(i?",[":"[")+std::to_string(streamRecord[i].second)+",\""+cpuList(choice.cluster(size_t(streamRecord[i].cluster)).cpus)+"\"]";
                json+="]";
                if(streamRecordFull)json+=",\"streamMovesComplete\":false";
            }
        }
    }
    // Build 16, the session record: each tracked thread's seconds on a cpu and waiting for one, and on a cpu per
    // cluster (by the cpu it last ran on at each check).
    if(full&&!threadUse.empty()){
        json+=",\"threads\":[";
        for(size_t i=0;i<threadUse.size();++i){
            std::snprintf(text,sizeof text,"%s{\"name\":\"%s\",\"spanS\":%.1f,\"runS\":%.1f,\"waitS\":%.1f,\"clusterRunS\":[",i?",":"",jsonText(threadUse[i].name).c_str(),
                          threadUse[i].spanSeconds,threadUse[i].runSeconds,threadUse[i].waitSeconds);
            json+=text;
            for(size_t k=0;k<threadUse[i].clusterSeconds.size();++k){
                std::snprintf(text,sizeof text,"%s%.1f",k?",":"",threadUse[i].clusterSeconds[k]);json+=text;
            }
            json+="]}";
        }
        json+="]";
    }
    return json+"}";
}
}  // namespace host_perf
