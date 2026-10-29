#pragma once
#include <cstdint>
#include <string>
#include <vector>
// Maps the owner's PE data, binds imports, tests lifted code and attempts CRT
// entry until the first unsupported host service. Returns an honest status.
std::string connectGuestRuntime(const char* executable);
void requestGuestRuntimeStop();
void setGuestPaused(bool paused);
void setGuestDisplaySize(unsigned width,unsigned height);
void setGuestKey(unsigned scan,bool down);
void clearGuestInput();
bool guestDrivingControls();
// Physical gamepad seen by the game as a DirectInput joystick: axes X, Y, Z
// (triggers), Rx, Ry, Rz in -1..1; buttons A B X Y LB RB Back Start LS RS as bits.
void setGuestPad(const float axes[6],uint32_t buttons,int hatX,int hatY);
void setGuestPadConnected(bool connected);
// True while the game has the joystick acquired (then the pad stops emulating keys).
bool guestGamepadInUse();
// Launcher option; only before the guest boots.
void setGuestWidescreen(bool enabled);
/** Launcher option; only before the guest boots. Off blanks the HUD wrong-way element names (free roam). */
void setGuestWrongWayWarning(bool enabled);
// Launcher "Reflections": the sharper reflection targets (true) or the game's original sizes.
void setGuestHdReflections(bool enabled);
// Launcher "Garage": the customization freedoms on, as NFS_GARAGE_* bits (src/runtime/nfs_garage.h). Before boot.
void setGuestGarage(uint32_t features);
// The app's folder for data the host makes from the game's (every rim size's wheel pack), outside the game
// folder. Before boot.
void setGuestGeneratedDir(const std::string& directory);
// Launcher "Effect states": the effects' render, sampler and texture stage states applied by the host (true,
// effect_states.h) or by the game's own D3DX code. Before boot.
void setGuestEffectStates(bool host);
void setGuestOnlineServer(const std::string& host);
/** Touch as mouse: x/y as a fraction of the game view; phase 0 down, 1 move, 2 up (left button). */
void setGuestPointer(float x,float y,int phase);
/** Posts WM_CHAR messages (the game's text fields read those); 8 is backspace. */
void typeGuestText(const std::vector<uint32_t>& characters);
void setGuestLanguage(const char* language);
void setGuestResolution(unsigned width,unsigned height);
void setGuestFrameLimit(unsigned framesPerSecond);
/** Launcher "CPU placement": 0 = system scheduler, 1 = render thread on the fastest cores (host_perf.h). */
void setGuestCpuPlacement(int placement);
/** Launcher "Loader reads": true = reads of 64 KB and more hand the guest machine to the other guest threads
 * while the kernel copies (guest_files.inc); false = every read holds the machine, as builds 1-3 and 5-6 did. */
void setGuestReadHandover(bool overlapped);
/** Session telemetry as JSON: frame intervals (frame_telemetry.h), DXVK pipeline compiles, audio
 * underruns, render size; live = the overlay view (last 240 frames), else the complete record. */
std::string guestTelemetryJson(bool live);
