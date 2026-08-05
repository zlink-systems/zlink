#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIGURATION="${CONFIGURATION:-Release}"
GRPC_URL="${GRPC_URL:-http://127.0.0.1:5071}"
GRPC_STATS_URL="${GRPC_STATS_URL:-http://127.0.0.1:5074}"
ZLINK_ENDPOINT="${ZLINK_ENDPOINT:-tcp://127.0.0.1:5072}"
ZLINK_STATS_URL="${ZLINK_STATS_URL:-http://127.0.0.1:5073}"
ZLINK_RAW_ENDPOINT="${ZLINK_RAW_ENDPOINT:-tcp://127.0.0.1:5075}"
ZLINK_RAW_COMMAND_ENDPOINT="${ZLINK_RAW_COMMAND_ENDPOINT:-tcp://127.0.0.1:5077}"
ZLINK_RAW_STATS_URL="${ZLINK_RAW_STATS_URL:-http://127.0.0.1:5076}"
RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTPUT="${OUTPUT:-${ROOT_DIR}/log/with_grpc_dotnet_${RUN_STAMP}}"
REPORT_FILE="${REPORT_FILE:-with_grpc_dotnet_${RUN_STAMP}.txt}"

dotnet build "${ROOT_DIR}/GrpcServer/WithGrpcBench.GrpcServer.csproj" -c "${CONFIGURATION}"
dotnet build "${ROOT_DIR}/ZLinkServer/WithGrpcBench.ZLinkServer.csproj" -c "${CONFIGURATION}"
dotnet build "${ROOT_DIR}/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj" -c "${CONFIGURATION}"
dotnet build "${ROOT_DIR}/Client/WithGrpcBench.Client.csproj" -c "${CONFIGURATION}"

grpc_log="${OUTPUT}/grpc-server.log"
zlink_log="${OUTPUT}/zlink-server.log"
zlink_raw_log="${OUTPUT}/zlink-raw-server.log"
mkdir -p "${OUTPUT}"

setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/GrpcServer/WithGrpcBench.GrpcServer.csproj" -- --url "${GRPC_URL}" --metrics-url "${GRPC_STATS_URL}" >"${grpc_log}" 2>&1 &
grpc_pid=$!

setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/ZLinkServer/WithGrpcBench.ZLinkServer.csproj" -- --endpoint "${ZLINK_ENDPOINT}" --metrics-url "${ZLINK_STATS_URL}" >"${zlink_log}" 2>&1 &
zlink_pid=$!

setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj" -- --endpoint "${ZLINK_RAW_ENDPOINT}" --command-endpoint "${ZLINK_RAW_COMMAND_ENDPOINT}" --metrics-url "${ZLINK_RAW_STATS_URL}" >"${zlink_raw_log}" 2>&1 &
zlink_raw_pid=$!

cleanup() {
  status=$?
  set +e
  for pid in "${grpc_pid}" "${zlink_pid}" "${zlink_raw_pid}"; do
    kill -TERM -- "-${pid}" >/dev/null 2>&1 || kill "${pid}" >/dev/null 2>&1 || true
  done
  sleep 1
  for pid in "${grpc_pid}" "${zlink_pid}" "${zlink_raw_pid}"; do
    kill -KILL -- "-${pid}" >/dev/null 2>&1 || kill -KILL "${pid}" >/dev/null 2>&1 || true
  done
  wait "${grpc_pid}" "${zlink_pid}" "${zlink_raw_pid}" >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

sleep 2

dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/Client/WithGrpcBench.Client.csproj" -- \
  --grpc-url "${GRPC_URL}" \
  --zlink-endpoint "${ZLINK_ENDPOINT}" \
  --zlink-raw-endpoint "${ZLINK_RAW_ENDPOINT}" \
  --zlink-raw-command-endpoint "${ZLINK_RAW_COMMAND_ENDPOINT}" \
  --grpc-stats-url "${GRPC_STATS_URL}" \
  --zlink-stats-url "${ZLINK_STATS_URL}" \
  --zlink-raw-stats-url "${ZLINK_RAW_STATS_URL}" \
  --payload-sizes "${PAYLOAD_SIZES:-1024,4096}" \
  --request-window "${REQUEST_WINDOW:-100}" \
  --send-concurrency "${SEND_CONCURRENCY:-8}" \
  --warmup "${WARMUP:-1000}" \
  --duration-seconds "${DURATION_SECONDS:-5}" \
  --command-settle-ms "${COMMAND_SETTLE_MS:-200}" \
  --configuration "${CONFIGURATION}" \
  --timeout-seconds "${TIMEOUT_SECONDS:-300}" \
  --output "${OUTPUT}" \
  --report-file "${REPORT_FILE}" \
  "$@"
