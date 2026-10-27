#include "d3d9_bridge.h"
#include "runtime_log.h"
#include "dxt_shadow.h"
#include "vertex_formats.h"
#include "graphics_quality.h"
#include <d3d9.h>
#include <algorithm>
#include <map>
#include <type_traits>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

extern "C" IDirect3D9* Direct3DCreate9(UINT sdk);
extern "C" void dxvkAndroidSetWindow(ANativeWindow* window);
extern "C" HWND dxvkAndroidGetWindowHandle();
extern "C" void dxvkAndroidSetRenderSize(uint32_t width,uint32_t height);
void setD3D9RenderSize(unsigned width,unsigned height){dxvkAndroidSetRenderSize(width,height);}
#ifndef __ANDROID__
extern "C" ANativeWindow* dxvkCreateTestWindow();
extern "C" void dxvkDestroyTestWindow(ANativeWindow* window);
#endif
namespace {
constexpr uint32_t methodBase=0x7e000000;
enum class Kind:uint32_t {D3D9=1,Device=2,Texture=3,Cube=4,Surface=5,VertexBuffer=6,IndexBuffer=7,
    VertexShader=8,PixelShader=9,Declaration=10,StateBlock=11,Query=12,SwapChain=13,VolumeTexture=14,Volume=15};
struct Object {IUnknown* native;Kind kind;uint32_t references=1;CompressedView compressed;uint32_t parent=0;uint32_t children=0;};
std::unordered_map<uint32_t,Object> objects;
std::map<IUnknown*,uint32_t> addresses;
void* guestBase;
uint32_t (*allocateGuest)(uint32_t);
void (*freeGuest)(uint32_t);
std::atomic<ANativeWindow*> hostWindow{nullptr};
std::atomic<bool> surfaceRequested{false},surfaceAvailable{false};
// Session call counts per D3D9 method, indexed like the first-call log (kind*256+method); the
// telemetry JSON reports the non-zero ones so a frame's draw/state profile is visible in a report.
std::atomic<uint32_t> methodTotals[4096];
// A race frame makes ~45,000 device calls (Pixel 10 Pro XL telemetry: 7,200 SetSamplerState, 6,400
// SetRenderState, 4,300 SetVertexShaderConstantF...). The device handle is resolved without the hash
// map, and render/sampler/texture-stage states that repeat the current value return before DXVK (which
// would also drop them, after its own lock). The shadow is forgotten whenever something else could have
// changed device state: Reset, a state block, device loss.
uint32_t cachedDeviceHandle=0;IDirect3DDevice9* cachedDevice=nullptr;
constexpr unsigned kRenderStates=256,kSamplers=20,kSamplerStates=16,kStages=8,kStageStates=40;
uint32_t renderStateShadow[kRenderStates];bool renderStateKnown[kRenderStates];
uint32_t samplerShadow[kSamplers][kSamplerStates];bool samplerKnown[kSamplers][kSamplerStates];
uint32_t stageShadow[kStages][kStageStates];bool stageKnown[kStages][kStageStates];
uint64_t statesFiltered=0,statesForwarded=0;
// Between BeginStateBlock and EndStateBlock every setter must reach DXVK: it is recorded, not applied.
bool recordingStateBlock=false;
void forgetStateShadow(){
    std::memset(renderStateKnown,0,sizeof renderStateKnown);std::memset(samplerKnown,0,sizeof samplerKnown);std::memset(stageKnown,0,sizeof stageKnown);
}
void* pointer(uint32_t address){return address?static_cast<uint8_t*>(guestBase)+address:nullptr;}
void write(uint32_t address,uint32_t value){if(!address)throw std::runtime_error("Null D3D9 output");std::memcpy(pointer(address),&value,4);}
uint32_t wrap(IUnknown* native,Kind kind){
    if(!native)return 0;auto previous=addresses.find(native);if(previous!=addresses.end()){++objects.at(previous->second).references;return previous->second;}
    static constexpr uint32_t sizes[]={0,17,119,22,22,17,14,14,5,5,5,6,8,10,22,11};
    uint32_t count=sizes[static_cast<uint32_t>(kind)];
    uint32_t table=allocateGuest(count*4),object=allocateGuest(4);
    for(uint32_t i=0;i<count;++i)write(table+i*4,methodBase+static_cast<uint32_t>(kind)*4096+i*16);
    write(object,table);objects.emplace(object,Object{native,kind});addresses.emplace(native,object);return object;
}
// The guest handle's kind decides the interface: D3D9 interfaces form single
// inheritance chains from IUnknown, so once the kind matches, static_cast is
// exact. dynamic_cast on every SetTexture/SetStreamSource cost ~3% of a race.
template<class T>bool kindMatches(Kind kind){
    if constexpr(std::is_same_v<T,IDirect3DSurface9>)return kind==Kind::Surface;
    else if constexpr(std::is_same_v<T,IDirect3DBaseTexture9>)return kind==Kind::Texture||kind==Kind::Cube||kind==Kind::VolumeTexture;
    else if constexpr(std::is_same_v<T,IDirect3DTexture9>)return kind==Kind::Texture;
    else if constexpr(std::is_same_v<T,IDirect3DCubeTexture9>)return kind==Kind::Cube;
    else if constexpr(std::is_same_v<T,IDirect3DVertexBuffer9>)return kind==Kind::VertexBuffer;
    else if constexpr(std::is_same_v<T,IDirect3DIndexBuffer9>)return kind==Kind::IndexBuffer;
    else if constexpr(std::is_same_v<T,IDirect3DVertexShader9>)return kind==Kind::VertexShader;
    else if constexpr(std::is_same_v<T,IDirect3DPixelShader9>)return kind==Kind::PixelShader;
    else if constexpr(std::is_same_v<T,IDirect3DVertexDeclaration9>)return kind==Kind::Declaration;
    else return false;
}
template<class T>T* nativeObject(uint32_t address){
    if(!address)return nullptr;auto found=objects.find(address);if(found==objects.end())throw std::runtime_error("Invalid D3D9 resource handle");
    if(kindMatches<T>(found->second.kind))return static_cast<T*>(found->second.native);
    auto native=dynamic_cast<T*>(found->second.native);if(!native)throw std::runtime_error("D3D9 resource interface mismatch");return native;
}
uint32_t wrapTexture(IDirect3DBaseTexture9* texture){if(!texture)return 0;switch(texture->GetType()){case D3DRTYPE_TEXTURE:return wrap(texture,Kind::Texture);case D3DRTYPE_CUBETEXTURE:return wrap(texture,Kind::Cube);case D3DRTYPE_VOLUMETEXTURE:return wrap(texture,Kind::VolumeTexture);default:throw std::runtime_error("Unknown D3D9 base texture type");}}
void retireGuestObject(uint32_t address){
    auto found=objects.find(address);if(found==objects.end())return;
    if(address==cachedDeviceHandle){cachedDeviceHandle=0;cachedDevice=nullptr;forgetStateShadow();}
    uint32_t table;std::memcpy(&table,pointer(address),4);
    freeGuest(table);freeGuest(address);addresses.erase(found->second.native);objects.erase(found);
}
void attachTextureSurface(uint32_t texture,uint32_t surface){
    if(!surface)return;
    auto& child=objects.at(surface);
    if(!child.parent){child.parent=texture;++objects.at(texture).children;}
    else if(child.parent!=texture)throw std::runtime_error("D3D9 subresource changed parent");
}
// Underground 2 copies its initial depth surface from 0x870854 into the first
// render surface descriptor at 0x86e774, then releases both on device Reset.
// The copy does not call AddRef, so account for that owned descriptor here.
void retainInitialDepthDescriptor(uint32_t outputAddress,uint32_t surfaceAddress){
    if(outputAddress!=0x870854||!surfaceAddress)return;
    auto& surface=objects.at(surfaceAddress);surface.native->AddRef();++surface.references;
}
float floatValue(uint32_t bits){float value;std::memcpy(&value,&bits,4);return value;}
uint32_t floatBits(float value){uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
CompressedView compressionView(uint32_t address){auto found=objects.find(address);return found==objects.end()?CompressedView{}:found->second.compressed;}
// DXVK lets the CPU run at most BackBufferCount+1 frames ahead of the GPU.
// Fewer back buffers (the game's request is logged) serialise a CPU-heavy frame
// with its GPU work (Present waited 7-10 ms with the GPU 67% busy). Two
// back buffers allow DXVK's default latency of 3; the game only ever reads
// back buffer 0, so the extra buffers are invisible to it.
// Launcher "CPU/GPU sync": 2 (default) or 1 for the lowest input latency.
std::atomic<uint32_t> configuredBackBuffers{2};
// Launcher "Compressed textures: convert on the CPU": DXT decoded to RGBA8 even when the
// driver reports BC support, for drivers whose BC sampling is broken.
std::atomic<bool> forceCpuTextures{false};
// Launcher "Anti-aliasing": MSAA samples offered to D3D9 for the back buffer (0 = the game's own
// request, which is none). Validated per device against the adapter; see applyAntialiasing.
std::atomic<unsigned> requestedMultisample{0};
// Launcher "Texture filtering": dxvk.conf with d3d9.samplerAnisotropy, handed to DXVK through
// DXVK_CONFIG_FILE right before Direct3DCreate9 (DXVK 1.9.2 reads it once, at interface creation).
std::mutex qualityMutex;
std::string dxvkConfigPath;
// Launcher "Performance overlay: full": DXVK_HUD (fps, frame-time graph, GPU load...), read by DXVK when
// the swap chain is created. Empty = no HUD.
std::string dxvkHud;
// What applyAntialiasing settled on for the current device, so a depth surface the game creates for
// the back buffer gets the same sample count (D3D9 requires render target and depth to match).
std::atomic<unsigned> activeMultisample{0};
std::atomic<uint32_t> activeBackBufferWidth{0},activeBackBufferHeight{0};
// MSAA is only valid on a DISCARD swap chain, and the game must not read the back buffer back.
std::atomic<bool> activeAutoDepth{true};
void applyAntialiasing(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,D3DPRESENT_PARAMETERS& p){
    // Whatever happens below describes the device about to exist: start from "no MSAA".
    activeMultisample=0;activeBackBufferWidth=p.BackBufferWidth;activeBackBufferHeight=p.BackBufferHeight;activeAutoDepth=p.EnableAutoDepthStencil!=0;
    unsigned wanted=requestedMultisample.load();
    if(!wanted||p.MultiSampleType!=D3DMULTISAMPLE_NONE)return;
    auto supported=[&](unsigned samples){
        DWORD quality=0;auto level=static_cast<D3DMULTISAMPLE_TYPE>(samples);
        if(FAILED(d3d->CheckDeviceMultiSampleType(adapter,type,p.BackBufferFormat,p.Windowed,level,&quality)))return false;
        return !p.EnableAutoDepthStencil||SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter,type,p.AutoDepthStencilFormat,p.Windowed,level,&quality));
    };
    unsigned samples=graphics_quality::chooseMultisample(wanted,supported);
    static unsigned logged=~0u;
    if(logged!=samples){logged=samples;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Anti-aliasing: %ux MSAA requested, %ux usable on this GPU for format %u/%u",wanted,samples,unsigned(p.BackBufferFormat),unsigned(p.AutoDepthStencilFormat));}
    activeMultisample=samples;
    if(!samples)return;
    p.MultiSampleType=static_cast<D3DMULTISAMPLE_TYPE>(samples);p.MultiSampleQuality=0;p.SwapEffect=D3DSWAPEFFECT_DISCARD;
}
uint32_t hostBackBufferCount(uint32_t requested){
    uint32_t count=std::max(requested,configuredBackBuffers.load());
    static bool logged=false;
    if(!logged){logged=true;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 game back buffers %u -> host %u",requested,count);}
    return count;
}
D3DPRESENT_PARAMETERS presentation(uint32_t address){
    auto p=static_cast<const uint32_t*>(pointer(address));if(!p)throw std::runtime_error("Null D3D9 presentation parameters");
    D3DPRESENT_PARAMETERS result{};result.BackBufferWidth=p[0];result.BackBufferHeight=p[1];result.BackBufferFormat=static_cast<D3DFORMAT>(p[2]);result.BackBufferCount=hostBackBufferCount(p[3]);
    result.MultiSampleType=static_cast<D3DMULTISAMPLE_TYPE>(p[4]);result.MultiSampleQuality=p[5];result.SwapEffect=static_cast<D3DSWAPEFFECT>(p[6]);result.hDeviceWindow=dxvkAndroidGetWindowHandle();
    result.Windowed=p[8];result.EnableAutoDepthStencil=p[9];result.AutoDepthStencilFormat=static_cast<D3DFORMAT>(p[10]);result.Flags=p[11];result.FullScreen_RefreshRateInHz=p[12];result.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;return result;
}
#include "d3d9_locks.inc"
bool compressedFormat(D3DFORMAT format){return format>=D3DFMT_DXT1&&format<=D3DFMT_DXT5;}
D3DFORMAT nativeTextureFormat(IDirect3DDevice9* device,D3DFORMAT format,DWORD usage,D3DRESOURCETYPE type){
    if(!compressedFormat(format)||(usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return format;
    if(forceCpuTextures.load())return D3DFMT_A8B8G8R8;
    IDirect3D9* d3d=nullptr;D3DDEVICE_CREATION_PARAMETERS parameters{};D3DDISPLAYMODE mode{};
    if(FAILED(device->GetDirect3D(&d3d)))return format;
    device->GetCreationParameters(&parameters);d3d->GetAdapterDisplayMode(parameters.AdapterOrdinal,&mode);
    HRESULT supported=d3d->CheckDeviceFormat(parameters.AdapterOrdinal,parameters.DeviceType,mode.Format,usage,type,format);d3d->Release();
    return SUCCEEDED(supported)?format:D3DFMT_A8B8G8R8;
}
void describeOriginal(uint32_t address,D3DSURFACE_DESC* desc){auto view=compressionView(address);if(view.texture&&desc)desc->Format=static_cast<D3DFORMAT>(view.texture->format);}
}
std::string d3d9CallCountsJson(){
    std::string json="{";bool first=true;
    for(unsigned index=0;index<4096;++index){
        uint32_t count=methodTotals[index].load(std::memory_order_relaxed);if(!count)continue;
        char item[48];std::snprintf(item,sizeof item,"%s\"%u/%u\":%u",first?"":",",index/256,index%256,count);json+=item;first=false;
    }
    return json+"}";
}
void setD3D9Compatibility(bool cpuTextures,unsigned backBuffers){
    forceCpuTextures=cpuTextures;configuredBackBuffers=backBuffers<1?1u:backBuffers>3?3u:backBuffers;
}
void setD3D9GraphicsQuality(unsigned anisotropy,unsigned msaaSamples,const std::string& configDirectory){
    requestedMultisample=msaaSamples;
    std::string text=graphics_quality::dxvkConfig(anisotropy),path;
    if(!text.empty()&&!configDirectory.empty()){
        path=configDirectory+"/dxvk.conf";
        std::ofstream file(path,std::ios::trunc);file<<text;
        if(!file){NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Could not write %s; texture filtering stays at the game's own",path.c_str());path.clear();}
    }
    {std::lock_guard<std::mutex> lock(qualityMutex);dxvkConfigPath=path;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Graphics quality for this game session: anisotropy %u (%s, read by DXVK at Direct3DCreate9), MSAA %u requested (applied at CreateDevice/Reset)",graphics_quality::clampAnisotropy(anisotropy),path.empty()?"not forced":path.c_str(),msaaSamples);
}
void setD3D9Hud(const std::string& config){
    {std::lock_guard<std::mutex> lock(qualityMutex);dxvkHud=config;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","DXVK HUD for this game session: %s",config.empty()?"off":config.c_str());
}
void configureD3D9Bridge(void* base,uint32_t (*allocator)(uint32_t),void (*deallocator)(uint32_t)){
    guestBase=base;allocateGuest=allocator;freeGuest=deallocator;
#ifdef __ANDROID__
    // android_main provides a writable private cache before creating DXVK.
    // Other bring-up callers can still run without a writable working directory.
    if(!std::getenv("DXVK_STATE_CACHE_PATH"))setenv("DXVK_STATE_CACHE","0",1);
    setenv("DXVK_LOG_PATH","none",1);
#endif
#ifndef __ANDROID__
    if(std::getenv("NFS_D3D9_TEST_DISPLAY")){auto window=dxvkCreateTestWindow();if(!window)throw std::runtime_error("Xlib test display unavailable");setD3D9HostWindow(window);acknowledgeD3D9Surface(true);}
#endif
}
extern bool deviceResetPending;  // defined below, with the Reset handling
void shutdownD3D9Bridge(){
    for(auto& lock:resourceLocks)freeGuest(lock.second.guest);resourceLocks.clear();
    for(uint32_t kind=15;kind;--kind)for(auto it=objects.begin();it!=objects.end();){
        if(static_cast<uint32_t>(it->second.kind)!=kind){++it;continue;}
        auto object=it->second;for(uint32_t n=0;n<object.references;++n)object.native->Release();
        uint32_t table;std::memcpy(&table,pointer(it->first),4);freeGuest(table);freeGuest(it->first);addresses.erase(object.native);it=objects.erase(it);
    }
    surfaceRequested=false;surfaceAvailable=false;
    activeMultisample=0;activeBackBufferWidth=0;activeBackBufferHeight=0;activeAutoDepth=true;deviceResetPending=false;
    cachedDeviceHandle=0;cachedDevice=nullptr;recordingStateBlock=false;forgetStateShadow();
    if(statesFiltered||statesForwarded)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 state calls this session: %llu repeated the current value (dropped here), %llu forwarded",
        static_cast<unsigned long long>(statesFiltered),static_cast<unsigned long long>(statesForwarded));
    statesFiltered=statesForwarded=0;
#ifndef __ANDROID__
    if(std::getenv("NFS_D3D9_TEST_DISPLAY")){auto window=hostWindow.load();setD3D9HostWindow(nullptr);dxvkDestroyTestWindow(window);}
#endif
}
void setD3D9HostWindow(ANativeWindow* window){hostWindow=window;dxvkAndroidSetWindow(window);}
bool d3d9RequestsSurface(){return surfaceRequested;}
void acknowledgeD3D9Surface(bool available){surfaceAvailable=available;}
uint32_t createGuestD3D9(uint32_t sdk){
    {   // Same thread that creates the interface: DXVK reads DXVK_CONFIG_FILE inside Direct3DCreate9.
        std::lock_guard<std::mutex> lock(qualityMutex);
        if(dxvkConfigPath.empty())unsetenv("DXVK_CONFIG_FILE");else setenv("DXVK_CONFIG_FILE",dxvkConfigPath.c_str(),1);
        if(dxvkHud.empty())unsetenv("DXVK_HUD");else setenv("DXVK_HUD",dxvkHud.c_str(),1);
    }
    auto native=Direct3DCreate9(sdk);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","DXVK native Direct3DCreate9 sdk=%u",sdk);
    return wrap(native,Kind::D3D9);
}
// Logs each vertex declaration type the first time the game uses it, flagging the
// ones this GPU cannot fetch (see vertex_formats.h): the evidence for stretched geometry.
void noteVertexDeclaration(const D3DVERTEXELEMENT9* elements){
    static bool seen[32]{};
    for(unsigned i=0;elements&&i<64&&elements[i].Stream!=0xff;++i){
        unsigned type=elements[i].Type;
        if(type>=32||seen[type])continue;
        seen[type]=true;
        const char* name="?";for(auto& entry:kD3DVertexFormats)if(entry.type==type)name=entry.name;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 game vertex type %s (usage %u)",name,unsigned(elements[i].Usage));
    }
}
// Speed.exe marks which of its 4:3 modes (640x480 ... 1280x1024) the adapter lists
// and then scans upward, without a bound, for the first one present; on Android
// DXVK lists only the panel's own size. The swapchain is scaled to the surface
// anyway, so the classic modes are offered first, then the native ones.
std::vector<D3DDISPLAYMODE> adapterModes(IDirect3D9* d3d,UINT adapter,D3DFORMAT format){
    std::vector<D3DDISPLAYMODE> modes;
    if(format==D3DFMT_X8R8G8B8||format==D3DFMT_R5G6B5)
        for(auto size:{std::pair<UINT,UINT>{640,480},{800,600},{1024,768},{1280,960},{1280,1024}})modes.push_back({size.first,size.second,60,format});
    UINT count=d3d->GetAdapterModeCount(adapter,format);
    for(UINT i=0;i<count;++i){D3DDISPLAYMODE mode{};
        if(FAILED(d3d->EnumAdapterModes(adapter,format,i,&mode)))continue;
        if(std::none_of(modes.begin(),modes.end(),[&](const D3DDISPLAYMODE& m){return m.Width==mode.Width&&m.Height==mode.Height&&m.RefreshRate==mode.RefreshRate;}))modes.push_back(mode);}
    return modes;
}
// Set when DXVK could not rebuild the swap chain in Reset. DXVK's TestCooperativeLevel
// always reports D3D_OK, so the game would keep drawing on the half-reset device and
// crash (null dereference, Mali reports); until a Reset succeeds the device reads as
// lost and needing a reset, which the game handles as on Windows.
bool deviceResetPending=false;
uint32_t dispatchGuestD3D9(uint32_t token,const uint32_t* args,uint32_t* argumentCount){
    uint32_t kind=(token-methodBase)/4096,method=((token-methodBase)%4096)/16;
    methodTotals[((token-methodBase)/16)&4095].fetch_add(1,std::memory_order_relaxed);
    // Device state setters: the hot path. Resolved without the map and filtered against the shadow.
    if(kind==2&&args[0]==cachedDeviceHandle&&cachedDevice&&!recordingStateBlock){
        switch(method){
        case 57:{*argumentCount=3;uint32_t state=args[1],value=args[2];
            if(state<kRenderStates){if(renderStateKnown[state]&&renderStateShadow[state]==value){++statesFiltered;return D3D_OK;}renderStateShadow[state]=value;renderStateKnown[state]=true;}
            ++statesForwarded;return cachedDevice->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state),static_cast<DWORD>(value));}
        case 69:{*argumentCount=4;uint32_t sampler=args[1],state=args[2],value=args[3];
            // D3DDMAPSAMPLER and the vertex texture samplers (257..260) map to 16..19.
            unsigned slot=sampler<16?sampler:sampler==256?16:sampler>=257&&sampler<=259?sampler-257+17:kSamplers;
            if(slot<kSamplers&&state<kSamplerStates){if(samplerKnown[slot][state]&&samplerShadow[slot][state]==value){++statesFiltered;return D3D_OK;}samplerShadow[slot][state]=value;samplerKnown[slot][state]=true;}
            ++statesForwarded;return cachedDevice->SetSamplerState(static_cast<DWORD>(sampler),static_cast<D3DSAMPLERSTATETYPE>(state),static_cast<DWORD>(value));}
        case 67:{*argumentCount=4;uint32_t stage=args[1],state=args[2],value=args[3];
            if(stage<kStages&&state<kStageStates){if(stageKnown[stage][state]&&stageShadow[stage][state]==value){++statesFiltered;return D3D_OK;}stageShadow[stage][state]=value;stageKnown[stage][state]=true;}
            ++statesForwarded;return cachedDevice->SetTextureStageState(static_cast<DWORD>(stage),static_cast<D3DTEXTURESTAGESTATETYPE>(state),static_cast<DWORD>(value));}
        default:break;
        }
    }
    auto found=objects.find(args[0]);
    if(found==objects.end()||static_cast<uint32_t>(found->second.kind)!=kind){
        NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU1","Invalid D3D9 COM call object=%08x token=%08x kind=%u method=%u actualKind=%u",args[0],token,kind,method,
            found==objects.end()?0:static_cast<uint32_t>(found->second.kind));
        throw std::runtime_error("Invalid guest D3D9 COM object");
    }
    auto object=found->second.native;
    if(kind==2&&args[0]!=cachedDeviceHandle){cachedDeviceHandle=args[0];cachedDevice=static_cast<IDirect3DDevice9*>(object);forgetStateShadow();}
    // State blocks apply state behind the shadow's back: forget it around their methods, and record
    // every setter while one is being built (BeginStateBlock 60 .. EndStateBlock 61).
    if(kind==11)forgetStateShadow();
    if(kind==2&&(method==60||method==61)){recordingStateBlock=method==60;forgetStateShadow();}
    // Android may replace ANativeWindow after backgrounding, even at the same
    // pointer address. Override the destination with this surface generation so
    // DXVK recreates its presenter while keeping the game's device/resources.
    if((kind==2&&method==17)||(kind==13&&method==3)){
        *argumentCount=kind==2?5:6;auto window=dxvkAndroidGetWindowHandle();
        if(!window||deviceResetPending)return D3DERR_DEVICELOST;
        auto source=static_cast<const RECT*>(pointer(args[1])),destination=static_cast<const RECT*>(pointer(args[2]));
        auto dirty=static_cast<const RGNDATA*>(pointer(args[4]));
        return kind==2?static_cast<IDirect3DDevice9*>(object)->Present(source,destination,window,dirty):
            static_cast<IDirect3DSwapChain9*>(object)->Present(source,destination,window,dirty,args[5]);
    }
    // Log the first calls of each method; tokens span 0x7e000000..0x7e00ffff.
    static uint8_t methodCalls[4096];auto& calls=methodCalls[((token-methodBase)/16)&4095];
    if(calls<3&&++calls)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 COM kind=%u method=%u",kind,method);
    // Counts belong to the guest COM wrapper. DXVK texture subresources
    // delegate AddRef/Release to their parent texture, so native counts can
    // include ownership through a different guest object. Returning them made
    // the game's release-until-zero loop call an already freed surface wrapper.
    if(method==1){*argumentCount=1;object->AddRef();return ++found->second.references;}
    if(method==2){*argumentCount=1;
        // The game's detail rebuild releases the cube parent after every face
        // lookup, although only its first Release owns a parent reference.
        // Keep the callable wrapper until the acquired faces are released.
        if(!found->second.references){
            if(found->second.children)return 0;
            throw std::runtime_error("D3D9 COM Release without owned reference");
        }
        auto nativeReferences=object->Release();uint32_t references=--found->second.references;
        if(kind==5)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Surface Release object=%08x guestRefs=%u nativeRefs=%u",args[0],references,uint32_t(nativeReferences));
        if(!references){
            if(found->second.children)return uint32_t(nativeReferences);
            auto parent=found->second.parent;retireGuestObject(args[0]);
            if(parent){auto owner=objects.find(parent);if(owner!=objects.end()&&!--owner->second.children&&!owner->second.references)retireGuestObject(parent);}
        }
        return references;}
    if(method==0){
        *argumentCount=3;IUnknown* result=nullptr;HRESULT status=object->QueryInterface(*static_cast<const GUID*>(pointer(args[1])),reinterpret_cast<void**>(&result));
        if(SUCCEEDED(status))write(args[2],wrap(result,found->second.kind));else write(args[2],0);return status;
    }
    if(kind==1){
        auto d3d=static_cast<IDirect3D9*>(object);
        switch(method){
        case 3:*argumentCount=2;return D3DERR_NOTAVAILABLE;
        case 4:*argumentCount=1;return d3d->GetAdapterCount();
        case 5:*argumentCount=4;return d3d->GetAdapterIdentifier(args[1],args[2],static_cast<D3DADAPTER_IDENTIFIER9*>(pointer(args[3])));
        case 6:*argumentCount=3;return uint32_t(adapterModes(d3d,args[1],static_cast<D3DFORMAT>(args[2])).size());
        case 7:{*argumentCount=5;auto modes=adapterModes(d3d,args[1],static_cast<D3DFORMAT>(args[2]));
            if(args[3]>=modes.size())return D3DERR_INVALIDCALL;
            std::memcpy(pointer(args[4]),&modes[args[3]],sizeof(D3DDISPLAYMODE));return D3D_OK;}
        case 8:*argumentCount=3;return d3d->GetAdapterDisplayMode(args[1],static_cast<D3DDISPLAYMODE*>(pointer(args[2])));
        case 9:*argumentCount=6;return d3d->CheckDeviceType(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]),args[5]);
        case 10:{*argumentCount=7;auto format=static_cast<D3DFORMAT>(args[6]);auto type=static_cast<D3DRESOURCETYPE>(args[5]);
            HRESULT status=d3d->CheckDeviceFormat(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),args[4],type,format);
            if(FAILED(status)&&compressedFormat(format)&&!(args[4]&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))&&(type==D3DRTYPE_TEXTURE||type==D3DRTYPE_CUBETEXTURE||type==D3DRTYPE_SURFACE))
                status=d3d->CheckDeviceFormat(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),args[4],type,D3DFMT_A8B8G8R8);
            return status;}
        case 11:*argumentCount=7;return d3d->CheckDeviceMultiSampleType(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),args[4],static_cast<D3DMULTISAMPLE_TYPE>(args[5]),static_cast<DWORD*>(pointer(args[6])));
        case 12:*argumentCount=6;return d3d->CheckDepthStencilMatch(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]),static_cast<D3DFORMAT>(args[5]));
        case 13:*argumentCount=5;return d3d->CheckDeviceFormatConversion(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]));
        case 14:*argumentCount=4;return d3d->GetDeviceCaps(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DCAPS9*>(pointer(args[3])));
        case 15:*argumentCount=2;return d3d->GetAdapterMonitor(args[1])?0xf0000300:0;
        case 16:{
            *argumentCount=7;auto p=static_cast<const uint32_t*>(pointer(args[5]));if(!p||!args[6])return D3DERR_INVALIDCALL;
            if(!hostWindow.load())throw std::runtime_error("D3D9 CreateDevice requires Android surface");
            surfaceRequested=true;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!surfaceAvailable){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Android surface handover timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            auto window=dxvkAndroidGetWindowHandle();if(!window)return D3DERR_DEVICELOST;
            D3DPRESENT_PARAMETERS parameters{};parameters.BackBufferWidth=p[0];parameters.BackBufferHeight=p[1];parameters.BackBufferFormat=static_cast<D3DFORMAT>(p[2]);parameters.BackBufferCount=hostBackBufferCount(p[3]);
            parameters.MultiSampleType=static_cast<D3DMULTISAMPLE_TYPE>(p[4]);parameters.MultiSampleQuality=p[5];parameters.SwapEffect=static_cast<D3DSWAPEFFECT>(p[6]);parameters.hDeviceWindow=window;
            parameters.Windowed=p[8];parameters.EnableAutoDepthStencil=p[9];parameters.AutoDepthStencilFormat=static_cast<D3DFORMAT>(p[10]);parameters.Flags=p[11];parameters.FullScreen_RefreshRateInHz=p[12];parameters.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
            applyAntialiasing(d3d,args[1],static_cast<D3DDEVTYPE>(args[2]),parameters);
            IDirect3DDevice9* device=nullptr;HRESULT status=d3d->CreateDevice(args[1],static_cast<D3DDEVTYPE>(args[2]),window,args[4],&parameters,&device);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 backbuffer %ux%u CreateDevice result=%08x",parameters.BackBufferWidth,parameters.BackBufferHeight,uint32_t(status));
            // Without a device the game carries on and faults later in its own
            // code (PowerVR GE8322 reports): after its retry also fails, stop
            // here with the reason instead.
            static unsigned failures=0;failures=FAILED(status)?failures+1:0;
            if(FAILED(status)&&failures<2)return status;
            if(FAILED(status))throw std::runtime_error("The GPU could not create the graphics device (Vulkan, CreateDevice "+
                std::to_string(uint32_t(status))+"). Use \"Test driver\" in the launcher to see what it lacks.");
            write(args[6],wrap(device,Kind::Device));return status;
        }
        }
    }
    if(kind==2){auto device=static_cast<IDirect3DDevice9*>(object);switch(method){
        case 9:{*argumentCount=2;D3DDEVICE_CREATION_PARAMETERS p{};HRESULT status=device->GetCreationParameters(&p);if(SUCCEEDED(status)){write(args[1],p.AdapterOrdinal);write(args[1]+4,p.DeviceType);write(args[1]+8,0xf0000301);write(args[1]+12,p.BehaviorFlags);}return status;}
        case 13:{*argumentCount=3;auto p=presentation(args[1]);
            {IDirect3D9* d3d=nullptr;D3DDEVICE_CREATION_PARAMETERS creation{};unsigned keepSamples=activeMultisample.load(),keepW=activeBackBufferWidth.load(),keepH=activeBackBufferHeight.load();bool keepAuto=activeAutoDepth.load();
             if(SUCCEEDED(device->GetDirect3D(&d3d))&&d3d){if(SUCCEEDED(device->GetCreationParameters(&creation)))applyAntialiasing(d3d,creation.AdapterOrdinal,creation.DeviceType,p);d3d->Release();}
             // An additional swap chain does not change which back buffer the game's depth surfaces belong to.
             activeMultisample=keepSamples;activeBackBufferWidth=keepW;activeBackBufferHeight=keepH;activeAutoDepth=keepAuto;}
            IDirect3DSwapChain9* result=nullptr;HRESULT status=device->CreateAdditionalSwapChain(&p,&result);write(args[2],SUCCEEDED(status)?wrap(result,Kind::SwapChain):0);return status;}
        case 16:{*argumentCount=2;auto p=presentation(args[1]);HRESULT status=D3DERR_DEVICELOST;
            {IDirect3D9* d3d=nullptr;D3DDEVICE_CREATION_PARAMETERS creation{};
             if(SUCCEEDED(device->GetDirect3D(&d3d))&&d3d){if(SUCCEEDED(device->GetCreationParameters(&creation)))applyAntialiasing(d3d,creation.AdapterOrdinal,creation.DeviceType,p);d3d->Release();}}
            // DXVK throws (DxvkError) when it cannot rebuild the swap chain; Windows
            // would report a lost device, which the game retries (Mali G615 report).
            // The old Android window can stay busy for a moment: retry once before giving up.
            for(int attempt=0;attempt<2;++attempt){
                try{status=device->Reset(&p);break;}catch(...){
                    NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU1","D3D9 Reset raised a DXVK error (attempt %d)",attempt+1);status=D3DERR_DEVICELOST;
                    if(!attempt)std::this_thread::sleep_for(std::chrono::milliseconds(100));}
            }
            deviceResetPending=status==D3DERR_DEVICELOST;forgetStateShadow();
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 Reset backbuffer %ux%u result=%08x",p.BackBufferWidth,p.BackBufferHeight,uint32_t(status));return status;}
        case 23:{*argumentCount=9;if(args[8])throw std::runtime_error("Shared texture handle pending");if(!args[7])return D3DERR_INVALIDCALL;
            auto original=static_cast<D3DFORMAT>(args[5]),format=nativeTextureFormat(device,original,args[4],D3DRTYPE_TEXTURE);IDirect3DTexture9* texture=nullptr;
            HRESULT status=device->CreateTexture(args[1],args[2],args[3],args[4],format,static_cast<D3DPOOL>(args[6]),&texture,nullptr);
            uint32_t address=SUCCEEDED(status)?wrap(texture,Kind::Texture):0;
            if(address&&format!=original){objects.at(address).compressed.texture=std::make_shared<CompressedTexture>(original,args[1],args[2],texture->GetLevelCount(),1);NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","CPU DXT fallback texture %ux%u format=%u",args[1],args[2],original);}
            write(args[7],address);return status;}
        case 25:{*argumentCount=8;if(args[7])throw std::runtime_error("Shared cube handle pending");if(!args[6])return D3DERR_INVALIDCALL;
            auto original=static_cast<D3DFORMAT>(args[4]),format=nativeTextureFormat(device,original,args[3],D3DRTYPE_CUBETEXTURE);IDirect3DCubeTexture9* texture=nullptr;
            HRESULT status=device->CreateCubeTexture(args[1],args[2],args[3],format,static_cast<D3DPOOL>(args[5]),&texture,nullptr);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","D3D9 CreateCubeTexture edge=%u levels=%u usage=%08x format=%u nativeFormat=%u pool=%u status=%08x output=%08x",args[1],args[2],args[3],unsigned(original),unsigned(format),args[5],uint32_t(status),args[6]);
            uint32_t address=SUCCEEDED(status)?wrap(texture,Kind::Cube):0;
            if(address&&format!=original)objects.at(address).compressed.texture=std::make_shared<CompressedTexture>(original,args[1],args[1],texture->GetLevelCount(),6);
            write(args[6],address);return status;}
        case 57:{*argumentCount=3;++statesForwarded;return device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(args[1]),static_cast<DWORD>(args[2]));}
        case 67:{*argumentCount=4;++statesForwarded;return device->SetTextureStageState(static_cast<DWORD>(args[1]),static_cast<D3DTEXTURESTAGESTATETYPE>(args[2]),static_cast<DWORD>(args[3]));}
        case 69:{*argumentCount=4;++statesForwarded;return device->SetSamplerState(static_cast<DWORD>(args[1]),static_cast<D3DSAMPLERSTATETYPE>(args[2]),static_cast<DWORD>(args[3]));}
        case 29:{*argumentCount=9;IDirect3DSurface9* surface=nullptr;if(args[8])throw std::runtime_error("D3D9 shared resource handles pending");
            auto samples=static_cast<D3DMULTISAMPLE_TYPE>(args[4]);DWORD quality=args[5];unsigned active=activeMultisample.load();
            // When the device has no automatic depth buffer, the full-size depth surface the game creates is
            // the back buffer's and must carry its sample count. With an automatic one, extra depth surfaces
            // belong to offscreen passes and are left alone (a mismatch there would break those passes).
            if(active&&!activeAutoDepth.load()&&samples==D3DMULTISAMPLE_NONE&&args[1]==activeBackBufferWidth.load()&&args[2]==activeBackBufferHeight.load()){
                IDirect3D9* d3d=nullptr;D3DDEVICE_CREATION_PARAMETERS creation{};DWORD levels=0;
                if(SUCCEEDED(device->GetDirect3D(&d3d))&&d3d){
                    if(SUCCEEDED(device->GetCreationParameters(&creation))&&SUCCEEDED(d3d->CheckDeviceMultiSampleType(creation.AdapterOrdinal,creation.DeviceType,static_cast<D3DFORMAT>(args[3]),TRUE,static_cast<D3DMULTISAMPLE_TYPE>(active),&levels))){
                        samples=static_cast<D3DMULTISAMPLE_TYPE>(active);quality=0;
                        static bool logged=false;if(!logged){logged=true;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Depth surface %ux%u format %u follows the back buffer at %ux MSAA",args[1],args[2],args[3],active);}
                    }
                    d3d->Release();
                }
            }
            HRESULT status=device->CreateDepthStencilSurface(static_cast<UINT>(args[1]),static_cast<UINT>(args[2]),static_cast<D3DFORMAT>(args[3]),samples,quality,static_cast<WINBOOL>(args[6]),args[7]?&surface:nullptr,nullptr);
            if(args[7])write(args[7],SUCCEEDED(status)?wrap(surface,Kind::Surface):0);return status;}
#include "d3d9_device_methods.inc"
    }}
    if(kind==3){auto texture=static_cast<IDirect3DTexture9*>(object);
        if(method==17){*argumentCount=3;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[2]));HRESULT status=texture->GetLevelDesc(args[1],desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==18){*argumentCount=3;IDirect3DSurface9* surface=nullptr;HRESULT status=texture->GetSurfaceLevel(args[1],&surface);uint32_t address=SUCCEEDED(status)?wrap(surface,Kind::Surface):0;if(address){attachTextureSurface(args[0],address);auto view=compressionView(args[0]);view.level=args[1];objects.at(address).compressed=view;}write(args[2],address);return status;}
        if(method==19){*argumentCount=5;return lockTexture(args[0],args[1],args[2],static_cast<const RECT*>(pointer(args[3])),args[4]);}
        if(method==20){*argumentCount=2;copyLockBack(args[0],args[1]);HRESULT status=texture->UnlockRect(args[1]);releaseLock(args[0],args[1]);return status;}}
    if(kind==4){auto texture=static_cast<IDirect3DCubeTexture9*>(object);
        if(method==17){*argumentCount=3;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[2]));HRESULT status=texture->GetLevelDesc(args[1],desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==18){*argumentCount=4;IDirect3DSurface9* surface=nullptr;HRESULT status=texture->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(args[1]),args[2],&surface);uint32_t address=SUCCEEDED(status)?wrap(surface,Kind::Surface):0;if(address){attachTextureSurface(args[0],address);auto view=compressionView(args[0]);view.face=args[1];view.level=args[2];objects.at(address).compressed=view;}write(args[3],address);return status;}
        if(method==19){*argumentCount=6;return lockCube(args[0],args[1],args[2],args[3],static_cast<const RECT*>(pointer(args[4])),args[5]);}
        if(method==20){*argumentCount=3;uint32_t subresource=args[1]*32+args[2];copyLockBack(args[0],subresource);HRESULT status=texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]),args[2]);releaseLock(args[0],subresource);return status;}}
    if(kind==5){auto surface=static_cast<IDirect3DSurface9*>(object);
        if(method==12){*argumentCount=2;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[1]));HRESULT status=surface->GetDesc(desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==13){*argumentCount=4;return lockSurface(args[0],args[1],static_cast<const RECT*>(pointer(args[2])),args[3]);}
        if(method==14){*argumentCount=1;copyLockBack(args[0],0);HRESULT status=surface->UnlockRect();releaseLock(args[0],0);return status;}}
    if(kind==6||kind==7){
        if(method==11){*argumentCount=5;return kind==6?lockBuffer<IDirect3DVertexBuffer9,D3DVERTEXBUFFER_DESC>(args[0],args[1],args[2],args[3],args[4]):lockBuffer<IDirect3DIndexBuffer9,D3DINDEXBUFFER_DESC>(args[0],args[1],args[2],args[3],args[4]);}
        if(method==12){*argumentCount=1;copyLockBack(args[0],0);HRESULT status=kind==6?static_cast<IDirect3DVertexBuffer9*>(object)->Unlock():static_cast<IDirect3DIndexBuffer9*>(object)->Unlock();releaseLock(args[0],0);return status;}}
#include "d3d9_resource_methods.inc"
    throw std::runtime_error("D3D9 guest method pending: kind="+std::to_string(kind)+" slot="+std::to_string(method));
}
