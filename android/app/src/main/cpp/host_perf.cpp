#include "host_perf.h"
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
    // DXVK's threads (named since build 9, ci/dxvk_extra.py; before that the lookup found none): the command
    // stream, which does the Vulkan half of every draw on its own thread, then submission and the queue.
    // They exist once the device does, before the first Present. Command stream first.
    std::vector<int32_t> commandStream=threadsNamed({"dxvk-cs"}),dxvk=commandStream;
    std::vector<int32_t> submit=threadsNamed({"dxvk-submit"}),queue=threadsNamed({"dxvk-queue"});
    for(int32_t thread:submit)dxvk.push_back(thread);
    for(int32_t thread:queue)dxvk.push_back(thread);
    tracked.clear();tracked.emplace_back("render",int32_t(tid));
    for(int32_t thread:commandStream)tracked.emplace_back("dxvk-cs",thread);
    for(int32_t thread:submit)tracked.emplace_back("dxvk-submit",thread);
    for(int32_t thread:queue)tracked.emplace_back("dxvk-queue",thread);
    if(placement==Placement::FastestCores&&!topology.fastest.empty()){
        cpu_set_t set;CPU_ZERO(&set);
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu){
            std::string needle=std::to_string(cpu);std::string list=","+topology.fastest+",";
            if(list.find(","+needle+",")!=std::string::npos)CPU_SET(cpu,&set);
        }
        if(sched_setaffinity(tid,sizeof set,&set)==0)affinityResult="render thread on cpu "+topology.fastest;
        else affinityResult=std::string("sched_setaffinity failed: ")+std::strerror(errno);
        // DXVK's threads stay off the render thread's cores. The performance hint boosts them too, and the
        // scheduler places a boosted thread on the biggest core that fits: the render thread's, where the
        // two would take turns. Build 8 races ran at 45-50 FPS under a 60 cap with that core idle 40 % of
        // the time, the render thread waiting on work outside it.
        cpu_set_t rest;CPU_ZERO(&rest);unsigned others=0;
        for(unsigned cpu=0;cpu<topology.cpus&&cpu<CPU_SETSIZE;++cpu)if(!CPU_ISSET(cpu,&set)){CPU_SET(cpu,&rest);++others;}
        if(dxvk.empty())dxvkResult="DXVK threads not found";
        else if(!others)dxvkResult="DXVK threads: system placement (one cluster)";
        else{
            unsigned placed=0;for(int32_t thread:dxvk)if(sched_setaffinity(pid_t(thread),sizeof rest,&rest)==0)++placed;
            char text[128];std::snprintf(text,sizeof text,"%u of %zu DXVK threads off cpu %s",placed,dxvk.size(),topology.fastest.c_str());dxvkResult=text;
        }
    }else{
        affinityResult=placement==Placement::System?"system placement":"no fastest cluster found";
        dxvkResult=dxvk.empty()?"DXVK threads not found":"DXVK threads: system placement";
    }
    // Apps may lower their own threads' nice value on Android (how THREAD_PRIORITY_URGENT_AUDIO works).
    if(setpriority(PRIO_PROCESS,id_t(tid),-10)==0)priorityResult="nice -10";
    else priorityResult=std::string("setpriority failed: ")+std::strerror(errno);
    // The command stream is on every frame's critical path as much as the render thread is.
    unsigned raised=0;for(int32_t thread:commandStream)if(setpriority(PRIO_PROCESS,id_t(thread),-10)==0)++raised;
    if(!commandStream.empty()){char text[64];std::snprintf(text,sizeof text,"; command stream nice -10 (%u of %zu)",raised,commandStream.size());dxvkResult+=text;}
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

void release(){
    std::lock_guard<std::mutex> lock(mutex);
    if(session&&closeSession)closeSession(session);
    session=nullptr;adopted=false;tracked.clear();
}

std::vector<std::pair<std::string,int32_t>> trackedThreads(){
    std::lock_guard<std::mutex> lock(mutex);
    return tracked;
}

std::string statusJson(){
    std::lock_guard<std::mutex> lock(mutex);
    char text[640];
    std::snprintf(text,sizeof text,"{\"affinity\":\"%s\",\"priority\":\"%s\",\"dxvk\":\"%s\",\"adpf\":\"%s\",\"targetFps\":%u,\"reports\":%llu,\"reportFailures\":%llu}",
        affinityResult.c_str(),priorityResult.c_str(),dxvkResult.c_str(),adpfResult.c_str(),currentTargetFps,static_cast<unsigned long long>(reports),static_cast<unsigned long long>(reportFailures));
    return text;
}
}  // namespace host_perf
