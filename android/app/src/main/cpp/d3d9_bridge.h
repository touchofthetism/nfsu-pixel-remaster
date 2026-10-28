#pragma once
#include <cstdint>
#include <string>
struct ANativeWindow;
void configureD3D9Bridge(void* guestBase,uint32_t (*allocator)(uint32_t),void (*deallocator)(uint32_t));
void shutdownD3D9Bridge();
void setD3D9HostWindow(ANativeWindow* window);
void setD3D9RenderSize(unsigned width,unsigned height);
// Launcher compatibility options; call before the game creates its device.
void setD3D9Compatibility(bool cpuTextures,unsigned backBuffers);
// Launcher graphics quality: anisotropic filtering (0 = game default, else 2..16) and MSAA samples
// (0 = off); configDirectory receives dxvk.conf. Call before the game creates Direct3D.
void setD3D9GraphicsQuality(unsigned anisotropy,unsigned msaaSamples,const std::string& configDirectory);
// Launcher performance overlay: DXVK_HUD configuration ("" = none). Call before the game creates Direct3D.
void setD3D9Hud(const std::string& config);
// Session call counts per D3D9 method as a JSON object {"kind/method": count}; see guestTelemetryJson.
std::string d3d9CallCountsJson();
// Time spent per D3D9 method (bridge and DXVK) as {"kind/method": nanoseconds}; d3dDispatch adds to it.
void d3d9AddMethodTime(uint32_t token,uint64_t nanos);
std::string d3d9MethodTimeJson();
// The back buffer of the game's device as last created or reset (0x0 before the first): the size the game
// renders at, which the launcher's internal resolution is meant to decide.
void d3d9BackBufferSize(unsigned& width,unsigned& height);
bool d3d9RequestsSurface();
void acknowledgeD3D9Surface(bool available);
uint32_t createGuestD3D9(uint32_t sdk);
uint32_t dispatchGuestD3D9(uint32_t token,const uint32_t* args,uint32_t* argumentCount);
