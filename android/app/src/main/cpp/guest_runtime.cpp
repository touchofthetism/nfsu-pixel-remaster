#include <initializer_list>
#include <set>
#include <condition_variable>
#include "guest_runtime.h"
#include "android_resolution.h"
#include "guest_memory.h"
#include "guest_heap.h"
#include "runtime_log.h"
#include "frame_pacer.h"
#include "frame_telemetry.h"
#include "guest_profiler.h"
#include "call_watch.h"
#include "host_perf.h"
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
#include "d3d9_bridge.h"
#include "nfs_widescreen.h"
#include "nfs_misc_fixes.h"
#include "nfs_reflections.h"
#include "nfs_garage.h"
#include "nfs_rims.h"
#include "nfs_fastpath.h"
#include "effect_states.h"
#include "rim_sizes.h"
#include "free_roam.h"
#include "nfs_freeroam.h"
#endif
#include <array>
#include <cstdio>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <filesystem>
#include <future>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <cerrno>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <link.h>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif
extern "C" {
#include "recomp_types.h"
#include "pe_format.h"
#include "nfs_runtime.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
#ifndef RECOMP_X87_WINDOW
double g_st[8];
#endif
int g_fp_top;
uint16_t g_fpu_cw = 0x027f;
uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint64_t g_mm[8];
V128 g_xmm[8];
uint32_t g_mxcsr = 0x1f80;
uint32_t g_fs_base, g_gs_base, g_cur_func;
ptrdiff_t g_mem_base;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;
}

namespace {
std::atomic<uint32_t> displayWidth{1280},displayHeight{720};
std::atomic<uint32_t> renderWidth{0},renderHeight{0};
std::atomic<unsigned> frameLimit{0};
// Session frame telemetry (Present intervals); read as JSON by GameActivity for the overlay and the
// session record. See frame_telemetry.h.
FrameTelemetry frameTelemetry;
#ifdef __ANDROID__
// GPU idle microseconds from the prepared DXVK (ci/dxvk_extra.py), the HUD's "GPU load" measure.
extern "C" uint64_t dxvkAndroidGpuIdleMicros();
// DXVK's submission queue now and its present totals (ci/dxvk_extra.py): waiting to submit, on the GPU, in
// vkQueuePresentKHR, waiting on a GPU fence, command stream waiting for room; presents, ns, longest ns, slow.
extern "C" void dxvkAndroidQueueState(uint64_t out[9]);
// Command list processing (ci/dxvk_extra.py, build 11): resets, fence waits, vkQueueSubmit, lists and descriptor
// pools created and destroyed, the cleanup thread's backlog, each step of a reset. Returns how many it has.
extern "C" size_t dxvkAndroidQueueTimings(uint64_t* out, size_t count);
#endif
// Launcher "CPU placement" (host_perf.h): applied on the render thread at its first Present.
std::atomic<int> cpuPlacement{int(host_perf::Placement::FastestCores)};
// Set when the game pauses: the frame that spans the pause is not reported to the performance hint session.
std::atomic<bool> frameInterrupted{false};
std::string hex(uint32_t va) {
    std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << va;
    return out.str();
}
struct GuestStop : std::runtime_error { using std::runtime_error::runtime_error; };
struct State {
    uint32_t eax{}, ecx{}, edx{}, esp{}, ebx{}, esi{}, edi{}, ebp{}, fs{}, gs{}, cur{};
    uint32_t flags[4]{}, mxcsr{0x1f80};
    uint16_t segments[6]{}, cw{0x027f};
    double st[8]{}; int top{};
    uint64_t mm[8]{}; V128 xmm[8]{};
    void save() {
        eax=g_eax; ecx=g_ecx; edx=g_edx; esp=g_esp; ebx=g_ebx; esi=g_esi; edi=g_edi; ebp=g_ebp;
        fs=g_fs_base; gs=g_gs_base; cur=g_cur_func; mxcsr=g_mxcsr;
        flags[0]=g_flag_k; flags[1]=g_flag_a; flags[2]=g_flag_b; flags[3]=g_flag_cf;
        segments[0]=g_seg_cs; segments[1]=g_seg_ds; segments[2]=g_seg_es;
        segments[3]=g_seg_fs; segments[4]=g_seg_gs; segments[5]=g_seg_ss;
        std::memcpy(st,g_st,sizeof st); top=g_fp_top; cw=g_fpu_cw;
        std::memcpy(mm,g_mm,sizeof mm); std::memcpy(xmm,g_xmm,sizeof xmm);
    }
    void load() const {
        g_eax=eax; g_ecx=ecx; g_edx=edx; g_esp=esp; g_ebx=ebx; g_esi=esi; g_edi=edi; g_ebp=ebp;
        g_fs_base=fs; g_gs_base=gs; g_cur_func=cur; g_mxcsr=mxcsr;
        g_flag_k=flags[0]; g_flag_a=flags[1]; g_flag_b=flags[2]; g_flag_cf=flags[3];
        g_seg_cs=segments[0]; g_seg_ds=segments[1]; g_seg_es=segments[2];
        g_seg_fs=segments[3]; g_seg_gs=segments[4]; g_seg_ss=segments[5];
        std::memcpy(g_st,st,sizeof st); g_fp_top=top; g_fpu_cw=cw;
        std::memcpy(g_mm,mm,sizeof mm); std::memcpy(g_xmm,xmm,sizeof xmm);
    }
};
struct Import { std::string name; recomp_func_t function{}; uint64_t calls{}; };
std::unique_ptr<GuestMemory> memory;
std::unique_ptr<GuestHeaps> heaps;
// The thread that presents (the game's busy thread): longer scheduler slices, and the profiler's "render".
thread_local bool renderThread=false;
// Sampling profiler (guest_profiler.h): who holds the machine, and whether the render thread is in a host
// call. Relaxed stores on lock/unlock and around D3D9/sound/input dispatch; a sampler thread reads them.
// Only the render thread writes renderPlace, so a place left set while another thread holds the machine
// (an import that waits) is never read for that thread.
std::atomic<uint8_t> machineHolder{GuestProfile::Nobody};
std::atomic<uint8_t> renderPlace{GuestProfile::Guest};
GuestProfile guestProfile;
constexpr unsigned kProfileIntervalUs=1000;
// Long D3D9 calls (call_watch.h): d3dDispatch publishes the call it is in, the profiler's sampler times
// the ones that stay 20 ms or more and records what DXVK and its threads were doing meanwhile.
CallWatch callWatch;
struct RenderPlace {
    bool active;
    explicit RenderPlace(GuestProfile::Place place):active(renderThread){if(active)renderPlace.store(place,std::memory_order_relaxed);}
    ~RenderPlace(){if(active)renderPlace.store(GuestProfile::Guest,std::memory_order_relaxed);}
    RenderPlace(const RenderPlace&)=delete;RenderPlace& operator=(const RenderPlace&)=delete;
};
// The guest machine lock: FIFO by ticket and recursive per thread, with the
// std::recursive_mutex interface the scheduler code already uses.
//
// std::recursive_mutex is not fair. The game thread releases the machine
// around imports and every 500 us, then asks for it again at once and usually
// wins; the guest audio mixer thread waited behind it, filled its DirectSound
// ring too late, and the mixer replayed stale data ("the sound loops"). The
// same starvation made the Windows build stutter until native32 got a ticket
// lock. Here every unlock hands the machine to the longest waiter.
class MachineMutex {
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t next_=0,serving_=0;
    std::thread::id owner_{};
    unsigned depth_=0;
public:
    void lock(){
        const auto me=std::this_thread::get_id();
        std::unique_lock<std::mutex> l(m_);
        if(owner_==me){++depth_;return;}
        const uint64_t ticket=next_++;
        cv_.wait(l,[&]{return serving_==ticket;});
        owner_=me;depth_=1;acquired=std::chrono::steady_clock::now();
        machineHolder.store(renderThread?GuestProfile::Render:GuestProfile::Other,std::memory_order_relaxed);
    }
    // When the current owner got the machine; only the owner reads it.
    std::chrono::steady_clock::time_point acquired{};
    bool try_lock(){
        const auto me=std::this_thread::get_id();
        std::lock_guard<std::mutex> l(m_);
        if(owner_==me){++depth_;return true;}
        if(next_!=serving_)return false;
        ++next_;owner_=me;depth_=1;acquired=std::chrono::steady_clock::now();
        machineHolder.store(renderThread?GuestProfile::Render:GuestProfile::Other,std::memory_order_relaxed);
        return true;
    }
    bool hasWaiters(){
        std::lock_guard<std::mutex> lock(m_);
        return next_>serving_+1; // the current owner already holds one ticket
    }
    void unlock(){
        {
            std::lock_guard<std::mutex> l(m_);
            if(--depth_)return;
            owner_={};++serving_;
            machineHolder.store(GuestProfile::Nobody,std::memory_order_relaxed);
            if(next_==serving_)return;          // nobody waiting
        }
        cv_.notify_all();
    }
};
MachineMutex machine;
thread_local std::unique_lock<MachineMutex>* machineLease=nullptr;
thread_local uint32_t guestThreadId=1;
std::vector<Import> imports;
std::map<std::string,uint32_t> modules;
thread_local uint32_t lastError=0;
uint32_t imageBase{}, imageSize{}, entry{};
uint32_t resourceRva{},resourceSize{};
// Set and read under the machine lock with no yield in between (lookup -> call):
// plain globals, not thread_local, which cost a TLS descriptor call per COM call.
uint32_t lastImport{};
std::string hostGameRoot;
std::string guestLanguage="Spanish";
bool guestWidescreen=true;
// Launcher "Wrong-way warning": off blanks the HUD's wrong-way element names in the mapped image (the
// hashed lookups then find nothing), which is what the Free Roam Mod's patcher does to Speed.exe so
// driving against the circuit direction is not flagged in free roam.
bool guestWrongWayWarning=true;
// Launcher "Reflections": sharper road, car and mirror reflection targets (reflections.c).
bool guestHdReflections=true;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
// Launcher "Garage" (nfs_garage.h): which customization freedoms the game gets, NFS_GARAGE_* bits.
uint32_t guestGarage=NFS_GARAGE_ALL;
#endif
// Launcher "Effect states": the effects' render, sampler and texture stage states applied by the host
// (effect_states.h) or by the game's own D3DX code.
bool guestEffectStates=true;
// The app's folder for data the host makes from the game's (GameActivity: files/generated), outside the game
// folder. Before boot.
std::string generatedDataDir;
// Launcher "Free roam" (free_roam.h; src/runtime/freeroam.c is the game side): the folder holding Beta 2.0's three
// track bundles, which the APK carries and GameActivity copies out (files/freeroam); empty: off. Before boot.
std::string freeRoamDir;
// Game files the host serves another file for, by the lowercased path gamePath resolves them to (the game names
// its files in any case, and free roam's do not exist in the game folder): every rim size's wheel pack for
// Cars/WHEELS/GEOMETRY.BIN, free roam's track bundles, texture packs and race streams. Filled before the guest
// boots, only read afterwards.
std::map<std::string,std::filesystem::path> servedFiles;
// Every rim size at boot (prepareRimSizes): whether the wheel pack is served, why, and where it came from.
struct RimSizesBoot{
    bool on=false,served=false;
    std::string reason="off";   // "on": the switch; "save": off, but a save wears a size only the pack has
    std::string cache="none";   // "hit": the pack made for these game files; "built" now; "failed"
    std::string error,save;
    unsigned parts=0,solids=0;
    uint64_t bytes=0;
    double ms=0;
};
RimSizesBoot rimSizesBoot;
void* ptr(uint32_t va, uint64_t bytes);
void applyWrongWayPatch(){
    struct Patch{uint32_t va;const char* expected;};
    static const Patch patches[]={{0x6c6958u,"HUD_FEATURE_WRONG_WAY"},{0x6c71e0u,"WrongWay_CountDown"},{0x6c71f4u,"WrongWayImage"},{0x6c7204u,"WrongWayText"}};
    unsigned applied=0;
    for(const auto& patch:patches){
        size_t length=std::strlen(patch.expected);
        if(!memory->readable(patch.va,uint64_t(length)+1))continue;
        if(std::memcmp(ptr(patch.va,length),patch.expected,length)!=0)continue;  // not the image these offsets belong to
        std::memset(ptr(patch.va,length),0,length);++applied;
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Wrong-way warning disabled: %u of 4 HUD names blanked",applied);
}
std::atomic<bool> stopRequested{false};
std::atomic<bool> guestPaused{false};
std::chrono::steady_clock::time_point bootDeadline;
std::string guestThreadFault;
bool timerTestActive=false;std::vector<uint32_t> timerTestCalls;
constexpr uint32_t timerTestAddress=0x7b000010u;
uint32_t read32(uint32_t va);
// Stall diagnostics: where each guest thread last handed the machine over and
// what it was waiting for, dumped when the game stops presenting frames.
struct GuestThreadTrace {
    std::atomic<uint32_t> func{0},handle{0},esp{0};std::atomic<const char*> wait{nullptr};
    std::array<std::atomic<uint32_t>,8> held{};  // critical sections this thread owns (outermost entries)
    void hold(uint32_t cs){for(auto& slot:held){uint32_t empty=0;if(slot.compare_exchange_strong(empty,cs))return;}}
    void release(uint32_t cs){for(auto& slot:held){uint32_t value=cs;if(slot.compare_exchange_strong(value,0))return;}}
};
std::mutex threadTracesMutex;
std::map<uint32_t,std::shared_ptr<GuestThreadTrace>> threadTraces;
thread_local std::shared_ptr<GuestThreadTrace> threadTrace;
GuestThreadTrace& currentTrace(){
    if(!threadTrace){threadTrace=std::make_shared<GuestThreadTrace>();std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces[guestThreadId]=threadTrace;}
    return *threadTrace;
}
void forgetThreadTrace(){std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces.erase(guestThreadId);threadTrace.reset();}
struct WaitScope {
    WaitScope(const char* what,uint32_t handle){auto& trace=currentTrace();trace.wait=what;trace.handle=handle;}
    ~WaitScope(){currentTrace().wait=nullptr;}
};
std::atomic<int64_t> lastPresentMs{0};
int64_t steadyMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void startStallWatch(){
    static std::once_flag started;
    std::call_once(started,[]{std::thread([]{
        int64_t reportedFor=0;
        while(!stopRequested.load()){
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int64_t last=lastPresentMs.load(),idle=steadyMs()-last;
            if(guestPaused.load()||idle<12000||reportedFor==last)continue;
            reportedFor=last;
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Guest stall: no frame for %lld s; guest threads:",static_cast<long long>(idle/1000));
            std::lock_guard<std::mutex> lock(threadTracesMutex);
            for(auto& [id,trace]:threadTraces){
                const char* wait=trace->wait.load();uint32_t handle=trace->handle.load();
                std::string detail,held,callers;
                // Read-only peeks at guest memory from this host thread: stable while the guest is stuck.
                if(wait&&!std::strcmp(wait,"EnterCriticalSection")&&memory&&memory->readable(handle,24))
                    detail=" (owner thread "+std::to_string(read32(handle+12))+", count "+std::to_string(read32(handle+8))+")";
                for(auto& slot:trace->held)if(uint32_t cs=slot.load())held+=" "+hex(cs);
                uint32_t esp=trace->esp.load();
                for(unsigned i=0,found=0;memory&&i<256&&found<10;++i){
                    if(!memory->readable(esp+i*4,4))break;
                    uint32_t value=read32(esp+i*4);
                    if(value>=0x401000u&&value<0x697000u&&recomp_lookup(value)==nullptr){callers+=" "+hex(value);++found;}
                }
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","  thread %u at %08x %s %08x%s; holds:%s; stack:%s",id,trace->func.load(),
                    wait?wait:"running",wait?handle:0u,detail.c_str(),held.empty()?" none":held.c_str(),callers.c_str());
            }
        }
    }).detach();});
}
void yieldGuestMachine(unsigned milliseconds=0,std::chrono::steady_clock::time_point deadline={}) {
    if(!machineLease||!machineLease->owns_lock())throw GuestStop("Guest scheduler ownership missing");
    {auto& trace=currentTrace();trace.func=g_cur_func;trace.esp=g_esp;}
    State saved;saved.save();machineLease->unlock();
    if(deadline!=std::chrono::steady_clock::time_point{})std::this_thread::sleep_until(deadline);
    else if(milliseconds)std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));else std::this_thread::yield();
    machineLease->lock();saved.load();
}
void checkGuestProgress() {
    while(guestPaused.load()&&!stopRequested.load())yieldGuestMachine(20);
    if(stopRequested.load())throw GuestStop("Guest execution cancelled");
    // Imports and lifted back edges can reach this hook many times per frame.
    // Keep cancellation immediate, but amortize full register saves and native
    // scheduler handovers over a short time slice rather than every call.
    // One time slice for whichever guest thread holds the machine.
    //
    // Slices count from when this thread acquired the machine (a shared
    // deadline left the incoming thread an expired slice, so it handed the
    // machine straight back). The render thread, the one presenting, gets
    // 3 ms and the others 500 us: every handover wakes the queued threads in
    // turn, and with 500 us slices the Underground 2 render thread spent 56%
    // of its wall time waiting to get the machine back (Redmi Note 8).
    static unsigned checkpoints=0;
    if((++checkpoints&63u)==0){
        auto now=std::chrono::steady_clock::now();
        if(now>bootDeadline)throw GuestStop("CRT bring-up time budget reached at "+hex(g_cur_func));
        auto slice=renderThread?std::chrono::microseconds(3000):std::chrono::microseconds(500);
        if(now-machine.acquired>=slice){if(machine.hasWaiters())yieldGuestMachine();}
    }
    if(!guestThreadFault.empty())throw GuestStop(guestThreadFault);
}
constexpr uint32_t importBase = 0x7f000000u;
constexpr uint32_t stackBase = 0x20000000u, stackSize = 16u << 20, tib = 0x21000000u;

void* ptr(uint32_t va, uint64_t bytes) { return memory->address(va, bytes); }
uint32_t read32(uint32_t va) { uint32_t v; std::memcpy(&v, ptr(va,4),4); return v; }
void write32(uint32_t va, uint32_t v) { std::memcpy(ptr(va,4),&v,4); }
uint32_t arg(uint32_t index) { return read32(g_esp + 4 + index*4); }
void ret(uint32_t value, uint32_t argc) { g_eax=value; g_esp+=4+argc*4; }
std::string guestString(uint32_t address) {
    if(!address) return {};
    std::string s;
    for(uint32_t i=0;i<32768;++i) {
        char c=*static_cast<char*>(ptr(address+i,1));
        if(!c) return s;
        s.push_back(c);
    }
    throw GuestStop("Unterminated guest string at "+hex(address));
}
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));}); return s;
}
uint32_t allocate(uint32_t bytes) {
    uint32_t result=heaps->alloc(GuestHeaps::process,8,bytes);
    if(!result) throw GuestStop("Guest allocator exhausted");
    return result;
}
void unsupported() {
    uint32_t index = (lastImport-importBase)/16;
    throw GuestStop("HLE pendiente: " + imports.at(index).name + " desde " + hex(g_cur_func));
}
void getVersion() {
    uint32_t out = arg(0), bytes=read32(out);
    if (bytes != 148 && bytes != 156) { ret(0,1); return; }
    std::memset(ptr(out+4,bytes-4),0,bytes-4);
    write32(out+4,5); write32(out+8,1); write32(out+12,2600); write32(out+16,2);
    if (bytes==156) { auto* p=static_cast<unsigned char*>(ptr(out+148,8)); p[6]=1; }
    ret(1,1);
}
// guest_modules.inc (game DLLs run as lifted code) needs callGuest, so it comes later.
uint32_t loadGuestModule(const std::string& name);
bool freeGuestModule(uint32_t handle);
uint32_t guestModuleHandle(const std::string& name);
bool guestModuleExport(uint32_t handle,const std::string& name,uint32_t& va);
bool isGuestModule(uint32_t handle);
void moduleHandle() {
    if(!arg(0)) { ret(imageBase,1); return; }
    auto name=lower(guestString(arg(0)));
    auto separator=name.find_last_of("/\\"); if(separator!=std::string::npos) name=name.substr(separator+1);
    if(name=="speed2.exe") { ret(imageBase,1); return; }
    if(name.find('.')==std::string::npos) name+=".dll";
    if(uint32_t loaded=guestModuleHandle(name)) { ret(loaded,1); return; }
    auto module=modules.find(name);
    static std::set<std::string> loggedLookups;
    if(loggedLookups.insert(name).second)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Module lookup %s",name.c_str());
    if(module==modules.end()) lastError=126;
    ret(module==modules.end()?0:module->second,1);
}
void heapCreate() {
    ret(heaps->create(arg(0),arg(1),arg(2)),3);
}
void heapAlloc() { ret(heaps->alloc(arg(0),arg(1),arg(2)),3); }
void heapFree() { ret(heaps->free(arg(0),arg(2)),3); }
void heapRealloc() { ret(heaps->realloc(arg(0),arg(1),arg(2),arg(3)),4); }
void heapSize() { ret(heaps->size(arg(0),arg(2)),3); }
void heapDestroy() { ret(heaps->destroy(arg(0)),1); }
void getProcessHeap() { ret(GuestHeaps::process,0); }
void getProcAddress();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
uint32_t d3dToken{};
void direct3DCreate(){ret(createGuestD3D9(arg(0)),1);}
void freeD3DGuest(uint32_t address){heaps->free(GuestHeaps::process,address);}
void serviceGuestSound();
void saveChangedOptions();  // after callGuest
uint64_t steadyNs(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
// The system call a thread is blocked in (arm64 numbers), for the common waits.
const char* syscallName(long number){
    switch(number){
        case 98:return "futex";case 29:return "ioctl";case 73:return "ppoll";case 22:return "epoll_pwait";case 63:return "read";
        case 64:return "write";case 101:return "nanosleep";case 115:return "clock_nanosleep";case 124:return "sched_yield";
        case 72:return "pselect6";case 67:return "pread64";case 56:return "openat";case 57:return "close";case 222:return "mmap";
        default:return nullptr;
    }
}
// What a thread is doing right now, from /proc (readable for threads of this process): its state (R
// running, S sleeping, D uninterruptible), the cpu it last ran on with that cpu's clock, and when it is
// blocked the system call and the kernel function it sleeps in (futex: a lock or condition variable in
// user space; ioctl: a driver, the GPU's or the display's; ...).
std::string threadState(int32_t tid){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/stat",int(tid));
    std::ifstream file(path);std::string line;if(!std::getline(file,line))return "gone";
    size_t close=line.rfind(')');if(close==std::string::npos||close+2>=line.size())return "?";
    std::istringstream fields(line.substr(close+2));std::string state,field;fields>>state;
    int cpu=-1;for(int index=4;index<=39&&fields>>field;++index)if(index==39)cpu=std::atoi(field.c_str());
    std::string out=state;
    if(cpu>=0){
        std::snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",cpu);
        unsigned khz=0;{std::ifstream freq(path);freq>>khz;}
        char text[40];std::snprintf(text,sizeof text," cpu%d %u MHz",cpu,khz/1000);out+=text;
    }
    if(state!="R"){
        std::snprintf(path,sizeof path,"/proc/self/task/%d/syscall",int(tid));
        std::ifstream sys(path);long number=-1;if(sys>>number&&number>=0){
            const char* name=syscallName(number);char text[32];
            if(name)std::snprintf(text,sizeof text," in %s",name);else std::snprintf(text,sizeof text," in syscall %ld",number);out+=text;}
        std::snprintf(path,sizeof path,"/proc/self/task/%d/wchan",int(tid));
        std::ifstream wchan(path);std::string where;if(std::getline(wchan,where)&&!where.empty()&&where!="0")out+=" ("+where+")";
    }
    return out;
}
// /proc/self/task/<tid>/schedstat: nanoseconds on a cpu, then nanoseconds runnable but waiting for one.
bool readSchedstat(int32_t tid,uint64_t& run,uint64_t& wait){
    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/schedstat",int(tid));
    std::ifstream file(path);unsigned long long onCpu=0,waiting=0;if(!(file>>onCpu>>waiting))return false;
    run=onCpu;wait=waiting;return true;
}
// The tracked threads' schedstat when a long call crossed the threshold (sampler thread only): the report
// then says how long each of them ran, and waited for a cpu, until the call ended. A thread that ran the
// whole time was the bottleneck; one that waited was starved of a core.
struct SchedMark{std::string name;int32_t tid;uint64_t run,wait;};
std::vector<SchedMark> schedMarks;
uint64_t schedMarkNs=0;
// Taken by the sampler once a D3D9 call has lasted 20 ms: DXVK's submission queue and the threads that
// could be holding the call up.
std::string longCallSnapshot(){
    std::string out;
#ifdef __ANDROID__
    uint64_t queue[9]{};dxvkAndroidQueueState(queue);
    uint64_t timings[13]{};dxvkAndroidQueueTimings(timings,13);
    char text[240];std::snprintf(text,sizeof text,"queue %llu to submit, %llu on the GPU%s%s%s",static_cast<unsigned long long>(queue[0]),
        static_cast<unsigned long long>(queue[1]),queue[2]?", in vkQueuePresentKHR":"",queue[3]?", waiting on a GPU fence":"",
        queue[4]?", command stream waiting for queue room":"");
    out=text;
    if(timings[12]){std::snprintf(text,sizeof text,", %llu waiting for the cleanup thread",static_cast<unsigned long long>(timings[12]));out+=text;}
#endif
    schedMarks.clear();schedMarkNs=steadyNs();
    for(const auto& thread:host_perf::trackedThreads()){
        out+="; "+thread.first+" "+threadState(thread.second);
        uint64_t run=0,wait=0;if(readSchedstat(thread.second,run,wait))schedMarks.push_back({thread.first,thread.second,run,wait});
    }
    return out;
}
// "; over the next 64 ms, ms on a cpu (waiting for one): render 1, dxvk-cs 0, ..., dxvk-queue 61 (2)"
std::string longCallRunTimes(){
    if(schedMarks.empty())return "";
    char text[96];std::snprintf(text,sizeof text,"; over the next %.0f ms, ms on a cpu (waiting for one):",(steadyNs()-schedMarkNs)/1e6);
    std::string out=text;bool first=true;
    for(const auto& mark:schedMarks){
        uint64_t run=0,wait=0;if(!readSchedstat(mark.tid,run,wait))continue;
        double ran=run>=mark.run?(run-mark.run)/1e6:0,waited=wait>=mark.wait?(wait-mark.wait)/1e6:0;
        if(waited>=0.5)std::snprintf(text,sizeof text,"%s %s %.0f (%.0f)",first?"":",",mark.name.c_str(),ran,waited);
        else std::snprintf(text,sizeof text,"%s %s %.0f",first?"":",",mark.name.c_str(),ran);
        out+=text;first=false;
    }
    return out;
}
void reportLongCall(const CallWatch::Call& call){
    static unsigned logged=0;
    if(++logged>200)return;
    const char* name=d3d9MethodName(call.token);
    char method[16];if(!name){uint32_t index=((call.token-0x7e000000u)/16)&4095;std::snprintf(method,sizeof method,"%u/%u",index/256,index%256);name=method;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Long D3D9 call: %s %.0f ms from sub_%08X (%s%s)",name,call.ns/1e6,call.func,call.detail.c_str(),longCallRunTimes().c_str());
}
// The profiler's sampler: every kProfileIntervalUs, who holds the guest machine and where (guest_profiler.h).
// A plain read of g_cur_func from this thread: an aligned 32-bit word, and a stale value only shifts one sample.
void startGuestProfiler(){
    static std::once_flag started;
    std::call_once(started,[]{std::thread([]{
        setpriority(PRIO_PROCESS,0,10);   // never compete with the game for a core
        callWatch.setSnapshot(longCallSnapshot);
        uint64_t placementChecked=0;
        while(!stopRequested.load(std::memory_order_relaxed)){
            std::this_thread::sleep_for(std::chrono::microseconds(kProfileIntervalUs));
            if(guestPaused.load(std::memory_order_relaxed)){callWatch.reset();continue;}
            auto holder=GuestProfile::Holder(machineHolder.load(std::memory_order_relaxed));
            uint32_t func=holder==GuestProfile::Nobody?0u:__atomic_load_n(&g_cur_func,__ATOMIC_RELAXED);
            auto place=holder==GuestProfile::Render?GuestProfile::Place(renderPlace.load(std::memory_order_relaxed)):GuestProfile::Guest;
            uint64_t now=steadyNs();
            guestProfile.add(now,holder,func,place);
            CallWatch::Call done;
            if(callWatch.sample(now,func,&done))reportLongCall(done);
            // Once a second while playing (not while paused: Android then holds the app to other cores on
            // purpose), put back any thread placement that was replaced (host_perf.h).
            if(now-placementChecked>=1000000000ull){placementChecked=now;host_perf::maintainPlacement();}
        }
    }).detach();});
}
// A frame slower than 80 ms that is not a pause: what the profiler saw during it, so a report says which
// functions (or D3D9 calls, or waiting) filled it. At most one line every 2 s and 300 per session.
void explainSlowFrame(std::chrono::steady_clock::time_point now){
    static uint64_t lastPresent=0,lastLog=0;static unsigned logged=0;
    uint64_t at=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    uint64_t previous=lastPresent;lastPresent=at;
    if(!previous||guestPaused.load(std::memory_order_relaxed))return;
    uint64_t frame=at-previous;
    if(frame<80000000ull||frame>2000000000ull||logged>=300||at-lastLog<2000000000ull)return;
    lastLog=at;++logged;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Slow frame %.0f ms: %s",frame/1e6,guestProfile.explain(previous,at).c_str());
}
// The frame cap (build 15): before each Present the render thread sleeps to the frame's deadline with the machine
// handed over (guest audio and streaming threads run meanwhile), so the frames reach DXVK a period apart. Build 14
// slept after the Present instead, which spaced the starts of the frames' work: each Present then came one frame's
// work after the deadline, and that work varies from frame to frame, so the frames reached the screen unevenly. A
// late frame presents at once, after a turn for any guest thread waiting for the machine; missed deadlines are not
// made up (frame_pacer.h). Returns the time it waited, which is no part of the frame's work.
std::chrono::nanoseconds paceFrame(){
    static FramePacer pacer;
    if(!renderThread){renderThread=true;machineHolder.store(GuestProfile::Render,std::memory_order_relaxed);}
    auto now=std::chrono::steady_clock::now();
    auto deadline=pacer.deadline(now,frameLimit.load(std::memory_order_relaxed));
    if(deadline>now)yieldGuestMachine(0,deadline);
    else if(machine.hasWaiters())yieldGuestMachine();
    else return {};
    return std::chrono::steady_clock::now()-now;
}
void d3dDispatch(){
    // The arguments are read in place on the guest stack (one bounds check)
    // instead of copying 128 bytes for each D3D9 call.
    const uint32_t* args=static_cast<const uint32_t*>(ptr(g_esp+4,128));uint32_t count=0;uint32_t token=d3dToken;
    static uint64_t bridgeCalls=0;static std::chrono::nanoseconds bridgeTime{},presentTime{};
    const bool present=token==0x7e002110u;
    // The waits since the last Present that worked, and when it returned: the frame's work lies between.
    static std::chrono::nanoseconds paced{};static std::chrono::steady_clock::time_point lastPresented{};
    if(present)paced+=paceFrame();
    // Time one call in 64 (and every Present) so the statistic does not cost
    // two clock reads on each of the ~40k calls in a race frame.
    bool timed=(++bridgeCalls&63u)==0||present;
    std::chrono::steady_clock::time_point bridgeStart;if(timed)bridgeStart=std::chrono::steady_clock::now();uint32_t result;
    {RenderPlace place(GuestProfile::D3D9);
     callWatch.begin(token);
     try{result=dispatchGuestD3D9(token,args,&count);}catch(...){
        callWatch.end();
        NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU1","D3D9 guest caller=%08x stack=%08x token=%08x self=%08x",g_cur_func,g_esp,token,args[0]);throw;}
     callWatch.end();}
    if(timed){auto done=std::chrono::steady_clock::now();auto spent=done-bridgeStart;bridgeTime+=token==0x7e002110u?spent:spent*64;presentTime+=token==0x7e002110u?spent:std::chrono::nanoseconds{};
        // Per-method time: Present in full, every other method estimated from the one call in 64 timed.
        d3d9AddMethodTime(token,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(token==0x7e002110u?spent:spent*64).count()));
        if(present&&result==0){
            explainSlowFrame(done);
            frameTelemetry.present(done,std::chrono::duration_cast<std::chrono::nanoseconds>(spent),frameLimit.load(std::memory_order_relaxed));
            // The thread that presents is the game's busy thread: place it once, then feed the frame's CPU work
            // (since the previous Present returned, less the pacing waits) to the performance hint session.
            host_perf::adoptRenderThread(static_cast<host_perf::Placement>(cpuPlacement.load()),frameLimit.load(std::memory_order_relaxed));
            bool interrupted=frameInterrupted.exchange(false,std::memory_order_relaxed);
            if(lastPresented.time_since_epoch().count()&&!interrupted)
                host_perf::reportFrame(std::chrono::duration_cast<std::chrono::nanoseconds>(done-lastPresented)-paced);
            lastPresented=done;paced={};
        }}
    if(present){nfs_widescreen_apply_frame_rate();lastPresentMs=steadyMs();startStallWatch();startGuestProfiler();saveChangedOptions();}
    if(present&&result==0){
        static unsigned frames=0;static auto since=std::chrono::steady_clock::now();
        if(++frames==120){serviceGuestSound();auto now=std::chrono::steady_clock::now();double seconds=std::chrono::duration<double>(now-since).count();
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest D3D9 presentation %.1f FPS over %.2f seconds; bridge ~%.2f ms/frame (present %.2f), %.0f calls/frame",
                frames/seconds,seconds,double(bridgeTime.count())/1e6/frames,double(presentTime.count())/1e6/frames,double(bridgeCalls)/frames);
            frames=0;since=now;bridgeTime=presentTime=std::chrono::nanoseconds{};bridgeCalls=0;}
    }ret(result,count);}
// The launcher's Garage switches as the session log names them.
void logGarage(){
    std::string on,off;
    for(unsigned i=0;i<NFS_GARAGE_FEATURE_COUNT;++i){
        std::string& list=(guestGarage>>i)&1u?on:off;
        if(!list.empty())list+=", ";
        list+=nfs_garage_feature_name(i);
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Garage: on: %s; off: %s; unlock-everything byte %u",on.empty()?"none":on.c_str(),
        off.empty()?"none":off.c_str(),unsigned(*static_cast<const uint8_t*>(ptr(NFS_GARAGE_UNLOCK_FLAG,1))));
}
// Garage and effect state counts for the session JSON (guestTelemetryJson).
std::string garageJson(){
    std::string json="{\"features\":"+std::to_string(nfs_garage_features())+",\"switches\":"+std::to_string(NFS_GARAGE_FEATURE_COUNT)+",\"changes\":{";
    for(unsigned i=0;i<NFS_GARAGE_FEATURE_COUNT;++i)
        json+=std::string(i?",":"")+"\""+nfs_garage_feature_key(i)+"\":"+std::to_string(nfs_garage_changes(i));
    json+="},\"unknownSites\":"+std::to_string(nfs_branch_unknown_sites());
    // The game's own unlock-everything byte as the session ends (no guest memory before the first boot).
    if(memory)json+=",\"unlockByte\":"+std::to_string(unsigned(*static_cast<const uint8_t*>(ptr(NFS_GARAGE_UNLOCK_FLAG,1))));
    json+="}";
    return json;
}
std::string effectStatesJson(){
    uint64_t kept=0,sent=0;d3d9EffectStateCounts(kept,sent);
    return "{\"host\":"+std::string(guestEffectStates?"true":"false")+
        ",\"applyCalls\":"+std::to_string(nfs_fastpath_calls(effect_states::kApply))+
        ",\"applyHandled\":"+std::to_string(nfs_fastpath_handled(effect_states::kApply))+
        ",\"evaluateCalls\":"+std::to_string(nfs_fastpath_calls(effect_states::kEvaluate))+
        ",\"evaluateHandled\":"+std::to_string(nfs_fastpath_handled(effect_states::kEvaluate))+
        ",\"kept\":"+std::to_string(kept)+",\"sent\":"+std::to_string(sent)+"}";
}
// Effect states (effect_states.h), as recomp_fastpath handlers: sub_005C97CB and sub_005CDCA7 come here
// first, with the guest registers in the globals and [esp] the dummy return address. A state this cannot
// take (another kind, another device, a state block being recorded) returns 0 and the game's code runs.
int applyEffectStateFast(){
    const uint32_t esp=g_esp,effect=g_ecx;
    auto call=effect_states::applyCall([](uint32_t va){return read32(va);},read32(esp+4),read32(esp+8));
    if(call.method==effect_states::Method::None)return 0;
    uint32_t hresult;
    {RenderPlace place(GuestProfile::D3D9);
     if(!d3d9EffectState(read32(effect+8),static_cast<unsigned>(call.method),call.a,call.b,call.c,&hresult))return 0;}
    g_eax=effect_states::applyResult(hresult,read32(effect+0x84));
    g_esp=esp+12;   // the return address and the two arguments: ret 8
    return 1;
}
int evaluateEffectStateFast(){
    const uint32_t esp=g_esp;
    auto read8=[](uint32_t va){uint8_t v;std::memcpy(&v,ptr(va,1),1);return v;};
    if(!effect_states::evaluateIsNoop([](uint32_t va){return read32(va);},read8,read32(esp+4)))return 0;
    g_eax=0;        // S_OK, as [ebp-4] holds on that exit
    g_esp=esp+8;    // the return address and the one argument: ret 4
    return 1;
}
#endif
// threadStart: a thread procedure may be stdcall or cdecl; Windows exits the thread right after either.
uint32_t callGuest(uint32_t va,const uint32_t* args,size_t count,uint32_t cleanup,bool threadStart);
// Braced argument lists stay on the host stack: callbacks run many times a
// frame and a std::vector per call cost a malloc/free pair.
inline uint32_t callGuest(uint32_t va,std::initializer_list<uint32_t> args,uint32_t cleanup=0,bool threadStart=false){return callGuest(va,args.begin(),args.size(),cleanup,threadStart);}
inline uint32_t callGuest(uint32_t va,const std::vector<uint32_t>& args,uint32_t cleanup=0,bool threadStart=false){return callGuest(va,args.data(),args.size(),cleanup,threadStart);}
// The game's own malloc (cdecl, 0x67121A) for nodes the Garage adds to the game's lists, which the game later
// frees as its own. Called from a hook inside lifted code: callGuest keeps the guest registers as they were.
// No exception crosses garage.c (C): a failure is a null pointer, and the Garage then adds nothing.
uint32_t gameMalloc(uint32_t bytes){
    try{return callGuest(NFS_GARAGE_GAME_MALLOC,{bytes},4);}
    catch(const std::exception& e){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Garage: the game's malloc failed (%s)",e.what());return 0;}
}
// Underground keeps its options (graphics detail, audio, controls) in the
// profile's .opt file and writes it only when the game is saved, so options
// changed in the menu were lost when the app was closed, which on Android is
// how players leave. Speed.exe serializes the live options at 0x7588C4
// (0x2E5 bytes) in 0x41E200; once they change while a profile is loaded
// (name at 0x736230) and stay unchanged for 2 s, run the game's own profile
// config save (0x41C7A0: .opt, then .cfg) as its save would.
// The Display Settings of the profile as the game last wrote them (its .cfg, KEY: value lines). Logged in
// full when a profile is first seen and as a list of changes after each save, so a session's frame rate can
// be matched with what was switched in the game's menu.
void logDisplaySettings(const std::string& profile){
    static const char* const keys[]={"PERFORMANCE","ENVMAP","WORLDENVMAP","SHADOW","HEADLIGHTS","CARLIGHTING","DAMAGE","CROWD",
        "ROADREFLECTION","FOG","MOTIONBLUR","LIGHTSTREAKS","LIGHTGLOW","ANIMATEDTEXTURE","PARTICLESYSTEMS","DEPTHOFFIELD","WorldLOD","CarLOD"};
    static std::map<std::string,std::map<std::string,std::string>> known;
    std::string path=hostGameRoot+"/"+profile;std::replace(path.begin(),path.end(),'\\','/');
    std::ifstream file(path);if(!file)return;
    std::map<std::string,std::string> values;std::string line;
    auto trim=[](std::string text){size_t a=text.find_first_not_of(" \t\r"),b=text.find_last_not_of(" \t\r");return a==std::string::npos?std::string():text.substr(a,b-a+1);};
    while(std::getline(file,line)){
        size_t colon=line.find(':');if(colon==std::string::npos)continue;
        std::string key=trim(line.substr(0,colon));
        if(std::find_if(std::begin(keys),std::end(keys),[&](const char* k){return key==k;})!=std::end(keys))values[key]=trim(line.substr(colon+1));
    }
    if(values.empty())return;
    std::string text;
    auto previous=known.find(profile);
    for(const char* key:keys){
        auto value=values.find(key);if(value==values.end())continue;
        if(previous==known.end())text+=std::string(text.empty()?"":" ")+key+"="+value->second;
        else{auto before=previous->second.find(key);
            if(before==previous->second.end()||before->second!=value->second)
                text+=std::string(text.empty()?"":", ")+key+" "+(before==previous->second.end()?"?":before->second)+" -> "+value->second;}
    }
    if(previous==known.end())NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Display settings of %s: %s",profile.c_str(),text.c_str());
    else if(!text.empty())NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Display settings changed in the game: %s",text.c_str());
    known[profile]=values;
}
void saveChangedOptions(){
    constexpr uint32_t options=0x7588C4u,optionsBytes=0x2E5u,profileName=0x736230u;
    static int64_t nextCheck=0,changedAt=0;static std::string profile;static std::vector<uint8_t> saved,pending;
    int64_t now=steadyMs();if(now<nextCheck)return;nextCheck=now+1000;
    if(!memory->readable(options,optionsBytes)||!memory->readable(profileName,32))return;
    std::string name=guestString(profileName);
    auto current=static_cast<const uint8_t*>(ptr(options,optionsBytes));
    if(name.empty()){profile.clear();saved.clear();return;}
    if(name!=profile||saved.empty()){profile=name;saved.assign(current,current+optionsBytes);changedAt=0;logDisplaySettings(profile);return;}
    if(std::equal(saved.begin(),saved.end(),current)){changedAt=0;return;}
    if(!changedAt||!std::equal(pending.begin(),pending.end(),current)){pending.assign(current,current+optionsBytes);changedAt=now;return;}
    if(now-changedAt<2000)return;
    callGuest(0x41C7A0u,{});
    saved=pending;changedAt=0;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Options changed in the menu: saved to the %s profile",profile.c_str());
    logDisplaySettings(profile);
}
#include "guest_threads.inc"
#include "guest_files.inc"
#include "guest_kernel.inc"
#include "guest_input.inc"
#include "guest_audio.inc"
#include "guest_network.inc"
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
// --- Every rim size: the wheel pack (rim_sizes.h makes it, nfs_rims.h is the game side) -------------------------
bool readWholeFile(const std::filesystem::path& path,std::vector<uint8_t>& out,uint64_t limit=uint64_t(1)<<30){
    out.clear();
    int descriptor=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(descriptor<0)return false;
    struct stat status{};
    if(fstat(descriptor,&status)||status.st_size<0||uint64_t(status.st_size)>limit){::close(descriptor);return false;}
    out.resize(size_t(status.st_size));size_t done=0;
    while(done<out.size()){ssize_t count=::read(descriptor,out.data()+done,out.size()-done);if(count<0&&errno==EINTR)continue;if(count<=0)break;done+=size_t(count);}
    ::close(descriptor);
    if(done!=out.size()){out.clear();return false;}
    return true;
}
// Written beside, synced, then renamed over: a reader sees the old file or the whole new one.
bool writeFileAtomically(const std::filesystem::path& path,const void* data,size_t size){
    auto temporary=path;temporary+=".tmp";
    int descriptor=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(descriptor<0)return false;
    const uint8_t* bytes=static_cast<const uint8_t*>(data);size_t done=0;
    while(done<size){ssize_t count=::write(descriptor,bytes+done,size-done);if(count<0&&errno==EINTR)continue;if(count<=0)break;done+=size_t(count);}
    bool ok=done==size&&::fsync(descriptor)==0;
    ok=::close(descriptor)==0&&ok;
    if(!ok||::rename(temporary.c_str(),path.c_str())){::unlink(temporary.c_str());return false;}
    return true;
}
// Size and modification time: what tells this game file from another.
std::string fileStamp(const std::filesystem::path& path){
    struct stat status{};if(::stat(path.c_str(),&status))return "missing";
    return std::to_string(uint64_t(status.st_size))+" "+std::to_string(int64_t(status.st_mtim.tv_sec))+"."+std::to_string(long(status.st_mtim.tv_nsec));
}
// The saves the game wrote (the profiles folder "NFS Underground", see shellFolderPath: the game asks for the
// all-users application data folder, which the host leaves unset, so it writes beside its own files).
std::vector<std::filesystem::path> saveFiles(){
    std::vector<std::filesystem::path> files;std::error_code error;
    for(auto& entry:std::filesystem::directory_iterator(hostGameRoot,error)){
        if(lower(entry.path().filename().string())!="nfs underground"||!entry.is_directory(error))continue;
        for(auto& file:std::filesystem::directory_iterator(entry.path(),error))if(file.is_regular_file(error))files.push_back(file.path());
    }
    return files;
}
// Before the guest boots: with every rim size on, or off while a save wears a size only the pack has (the
// game would find no part for it and draw the car with an empty rim slot, which 0x56A367 does not check), the
// pack is served in place of Cars/WHEELS/GEOMETRY.BIN: from the app's own folder when it was made from these
// game files, made now otherwise (about 80 ms of work and 42 MB written, once).
void prepareRimSizes(){
    rimSizesBoot=RimSizesBoot{};
    rimSizesBoot.on=(guestGarage&NFS_GARAGE_ALL_RIM_SIZES)!=0;
    nfs_garage_set_rim_sizes_served(0);
    const auto start=std::chrono::steady_clock::now();
    try{
        const auto wheelsPath=gamePath("C:\\nfsu1\\Cars\\WHEELS\\GEOMETRY.BIN"),globalPath=gamePath("C:\\nfsu1\\Global\\GlobalB.lzc");
        std::vector<uint8_t> wheels;
        if(rimSizesBoot.on)rimSizesBoot.reason="on";
        else{
            auto saves=saveFiles();
            if(saves.empty()||!readWholeFile(wheelsPath,wheels))return;
            const auto planned=rim_sizes::plannedPartHashes(wheels);
            std::vector<uint8_t> bytes;
            for(const auto& save:saves)
                if(readWholeFile(save,bytes,64u<<20)&&rim_sizes::containsAny(bytes,planned)){rimSizesBoot.save=save.filename().string();break;}
            if(rimSizesBoot.save.empty())return;
            rimSizesBoot.reason="save";
        }
        if(generatedDataDir.empty())throw std::runtime_error("no folder for generated data");
        const std::filesystem::path directory=std::filesystem::path(generatedDataDir)/"rim_sizes",pack=directory/"GEOMETRY.BIN",keyPath=directory/"key.txt";
        std::error_code error;std::filesystem::create_directories(directory,error);
        if(error)throw std::runtime_error("cannot make "+directory.string()+": "+error.message());
        // The key names the generator and the game files the pack was made from; then "pack <bytes> <parts> <solids>".
        const std::string key="rim sizes "+std::to_string(rim_sizes::kGeneratorVersion)+"\nwheels "+fileStamp(wheelsPath)+"\nglobal "+fileStamp(globalPath)+"\n";
        std::vector<uint8_t> stored;
        if(readWholeFile(keyPath,stored,4096)){
            std::string text(stored.begin(),stored.end());unsigned long long bytes=0;unsigned parts=0,solids=0;struct stat status{};
            if(text.rfind(key,0)==0&&std::sscanf(text.c_str()+key.size(),"pack %llu %u %u",&bytes,&parts,&solids)==3&&
               !::stat(pack.c_str(),&status)&&uint64_t(status.st_size)==bytes){
                rimSizesBoot.cache="hit";rimSizesBoot.bytes=bytes;rimSizesBoot.parts=parts;rimSizesBoot.solids=solids;
            }
        }
        if(rimSizesBoot.cache!="hit"){
            rimSizesBoot.cache="failed";
            if(wheels.empty()&&!readWholeFile(wheelsPath,wheels))throw std::runtime_error("Cars/WHEELS/GEOMETRY.BIN unreadable");
            std::vector<uint8_t> global,globalb;
            if(!readWholeFile(globalPath,global))throw std::runtime_error("Global/GlobalB.lzc unreadable");
            if(!rim_sizes::jdlzDecompress(global,globalb))globalb=std::move(global);   // a mod's may be stored unpacked
            auto result=rim_sizes::generate(wheels,globalb);
            if(!result.error.empty())throw std::runtime_error(result.error);
            ::unlink(keyPath.c_str());   // no key names a pack being replaced
            if(!writeFileAtomically(pack,result.file.data(),result.file.size()))throw std::runtime_error("cannot write "+pack.string()+": "+std::strerror(errno));
            const std::string text=key+"pack "+std::to_string(result.file.size())+" "+std::to_string(result.parts)+" "+std::to_string(result.solids)+"\n";
            if(!writeFileAtomically(keyPath,text.data(),text.size()))throw std::runtime_error("cannot write "+keyPath.string());
            rimSizesBoot.cache="built";rimSizesBoot.bytes=result.file.size();rimSizesBoot.parts=result.parts;rimSizesBoot.solids=result.solids;
        }
        servedFiles[servedKey(wheelsPath)]=pack;
        rimSizesBoot.served=true;
        nfs_garage_set_rim_sizes_served(1);
    }catch(const std::exception& e){rimSizesBoot.error=e.what();}
    rimSizesBoot.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(rimSizesBoot.served)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Rim sizes: the wheel pack (%u parts, %u solids added, %llu bytes) served for Cars/WHEELS/GEOMETRY.BIN, %s in %.0f ms%s%s",
            rimSizesBoot.parts,rimSizesBoot.solids,static_cast<unsigned long long>(rimSizesBoot.bytes),rimSizesBoot.cache=="hit"?"made earlier, checked":"made",
            rimSizesBoot.ms,rimSizesBoot.reason=="save"?"; every rim size is off, kept for the save ":"",rimSizesBoot.save.c_str());
    else if(rimSizesBoot.on||!rimSizesBoot.error.empty())
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Rim sizes: the wheel pack is not served (%s); the rim browser offers the sizes the game has",
            rimSizesBoot.error.empty()?"unknown":rimSizesBoot.error.c_str());
}
// --- Free roam: Beta 2.0's tracks served from the player's own game files (free_roam.h; freeroam.c the game side) --
struct FreeRoamTrackBoot{
    unsigned number=0;
    bool offered=false;
    std::string cache="none";   // "hit": made earlier from these files, checked; "built" now; "failed"
    std::string error,mapError; // error: not offered; mapError: offered without its map (the minimap stays off)
    uint32_t mapSize=0;
    uint64_t quads=0,tiles=0;
    float mapX=0,mapY=0,mapWidth=0;
    double ms=0;
};
// The select screen's pictures: the dynamic texture pack served with the offered tracks' (free_roam.h makePicturePack).
struct FreeRoamPicturesBoot{std::string cache="none",error;unsigned tracks=0,textures=0;uint64_t bytes=0;double ms=0;std::vector<std::string> skipped;};
struct FreeRoamBoot{bool on=false;std::string error;std::vector<FreeRoamTrackBoot> tracks;FreeRoamPicturesBoot pictures;double ms=0;};
FreeRoamBoot freeRoamBoot;
// A game function called from the free roam fast paths (registers as freeroam.c set them). No exception crosses
// freeroam.c (C): a failure is 0, and the game's own code then runs.
int freeRoamCall(uint32_t va,const uint32_t* args,unsigned count,uint32_t cleanup,uint32_t* result){
    try{const uint32_t value=callGuest(va,args,count,cleanup,false);if(result)*result=value;return 1;}
    catch(const std::exception& e){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Free roam: the game's function %08x failed (%s)",va,e.what());return 0;}
}
// One free roam track to serve: where it comes from, where it goes, and the key that says what it was made from.
struct FreeRoamJob{
    const free_roam::Track* track=nullptr;
    FreeRoamTrackBoot boot;
    std::filesystem::path bundle,base,tex,stream,outTrack,outTex,keyPath;
    std::vector<std::pair<free_roam::TrackRecord,std::filesystem::path>> maps;   // the stock races' maps under its map
    std::string key;
    bool made=false,hasMap=false;
};
// Makes a track's files (in parallel with the others: none of this touches the guest or a shared table).
void buildFreeRoamTrack(FreeRoamJob& job){
    const auto begin=std::chrono::steady_clock::now();
    try{
        job.boot.cache="failed";
        std::vector<uint8_t> bundle,raw,base,tex;
        if(!readWholeFile(job.bundle,bundle))throw std::runtime_error(job.bundle.filename().string()+" unreadable");
        {std::vector<uint8_t> unpacked;if(rim_sizes::jdlzDecompress(bundle,unpacked))bundle=std::move(unpacked);}   // Beta 2.0 stores them raw
        if(!readWholeFile(job.base,raw))throw std::runtime_error("Tracks/"+job.base.filename().string()+" unreadable");
        if(!rim_sizes::jdlzDecompress(raw,base))base=std::move(raw);
        if(!readWholeFile(job.tex,tex))tex.clear();   // the map needs it; the track does not
        std::vector<free_roam::MapTile> tiles;
        for(const auto& source:job.maps){
            std::vector<uint8_t> file;free_roam::TexturePack pack;free_roam::MapTile tile;
            if(!readWholeFile(source.second,file)||!free_roam::parsePack(file,pack).empty())continue;
            if(!free_roam::mapImage(pack,free_roam::mapName(source.first.number),tile.image))continue;
            tile.place={source.first.mapX,source.first.mapY,source.first.mapWidth};
            tiles.push_back(std::move(tile));
        }
        free_roam::MadeTrack made=free_roam::makeTrack(*job.track,std::move(bundle),base,tex,tiles);
        if(!made.error.empty())throw std::runtime_error(made.error);
        ::unlink(job.keyPath.c_str());   // no key names files being replaced
        if(!writeFileAtomically(job.outTrack,made.track.data(),made.track.size()))throw std::runtime_error("cannot write "+job.outTrack.string());
        if(!made.tex.empty()&&!writeFileAtomically(job.outTex,made.tex.data(),made.tex.size()))throw std::runtime_error("cannot write "+job.outTex.string());
        char line[256];
        std::snprintf(line,sizeof line,"made %llu %llu %u %a %a %a %llu %llu\n",static_cast<unsigned long long>(made.track.size()),
            static_cast<unsigned long long>(made.tex.size()),made.mapSize,double(made.place.offX),double(made.place.offY),double(made.place.width),
            static_cast<unsigned long long>(made.quads),static_cast<unsigned long long>(made.tiles));
        std::string text=job.key+line;
        if(!made.mapError.empty())text+="no map: "+made.mapError+"\n";
        if(!writeFileAtomically(job.keyPath,text.data(),text.size()))throw std::runtime_error("cannot write "+job.keyPath.string());
        job.made=true;job.hasMap=!made.tex.empty();
        job.boot.cache="built";job.boot.mapError=made.mapError;job.boot.mapSize=job.hasMap?made.mapSize:0;
        job.boot.quads=made.quads;job.boot.tiles=made.tiles;
        if(job.hasMap){job.boot.mapX=made.place.offX;job.boot.mapY=made.place.offY;job.boot.mapWidth=made.place.width;}
    }catch(const std::exception& e){job.boot.error=e.what();}
    job.boot.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
}
// The offered tracks' select screen pictures: the installed Global/DYNTEX.BIN with their backgrounds and routes
// added (free_roam.h makePicturePack: each route drawn from the served bundle's roads at its background's
// resolution), from the app's folder when made from these files, made now otherwise (a few hundred ms, once).
// Without it their SELECT LOCATION picture stays black; nothing else changes.
void prepareFreeRoamPictures(const std::filesystem::path& directory,const std::vector<FreeRoamJob>& jobs){
    auto& boot=freeRoamBoot.pictures;
    const auto start=std::chrono::steady_clock::now();
    try{
        const auto dyntexPath=gamePath("C:\\nfsu1\\Global\\DYNTEX.BIN");
        const std::filesystem::path out=directory/"DYNTEX.BIN",keyPath=directory/"pictures.txt";
        std::ostringstream key;
        key<<"free roam "<<free_roam::kGeneratorVersion<<" pictures\ndyntex "<<fileStamp(dyntexPath)<<"\n";
        unsigned pictures=0;
        for(const auto& job:jobs)if(job.made){key<<"track "<<job.track->number<<" "<<fileStamp(job.outTrack)<<"\n";++pictures;}
        if(!pictures)return;
        std::vector<uint8_t> stored;
        if(readWholeFile(keyPath,stored,65536)){
            const std::string text(stored.begin(),stored.end());unsigned long long bytes=0;unsigned tracks=0,textures=0;struct stat status{};
            if(text.rfind(key.str(),0)==0&&std::sscanf(text.c_str()+key.str().size(),"made %llu %u %u",&bytes,&tracks,&textures)==3&&
               !::stat(out.c_str(),&status)&&uint64_t(status.st_size)==bytes){boot.cache="hit";boot.bytes=bytes;boot.tracks=tracks;boot.textures=textures;}
        }
        if(boot.cache!="hit"){
            boot.cache="failed";
            std::vector<uint8_t> dyntex;
            if(!readWholeFile(dyntexPath,dyntex))throw std::runtime_error("Global/DYNTEX.BIN unreadable");
            std::vector<free_roam::Picture> made;
            for(const auto& job:jobs){
                if(!job.made)continue;
                std::vector<uint8_t> bundle;
                if(!readWholeFile(job.outTrack,bundle))throw std::runtime_error(job.outTrack.filename().string()+" unreadable");
                free_roam::Picture picture;picture.number=job.track->number;picture.quads=free_roam::roadQuads(bundle);
                made.push_back(std::move(picture));
            }
            free_roam::PictureReport report;
            const std::vector<uint8_t> file=free_roam::makePicturePack(dyntex,made,report);
            if(!report.error.empty())throw std::runtime_error("Global/DYNTEX.BIN: "+report.error);
            boot.skipped=report.skipped;
            ::unlink(keyPath.c_str());   // no key names a pack being replaced
            if(!writeFileAtomically(out,file.data(),file.size()))throw std::runtime_error("cannot write "+out.string());
            const std::string text=key.str()+"made "+std::to_string(file.size())+" "+std::to_string(report.tracks)+" "+std::to_string(report.textures)+"\n";
            if(!writeFileAtomically(keyPath,text.data(),text.size()))throw std::runtime_error("cannot write "+keyPath.string());
            boot.cache="built";boot.bytes=file.size();boot.tracks=report.tracks;boot.textures=report.textures;
        }
        servedFiles[servedKey(dyntexPath)]=out;
    }catch(const std::exception& e){boot.error=e.what();}
    boot.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
// Before the guest boots, with free roam on: each track's bundle (Beta 2.0's, with the installed base race's
// section table) and texture pack (the installed base race's, with a map of the district made from its races'
// maps and the track's roads) from the app's folder when they were made from these game files, made now
// otherwise (a few hundred ms a track, once); served with the base race's stream under the track's name.
// freeroam.c then adds the offered tracks to the Free Run list.
void prepareFreeRoam(){
    freeRoamBoot=FreeRoamBoot{};
    nfs_freeroam_configure(nullptr,0,nullptr);
    if(freeRoamDir.empty())return;
    freeRoamBoot.on=true;
    const auto start=std::chrono::steady_clock::now();
    static nfs_freeroam_track offered[NFS_FREEROAM_MAX_TRACKS];
    unsigned count=0;
    try{
        if(generatedDataDir.empty())throw std::runtime_error("no folder for generated data");
        const std::filesystem::path directory=std::filesystem::path(generatedDataDir)/"free_roam";
        std::error_code error;std::filesystem::create_directories(directory,error);
        if(error)throw std::runtime_error("cannot make "+directory.string()+": "+error.message());
        const auto globalPath=gamePath("C:\\nfsu1\\Global\\GlobalB.lzc");
        std::vector<uint8_t> global,globalb;
        if(!readWholeFile(globalPath,global))throw std::runtime_error("Global/GlobalB.lzc unreadable");
        if(!rim_sizes::jdlzDecompress(global,globalb))globalb=std::move(global);   // a mod's may be stored unpacked
        const auto records=free_roam::trackRecords(globalb);
        if(records.empty())throw std::runtime_error("Global/GlobalB.lzc has no track records");
        auto tracksPath=[](const std::string& name){return gamePath("C:\\nfsu1\\Tracks\\"+name);};
        std::vector<FreeRoamJob> jobs;
        for(const auto& track:free_roam::kTracks){
            FreeRoamJob job;job.track=&track;job.boot.number=track.number;
            job.bundle=std::filesystem::path(freeRoamDir)/free_roam::trackFile(track.number);
            job.base=tracksPath("TRACKB"+std::to_string(track.base)+".LZC");
            job.tex=tracksPath(free_roam::texFile(track.base));
            job.stream=tracksPath(free_roam::raceStream(track.region,track.base));
            job.outTrack=directory/("TRACKB"+std::to_string(track.number)+".LZC");
            job.outTex=directory/free_roam::texFile(track.number);
            job.keyPath=directory/("track"+std::to_string(track.number)+".txt");
            for(const auto& record:free_roam::mapSources(records,track))job.maps.emplace_back(record,tracksPath(free_roam::texFile(record.number)));
            // The key names the generator and every file the track is made from; then "made ..." says what was made.
            std::ostringstream key;
            key<<"free roam "<<free_roam::kGeneratorVersion<<" track "<<track.number<<"\nbundle "<<fileStamp(job.bundle)<<"\nbase "<<fileStamp(job.base)
               <<"\ntex "<<fileStamp(job.tex)<<"\nglobal "<<fileStamp(globalPath)<<"\n";
            for(const auto& source:job.maps)key<<"map "<<source.first.number<<" "<<fileStamp(source.second)<<"\n";
            job.key=key.str();
            if(fileStamp(job.bundle)=="missing"){job.boot.error="not in the free roam folder";jobs.push_back(std::move(job));continue;}
            const auto checking=std::chrono::steady_clock::now();
            std::vector<uint8_t> stored;
            if(readWholeFile(job.keyPath,stored,65536)){
                const std::string text(stored.begin(),stored.end());
                unsigned long long trackBytes=0,texBytes=0,quads=0,tiles=0;unsigned size=0;float x=0,y=0,width=0;
                struct stat status{};
                if(text.rfind(job.key,0)==0&&
                   std::sscanf(text.c_str()+job.key.size(),"made %llu %llu %u %a %a %a %llu %llu",&trackBytes,&texBytes,&size,&x,&y,&width,&quads,&tiles)==8&&
                   !::stat(job.outTrack.c_str(),&status)&&uint64_t(status.st_size)==trackBytes&&
                   (!texBytes||(!::stat(job.outTex.c_str(),&status)&&uint64_t(status.st_size)==texBytes))){
                    job.made=true;job.hasMap=texBytes!=0;job.boot.cache="hit";
                    job.boot.quads=quads;job.boot.tiles=tiles;
                    if(job.hasMap){job.boot.mapSize=size;job.boot.mapX=x;job.boot.mapY=y;job.boot.mapWidth=width;}
                    const size_t noMap=text.find("no map: ");
                    if(noMap!=std::string::npos)job.boot.mapError=text.substr(noMap+8,text.find('\n',noMap)-noMap-8);
                }
            }
            job.boot.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-checking).count();
            jobs.push_back(std::move(job));
        }
        std::vector<std::future<void>> building;
        for(auto& job:jobs)if(!job.made&&job.boot.error.empty())building.push_back(std::async(std::launch::async,[&job]{buildFreeRoamTrack(job);}));
        for(auto& done:building)done.get();
        for(auto& job:jobs){
            const free_roam::Track& track=*job.track;
            if(job.made){
                servedFiles[servedKey(tracksPath(free_roam::trackFile(track.number)))]=job.outTrack;
                servedFiles[servedKey(tracksPath(free_roam::texFile(track.number)))]=job.hasMap?job.outTex:job.tex;
                if(fileStamp(job.stream)!="missing")servedFiles[servedKey(tracksPath(free_roam::raceStream(track.region,track.number)))]=job.stream;
                nfs_freeroam_track& out=offered[count++];
                out=nfs_freeroam_track{};
                out.number=track.number;out.base=track.recordBase;out.icon=track.icon;
                std::snprintf(out.region,sizeof out.region,"%s",track.region);
                std::snprintf(out.region_directory,sizeof out.region_directory,"%s",track.regionDirectory);
                std::snprintf(out.name,sizeof out.name,"%s",track.name);
                out.map_x=job.boot.mapX;out.map_y=job.boot.mapY;out.map_width=job.hasMap?job.boot.mapWidth:0;
                job.boot.offered=true;
            }
            freeRoamBoot.tracks.push_back(job.boot);
        }
        prepareFreeRoamPictures(directory,jobs);
    }catch(const std::exception& e){freeRoamBoot.error=e.what();count=0;}
    static const nfs_freeroam_host host={gameMalloc,freeRoamCall};
    nfs_freeroam_configure(offered,count,&host);
    freeRoamBoot.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(!freeRoamBoot.error.empty()){
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Free roam: no tracks (%s)",freeRoamBoot.error.c_str());
        return;
    }
    for(const auto& track:freeRoamBoot.tracks){
        const char* name="?";for(const auto& t:free_roam::kTracks)if(t.number==track.number)name=t.name;
        if(!track.offered)NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Free roam: %s (%u) not offered: %s",name,track.number,track.error.c_str());
        else if(track.mapSize)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Free roam: %s (%u) in Free Run, %s in %.0f ms; map %ux%u over %.0f m from %llu race maps and %llu road polygons",
            name,track.number,track.cache=="hit"?"made earlier, checked":"made",track.ms,track.mapSize,track.mapSize,double(track.mapWidth),
            static_cast<unsigned long long>(track.tiles),static_cast<unsigned long long>(track.quads));
        else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Free roam: %s (%u) in Free Run, %s in %.0f ms; no map (%s): the minimap stays off",
            name,track.number,track.cache=="hit"?"made earlier, checked":"made",track.ms,track.mapError.c_str());
    }
    const auto& pictures=freeRoamBoot.pictures;
    if(pictures.cache=="hit"||pictures.cache=="built")
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Free roam: SELECT LOCATION pictures for %u tracks (%u textures added) served for Global/DYNTEX.BIN, %s in %.0f ms",
            pictures.tracks,pictures.textures,pictures.cache=="hit"?"made earlier, checked":"made",pictures.ms);
    else if(!pictures.error.empty())
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Free roam: no SELECT LOCATION pictures (%s): the free roam tracks' map there stays black",pictures.error.c_str());
    for(const auto& skipped:pictures.skipped)NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Free roam: no SELECT LOCATION picture for %s",skipped.c_str());
}
// For the session JSON: what was offered and what the game side did (nfs_freeroam_get_stats).
std::string freeRoamJson(){
    auto quoted=[](const std::string& text){std::string out="\"";for(char c:text){if(c=='"'||c=='\\')out+='\\';if(uint8_t(c)>=0x20)out+=c;}return out+"\"";};
    const nfs_freeroam_stats stats=nfs_freeroam_get_stats();
    std::ostringstream json;
    json<<"{\"on\":"<<(freeRoamBoot.on?"true":"false")<<",\"offered\":"<<nfs_freeroam_track_count()<<",\"ms\":"<<std::fixed<<std::setprecision(0)<<freeRoamBoot.ms;
    if(!freeRoamBoot.error.empty())json<<",\"error\":"<<quoted(freeRoamBoot.error);
    json<<",\"tracks\":[";
    for(size_t i=0;i<freeRoamBoot.tracks.size();++i){
        const auto& t=freeRoamBoot.tracks[i];
        json<<(i?",":"")<<"{\"number\":"<<t.number<<",\"offered\":"<<(t.offered?"true":"false")<<",\"cache\":"<<quoted(t.cache)
            <<",\"ms\":"<<t.ms<<",\"mapSize\":"<<t.mapSize<<",\"tiles\":"<<t.tiles<<",\"quads\":"<<t.quads;
        if(!t.error.empty())json<<",\"error\":"<<quoted(t.error);
        if(!t.mapError.empty())json<<",\"mapError\":"<<quoted(t.mapError);
        json<<"}";
    }
    const auto& pictures=freeRoamBoot.pictures;
    json<<"],\"pictures\":{\"cache\":"<<quoted(pictures.cache)<<",\"tracks\":"<<pictures.tracks<<",\"textures\":"<<pictures.textures
        <<",\"bytes\":"<<pictures.bytes<<",\"ms\":"<<pictures.ms;
    if(!pictures.error.empty())json<<",\"error\":"<<quoted(pictures.error);
    json<<"}";
    json<<",\"tracksAdded\":"<<stats.tracks_added<<",\"namesAdded\":"<<stats.names_added<<",\"races\":"<<stats.races
        <<",\"wrongWayCalls\":"<<stats.wrong_way_calls<<",\"wrongWayCleared\":"<<stats.wrong_way_cleared<<",\"updates\":"<<stats.updates
        <<",\"crumbs\":"<<stats.crumbs<<",\"resets\":"<<stats.resets<<",\"resetsPlaced\":"<<stats.resets_placed<<",\"resetsLeft\":"<<stats.resets_left
        <<",\"reverseEntriesLeftOut\":"<<stats.reverse_entries_left_out<<"}";
    return json.str();
}
// For the session JSON: the boot's outcome and what the game side did with it (nfs_garage_rim_stats).
std::string rimSizesJson(){
    auto quoted=[](const std::string& text){std::string out="\"";for(char c:text){if(c=='"'||c=='\\')out+='\\';if(uint8_t(c)>=0x20)out+=c;}return out+"\"";};
    const nfs_rim_stats stats=nfs_garage_rim_stats();
    std::ostringstream json;
    json<<"{\"on\":"<<(rimSizesBoot.on?"true":"false")<<",\"served\":"<<(rimSizesBoot.served?"true":"false")
        <<",\"reason\":"<<quoted(rimSizesBoot.reason)<<",\"cache\":"<<quoted(rimSizesBoot.cache)<<",\"ms\":"<<std::fixed<<std::setprecision(0)<<rimSizesBoot.ms
        <<",\"parts\":"<<rimSizesBoot.parts<<",\"solids\":"<<rimSizesBoot.solids<<",\"bytes\":"<<rimSizesBoot.bytes;
    if(!rimSizesBoot.error.empty())json<<",\"error\":"<<quoted(rimSizesBoot.error);
    json<<",\"lookups\":"<<stats.lookups<<",\"later\":"<<stats.later<<",\"shared\":"<<stats.shared<<",\"sorted\":"<<stats.sorted<<",\"opened\":"<<stats.opened<<"}";
    return json.str();
}
#endif
void testGuestScheduling(){
    State original;original.save();uint32_t savedError=lastError,savedTLS=tlsValues[1087];
    g_eax=0x11223344;g_fs_base=0x21000000;g_st[0]=123.5;g_xmm[7].u64[0]=0x123456789abcdef0ull;
    lastError=111;tlsValues[1087]=0x55;
    bool done=false;std::string failure;
    std::thread worker([&]{
        std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=9000;
        State fresh;fresh.eax=0xaabbccdd;fresh.fs=0xdead1000;fresh.st[0]=-456.25;fresh.xmm[7].u64[0]=0xfedcba9876543210ull;fresh.load();
        lastError=222;tlsValues[1087]=0xaa;
        try{for(unsigned i=0;i<100;++i){checkGuestProgress();yieldGuestMachine();
            if(g_eax!=0xaabbccdd||g_fs_base!=0xdead1000||g_st[0]!=-456.25||g_xmm[7].u64[0]!=0xfedcba9876543210ull||lastError!=222||tlsValues[1087]!=0xaa)
                throw GuestStop("Guest worker context was not isolated");}}
        catch(const std::exception& error){failure=error.what();}
        forgetThreadTrace();done=true;machineLease=nullptr;
    });
    try{while(!done){checkGuestProgress();yieldGuestMachine(0,std::chrono::steady_clock::now()+std::chrono::milliseconds(1));
        if(g_eax!=0x11223344||g_fs_base!=0x21000000||g_st[0]!=123.5||g_xmm[7].u64[0]!=0x123456789abcdef0ull||lastError!=111||tlsValues[1087]!=0x55)
            failure="Main guest context was not preserved during handover";
    }}catch(const std::exception& error){failure=error.what();}
    machineLease->unlock();worker.join();machineLease->lock();original.load();lastError=savedError;tlsValues[1087]=savedTLS;
    if(!failure.empty())throw GuestStop(failure);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest scheduler passed 100 handovers with separate integer, FS, x87, XMM, TLS and last-error state");
}
void testGuestSoundCaps(){
    State saved;saved.save();
    uint32_t output=allocate(96),device=createSoundObject(1);
    write32(output,96);
    uint32_t result=callGuest(soundMethodBase+0x1000+4*16,{device,output});
    bool ok=result==0&&read32(output)==96&&read32(output+4)==0xf1f&&read32(output+8)==100&&
        read32(output+12)==200000&&read32(output+16)==1&&read32(output+20)==0;
    callGuest(soundMethodBase+0x1000+2*16,{device});heaps->free(GuestHeaps::process,output);saved.load();
    if(!ok)throw GuestStop("DirectSound PCM capabilities regression");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","DirectSound caps passed: mono/stereo, 8/16-bit, continuous rates, software buffers and guest ABI");
}
recomp_func_t resolveFunction(const std::string& dll, const std::string& name) {
    if((dll=="shfolder.dll"||dll=="shell32.dll")&&name=="SHGetFolderPathA")return shellFolderPath;
    if(dll=="dsound.dll"&&(name=="#1"||name=="DirectSoundCreate"))return directSoundCreate;
    if(dll=="dinput8.dll"&&name=="DirectInput8Create")return directInputCreate;
    if(dll=="gdi32.dll"&&name=="DeleteObject")return deleteGdiObject;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(dll=="d3d9.dll"&&name=="Direct3DCreate9")return direct3DCreate;
#endif
    if(dll=="advapi32.dll"&&name=="RegOpenKeyA")return regOpenKeyA;
    if(dll=="advapi32.dll"&&name=="RegOpenKeyExA")return regOpenKeyExA;
    if(dll=="advapi32.dll"&&name=="RegCloseKey")return regCloseKey;
    if(dll=="advapi32.dll"&&name=="RegQueryValueExA")return regQueryValueExA;
    if(dll=="advapi32.dll"&&name=="RegSetValueExA")return regSetValueExA;
    if(dll=="ws2_32.dll"||dll=="wsock32.dll"){auto found=networkFunctions.find(name);if(found!=networkFunctions.end())return found->second;}
    if(dll=="netapi32.dll"&&name=="Netbios")return netbios;
    if(dll=="tapi32.dll"&&name=="lineInitialize")return lineInitialize;
    if(dll=="tapi32.dll"&&name=="lineShutdown")return lineShutdown;
    if(dll=="user32.dll") {auto found=windowFunctions.find(name);if(found!=windowFunctions.end())return found->second;}
    if(dll=="winmm.dll") {auto found=timerFunctions.find(name);if(found!=timerFunctions.end())return found->second;}
    if (dll == "kernel32.dll") {
        auto resource=resourceFunctions.find(name);if(resource!=resourceFunctions.end())return resource->second;
        auto file=fileFunctions.find(name);if(file!=fileFunctions.end())return file->second;
        auto kernel=kernelFunctions.find(name); if(kernel!=kernelFunctions.end()) return kernel->second;
        if (name=="GetVersionExA") return getVersion;
        if (name=="GetModuleHandleA") return moduleHandle;
        if (name=="HeapCreate") return heapCreate;
        if (name=="HeapAlloc") return heapAlloc;
        if (name=="HeapFree") return heapFree;
        if (name=="HeapReAlloc") return heapRealloc;
        if (name=="HeapSize") return heapSize;
        if (name=="HeapDestroy") return heapDestroy;
        if (name=="GetProcessHeap") return getProcessHeap;
    }
    return unsupported;
}
void getProcAddress() {
    uint32_t module=arg(0),namePointer=arg(1);
    auto found=std::find_if(modules.begin(),modules.end(),[&](auto& m){return m.second==module;});
    auto name=namePointer<=0xffff?"#"+std::to_string(namePointer):guestString(namePointer);
    if(uint32_t va;found==modules.end()&&guestModuleExport(module,name,va)) { ret(va,2); return; }
    if(found==modules.end()) { lastError=isGuestModule(module)?127:126; ret(0,2); return; }
    auto full=found->first+"!"+name;
    for(uint32_t i=0;i<imports.size();++i) if(imports[i].name==full) { ret(importBase+i*16,2); return; }
    // Only advertise exports that are actually implemented, not permissive stubs.
    auto fn=resolveFunction(found->first,name);
    if(fn==unsupported) { lastError=127; ret(0,2); return; }
    uint32_t va=importBase+uint32_t(imports.size())*16;
    imports.push_back({full,fn}); ret(va,2);
}

template<class T> T fileStruct(const std::vector<unsigned char>& file, uint64_t offset) {
    if (offset > file.size() || sizeof(T) > file.size()-offset) throw GuestStop("Truncated PE structure");
    T value{}; std::memcpy(&value,file.data()+offset,sizeof value); return value;
}
void moduleRange(uint32_t size, uint32_t rva, uint64_t bytes) {
    if (rva > size || bytes > uint64_t(size)-rva) throw GuestStop("PE image range invalid");
}
std::string moduleString(uint32_t base, uint32_t size, uint32_t rva) {
    moduleRange(size,rva,1); std::string value;
    for (uint32_t i=rva; i<size && value.size()<512; ++i) {
        char c = *static_cast<char*>(ptr(base+i,1)); if (!c) return value; value.push_back(c);
    }
    throw GuestStop("Unterminated PE import name");
}
void imageRange(uint32_t rva, uint64_t bytes) { moduleRange(imageSize,rva,bytes); }
// Writes an import token into every IAT slot of a mapped image (Speed.exe or a game DLL).
void bindImports(uint32_t base, uint32_t size, uint32_t directoryRva, uint32_t directorySize) {
    moduleRange(size,directoryRva,directorySize);
    bool terminated=false;
    for (uint32_t offset=0; uint64_t(offset)+sizeof(pe_import_descriptor)<=directorySize; offset+=sizeof(pe_import_descriptor)) {
        pe_import_descriptor descriptor{};
        std::memcpy(&descriptor,ptr(base+directoryRva+offset,sizeof descriptor),sizeof descriptor);
        if (!descriptor.Name && !descriptor.FirstThunk) { terminated=true; break; }
        auto dll=moduleString(base,size,descriptor.Name);
        std::transform(dll.begin(),dll.end(),dll.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(!modules.count(dll)) modules[dll]=0x71000000u+uint32_t(modules.size())*0x10000u;
        uint32_t names=descriptor.OriginalFirstThunk ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        for (uint32_t i=0;;++i) {
            uint64_t nameRva=uint64_t(names)+i*4ull, iatRva=uint64_t(descriptor.FirstThunk)+i*4ull;
            if (nameRva>UINT32_MAX || iatRva>UINT32_MAX) throw GuestStop("PE thunk overflow");
            moduleRange(size,uint32_t(nameRva),4); moduleRange(size,uint32_t(iatRva),4);
            uint32_t thunk=read32(base+uint32_t(nameRva)); if (!thunk) break;
            std::string name;
            if (thunk&PE_ORDINAL_FLAG) name="#"+std::to_string(thunk&0xffff);
            else { moduleRange(size,thunk,3); name=moduleString(base,size,thunk+2); }
            if (imports.size()>=4096) throw GuestStop("Too many PE imports");
            uint32_t va=importBase+uint32_t(imports.size())*16;
            imports.push_back({dll+"!"+name,resolveFunction(dll,name)});
            write32(base+uint32_t(iatRva),va);
        }
    }
    if (!terminated) throw GuestStop("Unterminated PE import descriptors");
}
void loadImage(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw GuestStop("Cannot open game PE");
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(stream)),{});
    auto dos=fileStruct<pe_dos_header>(file,0);
    if (dos.e_magic!=0x5a4d || dos.e_lfanew<0) throw GuestStop("Invalid DOS header");
    auto nt=fileStruct<pe_nt_headers32>(file,dos.e_lfanew);
    if (nt.Signature!=0x4550 || nt.FileHeader.Machine!=0x14c || nt.OptionalHeader.Magic!=0x10b ||
        nt.FileHeader.SizeOfOptionalHeader<sizeof(pe_opt_header32)) throw GuestStop("Expected x86 PE32");
    auto opt=nt.OptionalHeader; imageBase=opt.ImageBase; imageSize=opt.SizeOfImage;
    resourceRva=opt.DataDirectory[2].VirtualAddress;resourceSize=opt.DataDirectory[2].Size;
    if (imageBase!=0x400000 || imageSize!=0x394368 || opt.AddressOfEntryPoint!=0x270cb5)  // Speed.exe 1.4
        throw GuestStop("PE layout does not match generated NFSU1 code");
    memory->commit(imageBase,imageSize);
    imageRange(0,opt.SizeOfHeaders);
    if (opt.SizeOfHeaders>file.size()) throw GuestStop("Truncated PE headers");
    std::memcpy(ptr(imageBase,opt.SizeOfHeaders),file.data(),opt.SizeOfHeaders);
    uint64_t sections=uint64_t(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
    for (uint32_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        auto sec=fileStruct<pe_section_header>(file,sections+i*sizeof(pe_section_header));
        imageRange(sec.VirtualAddress,std::max(sec.VirtualSize,sec.SizeOfRawData));
        if (uint64_t(sec.PointerToRawData)+sec.SizeOfRawData>file.size()) throw GuestStop("Truncated PE section");
        std::memcpy(ptr(imageBase+sec.VirtualAddress,sec.SizeOfRawData),file.data()+sec.PointerToRawData,sec.SizeOfRawData);
    }
    entry=imageBase+opt.AddressOfEntryPoint;
    // Guest VAs stay fixed; the 64-bit host base is ADDR's offset. No PE
    // relocation toward the host address and no executable x86 mapping.
    auto directory=opt.DataDirectory[PE_DIR_IMPORT];
    bindImports(imageBase,imageSize,directory.VirtualAddress,directory.Size);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest PE mapped: base=%08x size=%08x entry=%08x; %zu imports bound; %u lifted functions",
        imageBase,imageSize,entry,imports.size(),recomp_dispatch_count);
}
uint32_t callGuest(uint32_t va, const uint32_t* args, size_t count, uint32_t cleanup, bool threadStart) {
    State saved; saved.save();
    try {
        recomp_func_t fn=recomp_lookup_manual(va);if(!fn)fn=recomp_lookup(va);
        if (!fn) fn=recomp_lookup_import(va);
        if (!fn) throw GuestStop("No guest function at " + hex(va));
        for (size_t i=count;i--;) { g_esp-=4; write32(g_esp,args[i]); }
        g_esp-=4; write32(g_esp,RECOMP_RETADDR);
        fn(); g_esp+=cleanup;
        if (g_esp!=saved.esp&&!(threadStart&&g_esp+4u*uint32_t(count)==saved.esp)) throw GuestStop("Guest call stack imbalance at " + hex(va));
        uint32_t result=g_eax; saved.load(); return result;
    } catch (...) { saved.load(); throw; }
}
#include "guest_modules.inc"
void timerTestCallback(){timerTestCalls.push_back(arg(0));ret(0,5);}
void testGuestTimers(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;throw GuestStop("Timer test import unavailable");};
    uint32_t set=imported("winmm.dll!timeSetEvent"),kill=imported("winmm.dll!timeKillEvent");
    auto wait=[&](unsigned count){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(timerTestCalls.size()<count){if(std::chrono::steady_clock::now()>until)throw GuestStop("Timer callback deadline failed");checkGuestProgress();yieldGuestMachine(1);}};
    timerTestActive=true;timerTestCalls.clear();
    try{
        auto first=callGuest(set,{2,0,timerTestAddress,0,0x100});if(!first)throw GuestStop("Timer creation failed");wait(1);
        if(timerTestCalls[0]!=first||timerHandles.count(first))throw GuestStop("One-shot timer retirement failed");
        auto worker=timerWorker->id;
        auto periodic=callGuest(set,{2,0,timerTestAddress,0,0x101});wait(3);
        if(callGuest(kill,{periodic})||timerHandles.count(periodic))throw GuestStop("Periodic timer cancellation failed");
        size_t after=timerTestCalls.size();auto cancelled=callGuest(set,{20,0,timerTestAddress,0,0x100});
        if(callGuest(kill,{cancelled}))throw GuestStop("Pending timer cancellation failed");
        auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(25);while(std::chrono::steady_clock::now()<until){checkGuestProgress();yieldGuestMachine(1);}
        if(timerTestCalls.size()!=after||!timerHandles.empty()||timerWorker->id!=worker||guestThreads.size()!=1)throw GuestStop("Timer cancellation/worker reuse failed");
        timerTestActive=false;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Timer worker passed: one-shot, periodic, synchronous cancel, pending cancel, retirement and single-thread reuse");
    }catch(...){timerTestActive=false;for(auto& timer:timerHandles)timer.second->cancelled=true;timerHandles.clear();throw;}
}
void testGuestFileTime(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;return 0u;};
    uint32_t local=imported("kernel32.dll!FileTimeToLocalFileTime"),system=imported("kernel32.dll!FileTimeToSystemTime");
    if(!local||!system){  // the self-check drives the game's own imports; Speed.exe has neither
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest FILETIME self-check skipped: the game imports no FILETIME conversion");
        return;
    }
    uint32_t data=allocate(48);uint64_t epoch=116444736000000000ull+1234ull*10000ull;
    std::memset(ptr(data,48),0xa5,48);std::memcpy(ptr(data,8),&epoch,8);
    bool okay=callGuest(local,{data,data+8})==1&&read32(data+8)==uint32_t(epoch)&&read32(data+12)==uint32_t(epoch>>32)
        &&callGuest(system,{data+8,data+16})==1;
    const uint16_t expected[]={1970,1,4,1,0,0,1,234};
    okay=okay&&!std::memcmp(ptr(data+16,16),expected,16)&&read32(data+32)==0xa5a5a5a5u;
    okay=okay&&callGuest(local,{data,data})==0;
    uint64_t invalid=0x8000000000000000ull;std::memcpy(ptr(data,8),&invalid,8);
    okay=okay&&callGuest(system,{data,data+16})==0;
    heaps->free(GuestHeaps::process,data);
    if(!okay)throw GuestStop("Guest file-time conversion self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest UTC FILETIME conversions passed: epoch, milliseconds, guards and invalid input");
}
void testGuestCriticalSections(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name=="kernel32.dll!"+name)return importBase+i*16;throw GuestStop("Critical-section import unavailable");};
    uint32_t initialize=imported("InitializeCriticalSection"),enter=imported("EnterCriticalSection"),leave=imported("LeaveCriticalSection"),remove=imported("DeleteCriticalSection");
    uint32_t data=allocate(32);std::memset(ptr(data,32),0xa5,32);
    callGuest(initialize,{data});callGuest(enter,{data});callGuest(enter,{data});
    bool okay=read32(data+8)==2&&read32(data+12)==guestThreadId&&read32(data+24)==0xa5a5a5a5u;
    callGuest(remove,{data});okay=okay&&criticalSections.count(data)&&pendingCriticalDeletes.count(data);
    callGuest(leave,{data});okay=okay&&read32(data+8)==1&&criticalSections.count(data);
    callGuest(leave,{data});okay=okay&&!criticalSections.count(data)&&!pendingCriticalDeletes.count(data)&&read32(data+8)==0;
    callGuest(initialize,{data});callGuest(remove,{data});okay=okay&&!criticalSections.count(data);
    heaps->free(GuestHeaps::process,data);
    if(!okay)throw GuestStop("Guest critical-section retirement self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest critical sections passed recursion, owner, guards, deferred retirement and reinitialization");
}
} // namespace

#ifdef NFS_SERVER_DLL
extern "C" const recomp_dispatch_entry_t server_recomp_dispatch_table[];
extern "C" const uint32_t server_recomp_dispatch_count;
#endif
namespace {
// Every indirect call and virtual method resolves its target here, so use an
// open-addressing table (built once, read-only afterwards) instead of a binary
// search over ~28k entries with a cache miss per step.
#include "guest_asset_guards.inc"
struct DispatchHash {
    static constexpr uint32_t slots=1u<<16;
    std::unique_ptr<uint32_t[]> keys{new uint32_t[slots]()};
    std::unique_ptr<recomp_func_t[]> funcs{new recomp_func_t[slots]()};
    static uint32_t slot(uint32_t va){return (va*0x9e3779b1u)>>16;}
    void add(const recomp_dispatch_entry_t* table,uint32_t count){
        for(uint32_t i=0;i<count;++i){
            uint32_t va=table[i].address,s=slot(va);
            if(va-0x7c000000u<0x04000000u)throw GuestStop("Lifted function overlaps reserved native dispatch stubs");
            while(keys[s])s=(s+1)&(slots-1);
            keys[s]=va;funcs[s]=table[i].func;
        }
    }
    DispatchHash(){
        static_assert(slots>2*(27742+1531),"dispatch hash load factor");
        add(recomp_dispatch_table,recomp_dispatch_count);
#ifdef NFS_SERVER_DLL
        add(server_recomp_dispatch_table,server_recomp_dispatch_count);
#endif
        uint32_t used=0;for(uint32_t i=0;i<slots;++i)used+=keys[i]!=0;
        if(used>slots/2)throw GuestStop("Dispatch hash over half full: "+std::to_string(used));
    }
    recomp_func_t find(uint32_t va)const{
        if(!va)return nullptr;
        for(uint32_t s=slot(va);keys[s];s=(s+1)&(slots-1))if(keys[s]==va)return funcs[s];
        return nullptr;
    }
};
}
extern "C" void nfs_validate_asset_entry(uint32_t va){
    if(va==0x00447c00u)validateResourceChunks(arg(2),arg(3));
    else if(va==0x0059fb80u)validateLanguageTable();
}
extern "C" recomp_func_t recomp_lookup(uint32_t va) {
    // Sound/input/D3D COM and import stubs resolve in recomp_lookup_import.
    // Avoid an always-failing hash probe on every native method call. The
    // table constructor checks that no lifted module occupies this range.
    if(va-0x7c000000u<0x04000000u)return nullptr;
    static const DispatchHash hash;
    return hash.find(va);
}
extern "C" recomp_func_t nfs_abk_lookup(uint32_t va);  // src/runtime/nfs_abk.h
thread_local uint32_t lastUnresolvedSoundTarget=0;
extern "C" int nfs_guest_readable(uint32_t va,uint32_t n){return memory&&memory->readable(va,n);}
// The sound library's deferred-callback drain (0x652960: for each node of the list at 0x713c70, call
// [node+8](node[+0xc])) has twice called through a stale pointer into heap memory that now holds texture
// data (0x12c2ea1c, 0x13d5d71c; both with the HD cars pack installed, at the profile menu, from the 10 ms
// service thread while the main thread was re-registering the static node 0x713c7c). A stopped game is
// the worst outcome for a missed sound callback: the call is skipped as a cdecl no-op returning 0 (the
// caller cleans its one argument) and reported with the bytes at the target, so the case stays visible.
static void skippedSoundCallback(){
    static unsigned reported=0;
    uint32_t target=lastUnresolvedSoundTarget;
    if(++reported<=8){
        char bytes[40]="";
        if(memory&&memory->readable(target,16))for(unsigned i=0;i<16;++i)std::snprintf(bytes+2*i,3,"%02x",*static_cast<unsigned char*>(ptr(target+i,1)));
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Sound callback %08x skipped (not code: %s) node=%08x next=%08x arg=%08x",target,bytes,g_eax,
            memory&&memory->readable(g_eax,16)?read32(g_eax):0u,memory&&memory->readable(g_eax,16)?read32(g_eax+12):0u);
    }
    g_eax=0;g_esp+=4;
}
extern "C" recomp_func_t recomp_lookup_manual(uint32_t va) {
    if(timerTestActive&&va==timerTestAddress)return timerTestCallback;
    // Routines of EA's sound banks run from the heap: their lifted copies (nfs_abk.h).
    if((va>=GuestHeaps::firstArena&&va<GuestHeaps::firstArenaEnd)||(va>=GuestHeaps::secondArena&&va-GuestHeaps::secondArena<GuestHeaps::secondArenaSize)){
        if(recomp_func_t fn=nfs_abk_lookup(va))return fn;
        if(g_cur_func==0x00652960u){lastUnresolvedSoundTarget=va;return skippedSoundCallback;}
    }
    return nfs_override_lookup(va);
}
extern "C" recomp_func_t recomp_lookup_import(uint32_t va) {
    checkGuestProgress();
    if(va>=soundMethodBase+0x1000&&va<soundMethodBase+0x3000&&!(va%16)){soundToken=va;return soundDispatch;}
    if(va>=inputMethodBase+0x1000&&va<inputMethodBase+0x5000&&!(va%16)){inputToken=va;return inputDispatch;}  // kinds 1-4: DirectInput, keyboard, mouse, gamepad
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){d3dToken=va;return d3dDispatch;}
#endif
    if (va<importBase || (va-importBase)%16) return nullptr;
    uint32_t index=(va-importBase)/16;
    if (index>=imports.size()) return nullptr;
    lastImport=va;
    if(++imports[index].calls<=3)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest import %s from %08x",imports[index].name.c_str(),g_cur_func);
    return imports[index].function;
}
extern "C" void recomp_unresolved(const char* kind,uint32_t va,uint32_t from) {
    // What the guest tried to call: the bytes at the target (a sound-bank routine the lift does not know,
    // or data) and, for an indirect call, the caller's registers, so the report alone identifies the case.
    std::string detail;
    if(memory&&memory->readable(va,32)){
        char hexbytes[80];for(unsigned i=0;i<32;++i)std::snprintf(hexbytes+2*i,3,"%02x",*static_cast<unsigned char*>(ptr(va+i,1)));
        detail+=std::string(" bytes ")+hexbytes;
    }else detail+=" (target not readable)";
    // Lifted code keeps its registers in locals and syncs the globals only around calls, so g_eax and
    // friends are stale here; the indirect-call ring (last targets and their callers) is reliable.
    detail+=" recent calls:";
    unsigned count=g_icall_trace_idx<8?g_icall_trace_idx:8;
    for(unsigned i=count;i-->0;){unsigned slot=(g_icall_trace_idx-1-i)&(ICALL_TRACE_SIZE-1);char item[32];std::snprintf(item,sizeof item," %08x<-%08x",g_icall_trace[slot],g_icall_from[slot]);detail+=item;}
    throw GuestStop(std::string(kind)+" unresolved "+hex(va)+" from "+hex(from)+detail);
}
extern "C" void recomp_unimpl(uint32_t va,const char* what) {
    throw GuestStop("Instruction unsupported at "+hex(va)+": "+what);
}
extern "C" void recomp_dump_trace(const char*) {}
// (NFSU2 rewrites its six-resolution menu here and intercepts the selector; Speed.exe's
// resolution menu is not mapped yet, so the game keeps its own modes and DXVK scales.)

std::string connectGuestRuntime(const char* executable) {
    std::unique_lock<MachineMutex> guard(machine);machineLease=&guard;
    if (memory) return "Runtime guest ya inicializado";
    State original; original.save();
    try {
        stopRequested=false;
        // Interactive Android sessions stop through lifecycle cancellation.
#ifdef __ANDROID__
        bootDeadline=std::chrono::steady_clock::time_point::max();
#else
        bootDeadline=std::chrono::steady_clock::now()+std::chrono::minutes(5);
#endif
        recomp_yield_hook=checkGuestProgress;
        memory=std::make_unique<GuestMemory>();
        hostGameRoot=std::string(executable); hostGameRoot=hostGameRoot.substr(0,hostGameRoot.find_last_of('/'));
        g_mem_base=reinterpret_cast<ptrdiff_t>(memory->base());
        testGuestHeaps(*memory);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest heaps passed ownership, realloc preservation, zero tail, in-place failure and reuse checks");
        heaps=std::make_unique<GuestHeaps>(*memory);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        configureD3D9Bridge(memory->base(),allocate,freeD3DGuest);
#endif
        loadImage(executable);
        if(!guestWrongWayWarning)applyWrongWayPatch();
        nfs_frontend_cursor_units(1);  // touch hands out the cursor in front-end units
        // The launcher's internal resolution is the game's back buffer (its own menu modes stop at 1280x1024).
        nfs_widescreen_set_render_size(renderWidth.load(),renderHeight.load());
#ifdef __ANDROID__
        frameTelemetry.gpuIdleSource(dxvkAndroidGpuIdleMicros);   // each second's GPU busy share
#endif
        float pictureAspect=4.f/3.f;   // the game's back buffer shape, for the reflection targets
        if(guestWidescreen){
            nfs_widescreen_init(hostGameRoot.c_str());
            // Landscape surface: the long side over the short one.
            float longSide=float(std::max(displayWidth.load(),displayHeight.load())),shortSide=float(std::min(displayWidth.load(),displayHeight.load()));
            float aspect=shortSide>0?longSide/shortSide:16.f/9.f;
            nfs_widescreen_set_display_aspect(aspect);pictureAspect=aspect;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen: render width follows the display aspect %.3f, 3D FOV corrected (front end and HUD still 4:3)",aspect);
        }else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen disabled: original 4:3 geometry");
        // Reflection targets (reflections.c) follow the render height; the originals are 320x240, 128, 256x64.
        nfs_reflections_configure(guestHdReflections,renderWidth.load(),renderHeight.load(),pictureAspect);
        {auto sizes=nfs_refl_sizes(guestHdReflections,renderWidth.load(),renderHeight.load(),pictureAspect);
         NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Reflections %s: road %ux%u, car cube map %u, rear-view mirror %ux%u",guestHdReflections?"sharp":"original",
             sizes.road_w,sizes.road_h,sizes.cube,sizes.mirror_w,sizes.mirror_h);}
        memory->commit(stackBase,stackSize); memory->commit(tib,0x1000);
        State fresh; fresh.esp=stackBase+stackSize-64; fresh.fs=tib; fresh.load();
        write32(tib,0xffffffff); write32(tib+4,stackBase+stackSize); write32(tib+8,stackBase); write32(tib+0x18,tib);
        testGuestScheduling();
        testGuestInput();
        testGuestSoundCaps();
        testGuestTimers();
        testGuestFileTime();
        testGuestCriticalSections();
        testGuestAssetGuards();
        // ThirteenAG's 44.1 kHz audio (misc_fixes.c): the sound library would open at 22.05 kHz at
        // performance tiers 0-1; the override it reads is set before the game runs.
        nfs_misc_fixes_boot();
        nfs_reflections_boot();   // the cube map and mirror sizes the target setup reads
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        nfs_garage_configure(guestGarage);
        nfs_garage_set_allocator(gameMalloc);
        nfs_garage_boot();        // the game's unlock-everything byte, when that switch is on
        logGarage();
        // Every rim size: the wheel pack (its parts are a second database, which the parts lookup reaches
        // through the host), and the rim browser's thumbnails shared by a style's sizes.
        servedFiles.clear();
        prepareRimSizes();
        nfs_fastpath_register(NFS_PARTS_LOOKUP,rimSizesBoot.served?nfs_garage_parts_lookup_fast:nullptr);
        nfs_fastpath_register(NFS_BROWSER_ADD_ITEM,rimSizesBoot.on?nfs_garage_browser_item_fast:nullptr);
        // Free roam: the tracks served (their records and names follow from freeroam.c's hooks), no wrong way on
        // them, and resets to where the car was driving.
        prepareFreeRoam();
        const bool freeRoam=nfs_freeroam_track_count()>0;
        nfs_fastpath_register(NFS_WRONG_WAY_TEST,freeRoam?nfs_freeroam_wrong_way_fast:nullptr);
        nfs_fastpath_register(NFS_PLAYER_UPDATE,freeRoam?nfs_freeroam_update_fast:nullptr);
        nfs_fastpath_register(NFS_PLAYER_RESET,freeRoam?nfs_freeroam_reset_fast:nullptr);
        nfs_fastpath_register(effect_states::kApply,guestEffectStates?applyEffectStateFast:nullptr);
        nfs_fastpath_register(effect_states::kEvaluate,guestEffectStates?evaluateEffectStateFast:nullptr);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Effect states: %s",guestEffectStates?
            "render, sampler and texture stage states of the game's effects applied by the host (already set ones skipped)":
            "applied by the game's own D3DX code");
#endif
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Sound: mixer rate override %u Hz (the game picks 22050 at performance tiers 0-1)",read32(NFS_MISC_SOUND_RATE_OVERRIDE));
        // Restore a clean CPU state before executing the real CRT entry.
        fresh.load();
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Entering actual lifted CRT entry %08x",entry);
        uint32_t result=callGuest(entry,{});
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();host_perf::release();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return "Entrada guest terminada: "+std::to_string(result);
    } catch (const std::exception& e) {
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Guest integration boundary: %s",e.what());
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return std::string("Arranque guest detenido: ")+e.what();
    }
}
void requestGuestRuntimeStop() {stopRequested=true;}
void setGuestPaused(bool paused) {
    // The AAudio stream keeps pulling from looping voices while the guest is
    // paused: in the background the last mixed sound repeated until the app
    // closed. Pause the output with the guest and restart it on resume (or
    // replace it, when Android took it meanwhile: sound_output.h).
    bool previous=guestPaused.exchange(paused);
    if(paused&&!previous){frameTelemetry.pause();frameInterrupted=true;}  // the next frame starts a new interval, not a 30 s "frame"
    if(previous!=paused)pauseGuestSound(paused);
}
bool guestDrivingControls() {return inputDrivingMode.load();}
void setGuestPad(const float axes[6],uint32_t buttons,int hatX,int hatY){
    std::lock_guard<std::mutex> lock(inputMutex);
    for(unsigned i=0;i<6;++i)inputPad.axis[i]=std::max(-1.f,std::min(1.f,axes[i]));
    inputPad.buttons=buttons;inputPad.hatX=hatX;inputPad.hatY=hatY;
}
void setGuestPadConnected(bool connected){
    std::lock_guard<std::mutex> lock(inputMutex);
    if(connected!=inputPadConnected)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Gamepad %s",connected?"connected":"disconnected");
    inputPadConnected=connected;if(!connected)inputPad=InputPad{};
}
bool guestGamepadInUse(){return inputPadAcquired.load()>0;}
void setGuestDisplaySize(unsigned width,unsigned height){displayWidth=width;displayHeight=height;}
void setGuestResolution(unsigned width,unsigned height){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    if(width<320||height<240||width>8192||height>8192){renderWidth=renderHeight=0;return;}
    renderWidth=width;renderHeight=height;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    setD3D9RenderSize(width,height);
#endif
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Selected render resolution %ux%u",width,height);
}
// Pipeline-compile counters from the prepared DXVK (ci/dxvk_extra.py): graphics, compute, total ns, max ns, >10 ms.
extern "C" void dxvkAndroidPipelineStats(uint64_t out[5]);
// This library's GNU build ID (the hex string crash reports print as "BuildId:"), so a report can tell this
// build's crashes from the older ones Android keeps in its crash log.
std::string nativeBuildId(){
    static const std::string id=[]{
        static const char anchor=0;
        Dl_info info{};
        if(!dladdr(&anchor,&info)||!info.dli_fbase)return std::string();
        struct Search{const void* base;std::string id;} search{info.dli_fbase,{}};
        dl_iterate_phdr([](dl_phdr_info* module,size_t,void* data)->int{
            auto* found=static_cast<Search*>(data);
            bool ours=false;
            for(int i=0;i<module->dlpi_phnum;++i){const auto& header=module->dlpi_phdr[i];
                if(header.p_type==PT_LOAD&&header.p_offset==0&&reinterpret_cast<const void*>(module->dlpi_addr+header.p_vaddr)==found->base)ours=true;}
            if(!ours)return 0;
            for(int i=0;i<module->dlpi_phnum;++i){const auto& header=module->dlpi_phdr[i];
                if(header.p_type!=PT_NOTE)continue;
                const auto* at=reinterpret_cast<const uint8_t*>(module->dlpi_addr+header.p_vaddr);const auto* end=at+header.p_memsz;
                while(at+12<=end){
                    uint32_t nameSize,descSize,type;std::memcpy(&nameSize,at,4);std::memcpy(&descSize,at+4,4);std::memcpy(&type,at+8,4);
                    const uint8_t* name=at+12;const uint8_t* desc=name+((nameSize+3u)&~3u);
                    if(desc+descSize>end)break;
                    if(type==NT_GNU_BUILD_ID&&nameSize==4&&std::memcmp(name,"GNU",4)==0){
                        static const char hex[]="0123456789abcdef";
                        for(uint32_t k=0;k<descSize;++k){found->id+=hex[desc[k]>>4];found->id+=hex[desc[k]&15];}
                        return 1;
                    }
                    at=desc+((descSize+3u)&~3u);
                }
            }
            return 1;
        },&search);
        return search.id;
    }();
    return id;
}
#ifdef __ANDROID__
// DXVK's command list processing for the session record (ci/dxvk_extra.py, build 11), and (build 13) the
// D3D9 Locks that waited for a buffer or texture the GPU still had: count, total and longest ms.
std::string dxvkQueueJson(){
    uint64_t t[26]{};
    size_t count=dxvkAndroidQueueTimings(t,26);
    if(count<26)return "{}";
    char text[960];
    std::snprintf(text,sizeof text,"{\"cleanupThread\":%s,\"submits\":[%llu,%.1f,%.2f],\"resets\":[%llu,%.1f,%.2f,%llu,%llu],"
        "\"resetStepsMs\":[[%.1f,%.2f],[%.1f,%.2f],[%.1f,%.2f],[%.1f,%.2f]],\"fenceWaits\":[%llu,%.1f],"
        "\"commandLists\":[%llu,%llu],\"descriptorPools\":[%llu,%llu],\"resourceWaits\":[%llu,%.1f,%.2f]}",
        d3d9CleanupThread()?"true":"false",
        static_cast<unsigned long long>(t[7]),t[8]/1e6,t[9]/1e6,
        static_cast<unsigned long long>(t[0]),t[1]/1e6,t[2]/1e6,static_cast<unsigned long long>(t[3]),static_cast<unsigned long long>(t[4]),
        t[15]/1e6,t[19]/1e6,t[16]/1e6,t[20]/1e6,t[17]/1e6,t[21]/1e6,t[18]/1e6,t[22]/1e6,
        static_cast<unsigned long long>(t[5]),t[6]/1e6,
        static_cast<unsigned long long>(t[10]),static_cast<unsigned long long>(t[11]),static_cast<unsigned long long>(t[13]),static_cast<unsigned long long>(t[14]),
        static_cast<unsigned long long>(t[23]),t[24]/1e6,t[25]/1e6);
    return text;
}
#endif
std::string guestTelemetryJson(bool live){
    uint64_t pipelines[5]{};
#ifdef __ANDROID__
    dxvkAndroidPipelineStats(pipelines);
#endif
    char tail[720];
    int32_t xruns=-1,bufferFrames=0,burst=0;unsigned rate=0,lost=0,replaced=0;uint64_t silentMs=0;
    // The size the game really renders at (its D3D9 back buffer), next to the size the launcher asked for.
    unsigned backBuffer[2]{};
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    d3d9BackBufferSize(backBuffer[0],backBuffer[1]);
#endif
#ifdef __ANDROID__
    {auto sound=soundDevice.report();xruns=sound.xruns;bufferFrames=sound.bufferFrames;burst=sound.burst;rate=sound.rate;
     lost=sound.lost;replaced=sound.replaced;silentMs=sound.silentMs;}
#endif
    // Build 15: streams Android took away ("lost"), the new ones opened for them, and how long the game ran without sound.
    std::snprintf(tail,sizeof tail,",\"pipelines\":{\"graphics\":%llu,\"compute\":%llu,\"totalMs\":%.1f,\"maxMs\":%.1f,\"over10Ms\":%llu},"
        "\"audio\":{\"xruns\":%d,\"bufferFrames\":%d,\"burstFrames\":%d,\"rate\":%u,\"lost\":%u,\"replaced\":%u,\"silentMs\":%llu},"
        "\"render\":{\"width\":%u,\"height\":%u,\"display\":[%u,%u],\"backBuffer\":[%u,%u]}",
        static_cast<unsigned long long>(pipelines[0]),static_cast<unsigned long long>(pipelines[1]),pipelines[2]/1e6,pipelines[3]/1e6,
        static_cast<unsigned long long>(pipelines[4]),xruns,bufferFrames,burst,rate,lost,replaced,static_cast<unsigned long long>(silentMs),
        renderWidth.load(),renderHeight.load(),displayWidth.load(),displayHeight.load(),backBuffer[0],backBuffer[1]);
    std::string json="{\"frames\":"+(live?frameTelemetry.liveJson():frameTelemetry.json())+tail+",\"host\":"+host_perf::statusJson(!live);
    if(!live)json+=",\"buildId\":\""+nativeBuildId()+"\"";
    if(!live){
        json+=",\"d3d9Calls\":"+d3d9CallCountsJson();
        json+=",\"d3d9Ns\":"+d3d9MethodTimeJson();
        json+=",\"d3d9Lookups\":"+d3d9LookupJson();
        json+=",\"longCalls\":"+callWatch.json(0x7e000000u);
#ifdef __ANDROID__
        {uint64_t queue[9]{};dxvkAndroidQueueState(queue);char text[96];
         std::snprintf(text,sizeof text,",\"vkPresent\":[%llu,%.1f,%.1f,%llu]",static_cast<unsigned long long>(queue[5]),queue[6]/1e6,queue[7]/1e6,static_cast<unsigned long long>(queue[8]));
         json+=text;}
        json+=",\"dxvkQueue\":"+dxvkQueueJson();
#endif
        json+=",\"profile\":"+guestProfile.json(kProfileIntervalUs);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        json+=",\"garage\":"+garageJson();
        json+=",\"rimSizes\":"+rimSizesJson();
        json+=",\"freeRoam\":"+freeRoamJson();
        json+=",\"effectStates\":"+effectStatesJson();
#endif
    }
    return json+"}";
}
void setGuestCpuPlacement(int placement){cpuPlacement=placement==0?0:placement==2?2:1;}   // host_perf::Placement
void setGuestReadHandover(bool overlapped){
    readHandover.store(overlapped,std::memory_order_relaxed);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Loader reads: %s",overlapped?"overlapped (reads of 64 KB and more hand the guest machine over while the kernel copies)":"atomic (every read holds the guest machine)");
}
void setGuestFrameLimit(unsigned framesPerSecond){
    frameLimit=(framesPerSecond>=30&&framesPerSecond<=240)?framesPerSecond:0;
    host_perf::setTargetFps(frameLimit.load());
    // The game paces itself to 1/60 s with a busy-wait (src/runtime/framerate.c): its target follows the
    // cap, and its time-step floor, tick rule, audio update, particle step and steering snap are corrected
    // for rates above 60 as in ThirteenAG's fix. With a cap the frame pacer below sleeps to it and the
    // game's own spin is switched off (in series the two made the spin fill the pacer's slack); uncapped,
    // the game's limiter alone paces at the 120 Hz panel's rate.
    unsigned gamePacing=frameLimit.load()?frameLimit.load():120;
    nfs_widescreen_set_frame_rate(gamePacing,frameLimit.load()!=0);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Android frame limit %u FPS (0 means uncapped); game pacing target %u FPS, %s",frameLimit.load(),gamePacing,
        frameLimit.load()?"the launcher sleeps to the cap (the game's own busy-wait off)":"the game's own busy-wait paces it");
}
void setGuestKey(unsigned scan,bool down){if(scan>=256)return;std::lock_guard<std::mutex> lock(inputMutex);if(bool(inputKeys[scan])==down)return;inputKeys[scan]=down?0x80:0;recordInputKey(scan,down);enqueueGuestKey(scan,down);}
uint64_t guestKeySequence(){std::lock_guard<std::mutex> lock(inputMutex);return inputSequence;}
uint64_t guestKeysDelivered(){return inputKeysDelivered.load(std::memory_order_relaxed);}
bool guestPeek(uint32_t va,void* out,size_t bytes){if(!memory||!bytes||!memory->readable(va,bytes))return false;std::memcpy(out,ptr(va,bytes),bytes);return true;}
void clearGuestInput(){std::lock_guard<std::mutex> lock(inputMutex);for(unsigned scan=0;scan<256;++scan)if(inputKeys[scan]){recordInputKey(scan,false);enqueueGuestKey(scan,false);}inputKeys.fill(0);inputMouseButtons.fill(0);inputMouseX=inputMouseY=inputMouseZ=0;touchButtons.clear();touchMoved=false;}
void setGuestPointer(float x,float y,int phase){
    guestPointerX=std::clamp(x,0.f,1.f);guestPointerY=std::clamp(y,0.f,1.f);
    {std::lock_guard<std::mutex> lock(inputMutex);touchMoved=true;if(phase!=1)touchButtons.push_back(phase==0?0x80:0);}
    enqueueGuestPointer(phase);
}
void typeGuestText(const std::vector<uint32_t>& characters){enqueueGuestText(characters);}
void setGuestOnlineServer(const std::string& host){std::lock_guard<MachineMutex> lock(machine);if(!memory&&!host.empty())onlineServer=host;}
void setGuestWidescreen(bool enabled){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestWidescreen=enabled;}
void setGuestWrongWayWarning(bool enabled){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestWrongWayWarning=enabled;}
void setGuestHdReflections(bool enabled){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestHdReflections=enabled;}
void setGuestEffectStates(bool host){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestEffectStates=host;}
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
void setGuestGarage(uint32_t features){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestGarage=features&NFS_GARAGE_ALL;}
void setGuestGeneratedDir(const std::string& directory){std::lock_guard<MachineMutex> lock(machine);if(!memory)generatedDataDir=directory;}
void setGuestFreeRoamDir(const std::string& directory){std::lock_guard<MachineMutex> lock(machine);if(!memory)freeRoamDir=directory;}
#endif
void setGuestLanguage(const char* language){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    for(const char* known:{"Spanish","English","French","German","Italian","Dutch","Swedish","Korean","Chinese"})
        if(language&&std::strcmp(known,language)==0){guestLanguage=known;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Selected guest language: %s",known);return;}
    guestLanguage="Spanish";
}
