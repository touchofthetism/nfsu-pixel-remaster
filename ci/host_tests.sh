#!/usr/bin/env bash
# Host-side unit tests (no Android needed): the frame pacer, the audio mixer and this fork's
# anti-aliasing policy. Each test is a standalone C++17 program that exits non-zero on failure.
# The tests whose code runs on several threads (the sound output and device, the core placement) run
# again under ThreadSanitizer and under AddressSanitizer with UBSan.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mktemp -d)"
cxx="${CXX:-g++}"
status=0
for test in "$here"/tests/*.cpp; do
    name="$(basename "$test" .cpp)"
    if "$cxx" -std=c++17 -O1 -Wall -Wextra -o "$out/$name" "$test" -lpthread && "$out/$name"; then
        echo "[tests] ok   $name"
    else
        echo "[tests] FAIL $name"; status=1
    fi
done
for name in sound_device sound_output host_perf; do
    for sanitizer in thread address,undefined; do
        binary="$out/${name}_${sanitizer%%,*}"
        if "$cxx" -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize="$sanitizer" -fno-sanitize-recover=all \
                -o "$binary" "$here/tests/$name.cpp" -lpthread \
           && TSAN_OPTIONS="halt_on_error=1" ASAN_OPTIONS="detect_leaks=1" "$binary" > "$binary.log" 2>&1; then
            echo "[tests] ok   $name ($sanitizer)"
        else
            echo "[tests] FAIL $name ($sanitizer)"; tail -40 "$binary.log" || true; status=1
        fi
    done
done
rm -rf "$out"
exit $status
