#!/usr/bin/env bash
# DXVK's submission queue as ci/dxvk_extra.py patches it, run on the host against a simulated GPU
# (tests/dxvk_queue): the real dxvk_queue.h/.cpp and dxvk_android_stats.h/.cpp from the prepared tree, with
# stubs for the rest of DXVK. Built with ThreadSanitizer, then with AddressSanitizer + UBSan.
# Needs: ci/setup_siblings.sh done, or DXVK_SOURCE set to a prepared tree that dxvk_extra.py was applied to.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
parent="$(dirname "$here")"
dxvk="${DXVK_SOURCE:-$parent/nfsu2-recompiled/build/dxvk-source}"
test -f "$dxvk/NFSU_EXTRA_APPLIED" || { echo "[dxvk-queue] $dxvk is not prepared: run ci/setup_siblings.sh first"; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp -r "$here/tests/dxvk_queue/stubs/src" "$work/src"
mkdir -p "$work/src/util/rc"
cp "$dxvk/src/util/rc/util_rc.h" "$dxvk/src/util/rc/util_rc_ptr.h" "$work/src/util/rc/"
for file in dxvk_queue.h dxvk_queue.cpp dxvk_android_stats.h dxvk_android_stats.cpp; do
    cp "$dxvk/src/dxvk/$file" "$work/src/dxvk/"
done
cxx="${CXX:-g++}"
status=0
for sanitizer in thread address,undefined; do
    binary="$work/queue_test_${sanitizer%%,*}"
    if "$cxx" -std=c++17 -O1 -g -Wall -Wextra -fno-omit-frame-pointer -fsanitize="$sanitizer" -fno-sanitize-recover=all \
            -I"$work/src/dxvk" -o "$binary" "$here/tests/dxvk_queue/queue_test.cpp" \
            "$work/src/dxvk/dxvk_queue.cpp" "$work/src/dxvk/dxvk_android_stats.cpp" -lpthread \
       && TSAN_OPTIONS="halt_on_error=1" ASAN_OPTIONS="detect_leaks=1" "$binary"; then
        echo "[dxvk-queue] ok   $sanitizer"
    else
        echo "[dxvk-queue] FAIL $sanitizer"; status=1
    fi
done
exit $status
