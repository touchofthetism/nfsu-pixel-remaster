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
std::string affinityResult="not applied",priorityResult="not applied",adpfResult="not applied";
uint64_t reports=0,reportFailures=0;

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
    if(placement==Placement::FastestCores&&!topology.fastest.empty()){
        cpu_set_t set;CPU_ZERO(&set);
        for(unsigned cpu=0;cpu<topology.cpus;++cpu){
            std::string needle=std::to_string(cpu);std::string list=","+topology.fastest+",";
            if(list.find(","+needle+",")!=std::string::npos)CPU_SET(cpu,&set);
        }
        if(sched_setaffinity(tid,sizeof set,&set)==0)affinityResult="render thread on cpu "+topology.fastest;
        else affinityResult=std::string("sched_setaffinity failed: ")+std::strerror(errno);
    }else affinityResult=placement==Placement::System?"system placement":"no fastest cluster found";
    // Apps may lower their own threads' nice value on Android (how THREAD_PRIORITY_URGENT_AUDIO works).
    if(setpriority(PRIO_PROCESS,id_t(tid),-10)==0)priorityResult="nice -10";
    else priorityResult=std::string("setpriority failed: ")+std::strerror(errno);
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
        std::vector<int32_t> tids=threadsNamed({"dxvk-cs","dxvk-submit","dxvk-queue"});
        tids.insert(tids.begin(),int32_t(tid));
        auto manager=getManager();
        session=manager?createSession(manager,tids.data(),tids.size(),targetNanos(targetFps)):nullptr;
        if(session){char text[96];std::snprintf(text,sizeof text,"session with %zu threads, target %.2f ms",tids.size(),targetNanos(targetFps)/1e6);adpfResult=text;}
        else adpfResult="session refused";
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Render thread placement: %s; priority: %s; ADPF: %s",affinityResult.c_str(),priorityResult.c_str(),adpfResult.c_str());
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
    session=nullptr;adopted=false;
}

std::string statusJson(){
    std::lock_guard<std::mutex> lock(mutex);
    char text[512];
    std::snprintf(text,sizeof text,"{\"affinity\":\"%s\",\"priority\":\"%s\",\"adpf\":\"%s\",\"targetFps\":%u,\"reports\":%llu,\"reportFailures\":%llu}",
        affinityResult.c_str(),priorityResult.c_str(),adpfResult.c_str(),currentTargetFps,static_cast<unsigned long long>(reports),static_cast<unsigned long long>(reportFailures));
    return text;
}
}  // namespace host_perf
