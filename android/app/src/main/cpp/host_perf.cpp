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
unsigned currentTargetFps=120;
std::string affinityResult="not applied",priorityResult="not applied",adpfResult="not applied",dxvkResult="not applied";
uint64_t reports=0,reportFailures=0;
std::vector<std::pair<std::string,int32_t>> tracked;
unsigned cpuCount=0;
unsigned placementLogs=0;
// Build 12: the render thread follows the fastest cluster (core_choice.h). Guarded by keeperMutex.
CoreChoice choice;
bool adaptive=false;
bool renderPlaced=false;                     // the render thread is pinned (to choice.current()'s cpus)
bool renderAlive=true;                       // false once a move found the render thread gone
int32_t renderTid=0;
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
// The kernel's cpu directory, the cpu count and the affinity calls: replaceable by tests/host_perf.cpp, which
// runs this file against a fake cpu directory, with this machine's cpus or with the Pixel's eight simulated.
std::string cpuSysfs="/sys/devices/system/cpu";
struct Os{
    std::function<long()> cpus=[]{return sysconf(_SC_NPROCESSORS_CONF);};
    std::function<int(int32_t,cpu_set_t*)> getAffinity=[](int32_t tid,cpu_set_t* mask){return sched_getaffinity(pid_t(tid),sizeof *mask,mask)==0?0:errno;};
    std::function<int(int32_t,const cpu_set_t&)> setAffinity=[](int32_t tid,const cpu_set_t& mask){return sched_setaffinity(pid_t(tid),sizeof mask,&mask)==0?0:errno;};
} os;

// A thread of this process by id: its name, or ESRCH once it is gone (or the id is another process's).
int threadName(int32_t tid,std::string* name){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/comm",int(tid));
    std::ifstream file(path);if(!file)return ESRCH;
    std::getline(file,*name);return 0;
}
PlacementKeeper::System systemCalls(){
    PlacementKeeper::System system;
    system.getName=threadName;
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
    PlacementKeeper::Thread thread;thread.label=label;thread.tid=tid;threadName(tid,&thread.name);
    if(mask){thread.keepMask=true;thread.mask=*mask;}
    thread.keepNice=keepNice;thread.nice=nice;
    return thread;
}

unsigned readKhz(const std::string& path){
    std::ifstream file(path);unsigned value=0;if(!(file>>value))return 0;return value;
}
int64_t targetNanos(unsigned fps){return int64_t(1000000000ll/(fps?fps:120));}

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
}  // namespace

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
    auto startedAt=std::chrono::steady_clock::now();
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
    choice=CoreChoice(topology.clusters);adaptive=false;renderPlaced=false;renderAlive=true;renderTid=int32_t(tid);dxvkPlacedTids.clear();
    clusterSeconds.assign(choice.size(),0.0);capMinKhz.assign(choice.size(),0u);capSumKhz.assign(choice.size(),0.0);capChecks=0;
    cpu_set_t set,rest;CPU_ZERO(&set);CPU_ZERO(&rest);
    bool renderPinned=false;std::vector<bool> dxvkPlaced(dxvkThreads.size(),false);
    if((placement==Placement::FastestCores||placement==Placement::BigCore)&&!topology.fastest.empty()){
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu){
            std::string needle=std::to_string(cpu);std::string list=","+topology.fastest+",";
            if(list.find(","+needle+",")!=std::string::npos)CPU_SET(cpu,&set);
        }
        int error=os.setAffinity(int32_t(tid),set);
        renderPinned=error==0;
        if(renderPinned)affinityResult="render thread on cpu "+topology.fastest;
        else affinityResult=std::string("sched_setaffinity failed: ")+std::strerror(error);
        // DXVK's threads stay off the render thread's cores. The performance hint boosts them too, and the
        // scheduler places a boosted thread on the biggest core that fits: the render thread's, where the
        // two would take turns. Build 8 races ran at 45-50 FPS under a 60 cap with that core idle 40 % of
        // the time, the render thread waiting on work outside it.
        unsigned others=0;
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu)if(!CPU_ISSET(cpu,&set)){CPU_SET(cpu,&rest);++others;}
        if(dxvk.empty())dxvkResult="DXVK threads not found";
        else if(!others)dxvkResult="DXVK threads: system placement (one cluster)";
        else{
            unsigned count=0;
            for(size_t i=0;i<dxvkThreads.size();++i)if(os.setAffinity(dxvkThreads[i].second,rest)==0){dxvkPlaced[i]=true;++count;}
            char text[128];std::snprintf(text,sizeof text,"%u of %zu DXVK threads off cpu %s",count,dxvk.size(),topology.fastest.c_str());dxvkResult=text;
        }
        for(size_t i=0;i<dxvkThreads.size();++i)if(dxvkPlaced[i])dxvkPlacedTids.push_back(dxvkThreads[i].second);
        // Build 12: follows the fastest cluster as the phone caps clocks (two clusters to choose from at least).
        size_t candidates=0;for(size_t i=0;i<choice.size();++i)candidates+=choice.eligible(i)?1:0;
        adaptive=placement==Placement::FastestCores&&renderPinned&&candidates>1;
        renderPlaced=renderPinned&&choice.current()>=0;
        if(adaptive)moveRecord.push_back({0,choice.current()});
        if(renderPinned)affinityResult+=adaptive?" (follows the fastest cluster)":" (fixed)";
    }else{
        affinityResult=placement==Placement::System?"system placement":"no fastest cluster found";
        dxvkResult=dxvk.empty()?"DXVK threads not found":"DXVK threads: system placement";
    }
    // Apps may lower their own threads' nice value on Android (how THREAD_PRIORITY_URGENT_AUDIO works).
    bool renderNice=setpriority(PRIO_PROCESS,id_t(tid),-10)==0;
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
        keeper.add(placed(dxvkThreads[i].first,dxvkThreads[i].second,dxvkPlaced[i]?&rest:nullptr,dxvkNice[i],-10));
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
}

void setTargetFps(unsigned targetFps){
    std::lock_guard<std::mutex> lock(mutex);
    currentTargetFps=targetFps;
    if(session&&updateTarget)updateTarget(session,targetNanos(targetFps));
}

void reportFrame(std::chrono::nanoseconds work){
    if(!adopted.load(std::memory_order_relaxed))return;
    std::lock_guard<std::mutex> lock(mutex);
    if(!session||!reportActual)return;
    int64_t nanos=work.count();if(nanos<=0)return;
    ++reports;
    if(reportActual(session,nanos)!=0)++reportFailures;
}

void maintainPlacement(){
    if(!adopted.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(keeperMutex);
    if(!keeper.size())return;
    if(adaptive&&renderAlive){
        std::vector<unsigned> caps=readCaps();
        int from=choice.current();
        if(from>=0)clusterSeconds[size_t(from)]+=1.0;
        ++capChecks;
        for(size_t i=0;i<caps.size();++i){
            unsigned cap=caps[i]?std::min(caps[i],choice.cluster(i).maxKhz):choice.cluster(i).maxKhz;
            if(!capMinKhz[i]||cap<capMinKhz[i])capMinKhz[i]=cap;
            capSumKhz[i]+=cap;
        }
        if(choice.check(caps)){
            int to=choice.current();
            cpu_set_t renderMask=maskOf(choice.cluster(size_t(to)).cpus),rest=maskOf(choice.helperCpus(to));
            // Through the keeper first: it still follows the render thread only while that thread lives under its
            // name, so an id the system has handed to another thread is never moved.
            int error=0;
            if(!keeper.setMask(renderTid,renderMask))error=ESRCH;
            else if((error=os.setAffinity(renderTid,renderMask))!=0)keeper.setMask(renderTid,maskOf(choice.cluster(size_t(from)).cpus));
            char text[256];
            if(error){
                choice.undo(from);
                if(error==ESRCH)renderAlive=false;
                std::snprintf(text,sizeof text,"Render thread stays on %s: moving it to %s failed (%s)",CoreChoice::describe(choice.cluster(size_t(from)).cpus).c_str(),
                              CoreChoice::describe(choice.cluster(size_t(to)).cpus).c_str(),std::strerror(error));
            }else{
                if(moveRecord.size()<kMoveRecords){
                    auto since=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-adoptedAt).count();
                    moveRecord.push_back({uint32_t(since>0?since:0),to});
                }else moveRecordFull=true;
                for(int32_t thread:dxvkPlacedTids)if(keeper.setMask(thread,rest))os.setAffinity(thread,rest);
                // The clock each may run at (an unreadable cap counts as the top clock, as in core_choice.h).
                auto allowedMhz=[&](int i){unsigned cap=caps[size_t(i)],top=choice.cluster(size_t(i)).maxKhz;return (cap&&cap<top?cap:top)/1000;};
                std::snprintf(text,sizeof text,"Render thread to %s, allowed %u MHz (speed %.0f), from %s allowed %u MHz (speed %.0f); DXVK threads on %s",
                              CoreChoice::describe(choice.cluster(size_t(to)).cpus).c_str(),allowedMhz(to),choice.speed(size_t(to),caps[size_t(to)]),
                              CoreChoice::describe(choice.cluster(size_t(from)).cpus).c_str(),allowedMhz(from),choice.speed(size_t(from),caps[size_t(from)]),
                              PlacementKeeper::describe(rest,cpuCount).c_str());
            }
            if(moveLogs<60){++moveLogs;NFS_RUNTIME_LOG(error?ANDROID_LOG_WARN:ANDROID_LOG_INFO,"NFSU1","%s",text);}
        }
    }
    for(const auto& change:keeper.check(cpuCount)){
        if(placementLogs>=40)break;   // a session that keeps losing it is visible in the counts
        ++placementLogs;
        NFS_RUNTIME_LOG(change.restored?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"NFSU1","Placement of %s: %s",change.label.c_str(),change.what.c_str());
    }
}

void release(){
    std::lock_guard<std::mutex> lock(mutex);
    if(session&&closeSession)closeSession(session);
    session=nullptr;adopted=false;tracked.clear();
    std::lock_guard<std::mutex> keeperLock(keeperMutex);keeper.clear();
}

std::vector<std::pair<std::string,int32_t>> trackedThreads(){
    std::lock_guard<std::mutex> lock(mutex);
    return tracked;
}

std::string statusJson(bool full){
    std::lock_guard<std::mutex> lock(mutex);
    std::lock_guard<std::mutex> keeperLock(keeperMutex);
    char text[720];
    std::snprintf(text,sizeof text,"{\"affinity\":\"%s\",\"priority\":\"%s\",\"dxvk\":\"%s\",\"adpf\":\"%s\",\"targetFps\":%u,\"reports\":%llu,\"reportFailures\":%llu,"
        "\"placementRestored\":%llu,\"placementFailed\":%llu",
        affinityResult.c_str(),priorityResult.c_str(),dxvkResult.c_str(),adpfResult.c_str(),currentTargetFps,static_cast<unsigned long long>(reports),static_cast<unsigned long long>(reportFailures),
        static_cast<unsigned long long>(keeper.restored()),static_cast<unsigned long long>(keeper.failed()));
    std::string json=text;
    // Build 12: the cpus the render thread is on now (the overlay shows them), and with FastestCores where it ran,
    // how often it moved and the clocks each cluster was allowed (min and average MHz, one check a second).
    if(renderPlaced)json+=",\"renderCpus\":\""+cpuList(choice.cluster(size_t(choice.current())).cpus)+"\"";
    if(adaptive){
        json+=",\"coreMoves\":"+std::to_string(choice.moves())+",\"clusters\":[";
        for(size_t i=0;i<choice.size();++i){
            char entry[160];
            std::snprintf(entry,sizeof entry,"%s{\"cpus\":\"%s\",\"capacity\":%u,\"maxMhz\":%u,\"seconds\":%.0f,\"capMinMhz\":%u,\"capAvgMhz\":%.0f}",
                          i?",":"",cpuList(choice.cluster(i).cpus).c_str(),choice.cluster(i).capacity,choice.cluster(i).maxKhz/1000,
                          clusterSeconds[i],capMinKhz[i]/1000,capChecks?capSumKhz[i]/capChecks/1000.0:0.0);
            json+=entry;
        }
        json+="]";
        // [seconds since the first frame, cpus] from the start, the home cluster first; the session record only.
        if(full){
            json+=",\"moves\":[";
            for(size_t i=0;i<moveRecord.size();++i)
                json+=(i?",[":"[")+std::to_string(moveRecord[i].second)+",\""+cpuList(choice.cluster(size_t(moveRecord[i].cluster)).cpus)+"\"]";
            json+="]";
            if(moveRecordFull)json+=",\"movesComplete\":false";
        }
    }
    return json+"}";
}
}  // namespace host_perf
