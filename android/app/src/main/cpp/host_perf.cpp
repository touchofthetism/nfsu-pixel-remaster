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

// A thread of this process by id: its name, or ESRCH once it is gone (or the id is another process's).
int threadName(int32_t tid,std::string* name){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/comm",int(tid));
    std::ifstream file(path);if(!file)return ESRCH;
    std::getline(file,*name);return 0;
}
PlacementKeeper::System systemCalls(){
    PlacementKeeper::System system;
    system.getName=threadName;
    system.getAffinity=[](int32_t tid,cpu_set_t* mask){return sched_getaffinity(pid_t(tid),sizeof *mask,mask)==0?0:errno;};
    system.setAffinity=[](int32_t tid,const cpu_set_t& mask){return sched_setaffinity(pid_t(tid),sizeof mask,&mask)==0?0:errno;};
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

CpuTopology readTopology(){
    CpuTopology topology;
    long count=sysconf(_SC_NPROCESSORS_CONF);topology.cpus=count>0?unsigned(count):0;
    std::vector<unsigned> khz(topology.cpus);
    char buffer[96];
    for(unsigned cpu=0;cpu<topology.cpus;++cpu){
        std::snprintf(buffer,sizeof buffer,"/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq",cpu);
        khz[cpu]=readKhz(buffer);topology.maxKhz=std::max(topology.maxKhz,khz[cpu]);
        std::snprintf(buffer,sizeof buffer,"%scpu%u %u",cpu?", ":"",cpu,khz[cpu]/1000);topology.summary+=buffer;
    }
    // The fastest cluster: every cpu within 5 % of the top clock (one Cortex-X on current Tensor/Snapdragon).
    for(unsigned cpu=0;cpu<topology.cpus;++cpu)if(topology.maxKhz&&khz[cpu]*20>=topology.maxKhz*19){
        std::snprintf(buffer,sizeof buffer,"%s%u",topology.fastest.empty()?"":",",cpu);topology.fastest+=buffer;
    }
    return topology;
}

void adoptRenderThread(Placement placement,unsigned targetFps){
    if(adopted.load(std::memory_order_acquire))return;   // every Present asks; only the first one works
    std::lock_guard<std::mutex> lock(mutex);
    if(adopted.exchange(true))return;
    currentTargetFps=targetFps;
    pid_t tid=pid_t(syscall(SYS_gettid));
    CpuTopology topology=readTopology();
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","CPU topology: %s MHz; fastest cluster: cpu %s",topology.summary.c_str(),topology.fastest.empty()?"?":topology.fastest.c_str());
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
    keeper.clear();placementLogs=0;
    cpu_set_t set,rest;CPU_ZERO(&set);CPU_ZERO(&rest);
    bool renderPinned=false;std::vector<bool> dxvkPlaced(dxvkThreads.size(),false);
    if(placement==Placement::FastestCores&&!topology.fastest.empty()){
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu){
            std::string needle=std::to_string(cpu);std::string list=","+topology.fastest+",";
            if(list.find(","+needle+",")!=std::string::npos)CPU_SET(cpu,&set);
        }
        renderPinned=sched_setaffinity(tid,sizeof set,&set)==0;
        if(renderPinned)affinityResult="render thread on cpu "+topology.fastest;
        else affinityResult=std::string("sched_setaffinity failed: ")+std::strerror(errno);
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
            for(size_t i=0;i<dxvkThreads.size();++i)if(sched_setaffinity(pid_t(dxvkThreads[i].second),sizeof rest,&rest)==0){dxvkPlaced[i]=true;++count;}
            char text[128];std::snprintf(text,sizeof text,"%u of %zu DXVK threads off cpu %s",count,dxvk.size(),topology.fastest.c_str());dxvkResult=text;
        }
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

std::string statusJson(){
    std::lock_guard<std::mutex> lock(mutex);
    std::lock_guard<std::mutex> keeperLock(keeperMutex);
    char text[720];
    std::snprintf(text,sizeof text,"{\"affinity\":\"%s\",\"priority\":\"%s\",\"dxvk\":\"%s\",\"adpf\":\"%s\",\"targetFps\":%u,\"reports\":%llu,\"reportFailures\":%llu,"
        "\"placementRestored\":%llu,\"placementFailed\":%llu}",
        affinityResult.c_str(),priorityResult.c_str(),dxvkResult.c_str(),adpfResult.c_str(),currentTargetFps,static_cast<unsigned long long>(reports),static_cast<unsigned long long>(reportFailures),
        static_cast<unsigned long long>(keeper.restored()),static_cast<unsigned long long>(keeper.failed()));
    return text;
}
}  // namespace host_perf
