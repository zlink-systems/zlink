#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/ln3pr2
TOOLS="$JOB/tools/node"
PROGRESS="$JOB/progress.md"
variant="$1"; step="$2"; pattern="$3"; payload="$4"
case "$variant" in
  A) WORKTREE=/root/worktree/zlink-v21-s033/ln3-base ;;
  B) WORKTREE=/root/worktree/zlink-v21-s033/ln3-pr ;;
  *) echo "bad variant: $variant" >&2; exit 2 ;;
esac
case "$step" in L2|L3) ;; *) echo "bad step: $step" >&2; exit 2 ;; esac
export WORKTREE
framework="$WORKTREE/framework/languages/node"
dist="$framework/packages/framework/dist"
addon="$framework/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node"
artifacts=(
  "$dist/runtime/foundation/service-wire-m6a-codec.js"
  "$dist/runtime/foundation/raw-service-mesh-runtime.js"
  "$dist/runtime/backend/node/node-raw-binding-port.js"
  "$addon"
)
sources=(
  "$framework/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts"
  "$framework/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts"
  "$framework/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts"
)
for i in 0 1 2; do
  [[ -f "${artifacts[$i]}" && "${artifacts[$i]}" -nt "${sources[$i]}" ]] || {
    echo "stale or missing framework artifact: ${artifacts[$i]}" >&2; exit 3;
  }
done
for f in "${artifacts[@]}"; do [[ -f "$f" ]] || { echo "missing artifact: $f" >&2; exit 3; }; done
head_commit="$(git -C "$WORKTREE" rev-parse HEAD)"
dirty=clean; [[ -z "$(git -C "$WORKTREE" status --porcelain)" ]] || dirty=dirty
base_commit="$(git -C "$WORKTREE" merge-base HEAD origin/main)"
hashes=""
for f in "${artifacts[@]}"; do hashes+="${f}#$(sha256sum "$f" | cut -d' ' -f1);"; done
printf -- '- %s %s-%s %s-%s pre-run: HEAD=%s dirty=%s merge-base=%s; artifacts=%s; call-shape: `client/main.js:259-276,307-324` source request/reply, binding receive `zlink-raw-server/main.js:47-53,64-75` vs ladder receive `zlink-framework-server/main.js:142-155` (both DontWait/NoData batch loop), binding reply `zlink-raw-server/main.js:77-86` vs ladder reply `zlink-framework-server/main.js:168-176,116-133` (new protobuf body; L2 envelope; L3 service frame); per-message cost: one header parse+encode at L2, L3 adds service correlation/frame/part wrappers and pack/unpack copies, no extra async waits or JS atomics.\n' \
  "$(date -u +%FT%TZ)" "$variant" "$step" "$pattern" "$payload" "$head_commit" "$dirty" "$base_commit" "$hashes" >> "$PROGRESS"

cell="${pattern}-${payload}"
out="$JOB/raw/$cell/$variant-$step/counted/attempt1"
mkdir -p "$out"
switches=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 ZLINK_LADDER_L2_ENVELOPE=1 ZLINK_LADDER_L2_CLIENT=1)
if [[ "$step" == L3 ]]; then switches+=(ZLINK_LADDER_L3_SERVICE_WIRE=1 ZLINK_LADDER_L3_CLIENT=1); fi
env "${switches[@]}" \
  IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
  PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 \
  WARMUP_SECONDS=1 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
  OUTPUT_DIR="$out" RUN_STAMP="ln3pr2-${variant}-${step}-${pattern}-${payload}-$(date -u +%Y%m%dT%H%M%SZ)" \
  bash "$TOOLS/run-local-framework.sh" >"$out/driver.log" 2>&1
result="$out/run1/zlink-framework-node-${pattern}-${payload}/results.json"
sched="${result%/results.json}/scheduling.txt"
[[ -f "$result" && -f "$sched" ]] || { echo "missing raw output: $result $sched" >&2; exit 4; }
python3 - "$result" "$sched" <<'PY'
import json, pathlib, sys
rpath, spath = map(pathlib.Path, sys.argv[1:])
c = json.loads(rpath.read_text())['cells'][0]
s = spath.read_text()
assert all(c.get(k, 0) == 0 for k in ('errors', 'server_errors')) and c['target_stats']['errors'] == 0, 'nonzero errors'
assert c['submitted'] == c['completed'] == c['target_stats']['received'], 'message counts differ'
assert 'scheduling_invalid=' not in s, 'invalid scheduling policy'
for label, expected in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
    vals = [line.rsplit('cpus=', 1)[1] for line in s.splitlines() if line.startswith(label+'_affinity ') and 'cpus=' in line]
    assert vals and all(x == expected for x in vals), f'{label} affinity invalid: {vals}'
policies = [line for line in s.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies), 'non-OTHER thread'
print(c['throughput_per_second'])
PY
rate="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$result")"
printf -- '- result %s %s-%s msg/s=%s; errors=0 submitted=completed=received; raw=%s; scheduling=%s\n' \
  "$(date -u +%FT%TZ)" "$variant" "$step" "$rate" "$result" "$sched" >> "$PROGRESS"
printf '%s\n' "$rate" > "$out/msgps.txt"
