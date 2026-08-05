#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOTNET_PERF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUNNER="${DOTNET_PERF_DIR}/run_benchmarks_multi.sh"

size="${PERF_DOTNET_MULTI_DD_WALLCLOCK_SIZE:-64}"
duration="${PERF_DOTNET_MULTI_DD_WALLCLOCK_DURATION:-1}"
max_latency_ms="${PERF_DOTNET_MULTI_DD_WALLCLOCK_MAX_LATENCY_MS:-100}"
tag="${PERF_DOTNET_MULTI_DD_WALLCLOCK_TAG:-multi_dealer_dealer_ws_wallclock_latency}"
log_file="$(mktemp -t zlink-dotnet-dd-wallclock.XXXXXX.log)"
trap 'rm -f "${log_file}"' EXIT

set +e
PERF_FAIL_FAST=1 \
"${RUNNER}" \
  --pattern MULTI_DEALER_DEALER \
  --transports ws \
  --msg-sizes "${size}" \
  --duration "${duration}" \
  --results-tag "${tag}" \
  >"${log_file}" 2>&1
status=$?
set -e

if [[ "${status}" -ne 0 ]]; then
  cat "${log_file}" >&2
  exit "${status}"
fi

python3 - "${log_file}" "${size}" "${max_latency_ms}" <<'PY'
import sys

log_path, expected_size, max_latency_ms = sys.argv[1], sys.argv[2], float(sys.argv[3])
latency = None
status_complete = False
msgunit_ok = False
auto_hwm_enabled = False

with open(log_path, encoding="utf-8", errors="replace") as f:
    for raw in f:
        line = raw.strip()
        if line == "- ctx_auto_hwm_enable: 1":
            auto_hwm_enabled = True
        if line.startswith("RESULT,current,MULTI_DEALER_DEALER,ws,"):
            cells = line.split(",")
            if len(cells) == 7 and cells[4] == expected_size and cells[5] == "latency":
                latency = float(cells[6])
        if line.startswith("|"):
            cols = [col.strip() for col in line.strip("|").split("|")]
            if len(cols) >= 5 and cols[0] == expected_size and cols[4] == expected_size:
                msgunit_ok = True
        if line == "- status: complete":
            status_complete = True

if not status_complete:
    print("benchmark did not report complete status", file=sys.stderr)
    sys.exit(1)
if not auto_hwm_enabled:
    print("auto-HWM was not enabled", file=sys.stderr)
    sys.exit(1)
if not msgunit_ok:
    print("Auto-HWM MsgUnit(B) did not match Size(B)", file=sys.stderr)
    sys.exit(1)
if latency is None:
    print("latency result line was not found", file=sys.stderr)
    sys.exit(1)
if latency > max_latency_ms:
    print(
        f"MULTI_DEALER_DEALER ws {expected_size}B latency {latency}ms "
        f"exceeds {max_latency_ms}ms",
        file=sys.stderr,
    )
    sys.exit(1)
PY
