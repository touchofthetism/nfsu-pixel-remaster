# Linux test host: the Android runtime (guest_runtime.cpp, the D3D9 bridge, the lifted game) built for x86-64 Linux,
# presenting through DXVK into an X11 window (Xvfb + Mesa's lavapipe in a container), with no physical audio.
# It runs the owner's game folder the way the phone does, so features can be tried end to end before a build
# goes to the phone: tools/testhost/README.md. Nothing here ships in the APK.
#
#   cmake -S tools/testhost -B build-testhost -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   ninja -C build-testhost nfsu-testhost
cmake_minimum_required(VERSION 3.20)
project(NFSU1TestHost C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The test host is a Linux build")
endif()
get_filename_component(NFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PORT "${NFS_ROOT}/android/app/src/main/cpp")
set(PCRECOMP "${NFS_ROOT}/../pcrecomp" CACHE PATH "pcrecomp checkout with the port's patches")
set(NFSU2_ROOT "${NFS_ROOT}/../nfsu2-recompiled" CACHE PATH "nfsu2-recompiled checkout")
set(DXVK_SOURCE "${NFSU2_ROOT}/build/dxvk-source" CACHE PATH "Prepared DXVK native source")
set(NFS_TESTHOST_GUEST_OPT "-O1" CACHE STRING "Optimisation of the lifted game code")

# DXVK with ports/dxvk_android's non-Android WSI (an Xlib surface on the test window).
set(DXVK_HOST_HEADLESS ON CACHE BOOL "" FORCE)
set(DXVK_ANDROID_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${NFS_ROOT}/ports/dxvk_android" "${CMAKE_CURRENT_BINARY_DIR}/dxvk")

# The lifted game and sound-bank code, compiled as for the phone (arm64_guest.h is architecture-neutral).
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${NFS_ROOT}/src/recomp/gen/recomp_*.c")
file(GLOB ABK_SOURCES CONFIGURE_DEPENDS "${NFS_ROOT}/src/recomp/gen_abk/abk_*.c")
if(NOT GEN_SOURCES OR NOT ABK_SOURCES)
  message(FATAL_ERROR "Lift the game first (ci/lift.sh <dir with Speed.exe and Sound/>)")
endif()
add_library(nfs_guest OBJECT ${GEN_SOURCES} ${ABK_SOURCES})
target_include_directories(nfs_guest PRIVATE "${PCRECOMP}/runtime/recomp32" "${NFS_ROOT}/src/recomp/gen" "${NFS_ROOT}/src/runtime")
target_compile_definitions(nfs_guest PRIVATE RECOMP_STRICT_ICALL RECOMP_LOCAL_REGS RECOMP_X87_WINDOW "RECOMP_MEM_QUAL=")
target_compile_options(nfs_guest PRIVATE ${NFS_TESTHOST_GUEST_OPT} -g0 -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off
  -fvisibility=hidden -include "${PORT}/arm64_guest.h")
# The biggest generated units need several GB at -O1: at most two at a time.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS guest_compile=2)
set_property(TARGET nfs_guest PROPERTY JOB_POOL_COMPILE guest_compile)

add_executable(nfsu-testhost main_linux.cpp
  "${PORT}/guest_runtime.cpp" "${PORT}/d3d9_bridge.cpp" "${PORT}/host_perf.cpp"
  "${NFS_ROOT}/src/runtime/overrides.c" "${NFS_ROOT}/src/runtime/widescreen.c" "${NFS_ROOT}/src/runtime/framerate.c"
  "${NFS_ROOT}/src/runtime/misc_fixes.c" "${NFS_ROOT}/src/runtime/reflections.c" "${NFS_ROOT}/src/runtime/garage.c"
  "${NFS_ROOT}/src/runtime/freeroam.c" "${NFS_ROOT}/src/runtime/fastpath.c" "${NFS_ROOT}/src/runtime/abk_code.c"
  $<TARGET_OBJECTS:nfs_guest>)
target_compile_features(nfsu-testhost PRIVATE cxx_std_17)
# NFS_D3D9_BACKEND: guest_runtime.cpp's D3D9 paths, as on the phone. NFS_TESTHOST: the few places that differ
# (an audio device that pulls the mix in real time, tests/fake_aaudio.h).
target_compile_definitions(nfsu-testhost PRIVATE RECOMP_STRICT_ICALL RECOMP_X87_WINDOW NFS_D3D9_BACKEND NFS_TESTHOST)
target_include_directories(nfsu-testhost PRIVATE "${PORT}" "${PCRECOMP}/runtime/recomp32" "${PCRECOMP}/runtime/win32hle"
  "${NFS_ROOT}/src/runtime" "${NFS_ROOT}/tests")
target_include_directories(nfsu-testhost SYSTEM PRIVATE "${DXVK_SOURCE}/include/native"
  "${DXVK_SOURCE}/include/native/windows" "${DXVK_SOURCE}/include/native/directx")
target_compile_options(nfsu-testhost PRIVATE -O1 -g -fno-strict-aliasing -ffp-contract=off -Wall -Wextra -Wno-missing-field-initializers)
find_library(NFS_X11_LIBRARY NAMES X11 REQUIRED)
target_link_libraries(nfsu-testhost PRIVATE dxvk_android ${NFS_X11_LIBRARY} m pthread dl)
