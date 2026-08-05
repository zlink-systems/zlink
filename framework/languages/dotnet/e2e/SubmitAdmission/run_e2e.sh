#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
SERVER_PROJECT="$SCRIPT_DIR/Server/SubmitAdmission.Server.csproj"
CLIENT_PROJECT="$SCRIPT_DIR/Client/SubmitAdmission.Client.csproj"
GATE_PROJECT="$SCRIPT_DIR/ReceiverGate/SubmitAdmission.ReceiverGate.csproj"
GATE_PLATFORM_MANIFEST="$SCRIPT_DIR/ReceiverGate/platform-socket-buffers.json"
SERVER_DLL="$SCRIPT_DIR/Server/bin/Debug/net8.0/SubmitAdmission.Server.dll"
CLIENT_DLL="$SCRIPT_DIR/Client/bin/Debug/net8.0/SubmitAdmission.Client.dll"
GATE_DLL="$SCRIPT_DIR/ReceiverGate/bin/Debug/net8.0/SubmitAdmission.ReceiverGate.dll"
UNIT_PROJECT="$SCRIPT_DIR/../../tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj"
SCENARIO="${*:-all}"
SCENARIO="${SCENARIO// /,}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
CONFIG_DIR="$(mktemp -d)"
CANDIDATE_ROOT="${ZLINK_SUBMIT_ADMISSION_PACKAGE_ROOT:-}"
CORE_RUNTIME="${ZLINK_SUBMIT_ADMISSION_CORE_RUNTIME:-}"
NUGET_CONFIG=""
CANDIDATE_PACKAGE=""
mkdir -p "$LOG_DIR"

if [[ -n "$CANDIDATE_ROOT" ]]; then
  CANDIDATE_ROOT="$(realpath "$CANDIDATE_ROOT")"
  mapfile -t candidate_packages < <(find "$CANDIDATE_ROOT/nuget" -maxdepth 1 -type f \
    -name 'Systems.Zlink.*.nupkg' -print | sort)
  if [[ "${#candidate_packages[@]}" != 1 ]]; then
    echo "Candidate mode requires exactly one Systems.Zlink package under $CANDIDATE_ROOT/nuget." >&2
    exit 2
  fi
  CANDIDATE_PACKAGE="${candidate_packages[0]}"
  NUGET_CONFIG="$CONFIG_DIR/NuGet.Config"
  export NUGET_PACKAGES="$LOG_DIR/nuget-packages"
  export NUGET_HTTP_CACHE_PATH="$LOG_DIR/nuget-http-cache"
  python3 - "$NUGET_CONFIG" "$CANDIDATE_ROOT/nuget" <<'PY'
import pathlib
import sys
import xml.sax.saxutils

output = pathlib.Path(sys.argv[1])
candidate = xml.sax.saxutils.escape(str(pathlib.Path(sys.argv[2]).resolve()))
output.write_text(
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<configuration>\n"
    "  <packageSources>\n"
    "    <clear />\n"
    f"    <add key=\"candidate\" value=\"{candidate}\" />\n"
    "    <add key=\"nuget.org\" value=\"https://api.nuget.org/v3/index.json\" />\n"
    "  </packageSources>\n"
    "  <packageSourceMapping>\n"
    "    <packageSource key=\"candidate\">\n"
    "      <package pattern=\"Systems.Zlink\" />\n"
    "    </packageSource>\n"
    "    <packageSource key=\"nuget.org\">\n"
    "      <package pattern=\"*\" />\n"
    "    </packageSource>\n"
    "  </packageSourceMapping>\n"
    "</configuration>\n",
    encoding="utf-8")
PY
fi

restore_candidate_project() {
  local project="$1"
  dotnet restore "$project" --configfile "$NUGET_CONFIG" --force --no-cache \
    --disable-parallel --verbosity quiet
}

build_project() {
  local project="$1"
  shift
  if [[ -n "$CANDIDATE_ROOT" ]]; then
    restore_candidate_project "$project"
    dotnet build "$project" --no-restore "$@"
  else
    dotnet build "$project" "$@"
  fi
}

record_candidate_evidence() {
  [[ -n "$CANDIDATE_ROOT" ]] || return 0
  local package_version package_cache metadata resolved_package native_entry native_name package_native output_native
  package_version="$(unzip -p "$CANDIDATE_PACKAGE" 'Systems.Zlink.nuspec' \
    | sed -n 's:.*<version>\([^<]*\)</version>.*:\1:p')"
  package_cache="$NUGET_PACKAGES/systems.zlink/$package_version"
  metadata="$package_cache/.nupkg.metadata"
  resolved_package="$package_cache/systems.zlink.$package_version.nupkg"
  native_entry="$(unzip -Z1 "$CANDIDATE_PACKAGE" \
    | grep -E '^runtimes/linux-x64/native/libzlink\.so\.[0-9]+\.[0-9]+\.[0-9]+$')"
  if [[ "$(printf '%s\n' "$native_entry" | grep -c .)" != 1 ]]; then
    echo "Candidate package must contain exactly one versioned Linux x64 Core runtime." >&2
    return 1
  fi
  native_name="${native_entry##*/}"
  package_native="$CONFIG_DIR/$native_name"
  output_native="$SCRIPT_DIR/Server/bin/Debug/net8.0/runtimes/linux-x64/native/$native_name"

  unzip -p "$CANDIDATE_PACKAGE" "$native_entry" >"$package_native"
  python3 - "$metadata" "$CANDIDATE_ROOT/nuget" <<'PY'
import json
import pathlib
import sys

metadata = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
expected = pathlib.Path(sys.argv[2]).resolve()
actual = pathlib.Path(metadata["source"]).resolve()
if actual != expected:
    raise SystemExit(f"Systems.Zlink resolved from {actual}, expected {expected}")
PY
  [[ "$(sha256sum "$CANDIDATE_PACKAGE" | awk '{print $1}')" \
      == "$(sha256sum "$resolved_package" | awk '{print $1}')" ]]
  [[ "$(sha256sum "$package_native" | awk '{print $1}')" \
      == "$(sha256sum "$output_native" | awk '{print $1}')" ]]
  if [[ -n "$CORE_RUNTIME" ]]; then
    CORE_RUNTIME="$(realpath "$CORE_RUNTIME")"
    [[ "$(sha256sum "$package_native" | awk '{print $1}')" \
        == "$(sha256sum "$CORE_RUNTIME" | awk '{print $1}')" ]]
  fi

  {
    printf 'candidate_package=%s\n' "$CANDIDATE_PACKAGE"
    printf 'candidate_package_sha256=%s\n' "$(sha256sum "$CANDIDATE_PACKAGE" | awk '{print $1}')"
    printf 'resolved_source=%s\n' "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source"])' "$metadata")"
    printf 'resolved_package_sha256=%s\n' "$(sha256sum "$resolved_package" | awk '{print $1}')"
    printf 'package_native_sha256=%s\n' "$(sha256sum "$package_native" | awk '{print $1}')"
    printf 'package_native_build_id=%s\n' "$(readelf -n "$package_native" | sed -n 's/.*Build ID: //p')"
    printf 'output_native_sha256=%s\n' "$(sha256sum "$output_native" | awk '{print $1}')"
    if [[ -n "$CORE_RUNTIME" ]]; then
      printf 'core_runtime=%s\n' "$CORE_RUNTIME"
      printf 'core_runtime_sha256=%s\n' "$(sha256sum "$CORE_RUNTIME" | awk '{print $1}')"
      printf 'core_runtime_build_id=%s\n' "$(readelf -n "$CORE_RUNTIME" | sed -n 's/.*Build ID: //p')"
    fi
  } >"$LOG_DIR/candidate-package.evidence.log"
}

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

CALLER_HTTP_PORT="$(pick_port)"
TARGET_HTTP_PORT="$(pick_port)"
GATE_HTTP_PORT="$(pick_port)"
PUBLISHER_HTTP_PORT="$(pick_port)"
CALLER_MESH_PORT="$(pick_port)"
TARGET_MESH_PORT="$(pick_port)"
GATE_MESH_PORT="$(pick_port)"
FANOUT_PORT="$(pick_port)"
OBJECT_CLIENT_HTTP_PORT="$(pick_port)"
OBJECT_CLIENT_MESH_PORT="$(pick_port)"
CALLER_URL="http://127.0.0.1:$CALLER_HTTP_PORT"
TARGET_URL="http://127.0.0.1:$TARGET_HTTP_PORT"
GATE_URL="http://127.0.0.1:$GATE_HTTP_PORT"
PUBLISHER_URL="http://127.0.0.1:$PUBLISHER_HTTP_PORT"
CALLER_ENDPOINT="tcp://127.0.0.1:$CALLER_MESH_PORT"
TARGET_ENDPOINT="tcp://127.0.0.1:$TARGET_MESH_PORT"
GATE_ENDPOINT="tcp://127.0.0.1:$GATE_MESH_PORT"
FANOUT_ENDPOINT="tcp://127.0.0.1:$FANOUT_PORT"
OBJECT_CLIENT_URL="http://127.0.0.1:$OBJECT_CLIENT_HTTP_PORT"
OBJECT_CLIENT_ENDPOINT="tcp://127.0.0.1:$OBJECT_CLIENT_MESH_PORT"
CALLER_RID="submit-caller"
TARGET_RID="submit-target"
PIDS=()
REDIS_CONTAINER=""
REDIS_ENDPOINT=""
REDIS_KEY_PREFIX="zlink:e2e:cfg13:$STAMP"

cleanup() {
  local code=$?
  rm -rf "$CONFIG_DIR"
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -INT -- "-$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
    fi
  done
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    local alive=0
    for pid in "${PIDS[@]:-}"; do
      if kill -0 "$pid" 2>/dev/null; then alive=1; break; fi
    done
    [[ "$alive" == 0 ]] && break
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    fi
  done
  wait "${PIDS[@]:-}" 2>/dev/null || true
  if [[ -n "$REDIS_CONTAINER" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  if [[ "$code" != 0 ]]; then echo "SubmitAdmission failed; logs=$LOG_DIR" >&2; fi
}
trap cleanup EXIT

write_config() {
  local output="$1"
  shift
  python3 "$SCRIPT_DIR/../write_role_config.py" "$output" -- "$@"
}

start_role() {
  local name="$1" config="$2"
  setsid dotnet "$SERVER_DLL" --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  PIDS+=("$!")
}

start_gate() {
  setsid dotnet "$GATE_DLL" \
    --listen-port "$GATE_MESH_PORT" --target-port "$TARGET_MESH_PORT" --http-url "$GATE_URL" \
    >"$LOG_DIR/receiver-gate.stdout.log" 2>"$LOG_DIR/receiver-gate.stderr.log" &
  PIDS+=("$!")
}

needs_object_client() {
  [[ "$SCENARIO" == "all"
     || ",$SCENARIO," == *",SA-E2E-08,"* ]]
}

wait_health() {
  local name="$1" url="$2"
  for _ in $(seq 1 30); do
    if curl --connect-timeout 0.2 --max-time 0.2 -fsS "$url/health" >/dev/null 2>&1; then return; fi
    sleep 0.1
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $url" >&2
  return 1
}

run_regression_checks() {
  if rg -n '\b(TrySubmit|trySubmit|try_submit)\b' \
    "$SCRIPT_DIR/../..//src/Zlink.Framework/Contracts" \
    "$SCRIPT_DIR/../..//samples" \
    "$SCRIPT_DIR/../..//e2e" \
    --glob '*.cs' --glob '!**/ContractNegative/**' --glob '!**/bin/**' --glob '!**/obj/**' \
    >"$LOG_DIR/removed-surface.log"; then
    cat "$LOG_DIR/removed-surface.log" >&2
    echo "SA-REG-01 failed: removed public TrySubmit remains." >&2
    return 1
  fi

  local negative_dir="$CONFIG_DIR/contract-negative"
  mkdir -p "$negative_dir"
  python3 - "$negative_dir" "$SCRIPT_DIR/../../src/Zlink.Framework/Zlink.Framework.csproj" <<'PY'
import pathlib
import sys

directory = pathlib.Path(sys.argv[1])
project_reference = pathlib.Path(sys.argv[2]).resolve()
(directory / "ContractNegative.csproj").write_text(
    "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
    "  <PropertyGroup><TargetFramework>net8.0</TargetFramework><OutputType>Exe</OutputType></PropertyGroup>\n"
    f"  <ItemGroup><ProjectReference Include=\"{project_reference}\" /></ItemGroup>\n"
    "</Project>\n",
    encoding="utf-8")
(directory / "Program.cs").write_text(
    "using Zlink.Framework.Contracts.Channels;\n"
    "static void RemovedSurfaceMustNotCompile(IZLinkSendCall call)\n"
    "{\n"
    "    _ = call.Try" + "Submit();\n"
    "}\n",
    encoding="utf-8")
PY
  if build_project "$negative_dir/ContractNegative.csproj" --maxcpucount:1 \
    >"$LOG_DIR/negative.stdout.log" 2>"$LOG_DIR/negative.stderr.log"; then
    echo "SA-REG-01 failed: removed TrySubmit compiled." >&2
    return 1
  fi
  grep -Eq "CS1061|does not contain a definition for 'TrySubmit'" \
    "$LOG_DIR/negative.stdout.log" "$LOG_DIR/negative.stderr.log"

  dotnet test "$UNIT_PROJECT" \
    --no-build --filter 'FullyQualifiedName~ZLinkAsyncSubmitterTests' --logger 'console;verbosity=minimal' \
    >"$LOG_DIR/internal-primitive.stdout.log" 2>"$LOG_DIR/internal-primitive.stderr.log"
  echo "SA-REG-01 PASS"
  echo "SA-REG-02 PASS"
}

if [[ "$SCENARIO" == "SA-REG-01" || "$SCENARIO" == "SA-REG-02" ]]; then
  if [[ -n "$CANDIDATE_ROOT" ]]; then
    build_project "$SERVER_PROJECT" --maxcpucount:1 >/dev/null
    build_project "$UNIT_PROJECT" --maxcpucount:1 >/dev/null
    record_candidate_evidence
  fi
  run_regression_checks
  exit 0
fi

build_project "$SERVER_PROJECT" --maxcpucount:1 >/dev/null
build_project "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null
build_project "$GATE_PROJECT" --maxcpucount:1 >/dev/null
if [[ -n "$CANDIDATE_ROOT" ]]; then
  build_project "$UNIT_PROJECT" --maxcpucount:1 >/dev/null
  record_candidate_evidence
fi

CALLER_CONFIG="$CONFIG_DIR/caller.json"
TARGET_CONFIG="$CONFIG_DIR/target.json"
PUBLISHER_CONFIG="$CONFIG_DIR/publisher.json"
CLIENT_CONFIG="$CONFIG_DIR/client.json"
OBJECT_CLIENT_CONFIG="$CONFIG_DIR/object-client.json"
write_config "$CALLER_CONFIG" \
  --role caller --rid "$CALLER_RID" --http-url "$CALLER_URL" \
  --mesh-endpoint "$CALLER_ENDPOINT" --peer-rid "$TARGET_RID" --peer-endpoint "$GATE_ENDPOINT" \
  --evidence-file "$LOG_DIR/caller.evidence.log"
write_config "$TARGET_CONFIG" \
  --role target --rid "$TARGET_RID" --http-url "$TARGET_URL" \
  --mesh-endpoint "$TARGET_ENDPOINT" --evidence-file "$LOG_DIR/target.evidence.log"
write_config "$PUBLISHER_CONFIG" \
  --role publisher --rid submit-publisher --http-url "$PUBLISHER_URL" \
  --fanout-endpoint "$FANOUT_ENDPOINT" --evidence-file "$LOG_DIR/publisher.evidence.log"
write_config "$CLIENT_CONFIG" \
  --caller-url "$CALLER_URL" --target-url "$TARGET_URL" --publisher-url "$PUBLISHER_URL" \
  --caller-rid "$CALLER_RID" --target-rid "$TARGET_RID" \
  --object-client-rid "$TARGET_RID" --scenario "$SCENARIO"

if needs_object_client; then
  zlink_redis_start_scoped_assign \
    REDIS_CONTAINER \
    REDIS_ENDPOINT \
    "zlink-redis-dotnet-e2e-submit-admission" \
    "redis:7.2-alpine" \
    "$LOG_DIR"
  zlink_redis_wait_ready \
    "$REDIS_CONTAINER" 60 "$LOCAL_READINESS_POLL_SECONDS"
  write_config "$OBJECT_CLIENT_CONFIG" \
    --role object-client --rid submit-object-client \
    --http-url "$OBJECT_CLIENT_URL" \
    --mesh-endpoint "$OBJECT_CLIENT_ENDPOINT" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
fi

start_role target "$TARGET_CONFIG"
wait_health target "$TARGET_URL"
start_gate
wait_health receiver-gate "$GATE_URL"
start_role caller "$CALLER_CONFIG"
wait_health caller "$CALLER_URL"
if [[ "$SCENARIO" != "SA-E2E-08" ]]; then
  start_role publisher "$PUBLISHER_CONFIG"
  wait_health publisher "$PUBLISHER_URL"
fi

OBJECT_CLIENT_RID="$TARGET_RID"
if needs_object_client; then
  start_role object-client "$OBJECT_CLIENT_CONFIG"
  wait_health object-client "$OBJECT_CLIENT_URL"
  object_client_identity="$(curl --connect-timeout 0.2 --max-time 2 -fsS \
    "$OBJECT_CLIENT_URL/object-client/identity")"
  read -r OBJECT_CLIENT_RID object_client_endpoint < <(
    python3 -c 'import json,sys; value=json.load(sys.stdin); print(value["rid"], value["endpoint"])' \
      <<<"$object_client_identity")
  curl --connect-timeout 0.2 --max-time 2 -fsS -X POST \
    "$CALLER_URL/admin/connect-object-client?rid=$OBJECT_CLIENT_RID&endpoint=$object_client_endpoint" \
    >/dev/null
  for _ in $(seq 1 300); do
    peer_state="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/topology/peer/$OBJECT_CLIENT_RID" 2>/dev/null || true)"
    if [[ -n "$peer_state" ]] && python3 - "$peer_state" <<'PY'
import json
import sys
raise SystemExit(0 if json.loads(sys.argv[1])["state"] == "Ready" else 1)
PY
    then
      break
    fi
    sleep 0.1
  done
  curl --connect-timeout 0.2 --max-time 1 -fsS \
    "$CALLER_URL/topology/peer/$OBJECT_CLIENT_RID" \
    >"$LOG_DIR/object-client-peer-before.json"
  write_config "$CLIENT_CONFIG" \
    --caller-url "$CALLER_URL" --target-url "$TARGET_URL" --publisher-url "$PUBLISHER_URL" \
    --caller-rid "$CALLER_RID" --target-rid "$TARGET_RID" \
    --object-client-rid "$OBJECT_CLIENT_RID" --scenario "$SCENARIO"
fi

for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
  if curl --connect-timeout 0.2 --max-time 0.2 -fsS \
    "$CALLER_URL/ready/$TARGET_RID" >/dev/null 2>&1; then break; fi
  sleep "$LOCAL_READINESS_POLL_SECONDS"
done
curl --connect-timeout 0.2 --max-time 0.5 -fsS "$CALLER_URL/ready/$TARGET_RID" >/dev/null

if [[ "$SCENARIO" == "SA-E2E-02" || "$SCENARIO" == "SA-E2E-03" \
      || "$SCENARIO" == "SA-E2E-04" \
      || "$SCENARIO" == "SA-E2E-06" \
      || "$SCENARIO" == "SA-E2E-07" \
      || "$SCENARIO" == "SA-REG-04" ]]; then
  gate_closed_json="$(curl --connect-timeout 0.2 --max-time 2 -fsS -X POST \
    "$GATE_URL/gate/close")"
  read -r gate_closed_at requested_socket_buffer caller_rcvbuf caller_sndbuf target_rcvbuf target_sndbuf < <(
    python3 -c 'import json,sys; value=json.load(sys.stdin); print(value["closedAtCallerBytes"], value["requestedSocketBufferBytes"], value["callerReceiveBufferBytes"], value["callerSendBufferBytes"], value["targetReceiveBufferBytes"], value["targetSendBufferBytes"])' \
      <<<"$gate_closed_json")
  platform_key="$(python3 - <<'PY'
import platform

system = platform.system().lower()
machine = platform.machine().lower()
architecture = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64"}.get(machine, machine)
print(f"{system}-{architecture}")
PY
)"
  python3 - "$GATE_PLATFORM_MANIFEST" "$platform_key" \
    "$requested_socket_buffer" "$caller_rcvbuf" "$caller_sndbuf" \
    "$target_rcvbuf" "$target_sndbuf" <<'PY'
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
platform_key = sys.argv[2]
if platform_key not in manifest:
    raise SystemExit(f"ReceiverGate has no checked-in socket buffer manifest for {platform_key}")
expected = manifest[platform_key]
actual = {
    "requestedSocketBufferBytes": int(sys.argv[3]),
    "callerReceiveBufferBytes": int(sys.argv[4]),
    "callerSendBufferBytes": int(sys.argv[5]),
    "targetReceiveBufferBytes": int(sys.argv[6]),
    "targetSendBufferBytes": int(sys.argv[7]),
}
for name, value in actual.items():
    if value != expected[name]:
        raise SystemExit(
            f"ReceiverGate {platform_key} {name}={value}, expected {expected[name]}")
PY
  fill_url="$CALLER_URL/submit/fill/node/$TARGET_RID"
  if [[ "$SCENARIO" == "SA-E2E-07" ]]; then fill_url="$fill_url?cancelable=true"; fi
  fill_json="$(curl --connect-timeout 0.2 --max-time 3 -fsS -X POST "$fill_url")"
  read -r operation_id pending started_count terminal_status < <(
    python3 -c 'import json,sys; value=json.load(sys.stdin); print(value["operationId"], str(value["pending"]).lower(), value["startedCount"], value.get("terminalStatus") or "-")' \
      <<<"$fill_json")
  if [[ -z "$operation_id" || "$pending" != "true" ]]; then
    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
    echo "$SCENARIO setup did not produce an incomplete public submit: $fill_json" >&2
    exit 1
  fi

  if [[ "$SCENARIO" == "SA-REG-04" ]]; then
    pending_evidence="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/evidence/$operation_id")"
    python3 - "$operation_id" "$pending_evidence" <<'PY'
import json
import sys

value = json.loads(sys.argv[2])
if value["operationId"] != sys.argv[1]:
    raise SystemExit("SA-REG-04 operation evidence mismatch")
if value["terminalCount"] != 0 or value["pendingWaiterCount"] != 1:
    raise SystemExit(f"SA-REG-04 operation was not pending before disposal: {value}")
if value["reservationCount"] != 1 or value["callbackCount"] != 0:
    raise SystemExit(f"SA-REG-04 resource baseline mismatch: {value}")
PY

    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST \
      "$GATE_URL/gate/open" >"$LOG_DIR/sa-reg-04-gate-open.log" &
    gate_open_pid=$!
    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST \
      "$CALLER_URL/admin/stop-twice" >"$LOG_DIR/sa-reg-04-stop.log" &
    stop_pid=$!
    wait "$gate_open_pid"
    wait "$stop_pid"

    caller_pid="${PIDS[2]}"
    caller_exit="running"
    for _ in $(seq 1 30); do
      caller_state="$(ps -o stat= -p "$caller_pid" 2>/dev/null || true)"
      if [[ -z "$caller_state" || "$caller_state" == Z* ]]; then
        set +e
        wait "$caller_pid"
        caller_exit=$?
        set -e
        break
      fi
      sleep 0.1
    done
    if [[ "$caller_exit" != 0 ]]; then
      echo "SA-REG-04 caller did not terminate within 3s." >&2
      exit 1
    fi
    if rg -in 'assertion|double free|aborted|callback-after-dispose' \
      "$LOG_DIR/caller.stderr.log" >"$LOG_DIR/sa-reg-04-native-errors.log"; then
      cat "$LOG_DIR/sa-reg-04-native-errors.log" >&2
      echo "SA-REG-04 detected a native lifecycle failure." >&2
      exit 1
    fi

    python3 - "$LOG_DIR/caller.evidence.log" "$operation_id" \
      "$LOG_DIR/sa-reg-04.evidence.log" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
operation_id = sys.argv[2]
output = pathlib.Path(sys.argv[3])
entries = [
    line.strip() for line in source.read_text(encoding="utf-8").splitlines()
    if f"operation={operation_id}" in line
]
terminal = [line for line in entries if line.startswith("terminal|")]
cleanup = [line for line in entries if "|event=cleanup|" in line]
attempts = [line for line in entries if "|event=transport-attempt|" in line]
commits = [line for line in entries if "|event=commit|" in line]
signals = [line for line in entries if "|event=send-ready|" in line]
retries = [line for line in entries if "|event=retry-attempt|" in line]
if len(terminal) != 1:
    raise SystemExit(f"SA-REG-04 terminal count={len(terminal)} entries={entries}")
if len(cleanup) != 1:
    raise SystemExit(f"SA-REG-04 cleanup count={len(cleanup)} entries={entries}")
if "pending=0|reservations=0|callbacks=0" not in cleanup[0]:
    raise SystemExit(f"SA-REG-04 cleanup snapshot mismatch: {cleanup[0]}")
if len(attempts) not in (1, 2) or len(commits) not in (0, 1):
    raise SystemExit(f"SA-REG-04 attempt/commit mismatch: {entries}")
if len(signals) != len(retries) or len(signals) > 1:
    raise SystemExit(f"SA-REG-04 signal/retry mismatch: {entries}")
status = terminal[0].rsplit("status=", 1)[-1]
if status not in ("Submitted", "Shutdown"):
    raise SystemExit(f"SA-REG-04 terminal status={status}")
output.write_text(
    "dispose_request_count=2\n"
    "caller_exit_code=0\n"
    "native_lifecycle_error_count=0\n"
    f"operation_id={operation_id}\n"
    f"terminal_status={status}\n"
    "terminal_count=1\n"
    f"transport_attempt_count={len(attempts)}\n"
    f"commit_count={len(commits)}\n"
    f"send_ready_signal_count={len(signals)}\n"
    f"retry_attempt_count={len(retries)}\n"
    "pending_waiter_count=0\n"
    "reservation_count=0\n"
    "callback_count=0\n",
    encoding="utf-8")
PY
    echo "SA-REG-04 integration diagnostic PASS"
    echo "SubmitAdmission PASS logs=$LOG_DIR"
    exit 0
  fi

  if [[ "$SCENARIO" == "SA-E2E-06" ]]; then
    pending_evidence="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/evidence/$operation_id")"
    python3 - "$operation_id" "$pending_evidence" <<'PY'
import json
import sys

value = json.loads(sys.argv[2])
if value["operationId"] != sys.argv[1]:
    raise SystemExit("SA-E2E-06 operation evidence mismatch")
if value["terminalCount"] != 0 or value["pendingWaiterCount"] != 1:
    raise SystemExit(f"SA-E2E-06 operation was not pending before shutdown: {value}")
if value["transportAttemptCount"] != 1 or value["commitCount"] != 0:
    raise SystemExit(f"SA-E2E-06 initial admission mismatch: {value}")
PY

    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST \
      "$CALLER_URL/admin/stop-twice" >"$LOG_DIR/sa-e2e-06-stop.log"

    caller_pid="${PIDS[2]}"
    caller_exit="running"
    for _ in $(seq 1 30); do
      caller_state="$(ps -o stat= -p "$caller_pid" 2>/dev/null || true)"
      if [[ -z "$caller_state" || "$caller_state" == Z* ]]; then
        set +e
        wait "$caller_pid"
        caller_exit=$?
        set -e
        break
      fi
      sleep 0.1
    done
    if [[ "$caller_exit" != 0 ]]; then
      echo "SA-E2E-06 caller did not terminate cleanly within 3s." >&2
      exit 1
    fi
    if rg -in 'assertion|double free|aborted|callback-after-dispose' \
      "$LOG_DIR/caller.stderr.log" >"$LOG_DIR/sa-e2e-06-native-errors.log"; then
      cat "$LOG_DIR/sa-e2e-06-native-errors.log" >&2
      echo "SA-E2E-06 detected a native lifecycle failure." >&2
      exit 1
    fi

    python3 - "$LOG_DIR/caller.evidence.log" "$operation_id" \
      "$LOG_DIR/shutdown-before-admission.evidence.log" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
operation_id = sys.argv[2]
output = pathlib.Path(sys.argv[3])
entries = [
    line.strip() for line in source.read_text(encoding="utf-8").splitlines()
    if f"operation={operation_id}" in line
]
terminal = [line for line in entries if line.startswith("terminal|")]
cleanup = [line for line in entries if "|event=cleanup|" in line]
attempts = [line for line in entries if "|event=transport-attempt|" in line]
commits = [line for line in entries if "|event=commit|" in line]
signals = [line for line in entries if "|event=send-ready|" in line]
retries = [line for line in entries if "|event=retry-attempt|" in line]
if len(terminal) != 1 or not terminal[0].endswith("status=Shutdown"):
    raise SystemExit(f"SA-E2E-06 terminal mismatch: {entries}")
if len(cleanup) != 1 or "pending=0|reservations=0|callbacks=0" not in cleanup[0]:
    raise SystemExit(f"SA-E2E-06 cleanup mismatch: {entries}")
if len(attempts) != 1 or commits or signals or retries:
    raise SystemExit(f"SA-E2E-06 late admission mismatch: {entries}")
output.write_text(
    "admission_closed_before_release=true\n"
    "caller_exit_code=0\n"
    "native_lifecycle_error_count=0\n"
    f"operation_id={operation_id}\n"
    "terminal_status=Shutdown\n"
    "terminal_count=1\n"
    "transport_attempt_count=1\n"
    "commit_count=0\n"
    "send_ready_signal_count=0\n"
    "retry_attempt_count=0\n"
    "pending_waiter_count=0\n"
    "reservation_count=0\n"
    "callback_count=0\n",
    encoding="utf-8")
PY
    echo "SA-E2E-06 integration diagnostic PASS"
    echo "SubmitAdmission PASS logs=$LOG_DIR"
    exit 0
  fi

  if [[ "$SCENARIO" == "SA-E2E-07" ]]; then
    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST \
      "$CALLER_URL/admin/cancel/$operation_id" >/dev/null

    terminal_status=""
    for _ in $(seq 1 30); do
      evidence_json="$(curl --connect-timeout 0.2 --max-time 0.2 -fsS \
        "$CALLER_URL/evidence/$operation_id" 2>/dev/null || true)"
      read -r terminal_status transport_attempts commit_count signal_count retry_count pending_waiters reservations callbacks < <(
        python3 -c 'import json,sys; value=json.load(sys.stdin); ready=value.get("terminalCount")==1 and value.get("pendingWaiterCount")==0 and value.get("reservationCount")==0; print((value.get("terminalStatus") or "-") if ready else "-", value.get("transportAttemptCount",0), value.get("commitCount",0), value.get("sendReadySignalCount",0), value.get("retryAttemptCount",0), value.get("pendingWaiterCount",0), value.get("reservationCount",0), value.get("callbackCount",0))' \
        <<<"$evidence_json" 2>/dev/null || true)
      [[ "$terminal_status" != "-" ]] && break
      sleep 0.1
    done
    if [[ "$terminal_status" != "TaskCanceledException" || "$transport_attempts" != "1" \
          || "$commit_count" != "0" || "$signal_count" != "0" || "$retry_count" != "0" \
          || "$pending_waiters" != "0" || "$reservations" != "0" || "$callbacks" != "0" ]]; then
      curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
      echo "SA-E2E-07 cancellation evidence mismatch: $evidence_json" >&2
      exit 1
    fi

    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null
    gate_resumed="false"
    for _ in $(seq 1 30); do
      gate_status_json="$(curl --connect-timeout 0.2 --max-time 0.2 -fsS \
        "$GATE_URL/gate/status" 2>/dev/null || true)"
      gate_resumed="$(python3 -c 'import json,sys; value=json.load(sys.stdin); print(str(value["open"] and value["callerBytesRead"] > value["closedAtCallerBytes"]).lower())' \
        <<<"$gate_status_json" 2>/dev/null || true)"
      [[ "$gate_resumed" == "true" ]] && break
      sleep 0.1
    done
    if [[ "$gate_resumed" != "true" ]]; then
      echo "SA-E2E-07 receiver gate did not resume forwarding: $gate_status_json" >&2
      exit 1
    fi
    late_evidence="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/evidence/$operation_id")"
    python3 - "$late_evidence" <<'PY'
import json
import sys

value = json.loads(sys.argv[1])
if value["terminalCount"] != 1 or value["transportAttemptCount"] != 1 or value["commitCount"] != 0:
    raise SystemExit(f"SA-E2E-07 admitted after cancellation: {value}")
if value["sendReadySignalCount"] != 0 or value["retryAttemptCount"] != 0:
    raise SystemExit(f"SA-E2E-07 retained a callback after cancellation: {value}")
if value["pendingWaiterCount"] != 0 or value["reservationCount"] != 0 or value["callbackCount"] != 0:
    raise SystemExit(f"SA-E2E-07 leaked admission resources: {value}")
PY

    dotnet "$CLIENT_DLL" --config "$CLIENT_CONFIG" | tee "$LOG_DIR/client.stdout.log"
    {
      printf 'receiver_gate=true\n'
      printf 'operation_id=%s\n' "$operation_id"
      printf 'terminal_status=Cancelled\n'
      printf 'terminal_count=1\n'
      printf 'transport_attempt_count=1\n'
      printf 'commit_count=0\n'
      printf 'late_transport_attempt_count=0\n'
      printf 'late_commit_count=0\n'
      printf 'pending_waiter_count=0\n'
      printf 'reservation_count=0\n'
      printf 'callback_count=0\n'
      printf 'pre_cancelled_and_invalid_precedence=true\n'
    } >"$LOG_DIR/cancellation-late-admission.evidence.log"
    echo "SA-E2E-07 integration diagnostic PASS"
    echo "SubmitAdmission PASS logs=$LOG_DIR"
    exit 0
  fi

  if [[ "$SCENARIO" == "SA-E2E-04" ]]; then
    terminal_status=""
    for _ in $(seq 1 30); do
      evidence_json="$(curl --connect-timeout 0.2 --max-time 0.2 -fsS \
        "$CALLER_URL/evidence/$operation_id" 2>/dev/null || true)"
      read -r terminal_status transport_attempts commit_count signal_count retry_count pending_waiters reservations callbacks < <(
        python3 -c 'import json,sys; value=json.load(sys.stdin); ready=value.get("terminalCount")==1 and value.get("pendingWaiterCount")==0 and value.get("reservationCount")==0; print((value.get("terminalStatus") or "-") if ready else "-", value.get("transportAttemptCount",0), value.get("commitCount",0), value.get("sendReadySignalCount",0), value.get("retryAttemptCount",0), value.get("pendingWaiterCount",0), value.get("reservationCount",0), value.get("callbackCount",0))' \
        <<<"$evidence_json" 2>/dev/null || true)
      [[ "$terminal_status" != "-" ]] && break
      sleep 0.1
    done
    if [[ "$terminal_status" != "TimedOut" || "$transport_attempts" != "1" \
          || "$commit_count" != "0" || "$signal_count" != "0" || "$retry_count" != "0" \
          || "$pending_waiters" != "0" || "$reservations" != "0" || "$callbacks" != "0" ]]; then
      curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
      echo "SA-E2E-04 timeout evidence mismatch: $evidence_json" >&2
      exit 1
    fi

    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null
    expected_setup_deliveries=$((started_count - 1))
    completed_setup_deliveries=-1
    for _ in $(seq 1 30); do
      completed_setup_deliveries="$(curl --connect-timeout 0.2 --max-time 0.2 -fsS \
        "$TARGET_URL/evidence-handler-completed-count" 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["count"])' 2>/dev/null || true)"
      if [[ "$completed_setup_deliveries" =~ ^[0-9]+$ ]] \
          && (( completed_setup_deliveries >= expected_setup_deliveries )); then
        break
      fi
      sleep 0.1
    done
    if [[ ! "$completed_setup_deliveries" =~ ^[0-9]+$ ]] \
        || (( completed_setup_deliveries < expected_setup_deliveries )); then
      echo "SA-E2E-04 setup payloads did not drain after gate open: expected=$expected_setup_deliveries actual=$completed_setup_deliveries" >&2
      exit 1
    fi

    late_evidence="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/evidence/$operation_id")"
    python3 - "$late_evidence" <<'PY'
import json
import sys

value = json.loads(sys.argv[1])
if value["terminalStatus"] != "TimedOut" or value["terminalCount"] != 1:
    raise SystemExit(f"SA-E2E-04 terminal changed after gate open: {value}")
if value["transportAttemptCount"] != 1 or value["commitCount"] != 0:
    raise SystemExit(f"SA-E2E-04 admitted after timeout: {value}")
if value["sendReadySignalCount"] != 0 or value["retryAttemptCount"] != 0:
    raise SystemExit(f"SA-E2E-04 retained a callback after timeout: {value}")
if value["pendingWaiterCount"] != 0 or value["reservationCount"] != 0 or value["callbackCount"] != 0:
    raise SystemExit(f"SA-E2E-04 leaked admission resources: {value}")
PY

    recovery_operation_id="recovery-$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
    recovery_json="$(python3 - "$recovery_operation_id" <<'PY'
import json
import sys

print(json.dumps({"operationId": sys.argv[1], "sequence": 1, "payload": "recovery"}))
PY
)"
    recovery_result="$(curl --connect-timeout 0.2 --max-time 2 -fsS \
      -H 'Content-Type: application/json' -d "$recovery_json" \
      "$CALLER_URL/submit/node/$TARGET_RID")"
    python3 - "$recovery_operation_id" "$recovery_result" <<'PY'
import json
import sys

value = json.loads(sys.argv[2])
if value["operationId"] != sys.argv[1] or value["status"] != "Submitted":
    raise SystemExit(f"SA-E2E-04 recovery submit mismatch: {value}")
if value["publicInvocationCount"] != 1 or value["terminalCount"] != 1:
    raise SystemExit(f"SA-E2E-04 recovery terminal mismatch: {value}")
PY

    {
      printf 'receiver_gate=true\n'
      printf 'operation_id=%s\n' "$operation_id"
      printf 'terminal_status=TimedOut\n'
      printf 'terminal_count=1\n'
      printf 'transport_attempt_count=1\n'
      printf 'commit_count=0\n'
      printf 'late_transport_attempt_count=0\n'
      printf 'late_commit_count=0\n'
      printf 'pending_waiter_count=0\n'
      printf 'reservation_count=0\n'
      printf 'callback_count=0\n'
      printf 'recovery_operation_id=%s\n' "$recovery_operation_id"
      printf 'recovery_terminal_status=Submitted\n'
    } >"$LOG_DIR/timeout-late-admission.evidence.log"
    echo "SA-E2E-04 integration diagnostic PASS"
    echo "SubmitAdmission PASS logs=$LOG_DIR"
    exit 0
  fi

  capacity_operation_id=""
  if [[ "$SCENARIO" == "SA-E2E-03" ]]; then
    capacity_json="$(curl --connect-timeout 0.2 --max-time 3 -fsS -X POST \
      "$CALLER_URL/submit/fill/node/$TARGET_RID?count=1")"
    read -r capacity_operation_id capacity_pending capacity_started capacity_status < <(
      python3 -c 'import json,sys; value=json.load(sys.stdin); print(value["operationId"], str(value["pending"]).lower(), value["startedCount"], value.get("terminalStatus") or "-")' \
        <<<"$capacity_json")
    if [[ -z "$capacity_operation_id" || "$capacity_pending" != "false" \
          || "$capacity_started" != "1" || "$capacity_status" != "Backpressured" ]]; then
      curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
      echo "SA-E2E-03 bounded pending admission mismatch: $capacity_json" >&2
      exit 1
    fi
    capacity_evidence="$(curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$CALLER_URL/evidence/$capacity_operation_id")"
    read -r capacity_attempts capacity_commits capacity_signals capacity_retries < <(
      python3 -c 'import json,sys; value=json.load(sys.stdin); print(value["transportAttemptCount"], value["commitCount"], value["sendReadySignalCount"], value["retryAttemptCount"])' \
        <<<"$capacity_evidence")
    if [[ "$capacity_attempts" != "1" || "$capacity_commits" != "0" \
          || "$capacity_signals" != "0" || "$capacity_retries" != "0" ]]; then
      curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
      echo "SA-E2E-03 attempt evidence mismatch: $capacity_evidence" >&2
      exit 1
    fi
  fi
  gate_status_json="$(curl --connect-timeout 0.2 --max-time 1 -fsS "$GATE_URL/gate/status")"
  gate_bytes_after_close="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["callerBytesReadAfterClose"])' \
    <<<"$gate_status_json")"
  if [[ "$gate_bytes_after_close" != "0" ]]; then
    curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null || true
    echo "$SCENARIO ReceiverGate read beyond close marker: $gate_status_json" >&2
    exit 1
  fi
  curl --connect-timeout 0.2 --max-time 1 -fsS -X POST "$GATE_URL/gate/open" >/dev/null

  terminal_status=""
  for _ in $(seq 1 30); do
    evidence_json="$(curl --connect-timeout 0.2 --max-time 0.2 -fsS \
      "$CALLER_URL/evidence/$operation_id" 2>/dev/null || true)"
    read -r terminal_status transport_attempts commit_count signal_count retry_count pending_waiters reservations callbacks < <(
      python3 -c 'import json,sys; value=json.load(sys.stdin); ready=value.get("terminalCount")==1 and value.get("pendingWaiterCount")==0 and value.get("reservationCount")==0; print((value.get("terminalStatus") or "-") if ready else "-", value.get("transportAttemptCount",0), value.get("commitCount",0), value.get("sendReadySignalCount",0), value.get("retryAttemptCount",0), value.get("pendingWaiterCount",0), value.get("reservationCount",0), value.get("callbackCount",0))' \
      <<<"$evidence_json" 2>/dev/null || true)
    [[ "$terminal_status" != "-" ]] && break
    sleep 0.1
  done
  if [[ "$terminal_status" != "Submitted" || "$transport_attempts" != "2" \
        || "$commit_count" != "1" || "$signal_count" != "1" || "$retry_count" != "1" \
        || "$pending_waiters" != "0" || "$reservations" != "0" || "$callbacks" != "0" ]]; then
    echo "$SCENARIO pending submit evidence mismatch: $evidence_json" >&2
    exit 1
  fi
  {
    printf 'receiver_gate=true\n'
    printf 'router_hwm=1\n'
    printf 'payload_bytes=32768\n'
    printf 'gate_forward_buffer_bytes=%s\n' "$requested_socket_buffer"
    printf 'gate_closed_at_caller_bytes=%s\n' "$gate_closed_at"
    printf 'gate_caller_bytes_after_close=%s\n' "$gate_bytes_after_close"
    printf 'gate_caller_receive_buffer_bytes=%s\n' "$caller_rcvbuf"
    printf 'gate_caller_send_buffer_bytes=%s\n' "$caller_sndbuf"
    printf 'gate_target_receive_buffer_bytes=%s\n' "$target_rcvbuf"
    printf 'gate_target_send_buffer_bytes=%s\n' "$target_sndbuf"
    printf 'operation_id=%s\n' "$operation_id"
    printf 'started_count=%s\n' "$started_count"
    printf 'pending=true\n'
    printf 'terminal_status=%s\n' "$terminal_status"
    printf 'transport_attempt_count=%s\n' "$transport_attempts"
    printf 'commit_count=%s\n' "$commit_count"
    printf 'send_ready_signal_count=%s\n' "$signal_count"
    printf 'retry_attempt_count=%s\n' "$retry_count"
    printf 'pending_waiter_count=%s\n' "$pending_waiters"
    printf 'reservation_count=%s\n' "$reservations"
    printf 'callback_count=%s\n' "$callbacks"
    if [[ -n "$capacity_operation_id" ]]; then
      printf 'capacity_operation_id=%s\n' "$capacity_operation_id"
      printf 'capacity_terminal_status=%s\n' "$capacity_status"
      printf 'capacity_transport_attempt_count=%s\n' "$capacity_attempts"
      printf 'capacity_commit_count=%s\n' "$capacity_commits"
    fi
  } >"$LOG_DIR/send-ready-integration.evidence.log"
  echo "$SCENARIO integration diagnostic PASS"
  echo "SubmitAdmission PASS logs=$LOG_DIR"
  exit 0
fi

dotnet "$CLIENT_DLL" --config "$CLIENT_CONFIG" | tee "$LOG_DIR/client.stdout.log"
if [[ "$SCENARIO" == "all" ]]; then run_regression_checks; fi
echo "SubmitAdmission PASS logs=$LOG_DIR"
