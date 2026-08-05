#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$CPP_DIR/../../.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
mkdir -p "$REPO_ROOT/.artifacts/tmp"
TEMP_DIR="$(mktemp -d "$REPO_ROOT/.artifacts/tmp/submit-admission.XXXXXX")"
CACHE_ROOT="$TEMP_DIR"
mkdir -p "$CACHE_ROOT"
BUILD_DIR="$CACHE_ROOT/framework-build"
INSTALL_DIR="$CACHE_ROOT/framework-install"
PACKAGE_ROOT="$CACHE_ROOT/local-package"
BINDING_BUILD_DIR="$CACHE_ROOT/binding-build"
CORE_INSTALL_DIR="$PACKAGE_ROOT/install/zlink-core"
EVIDENCE_FILE="$LOG_DIR/evidence.jsonl"
mkdir -p "$LOG_DIR" "$PACKAGE_ROOT/install/zlink-cpp"

IMPLEMENTED_PROCESS=(SA-E2E-01 SA-E2E-08 SA-E2E-09 SA-E2E-14 SA-E2E-20)
IMPLEMENTED_REGRESSION=(SA-REG-01 SA-REG-02 SA-REG-03)
ALL_KNOWN=()
for number in $(seq 1 20); do ALL_KNOWN+=("$(printf 'SA-E2E-%02d' "$number")"); done
for number in $(seq 1 4); do ALL_KNOWN+=("$(printf 'SA-REG-%02d' "$number")"); done

SELECTORS=()
if [[ "$#" -eq 0 || "$*" == "all" ]]; then
  SELECTORS=("${IMPLEMENTED_PROCESS[@]}" "${IMPLEMENTED_REGRESSION[@]}")
else
  for argument in "$@"; do
    IFS=',' read -ra items <<<"$argument"
    SELECTORS+=("${items[@]}")
  done
fi

contains() {
  local expected="$1"
  shift
  local value
  for value in "$@"; do [[ "$value" == "$expected" ]] && return 0; done
  return 1
}

for selector in "${SELECTORS[@]}"; do
  if ! contains "$selector" "${ALL_KNOWN[@]}"; then
    echo "Unknown SubmitAdmission selector: $selector" >&2
    exit 2
  fi
  if ! contains "$selector" "${IMPLEMENTED_PROCESS[@]}" \
      && ! contains "$selector" "${IMPLEMENTED_REGRESSION[@]}"; then
    echo "$selector is not implemented by the C++ Config 13 runner; see feature-map.ko.md" >&2
    exit 3
  fi
done

PIDS=()
cleanup() {
  local code=$?
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then kill -INT "$pid" >/dev/null 2>&1 || true; fi
  done
  for _ in $(seq 1 30); do
    local alive=0
    for pid in "${PIDS[@]:-}"; do
      if kill -0 "$pid" >/dev/null 2>&1; then alive=1; break; fi
    done
    [[ "$alive" == 0 ]] && break
    sleep 0.1
  done
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then kill -KILL "$pid" >/dev/null 2>&1 || true; fi
  done
  wait "${PIDS[@]:-}" >/dev/null 2>&1 || true
  rm -rf "$TEMP_DIR"
  if [[ "$code" != 0 ]]; then echo "SubmitAdmission failed; logs=$LOG_DIR" >&2; fi
}
trap cleanup EXIT

VERSION="$({ sed -n 's/.*ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "\([0-9][0-9.]*\)".*/\1/p' "$CPP_DIR/CMakeLists.txt"; } | head -n1)"
CORE_RUNTIME="$(readlink -f "$REPO_ROOT/core/build/lib/libzlink.so" 2>/dev/null || true)"
if [[ -z "$CORE_RUNTIME" || ! -f "$CORE_RUNTIME" ]]; then
  echo "Official Core runtime is missing: $REPO_ROOT/core/build/lib/libzlink.so" >&2
  exit 1
fi
if find "$REPO_ROOT/core/src" "$REPO_ROOT/core/include" -type f -newer "$CORE_RUNTIME" \
    -print -quit | grep -q .; then
  echo "Official Core runtime is older than core/src or core/include: $CORE_RUNTIME" >&2
  exit 1
fi
CORE_RUNTIME_SHA="$(sha256sum "$CORE_RUNTIME" | awk '{print $1}')"
CORE_RUNTIME_BUILD_ID="$(readelf -n "$CORE_RUNTIME" | sed -n 's/.*Build ID: //p' | head -n1)"
CORE_INSTALL_DIR="$CORE_INSTALL_DIR/$VERSION"
BINDING_INSTALL_DIR="$PACKAGE_ROOT/install/zlink-cpp/$VERSION"

# This E2E consumes the already-built official runtime. It installs that exact
# binary into an isolated package root and never rebuilds Core.
cmake --install "$REPO_ROOT/core/build" --prefix "$CORE_INSTALL_DIR" \
  >"$LOG_DIR/core-candidate-install.log" 2>&1
cmake -S "$REPO_ROOT/bindings/cpp" -B "$BINDING_BUILD_DIR" \
  -DCMAKE_INSTALL_PREFIX="$BINDING_INSTALL_DIR" \
  -DZLINK_CPP_CORE_PACKAGE_PREFIX="$CORE_INSTALL_DIR" \
  -DZLINK_CPP_BUILD_TESTS=OFF \
  -DZLINK_CPP_BUILD_SAMPLES=OFF \
  >"$LOG_DIR/binding-candidate-configure.log" 2>&1
cmake --build "$BINDING_BUILD_DIR" -j2 \
  >"$LOG_DIR/binding-candidate-build.log" 2>&1
cmake --install "$BINDING_BUILD_DIR" \
  >"$LOG_DIR/binding-candidate-install.log" 2>&1

CANDIDATE_NATIVE="$CORE_INSTALL_DIR/lib/libzlink.so"
if [[ ! -f "$CANDIDATE_NATIVE" ]]; then
  echo "C++ binding candidate native runtime is missing: $CANDIDATE_NATIVE" >&2
  exit 1
fi
CANDIDATE_NATIVE_SHA="$(sha256sum "$CANDIDATE_NATIVE" | awk '{print $1}')"
CANDIDATE_NATIVE_BUILD_ID="$(readelf -n "$CANDIDATE_NATIVE" | sed -n 's/.*Build ID: //p' | head -n1)"
if [[ "$CANDIDATE_NATIVE_SHA" != "$CORE_RUNTIME_SHA" \
      || "$CANDIDATE_NATIVE_BUILD_ID" != "$CORE_RUNTIME_BUILD_ID" ]]; then
  echo "C++ binding candidate does not contain the official Core runtime" >&2
  exit 1
fi
PACKAGE_SHA="$(cd "$PACKAGE_ROOT/install" && \
  { find . -type f -print0 | sort -z | xargs -0 sha256sum; } \
    | sha256sum | awk '{print $1}')"
echo "package_mode_root=$PACKAGE_ROOT" | tee "$LOG_DIR/package-mode.log"
echo "binding_package_version=$VERSION" | tee -a "$LOG_DIR/package-mode.log"
echo "binding_package_tree_sha256=$PACKAGE_SHA" | tee -a "$LOG_DIR/package-mode.log"
echo "core_runtime_sha256=$CORE_RUNTIME_SHA" | tee -a "$LOG_DIR/package-mode.log"
echo "core_runtime_build_id=$CORE_RUNTIME_BUILD_ID" | tee -a "$LOG_DIR/package-mode.log"
echo "candidate_native_sha256=$CANDIDATE_NATIVE_SHA" | tee -a "$LOG_DIR/package-mode.log"
echo "candidate_native_build_id=$CANDIDATE_NATIVE_BUILD_ID" | tee -a "$LOG_DIR/package-mode.log"

CMAKE_ARGUMENTS=(
  -S "$CPP_DIR"
  -B "$BUILD_DIR"
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF
  -DZLINK_FRAMEWORK_CPP_BUILD_E2E=ON
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT="$PACKAGE_ROOT"
)
VCPKG_PACKAGES="$REPO_ROOT/.tools/vcpkg-src/packages"
if [[ -d "$VCPKG_PACKAGES/protobuf_x64-linux/share/protobuf" ]]; then
  CMAKE_ARGUMENTS+=(
    -Dprotobuf_DIR="$VCPKG_PACKAGES/protobuf_x64-linux/share/protobuf"
    -Dabsl_DIR="$VCPKG_PACKAGES/abseil_x64-linux/share/absl"
    -Dutf8_range_DIR="$VCPKG_PACKAGES/utf8-range_x64-linux/share/utf8_range"
    -Dhiredis_DIR="$VCPKG_PACKAGES/hiredis_x64-linux/share/hiredis"
    -Dlibuv_DIR="$VCPKG_PACKAGES/libuv_x64-linux/share/libuv"
    -Dredis++_DIR="$VCPKG_PACKAGES/redis-plus-plus_x64-linux/share/redis++"
    -DCMAKE_PREFIX_PATH="$VCPKG_PACKAGES/redis-plus-plus_x64-linux;$VCPKG_PACKAGES/libuv_x64-linux;$VCPKG_PACKAGES/hiredis_x64-linux"
  )
fi

ZLINK_LOCAL_PACKAGE_ROOT="$PACKAGE_ROOT" cmake "${CMAKE_ARGUMENTS[@]}" \
  >"$LOG_DIR/configure.log" 2>&1
BUILD_TARGETS=(zlink_cpp_e2e_submit_admission_role)
if contains SA-REG-01 "${SELECTORS[@]}"; then
  BUILD_TARGETS+=(test_cpp_framework_contract_headers)
fi
if contains SA-REG-02 "${SELECTORS[@]}"; then
  BUILD_TARGETS+=(test_cpp_framework_messaging zlink_cpp_framework_mesh_node_vertical_test)
fi
cmake --build "$BUILD_DIR" --target "${BUILD_TARGETS[@]}" -j2 \
  >"$LOG_DIR/build.log" 2>&1
cmake --install "$BUILD_DIR" --prefix "$INSTALL_DIR" --component Framework \
  >"$LOG_DIR/install.log" 2>&1

run_contract_checks() {
  local positive="$TEMP_DIR/positive.cpp"
  local negative="$TEMP_DIR/negative.cpp"
  python3 - "$positive" "$negative" <<'PY'
import pathlib
import sys

positive, negative = map(pathlib.Path, sys.argv[1:])
positive.write_text(
    "#include <zlink/framework.hpp>\n"
    "static_assert(requires(zlink::framework::send_call_t &call) { call.submit(); });\n"
    "int main() { return 0; }\n",
    encoding="utf-8")
removed = "try_" + "submit"
negative.write_text(
    "#include <zlink/framework.hpp>\n"
    "void removed(zlink::framework::send_call_t &call) { call." + removed + "(); }\n"
    "int main() { return 0; }\n",
    encoding="utf-8")
PY
  local includes=(-I"$INSTALL_DIR/include" -I"$PACKAGE_ROOT/install/zlink-cpp/$VERSION/include")
  c++ -std=c++20 -fsyntax-only "${includes[@]}" "$positive" \
    >"$LOG_DIR/positive-compile.log" 2>&1
  if c++ -std=c++20 -fsyntax-only "${includes[@]}" "$negative" \
      >"$LOG_DIR/negative-compile.log" 2>&1; then
    echo "SA-REG-01 failed: removed synchronous terminator compiled." >&2
    return 1
  fi
  grep -Eq 'no member named|has no member named' "$LOG_DIR/negative-compile.log"
  bash "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --contract \
    >"$LOG_DIR/submit-contract.log" 2>&1
  bash "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --implementation \
    >"$LOG_DIR/submit-implementation.log" 2>&1
  "$BUILD_DIR/test_cpp_framework_contract_headers" \
    >"$LOG_DIR/framework-contract-headers.log" 2>&1
  echo "SA-REG-01 PASS package=$VERSION header_contract=test_cpp_framework_contract_headers" \
    | tee -a "$EVIDENCE_FILE"
}

run_internal_primitive_check() {
  "$BUILD_DIR/test_cpp_framework_messaging" \
    >"$LOG_DIR/internal-primitive.log" 2>&1
  "$BUILD_DIR/zlink_cpp_framework_mesh_node_vertical_test" \
    >"$LOG_DIR/local-node-submit-contract.log" 2>&1
  local allowlist_sha
  allowlist_sha="$(sed -n "s/readonly internal_allowlist_sha256='\([^']*\)'.*/\1/p" \
    "$REPO_ROOT/scripts/verify-framework-submit-api.sh")"
  echo "SA-REG-02 PASS allowlist_sha256=$allowlist_sha primitives=test_cpp_framework_messaging,zlink_cpp_framework_mesh_node_vertical_test" \
    | tee -a "$EVIDENCE_FILE"
}

PROCESS_SELECTORS=()
for selector in "${SELECTORS[@]}"; do
  case "$selector" in
    SA-REG-01) run_contract_checks ;;
    SA-REG-02) run_internal_primitive_check ;;
    SA-REG-03)
      echo 'SA-REG-03 N/A by contract: Kotlin-only result-preservation scenario' \
        | tee -a "$EVIDENCE_FILE" ;;
    *) PROCESS_SELECTORS+=("$selector") ;;
  esac
done

if [[ "${#PROCESS_SELECTORS[@]}" -gt 0 ]]; then
  read -r CALLER_HTTP TARGET_HTTP OBJECT_CLIENT_HTTP PUBLISHER_HTTP CS_CALLER_HTTP \
    CS_TARGET_A_HTTP CS_TARGET_B_HTTP STREAM_GATEWAY_HTTP STREAM_PEER_HTTP ACTOR_TARGET_HTTP \
    CALLER_MESH TARGET_MESH OBJECT_CLIENT_MESH \
    MESH_GATE_FRONT FANOUT_ENDPOINT CS_TARGET_A_ENDPOINT CS_TARGET_B_ENDPOINT \
    STREAM_GATEWAY_ENDPOINT STREAM_GATE_FRONT STREAM_GATEWAY_ACTOR_MESH ACTOR_TARGET_MESH \
    GATE_CONTROL COLLECTOR_HTTP \
    STREAM_GATE_CONTROL < <(
    python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(24):
        value = socket.socket()
        value.bind(("127.0.0.1", 0))
        sockets.append(value)
        ports.append(value.getsockname()[1])
    print(*(f"http://127.0.0.1:{port}" for port in ports[:10]),
          *(f"tcp://127.0.0.1:{port}" for port in ports[10:21]),
          *(f"http://127.0.0.1:{port}" for port in ports[21:]))
finally:
    for value in sockets:
        value.close()
PY
  )
  CALLER_RID=submit-caller
  TARGET_RID=submit-target
  OBJECT_CLIENT_RID=submit-object-client
  TARGET_MESH="tcp://127.0.0.2:${MESH_GATE_FRONT##*:}"
  ROLE="$BUILD_DIR/zlink_cpp_e2e_submit_admission_role"
  CONFIG_DIR="$TEMP_DIR/config"
  mkdir -p "$CONFIG_DIR"

  write_role_config() {
    local output="$1" role="$2" rid="$3" http="$4" mesh="$5" peer_rid="$6" peer="$7" fanout="$8"
    local client_server="$9" client_server_peer="${10}" stream="${11}"
    python3 - "$output" "$role" "$rid" "$http" "$mesh" "$peer_rid" "$peer" "$fanout" \
      "$client_server" "$client_server_peer" "$stream" "$LOG_DIR" <<'PY'
import json
import os
import pathlib
import stat
import sys

path, role, rid, http, mesh, peer_rid, peer, fanout, client_server, client_server_peer, stream, log_dir = sys.argv[1:]
config = {"e2e": {
    "role": role, "rid": rid, "httpEndpoint": http, "meshEndpoint": mesh,
    "peerRid": peer_rid, "peerEndpoint": peer, "fanoutEndpoint": fanout,
    "clientServerEndpoint": client_server,
    "clientServerPeerEndpoint": client_server_peer,
    "streamEndpoint": stream,
    "redis": {
        "endpoint": "127.0.0.1:6379",
        "keyPrefix": f"zlink:e2e:submit-admission:{pathlib.Path(log_dir).name}:",
    },
    "logDir": log_dir
}}
if role == "target":
    config["e2e"]["meshAdvertiseHost"] = "127.0.0.1"
pathlib.Path(path).write_text(json.dumps(config, indent=2), encoding="utf-8")
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  }

  write_role_config "$CONFIG_DIR/target.json" target "$TARGET_RID" "$TARGET_HTTP" "$TARGET_MESH" '' '' '' '' '' ''
  write_role_config "$CONFIG_DIR/caller.json" caller "$CALLER_RID" "$CALLER_HTTP" "$CALLER_MESH" "$TARGET_RID" "$MESH_GATE_FRONT" '' '' '' ''
  write_role_config "$CONFIG_DIR/object-client.json" object-client "$OBJECT_CLIENT_RID" \
    "$OBJECT_CLIENT_HTTP" "$OBJECT_CLIENT_MESH" "$CALLER_RID" "$CALLER_MESH" '' '' '' ''
  write_role_config "$CONFIG_DIR/publisher.json" publisher submit-publisher "$PUBLISHER_HTTP" '' '' '' "$FANOUT_ENDPOINT" '' '' ''
  write_role_config "$CONFIG_DIR/client-server-target-a.json" client-server-target \
    submit-client-server-target-a "$CS_TARGET_A_HTTP" '' '' '' '' "$CS_TARGET_A_ENDPOINT" '' ''
  write_role_config "$CONFIG_DIR/client-server-target-b.json" client-server-target \
    submit-client-server-target-b "$CS_TARGET_B_HTTP" '' '' '' '' "$CS_TARGET_B_ENDPOINT" '' ''
  write_role_config "$CONFIG_DIR/client-server-caller.json" client-server-caller \
    submit-client-server-caller "$CS_CALLER_HTTP" '' '' '' '' \
    "$CS_TARGET_A_ENDPOINT" "$CS_TARGET_B_ENDPOINT" ''
  write_role_config "$CONFIG_DIR/stream-gateway.json" stream-gateway \
    submit-stream-gateway "$STREAM_GATEWAY_HTTP" "$STREAM_GATEWAY_ACTOR_MESH" \
    submit-actor-target "$ACTOR_TARGET_MESH" '' '' '' "$STREAM_GATEWAY_ENDPOINT"
  write_role_config "$CONFIG_DIR/stream-peer.json" stream-peer \
    submit-stream-peer "$STREAM_PEER_HTTP" '' '' '' '' '' '' "$STREAM_GATE_FRONT"
  write_role_config "$CONFIG_DIR/actor-target.json" actor-target \
    submit-actor-target "$ACTOR_TARGET_HTTP" "$ACTOR_TARGET_MESH" \
    submit-stream-gateway "$STREAM_GATEWAY_ACTOR_MESH" '' '' '' ''

  start_role() {
    local name="$1" config="$2"
    "$ROLE" --config="$config" >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
    PIDS+=("$!")
  }
  wait_health() {
    local name="$1" url="$2"
    for _ in $(seq 1 30); do
      if curl --connect-timeout 0.2 --max-time 0.2 -fsS "$url/health" >/dev/null 2>&1; then return 0; fi
      sleep 0.1
    done
    echo "Timed out waiting 3s for $name at $url" >&2
    return 1
  }

  python3 "$SCRIPT_DIR/Support/evidence_collector.py" --listen "$COLLECTOR_HTTP" \
    >"$LOG_DIR/evidence-collector.stdout.log" 2>"$LOG_DIR/evidence-collector.stderr.log" &
  PIDS+=("$!")
  wait_health evidence-collector "$COLLECTOR_HTTP"

  mesh_process=0
  for selector in "${PROCESS_SELECTORS[@]}"; do
    case "$selector" in
      SA-E2E-01|SA-E2E-08|SA-E2E-09|SA-E2E-20) mesh_process=1 ;;
    esac
  done
  if [[ "$mesh_process" == 1 ]]; then
    start_role target "$CONFIG_DIR/target.json"
    wait_health target "$TARGET_HTTP"
    python3 "$SCRIPT_DIR/Support/receiver_gate.py" \
      --listen "$MESH_GATE_FRONT" --backend "$TARGET_MESH" --control "$GATE_CONTROL" \
      >"$LOG_DIR/mesh-receiver-gate.stdout.log" 2>"$LOG_DIR/mesh-receiver-gate.stderr.log" &
    PIDS+=("$!")
    wait_health mesh-receiver-gate "$GATE_CONTROL"
    start_role caller "$CONFIG_DIR/caller.json"
    wait_health caller "$CALLER_HTTP"
    for _ in $(seq 1 30); do
      if curl --connect-timeout 0.2 --max-time 0.2 -sS \
          "$CALLER_HTTP/ready?targetRid=$TARGET_RID" \
          >"$LOG_DIR/route-ready-last.json" 2>/dev/null \
          && python3 - "$LOG_DIR/route-ready-last.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    raise SystemExit(0 if json.load(source).get("ready") is True else 1)
PY
      then
        ready=1
        break
      fi
      sleep 0.1
    done
    if [[ "${ready:-0}" != 1 ]]; then
      cat "$LOG_DIR/route-ready-last.json" >&2 || true
      echo "Target route did not become ready within 3s." >&2
      exit 1
    fi
  fi
  if contains SA-E2E-08 "${PROCESS_SELECTORS[@]}"; then
    start_role object-client "$CONFIG_DIR/object-client.json"
    wait_health object-client "$OBJECT_CLIENT_HTTP"
    object_client_ready=0
    for _ in $(seq 1 30); do
      if curl --connect-timeout 0.2 --max-time 0.2 -sS \
          "$CALLER_HTTP/ready?targetRid=$OBJECT_CLIENT_RID" \
          >"$LOG_DIR/object-client-ready-last.json" 2>/dev/null \
          && python3 - "$LOG_DIR/object-client-ready-last.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    raise SystemExit(0 if json.load(source).get("ready") is True else 1)
PY
      then
        object_client_ready=1
        break
      fi
      sleep 0.1
    done
    if [[ "$object_client_ready" != 1 ]]; then
      cat "$LOG_DIR/object-client-ready-last.json" >&2 || true
      echo "Object Client route did not become Ready within 3s." >&2
      exit 1
    fi
  fi
  if contains SA-E2E-14 "${PROCESS_SELECTORS[@]}"; then
    start_role publisher "$CONFIG_DIR/publisher.json"
    wait_health publisher "$PUBLISHER_HTTP"
  fi

  python3 "$SCRIPT_DIR/Support/scenario_client.py" \
    --caller-url "$CALLER_HTTP" --target-url "$TARGET_HTTP" \
    --publisher-url "$PUBLISHER_HTTP" --caller-rid "$CALLER_RID" \
    --target-rid "$TARGET_RID" --object-client-rid "$OBJECT_CLIENT_RID" \
    --client-server-caller-url "$CS_CALLER_HTTP" \
    --client-server-target-url "$CS_TARGET_A_HTTP" \
    --client-server-target-url "$CS_TARGET_B_HTTP" \
    --stream-gateway-url "$STREAM_GATEWAY_HTTP" --stream-peer-url "$STREAM_PEER_HTTP" \
    --actor-target-url "$ACTOR_TARGET_HTTP" \
    --stream-gateway-rid submit-stream-gateway --actor-target-rid submit-actor-target \
    --receiver-gate-url "$GATE_CONTROL" --stream-gate-url "$STREAM_GATE_CONTROL" \
    --collector-url "$COLLECTOR_HTTP" \
    --socket-buffer-manifest "$SCRIPT_DIR/Support/socket-buffer-manifest.json" \
    --evidence "$EVIDENCE_FILE" \
    "${PROCESS_SELECTORS[@]}" 2>&1 | tee "$LOG_DIR/client.log"
fi

bash "$REPO_ROOT/scripts/verify-framework-doc-contracts.sh" \
  >"$LOG_DIR/doc-contracts.log" 2>&1
VERSION_AFTER="$({ sed -n 's/.*ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "\([0-9][0-9.]*\)".*/\1/p' "$CPP_DIR/CMakeLists.txt"; } | head -n1)"
if [[ "$VERSION_AFTER" != "$VERSION" ]]; then
  echo "C++ framework binding reference changed during the run: $VERSION -> $VERSION_AFTER" >&2
  exit 1
fi
echo "shared_version_reference_unchanged=$VERSION_AFTER" | tee -a "$LOG_DIR/package-mode.log"
echo "SubmitAdmission PASS scenarios=${SELECTORS[*]} logs=$LOG_DIR"
