#!/usr/bin/env bash
# Recompiles the owner's Speed.exe 1.4 and sound banks into C under src/recomp/ (gitignored).
#   ci/lift.sh <lift-inputs-dir>     the dir holding Speed.exe and Sound/**/*.abk (lift-inputs.zip unpacked)
# Needs ../pcrecomp prepared by ci/setup_siblings.sh and python3 with capstone + pefile.
# The seed passes follow the scripts' own documentation (scripts/data_seeds.py, scripts/code_seeds.py):
# vtable runs + aligned data pointers -> catalog -> switch arms/imm32 callbacks -> catalog again -> lift.
set -euo pipefail
inputs="$(cd "$1" && pwd)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PCRECOMP="${PCRECOMP:-$(dirname "$here")/pcrecomp}"
cd "$here"
mkdir -p work src/recomp/gen src/recomp/gen_abk
exe="$inputs/Speed.exe"
[ -f "$exe" ] || { echo "No Speed.exe in $inputs" >&2; exit 1; }
# shellcheck source=pins.env
source "$here/ci/pins.env"
actual="$(sha256sum "$exe" | cut -c1-64)"
[ "$actual" = "$SPEED_EXE_SHA256" ] || { echo "Speed.exe sha256 $actual is not the 1.4 build this port targets ($SPEED_EXE_SHA256)" >&2; exit 1; }
echo "[lift] Speed.exe verified ($actual)"
cp "$exe" work/Speed.analysis.exe

echo "[lift] seeds"
python3 "$PCRECOMP/tools/cpp/vtable_scan.py" work/Speed.analysis.exe --seeds work/vtable_seeds.json
python3 scripts/data_seeds.py work/Speed.analysis.exe work/seeds_all.json --merge work/vtable_seeds.json
echo "[lift] catalog (pass 1)"
python3 scripts/run_lift.py catalog work/Speed.analysis.exe work/seeds_all.json work/catalog.json
echo "[lift] code seeds"
python3 scripts/code_seeds.py work/Speed.analysis.exe work/catalog.json work/code_seeds.json --merge work/seeds_all.json
echo "[lift] catalog (pass 2)"
python3 scripts/run_lift.py catalog work/Speed.analysis.exe work/code_seeds.json work/catalog.json
echo "[lift] lift Speed.exe"
python3 scripts/run_lift.py lift work/Speed.analysis.exe work/catalog.json src/recomp/gen

echo "[lift] sound banks"
python3 scripts/abk_module.py "$inputs" work/abk_analysis.dll work/abk_seeds.json src/recomp/gen_abk/abk_blocks.c
python3 scripts/run_lift.py catalog work/abk_analysis.dll work/abk_seeds.json work/abk_catalog.json
python3 scripts/run_lift.py lift work/abk_analysis.dll work/abk_catalog.json src/recomp/gen_abk --prefix abk_

ls -la src/recomp/gen | head -5
echo "[lift] generated $(ls src/recomp/gen/recomp_*.c | wc -l) game units, $(ls src/recomp/gen_abk/abk_*.c | wc -l) sound-bank units"
[ -f work/catalog.json ] && python3 - <<'EOF'
import json; d=json.load(open("work/catalog.json")); print(f"[lift] catalog: {len(d['functions'])} functions")
EOF
