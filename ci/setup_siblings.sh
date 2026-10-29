#!/usr/bin/env bash
# Puts the pinned sibling checkouts next to this repository and prepares DXVK:
#   <parent>/nfsu2-recompiled   (DXVK recipe, libadrenotools, pcrecomp patches)
#   <parent>/pcrecomp           (recompiler at the pinned commit + the 14 patches + ci/pcrecomp/*.patch)
#   <parent>/dxvk-native        (DXVK native 1.9.2b source)
#   <parent>/nfsu2-recompiled/build/dxvk-source   (prepared + shaders generated + ci/dxvk_extra.py)
# Idempotent: existing checkouts are reset to the pins. Needs git, python3, glslangValidator, spirv-val.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
parent="$(dirname "$here")"
# shellcheck source=pins.env
source "$here/ci/pins.env"

clone_at() { # url dir commit
    local url="$1" dir="$2" commit="$3"
    if [ ! -d "$dir/.git" ]; then
        git clone --filter=blob:none "$url" "$dir"
    fi
    git -C "$dir" fetch --depth 1 origin "$commit" 2>/dev/null || git -C "$dir" fetch origin
    git -C "$dir" checkout -q --force "$commit"
    echo "[siblings] $(basename "$dir") at $(git -C "$dir" rev-parse --short HEAD)"
}

clone_at "$NFSU2_REPO" "$parent/nfsu2-recompiled" "$NFSU2_COMMIT"
git -C "$parent/nfsu2-recompiled" submodule update --init --recursive --depth 1 third_party/libadrenotools

clone_at "$PCRECOMP_REPO" "$parent/pcrecomp" "$PCRECOMP_COMMIT"
# Re-apply the patch series from a clean pinned tree every time (git am is not idempotent).
git -C "$parent/pcrecomp" am --abort 2>/dev/null || true
git -C "$parent/pcrecomp" checkout -q --force "$PCRECOMP_COMMIT"
git -C "$parent/pcrecomp" -c user.name=ci -c user.email=ci@localhost am "$parent"/nfsu2-recompiled/patches/pcrecomp/*.patch
# This port's own lifter changes on top (ci/pcrecomp): RECOMP_BRANCH and recomp_fastpath.
git -C "$parent/pcrecomp" -c user.name=ci -c user.email=ci@localhost am "$here"/ci/pcrecomp/*.patch
echo "[siblings] pcrecomp: $(git -C "$parent/pcrecomp" rev-list --count "$PCRECOMP_COMMIT"..HEAD) patches applied"

clone_at "$DXVK_REPO" "$parent/dxvk-native" "$DXVK_COMMIT"
rm -rf "$parent/nfsu2-recompiled/build/dxvk-source"
python3 "$parent/nfsu2-recompiled/ports/dxvk_android/prepare.py" "$parent/dxvk-native" "$parent/nfsu2-recompiled/build/dxvk-source"
python3 "$parent/nfsu2-recompiled/ports/dxvk_android/shaders.py" "$parent/nfsu2-recompiled/build/dxvk-source"
test -f "$parent/nfsu2-recompiled/build/dxvk-source/ANDROID_PREPARE_SHA256"
# This fork: DXVK info logs to logcat and pipeline-compile counters (telemetry; rendering untouched).
python3 "$here/ci/dxvk_extra.py" "$parent/nfsu2-recompiled/build/dxvk-source"
echo "[siblings] DXVK prepared at $parent/nfsu2-recompiled/build/dxvk-source"
