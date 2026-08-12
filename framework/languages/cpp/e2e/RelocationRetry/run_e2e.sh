#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$ROOT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$CPP_DIR/build}"
source "$ROOT_DIR/../redis-common.sh"
zlink_cpp_e2e_acquire_run_lock "${BASH_SOURCE[0]}" "$@"
zlink_cpp_e2e_install_cleanup_trap

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zlink-cpp-relocation-retry.XXXXXXXX")"
PACKAGE_ROOT="$WORK_DIR/package"
CONSUMER_BUILD="$WORK_DIR/build"
TRIGGER_FILE="$WORK_DIR/retry.trigger"
STOP_FILE="$WORK_DIR/stop.trigger"
mkdir -p "$LOG_DIR" "$PACKAGE_ROOT" "$CONSUMER_BUILD"

REDIS_CONTAINER=""
source_pid=""
target_pid=""

cleanup() {
  local code=$?
  for pid in "$source_pid" "$target_pid"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  wait "$source_pid" "$target_pid" >/dev/null 2>&1 || true
  if [[ -n "$REDIS_CONTAINER" ]]; then
    zlink_redis_remove_by_id "$REDIS_CONTAINER" || true
  fi
  if [[ -d "$WORK_DIR" && "$WORK_DIR" == /tmp/zlink-cpp-relocation-retry.* ]]; then
    find "$WORK_DIR" -xdev -mindepth 1 -delete
    rmdir "$WORK_DIR"
  fi
  if [[ "$code" -ne 0 ]]; then
    echo "RelocationRetry failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

wait_log() {
  local file="$1" pattern="$2" timeout_seconds="$3"
  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]] && grep -Fq "$pattern" "$file"; then
      return 0
    fi
    sleep 0.05
  done
  echo "Timed out waiting for '$pattern' in $file" >&2
  return 1
}

wait_process() {
  local pid="$1" timeout_seconds="$2"
  local deadline=$((SECONDS + timeout_seconds))
  while kill -0 "$pid" >/dev/null 2>&1; do
    if (( SECONDS >= deadline )); then
      echo "Timed out waiting for process $pid" >&2
      return 1
    fi
    sleep 0.05
  done
  wait "$pid"
}

cmake --build "$BUILD_DIR" --parallel 2 --target \
  zlink_framework \
  zlink_stream_connector \
  zlink_unreal_stream_connector \
  zlink_axmol_stream_connector \
  zlink_godot_stream_connector >/dev/null
for component in Framework StreamConnector FrameworkDependency; do
  cmake --install "$BUILD_DIR" --component "$component" --prefix "$PACKAGE_ROOT" >/dev/null
done

VCPKG_PREFIX="$CPP_DIR/build/linux-ninja-vcpkg-debug/vcpkg_installed/x64-linux"
if [[ ! -f "$VCPKG_PREFIX/share/protobuf/protobuf-config.cmake" ]]; then
  echo "C++ framework dependency prefix is missing: $VCPKG_PREFIX" >&2
  exit 1
fi
cmake -S "$ROOT_DIR" -B "$CONSUMER_BUILD" \
  -Dhiredis_DIR="$VCPKG_PREFIX/share/hiredis" \
  -Dlibuv_DIR="$VCPKG_PREFIX/share/libuv" \
  -Dredis++_DIR="$VCPKG_PREFIX/share/redis++" \
  -DCMAKE_PREFIX_PATH="$PACKAGE_ROOT;$VCPKG_PREFIX" >/dev/null
cmake --build "$CONSUMER_BUILD" --parallel 2 >/dev/null

sha256sum "$PACKAGE_ROOT/lib/libzlink_framework.a" >"$LOG_DIR/package.sha256.log"
find "$PACKAGE_ROOT/include" -type f \( -name '*.hpp' -o -name '*.h' \) \
  | wc -l | tr -d ' ' >"$LOG_DIR/package-header-count.log"

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-cpp-relocation-retry-$RUN_ID"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
KEY_PREFIX="zlink:e2e:relocation-retry:$RUN_ID"
ROLE_BIN="$CONSUMER_BUILD/zlink_cpp_e2e_relocation_retry"

"$ROLE_BIN" --role=source --redis="$REDIS_ENDPOINT" --key-prefix="$KEY_PREFIX" \
  --trigger-file="$TRIGGER_FILE" --stop-file="$STOP_FILE" \
  >"$LOG_DIR/source.stdout.log" 2>"$LOG_DIR/source.stderr.log" &
source_pid=$!
wait_log "$LOG_DIR/source.stdout.log" "first-relocation outcome=1 reason=1" 10

: >"$TRIGGER_FILE"
wait_log "$LOG_DIR/source.stdout.log" "second-relocation state=waiting-for-target" 5

"$ROLE_BIN" --role=target --redis="$REDIS_ENDPOINT" --key-prefix="$KEY_PREFIX" \
  --trigger-file="$TRIGGER_FILE" --stop-file="$STOP_FILE" \
  >"$LOG_DIR/target.stdout.log" 2>"$LOG_DIR/target.stderr.log" &
target_pid=$!
wait_log "$LOG_DIR/target.stdout.log" "role=target state=serving" 10

wait_log "$LOG_DIR/source.stdout.log" "second-relocation outcome=0 reason=0" 10
wait_process "$source_pid" 10
source_pid=""

: >"$STOP_FILE"
wait_process "$target_pid" 10
target_pid=""

grep -Fq "role=source result=passed" "$LOG_DIR/source.stdout.log"
grep -Fq "role=target result=passed" "$LOG_DIR/target.stdout.log"
echo "CPP-RELOC-001 CPP-RELOC-002 PASS package=$PACKAGE_ROOT logs=$LOG_DIR"
