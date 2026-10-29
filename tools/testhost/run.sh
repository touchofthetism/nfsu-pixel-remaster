#!/usr/bin/env bash
# Runs the Linux test host on a virtual X display (Xvfb) with Mesa's software Vulkan driver (lavapipe), so it works
# in a container without a GPU:  tools/testhost/run.sh <build dir> --game DIR [--script FILE] [--out DIR] ...
# Everything after the build directory goes to nfsu-testhost (main_linux.cpp lists the arguments).
set -euo pipefail
build="$1"; shift
binary="$build/nfsu-testhost"
[ -x "$binary" ] || { echo "no $binary: build tools/testhost first" >&2; exit 1; }
# lavapipe maps memory for every descriptor set; DXVK keeps thousands alive, past Linux's default limit of 65530
# mappings a process (a failed mmap then crashes lavapipe in a memset). Raised when allowed (root), else a warning.
if [ "$(cat /proc/sys/vm/max_map_count 2>/dev/null || echo 0)" -lt 1048576 ]; then
    sysctl -q -w vm.max_map_count=1048576 2>/dev/null ||
        echo "warning: vm.max_map_count is $(cat /proc/sys/vm/max_map_count); lavapipe may crash in races (raise it to 1048576)" >&2
fi
display=":$(( 90 + RANDOM % 9 ))"
Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X${display#:}" ] && break; sleep 0.1; done
export DISPLAY="$display"
# lavapipe unless the caller chose a driver. DXVK's own log: warnings only, in the run's --out folder (its logger
# reads both when the program loads, before nfsu-testhost's main could set them).
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d)}"
out="."
previous=""
for arg in "$@"; do [ "$previous" = "--out" ] && out="$arg"; previous="$arg"; done
mkdir -p "$out"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-warn}"
export DXVK_LOG_PATH="${DXVK_LOG_PATH:-$out}"
# TESTHOST_WRAP runs it under another program, e.g. TESTHOST_WRAP="gdb -batch -ex run -ex bt --args".
${TESTHOST_WRAP:-} "$binary" "$@"
