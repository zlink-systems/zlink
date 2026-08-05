#!/usr/bin/env bash
# C++ cross-language smoke (G6).
#
# Runs the C++ public package against the packages of the languages that already
# closed G7 (.NET first). Each stage is one producer/consumer direction: the C++
# host and the peer language's own test host talk over the real wire, so packet
# identity, JSON codec and frame layout are all exercised end to end.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${CPP_ROOT}/../../.." && pwd)"

BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-${CPP_ROOT}/build-redis-vcpkg}"
CPP_HOST="${BUILD_DIR}/zlink_cpp_cross_language_host"
DOTNET_TEST_HOST="${REPO_ROOT}/framework/languages/dotnet/testapps/Zlink.Framework.TestHost/Zlink.Framework.TestHost.csproj"
NODE_PEER_HOST="${SCRIPT_DIR}/node_peer_host.js"

RUN_DIR="$(mktemp -d)"
PIDS=()
RESULTS=()

cleanup() {
  local code=$?
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" >/dev/null 2>&1 || true
  done
  if [[ "${ZLINK_CPP_CROSS_KEEP_RUN_DIR:-}" == "1" ]]; then
    echo "runDir=${RUN_DIR}"
  else
    rm -rf "${RUN_DIR}"
  fi
  exit "${code}"
}
trap cleanup EXIT

if [[ ! -x "${CPP_HOST}" ]]; then
  echo "cross-language host is missing: ${CPP_HOST}" >&2
  echo "build it with: cmake --build ${BUILD_DIR} --target zlink_cpp_cross_language_host" >&2
  exit 1
fi

free_port() {
  python3 - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

start_cpp() {
  local name="$1"
  shift
  "${CPP_HOST}" "$@" >"${RUN_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

start_dotnet() {
  local name="$1"
  shift
  # --framework net8.0 matches the TestHost target the Node smoke uses; the
  # stop file is the host's own graceful-stop contract.
  dotnet run --project "${DOTNET_TEST_HOST}" --framework net8.0 -- \
    --ready-file "${RUN_DIR}/${name}.ready" \
    --stop-file "${RUN_DIR}/${name}.stop" \
    "$@" >"${RUN_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

start_node() {
  local name="$1"
  shift
  node "${NODE_PEER_HOST}" "$@" \
    --ready-file "${RUN_DIR}/${name}.ready" \
    >"${RUN_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

wait_for_ready() {
  local file="$1"
  local timeout_seconds="${2:-60}"
  local deadline=$((SECONDS + timeout_seconds))
  while ((SECONDS < deadline)); do
    if [[ -f "${file}" ]]; then
      return 0
    fi
    sleep 0.2
  done
  echo "timed out waiting for ready file ${file}" >&2
  return 1
}

wait_for_line() {
  local file="$1"
  local needle="$2"
  local timeout_seconds="${3:-30}"
  local deadline=$((SECONDS + timeout_seconds))
  while ((SECONDS < deadline)); do
    if [[ -f "${file}" ]] && grep -qF -- "${needle}" "${file}"; then
      return 0
    fi
    sleep 0.2
  done
  echo "timed out waiting for '${needle}' in ${file}" >&2
  [[ -f "${file}" ]] && tail -40 "${file}" >&2 || true
  return 1
}

stop_all() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" >/dev/null 2>&1 || true
  done
  PIDS=()
}

# --- messaging: C++ client -> .NET channel server -----------------------------
stage_cpp_client_dotnet_channel_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/dotnet-channel-server.events"
  start_dotnet dotnet-channel-server channel-server \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_ready "${RUN_DIR}/dotnet-channel-server.ready" 180
  start_cpp cpp-channel-client channel-client \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-channel-client.events" \
    --value cpp-to-dotnet
  wait_for_line "${RUN_DIR}/cpp-channel-client.events" "channel-client-reply|cpp-to-dotnet" 30
  wait_for_line "${events}" "channel-server-send|cpp-to-dotnet-send" 30
  stop_all
  RESULTS+=("messaging: C++ client -> .NET channel server (request/reply + one-way send)")
}

# --- messaging: .NET client -> C++ channel server -----------------------------
stage_dotnet_client_cpp_channel_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-channel-server.events"
  start_cpp cpp-channel-server channel-server \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${events}" \
    --ready-file "${RUN_DIR}/cpp-channel-server.ready"
  wait_for_ready "${RUN_DIR}/cpp-channel-server.ready" 60
  start_dotnet dotnet-channel-client channel-client \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/dotnet-channel-client.events" \
    --publish-value dotnet-to-cpp
  wait_for_line "${events}" "channel-server-request|dotnet-to-cpp" 90
  stop_all
  RESULTS+=("messaging: .NET client -> C++ channel server (request/reply)")
}

# --- fanout wire: C++ publisher -> .NET fanout subscriber ---------------------
stage_cpp_publisher_dotnet_subscriber() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/dotnet-subscriber.events"
  start_cpp cpp-publisher channel-publisher \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --topic profile.changed \
    --value cpp-publish \
    --event-file "${RUN_DIR}/cpp-publisher.events"
  start_dotnet dotnet-subscriber channel-subscriber \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_line "${events}" "profile.changed:cpp-publish" 90
  stop_all
  RESULTS+=("flow-wire: C++ fanout publisher -> .NET subscriber (envelope + topic)")
}

# --- fanout wire: .NET publisher -> C++ fanout subscriber ---------------------
stage_dotnet_publisher_cpp_subscriber() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-subscriber.events"
  start_dotnet dotnet-publisher channel-publisher \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --publish-topic profile.changed \
    --publish-value dotnet-publish
  start_cpp cpp-subscriber channel-subscriber \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_line "${events}" "profile.changed:dotnet-publish" 90
  stop_all
  RESULTS+=("flow-wire: .NET fanout publisher -> C++ subscriber (envelope + topic)")
}

# --- STREAM wire: C++ connector -> .NET raw stream server ---------------------
stage_cpp_connector_dotnet_stream_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/dotnet-stream-raw.events"
  start_dotnet dotnet-stream-raw stream-raw \
    --stream-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_ready "${RUN_DIR}/dotnet-stream-raw.ready" 180
  "${CPP_HOST}" stream-connector \
    --stream-endpoint "${endpoint}" \
    --value cpp-connector-to-dotnet \
    --event-file "${RUN_DIR}/cpp-connector.events" >"${RUN_DIR}/cpp-connector.log" 2>&1
  wait_for_line "${RUN_DIR}/cpp-connector.events" "connector-reply|pong" 10
  wait_for_line "${events}" "raw|cpp-connector-to-dotnet" 10
  stop_all
  RESULTS+=("messaging: C++ STREAM connector -> .NET raw stream server (frame + codec)")
}

# --- STREAM wire: .NET connector -> C++ stream server -------------------------
stage_dotnet_connector_cpp_stream_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-stream-server.events"
  start_cpp cpp-stream-server stream-server \
    --stream-endpoint "${endpoint}" \
    --event-file "${events}" \
    --ready-file "${RUN_DIR}/cpp-stream-server.ready"
  wait_for_ready "${RUN_DIR}/cpp-stream-server.ready" 60
  start_dotnet dotnet-stream-client stream-client \
    --stream-endpoint "${endpoint}" \
    --publish-value dotnet-connector-to-cpp \
    --event-file "${RUN_DIR}/dotnet-stream-client.events"
  wait_for_line "${events}" "raw|RawPing|" 90
  stop_all
  RESULTS+=("messaging: .NET STREAM connector -> C++ stream server (frame + codec)")
}

# --- messaging: C++ client -> Node channel server -----------------------------
stage_cpp_client_node_channel_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/node-channel-server.events"
  start_node node-channel-server channel-server \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_ready "${RUN_DIR}/node-channel-server.ready" 90
  start_cpp cpp-channel-client-node channel-client \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-channel-client-node.events" \
    --value cpp-to-node
  wait_for_line "${RUN_DIR}/cpp-channel-client-node.events" "channel-client-reply|cpp-to-node" 30
  wait_for_line "${events}" "channel-server-send|cpp-to-node-send" 30
  stop_all
  RESULTS+=("messaging: C++ client -> Node channel server (request/reply + one-way send)")
}

# --- messaging: Node client -> C++ channel server ------------------------------
stage_node_client_cpp_channel_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-channel-server-node.events"
  start_cpp cpp-channel-server-node channel-server \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${events}" \
    --ready-file "${RUN_DIR}/cpp-channel-server-node.ready"
  wait_for_ready "${RUN_DIR}/cpp-channel-server-node.ready" 60
  start_node node-channel-client channel-client \
    --channel-name profiles \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/node-channel-client.events" \
    --value node-to-cpp
  wait_for_line "${events}" "channel-server-request|node-to-cpp" 90
  wait_for_line "${events}" "channel-server-send|node-to-cpp-send" 30
  stop_all
  RESULTS+=("messaging: Node client -> C++ channel server (request/reply + one-way send)")
}

# --- fanout wire: C++ publisher -> Node subscriber -----------------------------
stage_cpp_publisher_node_subscriber() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/node-subscriber.events"
  start_cpp cpp-publisher-node channel-publisher \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --topic profile.changed \
    --value cpp-publish-node \
    --event-file "${RUN_DIR}/cpp-publisher-node.events"
  start_node node-subscriber channel-subscriber \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_line "${events}" "profile.changed:cpp-publish-node" 90
  stop_all
  RESULTS+=("flow-wire: C++ fanout publisher -> Node subscriber (envelope + topic)")
}

# --- fanout wire: Node publisher -> C++ subscriber -----------------------------
stage_node_publisher_cpp_subscriber() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-subscriber-node.events"
  start_node node-publisher channel-publisher \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --topic profile.changed \
    --value node-publish-cpp
  wait_for_ready "${RUN_DIR}/node-publisher.ready" 90
  start_cpp cpp-subscriber-node channel-subscriber \
    --channel-name profiles.events \
    --publisher-endpoint "${endpoint}" \
    --event-file "${events}"
  wait_for_line "${events}" "profile.changed:node-publish-cpp" 90
  stop_all
  RESULTS+=("flow-wire: Node fanout publisher -> C++ subscriber (envelope + topic)")
}

# --- STREAM wire: Browser TypeScript connector -> C++ stream server -----------
stage_node_connector_cpp_stream_server() {
  local port endpoint events
  port="$(free_port)"
  endpoint="ws://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-stream-server-node.events"
  start_cpp cpp-stream-server-node stream-server \
    --stream-endpoint "${endpoint}" \
    --event-file "${events}" \
    --ready-file "${RUN_DIR}/cpp-stream-server-node.ready"
  wait_for_ready "${RUN_DIR}/cpp-stream-server-node.ready" 60
  start_node browser-stream-connector browser-stream-connector \
    --stream-endpoint "${endpoint}" \
    --value node-connector-to-cpp \
    --event-file "${RUN_DIR}/node-stream-connector.events"
  wait_for_line "${events}" "raw|RawPing|" 90
  wait_for_line "${RUN_DIR}/node-stream-connector.events" "connector-reply|pong" 30
  stop_all
  RESULTS+=("codec: Browser TypeScript STREAM connector (LZ4) -> C++ stream server (frame + compression)")
}

# --- messageFollow wire: C++ raw owner <-> Node raw owner ---------------------
stage_cpp_node_message_follow() {
  local cpp_port node_port cpp_endpoint node_endpoint
  cpp_port="$(free_port)"
  node_port="$(free_port)"
  cpp_endpoint="tcp://127.0.0.1:${cpp_port}"
  node_endpoint="tcp://127.0.0.1:${node_port}"
  start_cpp cpp-message-follow message-follow \
    --node-rid cpp-message-follow \
    --peer-rid node-message-follow \
    --bind-endpoint "${cpp_endpoint}" \
    --peer-endpoint "${node_endpoint}" \
    --event-file "${RUN_DIR}/cpp-message-follow.events" \
    --ready-file "${RUN_DIR}/cpp-message-follow.ready"
  start_node node-message-follow message-follow \
    --node-rid node-message-follow \
    --peer-rid cpp-message-follow \
    --bind-endpoint "${node_endpoint}" \
    --peer-endpoint "${cpp_endpoint}" \
    --event-file "${RUN_DIR}/node-message-follow.events"
  wait_for_ready "${RUN_DIR}/cpp-message-follow.ready" 30
  wait_for_ready "${RUN_DIR}/node-message-follow.ready" 30
  wait_for_line "${RUN_DIR}/cpp-message-follow.events" \
    "message-follow-received|source-node=node-message-follow|target-node=cpp-message-follow|operation-low=202" 60
  wait_for_line "${RUN_DIR}/node-message-follow.events" \
    "message-follow-received|source-node=cpp-message-follow|target-node=node-message-follow|operation-low=101" 60
  stop_all
  RESULTS+=("message-follow: C++ and Node raw owners (command 50 + route fence)")
}

if [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" == "message-follow" ]]; then
  stage_cpp_node_message_follow
  echo "cross-language smoke stage=message-follow result=passed"
  exit 0
fi

stage_cpp_client_dotnet_channel_server
stage_dotnet_client_cpp_channel_server
stage_cpp_publisher_dotnet_subscriber
stage_dotnet_publisher_cpp_subscriber
stage_cpp_connector_dotnet_stream_server
stage_dotnet_connector_cpp_stream_server
stage_cpp_client_node_channel_server
stage_node_client_cpp_channel_server
stage_cpp_publisher_node_subscriber
stage_node_publisher_cpp_subscriber
stage_node_connector_cpp_stream_server
stage_cpp_node_message_follow

for result in "${RESULTS[@]}"; do
  echo "ok - ${result}"
done
echo "cross-language smoke result=passed"
