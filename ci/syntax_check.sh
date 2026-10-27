#!/usr/bin/env bash
# Compiles the host side of the Android app with the NDK's clang in -fsyntax-only mode, with the same
# flags, defines and include paths as android/app/src/main/cpp/CMakeLists.txt. No lifted game code is
# needed, so this runs in minutes and catches C++ mistakes before the hour-long full build.
# Needs: ci/setup_siblings.sh done, ANDROID_NDK_HOME set to the pinned NDK.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
parent="$(dirname "$here")"
ndk="${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the pinned NDK}"
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
dxvk="$parent/nfsu2-recompiled/build/dxvk-source"
pcrecomp="$parent/pcrecomp"
cpp="$here/android/app/src/main/cpp"
common=(--target=aarch64-linux-android29 -fsyntax-only -Wall -Wextra -Werror -Wno-missing-field-initializers
        -fno-strict-aliasing -ffp-contract=off
        -DRECOMP_STRICT_ICALL -DRECOMP_X87_WINDOW -DVK_USE_PLATFORM_ANDROID_KHR -DNFS_CUSTOM_DRIVERS -DNFS_SERVER_DLL
        -isystem "$dxvk/include/native" -isystem "$dxvk/include/native/windows" -isystem "$dxvk/include/native/directx"
        -I"$pcrecomp/runtime/recomp32" -I"$pcrecomp/runtime/win32hle" -I"$here/src/runtime"
        -I"$ndk/sources/android/native_app_glue" -I"$parent/nfsu2-recompiled/third_party/libadrenotools/include")
status=0
for source in main.cpp guest_runtime.cpp d3d9_bridge.cpp vulkan_driver.cpp gpu_probe.cpp; do
    if "$clang" -x c++ -std=c++17 "${common[@]}" "$cpp/$source"; then echo "[syntax] ok   $source"; else echo "[syntax] FAIL $source"; status=1; fi
done
for source in overrides.c widescreen.c abk_code.c; do
    if "$clang" -x c -std=c11 "${common[@]}" "$here/src/runtime/$source"; then echo "[syntax] ok   src/runtime/$source"; else echo "[syntax] FAIL src/runtime/$source"; status=1; fi
done
exit $status
