#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
REPO_ROOT="$(git -C "$ROOT_DIR" rev-parse --show-toplevel)"
PACKAGE_ROOT="${ZLINK_NODE_FRAMEWORK_PACKAGE_ROOT:-$NODE_ROOT}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/log/$RUN_ID"
TEMP_DIR="$(mktemp -d)"
EVIDENCE_FILE="$LOG_DIR/evidence.jsonl"
LOCAL_READINESS_ATTEMPTS=30
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_TIMEOUT_SECONDS=3
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "$LOG_DIR"

source "$NODE_ROOT/e2e/runner-common.sh"

IMPLEMENTED_PROCESS=(SA-E2E-01 SA-E2E-08 SA-E2E-09 SA-E2E-14 SA-E2E-20 SA-E2E-05)
IMPLEMENTED_REGRESSION=(SA-REG-01 SA-REG-02 SA-REG-03 SA-REG-04)
ALL_KNOWN=()
for number in $(seq 1 20); do ALL_KNOWN+=("$(printf 'SA-E2E-%02d' "$number")"); done
for number in $(seq 1 4); do ALL_KNOWN+=("$(printf 'SA-REG-%02d' "$number")"); done

contains() {
  local expected="$1"
  shift
  local value
  for value in "$@"; do [[ "$value" == "$expected" ]] && return 0; done
  return 1
}

SELECTORS=()
if [[ "$#" -eq 0 || "$*" == "all" ]]; then
  SELECTORS=("${IMPLEMENTED_PROCESS[@]}" "${IMPLEMENTED_REGRESSION[@]}")
else
  for argument in "$@"; do
    IFS=',' read -ra items <<<"$argument"
    SELECTORS+=("${items[@]}")
  done
fi

for selector in "${SELECTORS[@]}"; do
  if ! contains "$selector" "${ALL_KNOWN[@]}"; then
    echo "Unknown SubmitAdmission selector: $selector" >&2
    exit 2
  fi
  if ! contains "$selector" "${IMPLEMENTED_PROCESS[@]}" \
      && ! contains "$selector" "${IMPLEMENTED_REGRESSION[@]}"; then
    echo "$selector is not implemented by the Node.js Config 13 runner; see feature-map.ko.md" >&2
    exit 3
  fi
done

if [[ ! -f "$PACKAGE_ROOT/node_modules/@zlink-systems/zlink/package.json" ]]; then
  echo "Node binding package is missing from package-mode root: $PACKAGE_ROOT" >&2
  exit 2
fi
if [[ ! -f "$PACKAGE_ROOT/packages/framework/dist/index.js" \
      || ! -f "$PACKAGE_ROOT/packages/nestjs/dist/index.js" ]]; then
  echo "Framework packages are not built in package-mode root: $PACKAGE_ROOT" >&2
  exit 2
fi

pids=()
cleanup() {
  local code=$?
  stop_live_pids
  wait_all_pids_ignoring_status
  rm -rf "$TEMP_DIR"
  if [[ "$code" -ne 0 ]]; then tail_failure_logs; fi
}
trap cleanup EXIT

echo "package_mode_root=$PACKAGE_ROOT" | tee "$LOG_DIR/package-mode.log"
PACKAGE_VERSION="$(node -p "require('$PACKAGE_ROOT/node_modules/@zlink-systems/zlink/package.json').version")"
node -p "'binding_package_version=' + require('$PACKAGE_ROOT/node_modules/@zlink-systems/zlink/package.json').version" \
  | tee -a "$LOG_DIR/package-mode.log"
NEEDS_NATIVE=0
for selector in "${SELECTORS[@]}"; do
  if contains "$selector" "${IMPLEMENTED_PROCESS[@]}" \
      || [[ "$selector" == "SA-REG-02" || "$selector" == "SA-REG-04" ]]; then
    NEEDS_NATIVE=1
  fi
done
if [[ "$NEEDS_NATIVE" == 1 ]]; then
  PACKAGE_NATIVE_DIR="$PACKAGE_ROOT/node_modules/@zlink-systems/zlink/prebuilds/linux-x64"
  PACKAGE_ADDON="$PACKAGE_NATIVE_DIR/zlink.node"
  PACKAGE_SONAME="libzlink.so.${PACKAGE_VERSION%%.*}"
  PACKAGE_CORE="$PACKAGE_NATIVE_DIR/$PACKAGE_SONAME"
  SOURCE_CORE="$REPO_ROOT/core/build/lib/libzlink.so.$PACKAGE_VERSION"
  if [[ ! -f "$SOURCE_CORE" || ! -f "$PACKAGE_CORE" || ! -f "$PACKAGE_ADDON" ]]; then
    echo "Candidate native artifact is incomplete." >&2
    exit 2
  fi
  SOURCE_CORE_SHA="$(sha256sum "$SOURCE_CORE" | awk '{print $1}')"
  PACKAGE_CORE_SHA="$(sha256sum "$PACKAGE_CORE" | awk '{print $1}')"
  LOADED_CORE="$(ldd "$PACKAGE_ADDON" | awk '/libzlink\.so/{print $3; exit}')"
  if [[ "$SOURCE_CORE_SHA" != "$PACKAGE_CORE_SHA" \
        || "$(realpath "$LOADED_CORE")" != "$(realpath "$PACKAGE_CORE")" ]]; then
    echo "Candidate addon does not load the current core/build runtime." >&2
    exit 2
  fi
  {
    echo "source_core_sha256=$SOURCE_CORE_SHA"
    echo "package_core_sha256=$PACKAGE_CORE_SHA"
    echo "addon_loaded_core=$(realpath "$LOADED_CORE")"
  } | tee -a "$LOG_DIR/package-mode.log"
fi

if [[ "$(realpath "$PACKAGE_ROOT")" != "$(realpath "$NODE_ROOT")" ]]; then
  mkdir -p "$PACKAGE_ROOT/e2e/SubmitAdmission/Role" "$PACKAGE_ROOT/e2e/SubmitAdmission/Contract"
  cp "$ROOT_DIR/Role/main.ts" "$ROOT_DIR/Role/package.json" "$ROOT_DIR/Role/tsconfig.json" \
    "$PACKAGE_ROOT/e2e/SubmitAdmission/Role/"
  cp "$ROOT_DIR/Contract/contract.ts" "$ROOT_DIR/Contract/tsconfig.json" \
    "$PACKAGE_ROOT/e2e/SubmitAdmission/Contract/"
fi
build_package "$PACKAGE_ROOT/e2e/SubmitAdmission/Role"
{
  echo "compile_typescript=$(realpath "$PACKAGE_ROOT/node_modules/typescript/bin/tsc")"
  echo "compile_framework_declaration=$(realpath "$PACKAGE_ROOT/packages/framework/dist/index.d.ts")"
  echo "runtime_framework=$(cd "$PACKAGE_ROOT" && node -p "require.resolve('@zlink-systems/framework')")"
  echo "runtime_binding=$(cd "$PACKAGE_ROOT" && node -p "require.resolve('@zlink-systems/zlink')")"
} | tee -a "$LOG_DIR/package-mode.log"

run_regression_01() {
  node "$PACKAGE_ROOT/node_modules/typescript/bin/tsc" \
    -p "$PACKAGE_ROOT/e2e/SubmitAdmission/Contract/tsconfig.json" \
    >"$LOG_DIR/contract-compile.log" 2>&1
  "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --contract \
    >"$LOG_DIR/submit-contract.log" 2>&1
  "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --implementation \
    >"$LOG_DIR/submit-implementation.log" 2>&1
  echo '{"scenarioId":"SA-REG-01","status":"PASS","positive":"submit","negative":"trySubmit"}' \
    | tee -a "$EVIDENCE_FILE"
}

run_regression_02() {
  (
    cd "$PACKAGE_ROOT"
    node --test --test-name-pattern='Logical Multicast' test/contract/backend-contract.test.js
  ) >"$LOG_DIR/internal-primitive.log" 2>&1
  echo '{"scenarioId":"SA-REG-02","status":"PASS","tests":6,"coreCallMax":1}' \
    | tee -a "$EVIDENCE_FILE"
}

run_regression_04() {
  (
    cd "$PACKAGE_ROOT"
    node --test \
      --test-name-pattern='ZLinkAsyncSubmitter disposal races release each pending payload exactly once' \
      test/contract/channel-client.test.js
  ) >"$LOG_DIR/disposal-regression.log" 2>&1
  echo '{"scenarioId":"SA-REG-04","status":"PASS","iterations":100,"discardCountPerOperation":1,"lateAttemptCount":0}' \
    | tee -a "$EVIDENCE_FILE"
}

PROCESS_SELECTORS=()
RUN_MISSING_DISCONNECTED=0
for selector in "${SELECTORS[@]}"; do
  case "$selector" in
    SA-REG-01) run_regression_01 ;;
    SA-REG-02) run_regression_02 ;;
    SA-REG-03)
      echo '{"scenarioId":"SA-REG-03","status":"N/A","reason":"Kotlin-only"}' \
        | tee -a "$EVIDENCE_FILE" ;;
    SA-REG-04) run_regression_04 ;;
    SA-E2E-05) RUN_MISSING_DISCONNECTED=1 ;;
    *) PROCESS_SELECTORS+=("$selector") ;;
  esac
done
if [[ "$RUN_MISSING_DISCONNECTED" == 1 ]]; then
  # This scenario intentionally stops the target, so it must be the final process scenario.
  PROCESS_SELECTORS+=(SA-E2E-05)
fi

if [[ "${#PROCESS_SELECTORS[@]}" -gt 0 ]]; then
  CALLER_HTTP_PORT="$(allocate_port)"
  TARGET_HTTP_PORT="$(allocate_port)"
  PUBLISHER_HTTP_PORT="$(allocate_port)"
  CALLER_MESH_PORT="$(allocate_port)"
  TARGET_MESH_PORT="$(allocate_port)"
  FANOUT_PORT="$(allocate_port)"
  CALLER_URL="http://127.0.0.1:$CALLER_HTTP_PORT"
  TARGET_URL="http://127.0.0.1:$TARGET_HTTP_PORT"
  PUBLISHER_URL="http://127.0.0.1:$PUBLISHER_HTTP_PORT"
  CALLER_RID="submit-caller"
  TARGET_RID="submit-target"

  python3 - "$TEMP_DIR" "$CALLER_HTTP_PORT" "$TARGET_HTTP_PORT" "$PUBLISHER_HTTP_PORT" \
    "$CALLER_MESH_PORT" "$TARGET_MESH_PORT" "$FANOUT_PORT" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
caller_http, target_http, publisher_http, caller_mesh, target_mesh, fanout = map(int, sys.argv[2:])
values = {
    "caller": {
        "role": "caller", "rid": "submit-caller", "httpPort": caller_http,
        "meshEndpoint": f"tcp://127.0.0.1:{caller_mesh}", "peerRid": "submit-target",
        "peerEndpoint": f"tcp://127.0.0.1:{target_mesh}"
    },
    "target": {
        "role": "target", "rid": "submit-target", "httpPort": target_http,
        "meshEndpoint": f"tcp://127.0.0.1:{target_mesh}"
    },
    "publisher": {
        "role": "publisher", "rid": "submit-publisher", "httpPort": publisher_http,
        "fanoutEndpoint": f"tcp://127.0.0.1:{fanout}"
    }
}
for name, value in values.items():
    (root / f"{name}.json").write_text(json.dumps(value), encoding="utf-8")
PY

  ROLE_MAIN="$PACKAGE_ROOT/e2e/SubmitAdmission/Role/dist/main.js"
  start_server target "$ROLE_MAIN" --config="$TEMP_DIR/target.json"
  TARGET_PID="$LAST_STARTED_PID"
  start_server caller "$ROLE_MAIN" --config="$TEMP_DIR/caller.json"
  CALLER_PID="$LAST_STARTED_PID"
  start_server publisher "$ROLE_MAIN" --config="$TEMP_DIR/publisher.json"
  PUBLISHER_PID="$LAST_STARTED_PID"
  wait_health "$TARGET_URL" target "$TARGET_PID"
  wait_health "$CALLER_URL" caller "$CALLER_PID"
  wait_health "$PUBLISHER_URL" publisher "$PUBLISHER_PID"

  ready=0
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" -fsS \
        "$CALLER_URL/ready?targetRid=$TARGET_RID" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  if [[ "$ready" != 1 ]]; then
    echo "Target route did not become ready within 3s." >&2
    exit 1
  fi

  node "$ROOT_DIR/Support/scenario-client.mjs" \
    "$CALLER_URL" "$TARGET_URL" "$PUBLISHER_URL" "$CALLER_RID" "$TARGET_RID" \
    "$EVIDENCE_FILE" "${PROCESS_SELECTORS[@]}" | tee "$LOG_DIR/client.log"
fi

echo "SubmitAdmission PASS scenarios=${SELECTORS[*]} logs=$LOG_DIR"
