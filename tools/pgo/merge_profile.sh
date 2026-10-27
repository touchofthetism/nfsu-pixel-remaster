#!/bin/bash
# merge_profile.sh [-o nfsu1.profdata] <diagnostics ZIP | .profraw>...
#
# Turns the raw profiles an instrumented build (private BUILD pgo=generate) writes into the indexed
# profile the optimised build (pgo=use) consumes. Diagnostics ZIPs are unpacked for their pgo/*.profraw;
# bare .profraw files are taken as they are. Uses the pinned NDK's llvm-profdata (same LLVM as the build),
# then prints the merged profile's summary and its 25 hottest lifted functions by entry count.
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
out=nfsu1.profdata
if [ "${1:-}" = "-o" ]; then out=$2; shift 2; fi
[ $# -ge 1 ] || { echo "usage: $0 [-o out.profdata] <diagnostics.zip|game.profraw>..."; exit 2; }

# The NDK's llvm-profdata: $ANDROID_NDK_HOME, else the pinned NDK under $ANDROID_HOME / $ANDROID_SDK_ROOT.
ndk_version=$(sed -n 's/^ANDROID_NDK=//p' "$here/ci/pins.env" | tr -d '"' | head -1)
candidates=("${ANDROID_NDK_HOME:-}" "${ANDROID_HOME:-}/ndk/$ndk_version" "${ANDROID_SDK_ROOT:-}/ndk/$ndk_version" "$HOME/android-sdk/ndk/$ndk_version")
profdata=""
for c in "${candidates[@]}"; do
  [ -n "$c" ] || continue
  for p in "$c"/toolchains/llvm/prebuilt/*/bin/llvm-profdata; do [ -x "$p" ] && profdata=$p && break 2; done
done
[ -n "$profdata" ] || profdata=$(command -v llvm-profdata || true)
[ -n "$profdata" ] || { echo "llvm-profdata not found: set ANDROID_NDK_HOME to NDK $ndk_version"; exit 1; }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
raws=()
for input in "$@"; do
  case "$input" in
    *.zip)
      dir="$work/$(basename "$input" .zip)"; mkdir -p "$dir"
      # Only the profiles: a diagnostics ZIP also holds logs, caches and saves.
      unzip -q -o "$input" 'pgo/*.profraw' -d "$dir" 2>/dev/null || { echo "$input: no pgo/*.profraw inside (not an instrumented build's diagnostics?)"; continue; }
      while IFS= read -r f; do raws+=("$f"); done < <(find "$dir/pgo" -name '*.profraw' -size +0)
      ;;
    *.profraw) raws+=("$input") ;;
    *) echo "skipping $input (not a .zip or .profraw)";;
  esac
done
[ ${#raws[@]} -ge 1 ] || { echo "no raw profiles found"; exit 1; }
echo "merging ${#raws[@]} raw profile(s) with $profdata"
"$profdata" merge --sparse -o "$out" "${raws[@]}"
echo "wrote $out ($(du -h "$out" | cut -f1))"
echo "--- summary and the hottest functions by block count"
"$profdata" show --topn=25 "$out" 2>/dev/null | grep -vE "^(Counters|Functions shown|Total count):" | head -34 || true
