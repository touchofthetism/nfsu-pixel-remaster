#!/usr/bin/env bash
# Host-side unit tests (no Android needed): the frame pacer, the audio mixer and this fork's
# anti-aliasing policy. Each test is a standalone C++17 program that exits non-zero on failure.
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
rm -rf "$out"
exit $status
