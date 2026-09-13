#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../../../.." && pwd)"
# shellcheck source=../runner_args.sh
source "${ROOT_DIR}/../runner_args.sh"
bench_runner_args c "$@"

# §1.2: C 기준 bench에는 framework 행이 없다. 입력 이름은 §3.1과 같고 격자만 좁힌다.
case "${IMPLEMENTATION}" in
  all) bench_implementations=(grpc-c zlink-c) ;;
  zlink-framework-c) echo "C 기준 bench에는 framework 행이 없다" >&2; exit 2 ;;
esac

# README §7.2 formula 1은 `zlink-<lang> / zlink-c`다. 두 행이 같은 조건에서 잰 값이어야
# 그 비율이 binding 계층 비용이 된다. 언어 행은 모두 로컬 패키지의 Core를 로드하므로
# C 기준도 같은 바이너리를 써야 한다 — `core/build`의 개발 빌드와는 실측 6% 차이가
# 난다(#308: 600.2 대 636.6 KOPS, latency 13.1 대 8.1 ms).
ZLINK_LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}"
ZLINK_C_CORE_VERSION="${ZLINK_C_CORE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "${REPO_ROOT}/VERSION")}"
ZLINK_C_CORE_DEFAULT_PREFIX="${ZLINK_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_C_CORE_VERSION}"
if [[ ! -d "${ZLINK_C_CORE_DEFAULT_PREFIX}" ]]; then
  echo "C 기준 벤치가 쓸 Core 패키지가 없다: ${ZLINK_C_CORE_DEFAULT_PREFIX}" >&2
  echo "ZLINK_CORE_PACKAGE_PREFIX로 명시하거나 로컬 패키지를 먼저 만들어라." >&2
  exit 1
fi
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
WINDOW_SIZE="${WINDOW_SIZE:-100}"
MAX_OUTSTANDING="${MAX_OUTSTANDING:-4096}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-5000}"
ENABLE_ZMQ_SEND_SEND="${ENABLE_ZMQ_SEND_SEND:-0}"

# SKIP_BUILD=1 keeps the build out of a measurement window (plan 3.2: no build
# may overlap a measurement). Pre-build with the same targets, then measure.
if [[ "${SKIP_BUILD}" != "1" ]]; then
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DZLINK_C_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ZLINK_C_CORE_DEFAULT_PREFIX}}"
build_targets=(
  bench_c_with_grpc_zlink_server
  bench_c_with_grpc_zlink_client
  bench_c_with_grpc_grpc_server
  bench_c_with_grpc_grpc_client
)
if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
  build_targets+=(
    bench_c_with_grpc_zmq_server
    bench_c_with_grpc_zmq_client
  )
fi
cmake --build "${BUILD_DIR}" --target "${build_targets[@]}" -j"$(nproc)"
fi

mkdir -p "${OUTPUT}"
RUNNER_LOG="${OUTPUT}/runner.log"
: >"${RUNNER_LOG}"

export PAYLOAD_SIZES
export DURATION_SECONDS
export WARMUP_SECONDS
export WINDOW_SIZE
export MAX_OUTSTANDING
export DRAIN_TIMEOUT_MS
# 클라이언트와 zlink 서버는 같은 목록을 읽는다. 서버가 같은 패턴만 열어야 선택하지 않은
# 패턴의 endpoint가 남아 측정 중인 셀에 섞이지 않는다.
BENCH_SCENARIOS="$(IFS=,; echo "${bench_patterns[*]}")"
export ZLINK_BENCH_SCENARIOS="${BENCH_SCENARIOS}"
export GRPC_BENCH_SCENARIOS="${BENCH_SCENARIOS}"

zlink_server="${BUILD_DIR}/bench_c_with_grpc_zlink_server"
zlink_client="${BUILD_DIR}/bench_c_with_grpc_zlink_client"
grpc_server="${BUILD_DIR}/bench_c_with_grpc_grpc_server"
grpc_client="${BUILD_DIR}/bench_c_with_grpc_grpc_client"
zmq_server="${BUILD_DIR}/bench_c_with_grpc_zmq_server"
zmq_client="${BUILD_DIR}/bench_c_with_grpc_zmq_client"

# `setsid` forks when the caller is not already a process-group leader, so `$!`
# is a short-lived wrapper rather than the server. SERVER_PID is what the client
# samples server CPU/RSS from, and killing the wrapper's group does not reach the
# server, so each server records its own pid into a pidfile and we read that.
start_server() {
  local name="$1" binary="$2" pidfile="${OUTPUT}/$1.pid"
  rm -f "${pidfile}"
  setsid bash -c 'echo $$ >"$1"; exec "$2"' _ "${pidfile}" "${binary}" \
    >"${OUTPUT}/${name}.log" 2>&1 &
  for _ in $(seq 1 200); do
    if [[ -s "${pidfile}" ]]; then
      cat "${pidfile}"
      return 0
    fi
    sleep 0.05
  done
  echo "[bench] failed to start ${name}" >&2
  return 1
}

zlink_pid="$(start_server zlink-server "${zlink_server}")"
grpc_pid="$(start_server grpc-server "${grpc_server}")"
zmq_pid=""
if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
  zmq_pid="$(start_server zmq-server "${zmq_server}")"
fi

cleanup() {
  status=$?
  set +e
  kill -TERM -- "-${zlink_pid}" "-${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    kill -TERM -- "-${zmq_pid}" >/dev/null 2>&1 || true
  fi
  sleep 1
  kill -KILL -- "-${zlink_pid}" "-${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    kill -KILL -- "-${zmq_pid}" >/dev/null 2>&1 || true
  fi
  wait "${zlink_pid}" "${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    wait "${zmq_pid}" >/dev/null 2>&1 || true
  fi
  exit "${status}"
}
trap cleanup EXIT

sleep 1

# 셀 원본은 언어마다 같은 모양이어야 하는데(§3.1) C harness는 client 하나가 격자를 전부
# 돌고 표만 찍는다. 그래서 client 출력을 그대로 두고 여기서 셀 문서로 옮긴다.
run_client() {
  local implementation="$1" binary="$2" server_pid="$3"
  local client_log="${OUTPUT}/${implementation}-client.log"
  echo "[bench] client=${implementation}: start" >&2
  SERVER_PID="${server_pid}" "${binary}" | tee "${client_log}" >>"${RUNNER_LOG}"
  python3 "${ROOT_DIR}/../tools/c_cells_from_report.py" "${client_log}" "${OUTPUT}" \
    --metadata "{\"language\":\"c\",\"corePrefix\":\"${ZLINK_CORE_PACKAGE_PREFIX:-${ZLINK_C_CORE_DEFAULT_PREFIX}}\"}"
}

for implementation in "${bench_implementations[@]}"; do
  case "${implementation}" in
    grpc-c) run_client grpc-c "${grpc_client}" "${grpc_pid}" ;;
    zlink-c) run_client zlink-c "${zlink_client}" "${zlink_pid}" ;;
  esac
done
if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
  # 격자 밖 보조 측정이라 셀 문서는 만들지 않는다. 로그로만 남는다.
  SERVER_PID="${zmq_pid}" "${zmq_client}" | tee "${OUTPUT}/zmq-c-client.log" >>"${RUNNER_LOG}"
fi

bench_write_report "${OUTPUT}"
echo "[bench] results=${OUTPUT}" >&2
