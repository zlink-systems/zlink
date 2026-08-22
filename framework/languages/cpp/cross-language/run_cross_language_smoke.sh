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
DOTNET_TEST_HOST="${REPO_ROOT}/framework/languages/dotnet/cross-language/Zlink.Framework.TestHost/Zlink.Framework.TestHost.csproj"
NODE_PEER_HOST="${SCRIPT_DIR}/node_peer_host.js"
NODE_USER_SPOT_JOIN_HOST="${REPO_ROOT}/framework/languages/node/cross-language/user_spot_join_host.js"
JAVA_CROSS_LANGUAGE_ROOT="${REPO_ROOT}/framework/languages/java/cross-language"
JAVA_HOST="${JAVA_CROSS_LANGUAGE_ROOT}/Host/build/install/zlink-cross-language-host/bin/zlink-cross-language-host"

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

# The java-cross and relocation stages never spawn the C++ host (java-cross
# is Java<->Node and Java<->.NET only; relocation covers Node<->.NET and
# Java<->.NET), so none of them require the
# C++ binary to be built.
if [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "java-cross" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "relocation" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "relocation-java-dotnet" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "relocation-dotnet-java" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "relocation-node-dotnet" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-node-dotnet" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-node-dotnet-multiattempt" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-dotnet-node" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-java-dotnet" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-node-java" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-dotnet-java" ]] \
  && [[ "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" != "user-spot-join-java-node" ]] \
  && [[ ! -x "${CPP_HOST}" ]]; then
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

start_redis() {
  local name="$1"
  local port="$2"
  redis-server --port "${port}" --bind 127.0.0.1 --save '' --appendonly no \
    --daemonize no >"${RUN_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
  local deadline=$((SECONDS + 20))
  while ((SECONDS < deadline)); do
    if python3 - "${port}" <<'PY' >/dev/null 2>&1
import socket
import sys
sock = socket.socket()
sock.settimeout(0.5)
sock.connect(("127.0.0.1", int(sys.argv[1])))
sock.sendall(b"PING\r\n")
reply = sock.recv(64)
sock.close()
sys.exit(0 if reply.startswith(b"+PONG") else 1)
PY
    then
      redis-cli -p "${port}" FLUSHALL >/dev/null
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for redis on port ${port}" >&2
  return 1
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

start_node_user_spot_join() {
  local name="$1"
  shift
  node "${NODE_USER_SPOT_JOIN_HOST}" "$@" \
    --ready-file "${RUN_DIR}/${name}.ready" \
    >"${RUN_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

start_java() {
  local name="$1"
  shift
  "${JAVA_HOST}" "$@" \
    --ready-file "${RUN_DIR}/${name}.ready" \
    --stop-file "${RUN_DIR}/${name}.stop" \
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

wait_for_canonical_actor_join() {
  local file="$1"
  local deadline=$((SECONDS + 60))
  local canonical_signal="canonical actorJoin: wire_command=28 canonical=true packet=ZLinkFrameworkActorJoinRequest"
  while ((SECONDS < deadline)); do
    if [[ -f "${file}" ]] && grep -qF -- "${canonical_signal}" "${file}"; then
      return 0
    fi
    if [[ -f "${file}" ]] \
      && grep -qF -- "canonical actorJoin: wire_command=28 canonical=false" "${file}"; then
      echo "User-Spot Join stage: command 28 used the private fallback" >&2
      tail -40 "${file}" >&2
      return 1
    fi
    sleep 0.2
  done
  echo "timed out waiting for canonical actorJoin(28) in ${file}" >&2
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
  # The ready file confirms that the host has built its listener, while the
  # C++ client's selectable-target snapshot is populated asynchronously.
  sleep 16
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

# --- spot-route wire: reply tail dialect (CONVERGED) --------------------------
# The mesh service wire (spec 51, service-wire-v1.schema.json command 20
# "reply") used to be implemented in two incompatible dialects. It no longer
# is: `service-wire-v1.schema.json` declares reply(20).tail as
# `request-specific-tail`, a conditional-union WITHOUT `bodyLengthType`, so
# the selected case's fields are written INLINE right after failureCode and an
# empty tail yields exactly 21 bytes. (Contrast `actor-join-reply-tail`, same
# construct but with `bodyLengthType: u16`, which does carry an inner length
# prefix.) C++ and Java were already schema-correct; Node
# (service-wire-m6a-codec.ts) and .NET (ZLinkServiceWireCodec.cs) used to
# emit/require a spurious mandatory u16 tail length (>= 23 bytes) and were
# converged onto the inline form. All four languages now agree byte-for-byte;
# the golden vectors pinning the layout live in each language's unit tests.
#
# Separately (verified same-language, C++ client -> C++ host — independent of
# peer language and of the former tail-dialect divergence), a C++-side bug
# used to make every outbound direct-target requestToNode/sendToNode fail
# with not_found regardless of admission state: mesh_node_runtime.cpp's
# classify_node_direct_target (the Location-Store pre-check that runs before
# raw_mesh_node_owner_t ever sees the request) returned
# submit_result_t::not_found unconditionally whenever the target routing id
# was absent from its own Location Store page, instead of falling through
# (std::nullopt) to let the real transport-level admission check
# (raw_mesh_node_owner_t::request_with_header's `_topology.peer(...)` gate,
# which does correctly reflect the SAME owner's own admission trace once the
# handshake completes) decide. A route-only (object_role=none) MeshNode peer
# is never published to the Location Store, so it always missed and the
# direct-target path always short-circuited with not_found before the
# network call was ever attempted -- confirmed by instrumenting both
# classify_node_direct_target and request_with_header directly: classify hit
# its "exhausted, target absent" branch on every attempt (return not_found),
# so request_with_header's peer() gate never even ran until after the fix.
# Fixed in mesh_node_runtime.cpp: that branch now returns std::nullopt, same
# as the "no location_store_t registered" branch above it, so the real
# topology-backed peer() check is the actual authority. Repro (still valid
# for regression, now green instead of red): run this stage's two processes
# directly (spot-route-server / spot-route-client, any two C++ hosts) with
# ZLINK_CPP_MESH_TRACE=1 -- the client used to exhaust its retry deadline on
# not_found despite the admission trace already reporting "peer=present";
# now it completes fast.
stage_cpp_spot_route_client_dotnet_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-spotroute-client-dotnet.events"
  start_dotnet dotnet-spotroute-host route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/dotnet-spotroute-host.events"
  wait_for_ready "${RUN_DIR}/dotnet-spotroute-host.ready" 180
  start_cpp cpp-spotroute-client spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid dotnet-route \
    --event-file "${events}" \
    --value cpp-to-dotnet-spot
  wait_for_line "${events}" "spot-route-reply|cpp-to-dotnet-spot|dotnet" 90
  # The C++ decoder reports origin=unspecified for both failures here (not
  # "application" as the .NET/Node/Java CLIENT-side decoders report against
  # C++'s own marker-less reply in the reverse directions): record_failure's
  # fallback string fires because this C++-client decode path does not
  # attach a framework_exception_t to the result, regardless of peer
  # language -- verified identically same-language in
  # stage_cpp_spot_route_client_cpp_host, so this is a C++-client decode
  # property/divergence, not a .NET-specific marker gap. Asserted as
  # observed, not a claim about what it should be.
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=unspecified" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=unspecified" 30
  stop_all
  RESULTS+=("spot-route: C++ client -> .NET host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: .NET client -> C++ route host ---------------------------
# Runs the full common scenario for real now that the reply tail dialect is
# converged: (a) reply round trip, (b) framework not_found (C++'s route-mesh
# handler-missing reply carries no zlink.origin marker, so the .NET decoder
# classifies it "application" — same as the Java/Node decoders facing the same
# C++ reply), (c) application rejected without the marker.
stage_dotnet_spot_route_client_cpp_host() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/dotnet-spotroute-client.events"
  start_cpp cpp-spotroute-host spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-spotroute-host.events" \
    --ready-file "${RUN_DIR}/cpp-spotroute-host.ready"
  wait_for_ready "${RUN_DIR}/cpp-spotroute-host.ready" 60
  start_dotnet dotnet-spotroute-client spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --peer-rid cpp-spot-route \
    --event-file "${events}" \
    --publish-value dotnet-to-cpp-spot
  wait_for_line "${RUN_DIR}/cpp-spotroute-host.events" "spot-route-server|dotnet-to-cpp-spot|" 180
  wait_for_line "${events}" "spot-route-reply|dotnet-to-cpp-spot|cpp" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: .NET client -> C++ host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: C++ client -> Node route host ---------------------------
# The C++-side classify_node_direct_target bug (see the block comment above
# stage_cpp_spot_route_client_dotnet_host) is fixed and independently
# verified same-language (stage_cpp_spot_route_client_cpp_host) and against
# .NET and Java hosts (stage_cpp_spot_route_client_dotnet_host,
# stage_cpp_spot_route_client_java_host). This direction previously pinned a
# DIFFERENT and unrelated failure: node_peer_host.js's spot-route-server mode
# (`addRouteMesh(...).listen(...)` then `await new Promise(() => {})`) called
# writeReady() and then the Node process exited on its own with code 0,
# confirmed by running that mode standalone, which returned immediately
# despite the never-resolving await, unlike channel-server mode
# (`addClientServerChannel(...).enableServer(...)`), which correctly blocks
# for the process lifetime. RouteMesh's Node listen path was not holding an
# active libuv handle the way ClientServer's is -- the framework's native
# completions are not libuv handles, so nothing kept the event loop alive
# (same root cause as the keep-alive comment already in spotRouteClient()).
# Fixed harness-side in node_peer_host.js by adding the same
# `setInterval(() => {}, 1000)` keep-alive to spotRouteServer() that
# spotRouteClient() already used; this direction now runs the full common
# scenario for real: (a) reply round trip, (b) framework not_found, (c)
# application rejected. Node's route-mesh reply carries no zlink.origin
# marker for either failure kind; the C++ client's own decoder (unlike
# node_peer_host.js's errorOriginWireName, which reports a missing marker
# as "none") reports a missing marker as "unspecified" -- same as the
# same-language C++ client -> C++ host direction below, which faces an
# equally marker-less reply from a C++ server.
stage_cpp_spot_route_client_node_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-spotroute-client-node.events"
  start_node node-spotroute-host spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --node-rid node-spot-route \
    --event-file "${RUN_DIR}/node-spotroute-host.events"
  wait_for_ready "${RUN_DIR}/node-spotroute-host.ready" 90
  start_cpp cpp-spotroute-client-node spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid node-spot-route \
    --event-file "${events}" \
    --value cpp-to-node-spot
  wait_for_line "${RUN_DIR}/node-spotroute-host.events" "spot-route-server|cpp-to-node-spot" 90
  wait_for_line "${events}" "spot-route-reply|cpp-to-node-spot|node" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=unspecified" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=unspecified" 30
  stop_all
  RESULTS+=("spot-route: C++ client -> Node host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: Node client -> C++ route host ---------------------------
# Runs the full common scenario for real now that the reply tail dialect is
# converged: (a) reply round trip, (b) framework not_found, (c) application
# rejected. C++'s route-mesh reply carries no zlink.origin marker for either
# failure kind. Residual-convergence fix: the Node route surface's error
# completion path (channel-envelope.ts decodeChannelError) now classifies
# every error decoded from an actual remote reply as either "framework"
# (marker present) or "application" (marker absent) -- it never leaves
# `error.origin` unset for a reply that crossed the wire, matching .NET's
# ZLinkErrorOriginWire.RemoteReplyOrigin (see
# stage_dotnet_spot_route_client_cpp_host, which reports origin=application
# against this same marker-less C++ reply). Both failures here are therefore
# origin=application, not the previously observed origin=none.
stage_node_spot_route_client_cpp_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/node-spotroute-client.events"
  start_cpp cpp-spotroute-host-node spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-spotroute-host-node.events" \
    --ready-file "${RUN_DIR}/cpp-spotroute-host-node.ready"
  wait_for_ready "${RUN_DIR}/cpp-spotroute-host-node.ready" 60
  start_node node-spotroute-client spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid cpp-spot-route \
    --event-file "${events}" \
    --value node-to-cpp-spot
  wait_for_line "${RUN_DIR}/cpp-spotroute-host-node.events" "spot-route-server|node-to-cpp-spot|" 90
  wait_for_line "${events}" "spot-route-reply|node-to-cpp-spot|cpp" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: Node client -> C++ host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: Java client -> C++ route host ---------------------------
# All four languages now share the schema's inline 21-byte service-wire reply
# header (no u16 tail length) and Java's client-side
# node-target classification (ZLinkJavaRawSpotNode.classifyNodeSendTarget)
# tracks real peer admission state, so this direction runs the full common
# scenario for real: (a) reply round trip, (b) framework not_found (C++'s
# route-mesh handler-missing reply carries no zlink.origin marker, so the
# Java decoder classifies it "application" — same as the dotnet/Node decoders
# facing the same C++ reply), (c) application rejected without the marker.
stage_java_spot_route_client_cpp_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/java-spotroute-client.events"
  start_cpp cpp-spotroute-host-java spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-spotroute-host-java.events" \
    --ready-file "${RUN_DIR}/cpp-spotroute-host-java.ready" \
    --node-rid cpp-spot-route-java
  wait_for_ready "${RUN_DIR}/cpp-spotroute-host-java.ready" 60
  start_java java-spotroute-client spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --peer-rid cpp-spot-route-java \
    --node-rid java-spot-route-client \
    --event-file "${events}" \
    --value java-to-cpp-spot
  wait_for_line "${events}" "spot-route-reply|java-to-cpp-spot|cpp" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: Java client -> C++ host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: C++ client -> Java route host ----------------------------
# The C++-side classify_node_direct_target bug (see the block comment above
# stage_cpp_spot_route_client_dotnet_host) is fixed and independently
# verified same-language (stage_cpp_spot_route_client_cpp_host) and against
# .NET (stage_cpp_spot_route_client_dotnet_host). This direction previously
# pinned a DIFFERENT and unrelated failure: with ZLINK_CPP_MESH_TRACE=1, the
# C++ client's raw ZMTP monitor showed HANDSHAKE_SUCCEEDED (event=4096)
# immediately followed by DISCONNECTED (event=512) in a tight reconnect loop
# against the Java host. This is the ONLY stage in this harness that
# exercises Java as a RouteMesh *server* (`.listen(...)`, spot-route-server
# mode) -- stage_java_spot_route_client_cpp_host only ever runs Java as
# spot-route-client (connect-only) -- so Java's accept side had zero passing
# coverage anywhere to contrast against.
#
# Root-caused with ZLINK_JAVA_STREAM_TRACE=1 on a standalone repro (Java
# host + C++ client, no harness): every single admission attempt logged
# "admission-stage stage=entered" immediately followed by
# "disconnect-transport-pair" -- ZLinkJavaRawMeshNode.dispatchAdmission's
# unconfigured-inbound fallback (no PeerIntent registered, i.e. a listen-only
# server accepting any client) computed expectedSecurityIdentity as
# inbound.source().toString() (the raw ZMTP transport identity), instead of
# null ("no constraint") like the expectedEndpoint and
# expectedLifecycleGeneration fallbacks two lines above it. That was compared
# against the incoming descriptor's securityIdentity field, which is not a
# routing id but the plaintext-transport placeholder "default" (confirmed by
# tracing descriptor.securityIdentity()="default" from the C++ client, and by
# .NET's ZLinkManagedMeshNode using the equivalent
# ZLinkServiceSecurityIdentity.Plaintext="default" constant for its own
# unconfigured-inbound fallback). The two values only ever coincided for
# Java-to-Java peers (both ends set their ZMTP identity to their own routing
# id, and Java's own descriptor encodes securityIdentity as its routing id
# too), which is why this was invisible to every existing Java-only test.
# Fixed in ZLinkJavaRawMeshNode.dispatchAdmission (framework/languages/java)
# by falling back to null there too.
#
# Fixing admission surfaced a second, independent Java bug at the request
# layer: ZLinkMeshApplicationDispatcher.dispatchRequest's "handler is not
# registered" rejection used the reject(record, message, claim) overload,
# whose reply defaults to ZLinkFrameworkErrorKind.INTERNAL_FAILURE, instead of
# NOT_FOUND -- unlike the one-way case immediately above it in the same file
# (submitLocalNodeSend), which already uses ZLinkOneWayCalls.TARGET_NOT_FOUND,
# and unlike every other cross-language host's route-mesh reply for a missing
# handler. Fixed by adding a reject/replyError overload that takes an
# explicit ZLinkFrameworkErrorKind and passing NOT_FOUND at that call site.
#
# With both fixes this direction now runs the full common scenario for real:
# (a) reply round trip, (b) framework not_found, (c) application rejected.
# Java's route-mesh reply carries no zlink.origin marker for either failure
# kind, so origin reads "unspecified" here -- same as the same-language C++
# client -> C++ host direction below, which faces an equally marker-less
# reply from a C++ server.
stage_cpp_spot_route_client_java_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-spotroute-client-java.events"
  start_java java-spotroute-host spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/java-spotroute-host.events" \
    --node-rid java-spot-route-host
  wait_for_ready "${RUN_DIR}/java-spotroute-host.ready" 90
  start_cpp cpp-spotroute-client-java spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid java-spot-route-host \
    --event-file "${events}" \
    --value cpp-to-java-spot
  wait_for_line "${RUN_DIR}/java-spotroute-host.events" "spot-route-server|cpp-to-java-spot" 90
  wait_for_line "${events}" "spot-route-reply|cpp-to-java-spot|java" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=unspecified" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=unspecified" 30
  stop_all
  RESULTS+=("spot-route: C++ client -> Java host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: C++ client -> C++ route host (same-language regression) -
# The original repro for the classify_node_direct_target bug (see the block
# comment above stage_cpp_spot_route_client_dotnet_host) used two C++
# processes -- it is peer-language-agnostic, so this same-language direction
# pins the fix directly with no cross-language wire dialect in play at all.
# The C++ decoder reports origin=unspecified here (not "application" as in
# the cross-language directions): record_failure's fallback string fires
# because this same-process failure path does not attach a
# framework_exception_t to the result, unlike the cross-language decode
# paths -- asserted as observed, not a claim about what it should be.
stage_cpp_spot_route_client_cpp_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/cpp-spotroute-client-cpp.events"
  start_cpp cpp-spotroute-host-cpp spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/cpp-spotroute-host-cpp.events" \
    --ready-file "${RUN_DIR}/cpp-spotroute-host-cpp.ready"
  wait_for_ready "${RUN_DIR}/cpp-spotroute-host-cpp.ready" 60
  start_cpp cpp-spotroute-client-cpp spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid cpp-spot-route \
    --event-file "${events}" \
    --value cpp-to-cpp-spot
  wait_for_line "${events}" "spot-route-reply|cpp-to-cpp-spot|cpp" 30
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=unspecified" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=unspecified" 30
  stop_all
  RESULTS+=("spot-route: C++ client -> C++ host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: Java client -> Node route host --------------------------
# Same common (a)/(b)/(c) scenario as stage_cpp_spot_route_client_node_host,
# with a Java client instead of a C++ one. Node's route-mesh reply carries no
# zlink.origin marker for either failure kind (see the comment above
# stage_cpp_spot_route_client_node_host); Java's client-side decoder
# (Program.recordFailure) reports a framework exception with no
# "zlink.origin"="framework" metadata entry as origin=application (not
# "unspecified" -- that fallback only fires when no ZLinkFrameworkException was
# attached at all, which does not happen for a decoded remote reply). Asserted
# as observed.
stage_java_spot_route_client_node_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/java-spotroute-client-node.events"
  start_node node-spotroute-host-java spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --node-rid node-spot-route-java \
    --event-file "${RUN_DIR}/node-spotroute-host-java.events"
  wait_for_ready "${RUN_DIR}/node-spotroute-host-java.ready" 90
  start_java java-spotroute-client-node spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --peer-rid node-spot-route-java \
    --node-rid java-spot-route-client-node \
    --event-file "${events}" \
    --value java-to-node-spot
  wait_for_line "${events}" "spot-route-reply|java-to-node-spot|node" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: Java client -> Node host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: Node client -> Java route host ---------------------------
# Same common (a)/(b)/(c) scenario as stage_cpp_spot_route_client_java_host,
# with a Node client instead of a C++ one. Java's route-mesh reply carries no
# zlink.origin marker for either failure kind (see the comment above
# stage_cpp_spot_route_client_java_host); the Node route surface now
# classifies every decoded remote-reply error as origin=application when
# unmarked (see stage_node_spot_route_client_cpp_host) -- same result as
# that stage facing the equally marker-less C++ host.
stage_node_spot_route_client_java_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/node-spotroute-client-java.events"
  start_java java-spotroute-host-node spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --node-rid java-spot-route-host-node \
    --event-file "${RUN_DIR}/java-spotroute-host-node.events"
  wait_for_ready "${RUN_DIR}/java-spotroute-host-node.ready" 90
  start_node node-spotroute-client-java spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --bind-endpoint "tcp://127.0.0.1:${bind_port}" \
    --peer-rid java-spot-route-host-node \
    --event-file "${events}" \
    --value node-to-java-spot
  wait_for_line "${RUN_DIR}/java-spotroute-host-node.events" "spot-route-server|node-to-java-spot" 90
  wait_for_line "${events}" "spot-route-reply|node-to-java-spot|java" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: Node client -> Java host (reply + framework not_found + application rejected)")
}

# --- spot-route wire: Java client -> .NET route host ---------------------------
# Same common (a)/(b)/(c) scenario as stage_cpp_spot_route_client_dotnet_host,
# with a Java client instead of a C++ one, against .NET's "route-server" mode
# (fixed routing id "dotnet-route", ConfigureRouteServer in
# TestHostScenarioConfigurator.cs -- the same mode the C++/.NET stage above
# uses). Unlike a marker-less host, .NET's own missing-handler reply DOES
# attach the zlink.origin=framework metadata entry
# (ZLinkChannelRequestDispatchPipeline.cs sets Origin = ZLinkErrorOrigin.Framework
# when no request handler is registered for the incoming packet), so Java's
# decoder reports origin=framework for (b). The application-thrown rejected
# exception in (c) carries no such metadata, so Java reports origin=application
# there, same as every other marker-less direction. Asserted as observed.
stage_java_spot_route_client_dotnet_host() {
  local port bind_port endpoint events
  port="$(free_port)"
  bind_port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/java-spotroute-client-dotnet.events"
  start_dotnet dotnet-spotroute-host-java route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --event-file "${RUN_DIR}/dotnet-spotroute-host-java.events"
  wait_for_ready "${RUN_DIR}/dotnet-spotroute-host-java.ready" 180
  start_java java-spotroute-client-dotnet spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --peer-rid dotnet-route \
    --node-rid java-spot-route-client-dotnet \
    --event-file "${events}" \
    --value java-to-dotnet-spot
  wait_for_line "${events}" "spot-route-reply|java-to-dotnet-spot|dotnet" 90
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=framework" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: Java client -> .NET host (reply + framework not_found[marker] + application rejected)")
}

# --- spot-route wire: .NET client -> Java route host ---------------------------
# Same common (a)/(b)/(c) scenario as stage_dotnet_spot_route_client_cpp_host,
# against a Java "spot-route-server" host instead of a C++ one. Java's
# route-mesh reply carries no zlink.origin marker for either failure kind (see
# the comment above stage_cpp_spot_route_client_java_host); .NET's client-side
# decode (ZLinkErrorOriginWire.RemoteReplyOrigin) always classifies a decoded
# remote-reply error as either Framework (marker present) or Application
# (marker absent) -- it never leaves Origin unset for a reply that actually
# crossed the wire -- so both failures read origin=application here, same as
# stage_dotnet_spot_route_client_cpp_host facing the equally marker-less C++
# host. Asserted as observed.
stage_dotnet_spot_route_client_java_host() {
  local port endpoint events
  port="$(free_port)"
  endpoint="tcp://127.0.0.1:${port}"
  events="${RUN_DIR}/dotnet-spotroute-client-java.events"
  start_java java-spotroute-host-dotnet spot-route-server \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --node-rid java-spot-route-host-dotnet \
    --event-file "${RUN_DIR}/java-spotroute-host-dotnet.events"
  wait_for_ready "${RUN_DIR}/java-spotroute-host-dotnet.ready" 90
  start_dotnet dotnet-spotroute-client-java spot-route-client \
    --channel-name cross.spotroute \
    --server-endpoint "${endpoint}" \
    --peer-rid java-spot-route-host-dotnet \
    --event-file "${events}" \
    --publish-value dotnet-to-java-spot
  wait_for_line "${RUN_DIR}/java-spotroute-host-dotnet.events" "spot-route-server|dotnet-to-java-spot" 90
  wait_for_line "${events}" "spot-route-reply|dotnet-to-java-spot|java" 60
  wait_for_line "${events}" "spot-route-missing|kind=not_found|origin=application" 30
  wait_for_line "${events}" "spot-route-app-error|kind=rejected|origin=application" 30
  stop_all
  RESULTS+=("spot-route: .NET client -> Java host (reply + framework not_found + application rejected)")
}

# --- entry-spot relocation: Node source -> .NET target ------------------------
# Spec 28-relocation-flow.ko.md:589-591: "source와 target이 서로 다른 언어
# runtime인 relocation이 chunk 전송, checksum 검증과 owner 전환을 같은 결과로
# 통과한다." Uses JoinEntrySpot (spec 15-spot-actor.ko.md:489, 938) -- the
# only join shape usable cross-language today, since normal User Spot join's
# four dialect-incompatible actorJoin admission replies make that impossible.
# Two nodes share a locally-provisioned Redis-backed Location/Relocation
# Store; both register the same entry spot + actor factory; the source
# creates a >32 KiB actor (forcing multi-chunk relocationState(52) transfer)
# and triggers a whole-node relocate() drain; with exactly one other eligible
# peer in the mesh the destination is deterministic (asserted via
# SetPlacementWeight + an owner-before event, not merely assumed). A
# post-relocation probe request confirms the actor answers on the target
# node (owner transition), completing a live message round trip.
stage_node_source_dotnet_target_relocation() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/node-relocation-source.events"
  target_events="${RUN_DIR}/dotnet-relocation-target.events"
  start_redis relocation-redis "${redis_port}"
  # .NET Object-role MeshNodes (entry spot / actor factory registered)
  # cannot use a fixed routing id -- ZLinkFrameworkRegistrationValidator
  # rejects SetRoutingId combined with an Object role, so the target's actual
  # id is framework-assigned via the shared Location Store instead of the
  # --node-rid convention every other stage uses. --peer-rid here names the
  # SOURCE's (fixed, Node-side) routing id so the target can tell a
  # pre-relocation reply (still answered by the source) apart from a real
  # post-relocation one (answered by itself), without needing to predict its
  # own assigned id.
  start_dotnet dotnet-relocation-target entry-spot-target \
    --mesh-name cross.relocation \
    --peer-rid node-relocation-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/dotnet-relocation-target.ready" 180
  start_node node-relocation-source entry-spot-relocate \
    --role source \
    --mesh-name cross.relocation \
    --node-rid node-relocation-source \
    --peer-rid dotnet-relocation-target \
    --bind-endpoint "${source_endpoint}" \
    --peer-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --payload-bytes 100000 \
    --event-file "${source_events}"
  wait_for_line "${source_events}" "entry-spot-owner-before|node=node-relocation-source" 60
  wait_for_line "${source_events}" "relocate-result|outcome=Relocated" 60
  wait_for_line "${target_events}" "entry-spot-probe|nodeRid=" 90
  if grep -qF "entry-spot-probe|nodeRid=node-relocation-source" "${target_events}"; then
    echo "relocation stage: target probe still answered by the source -- no owner transition" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("relocation: Node source -> .NET target (JoinEntrySpot create, multi-chunk relocate, owner transition + liveness probe)")
}

# --- User-Spot JoinSpot: Node source -> .NET target, canonical actorJoin(28) --
stage_node_source_dotnet_target_user_spot_join() {
  local source_mode="${1:-user-spot-join-source}"
  local multiattempt=false
  if [[ "${source_mode}" == "user-spot-join-source-multiattempt" ]]; then
    multiattempt=true
  fi
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events start_file force_private_args
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/node-user-spot-join-source.events"
  target_events="${RUN_DIR}/dotnet-user-spot-target.events"
  start_file="${RUN_DIR}/user-spot-join.start"
  force_private_args=()
  if [[ "${ZLINK_NODE_CROSS_FORCE_PRIVATE_ACTOR_JOIN:-0}" == "1" ]]; then
    force_private_args+=(--force-private-join 1)
  fi

  start_redis user-spot-join-redis "${redis_port}"
  start_dotnet dotnet-user-spot-target user-spot-target \
    --mesh-name cross.user-spot-join \
    --peer-rid node-user-spot-join-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-target.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_node_user_spot_join node-user-spot-join-source "${source_mode}" \
    --mesh-name cross.user-spot-join \
    --node-rid node-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}" \
    --start-file "${start_file}" \
    "${force_private_args[@]}"

  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 60
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 60
  touch "${start_file}"

  # This line is emitted only after the generated command-28 decoder has read
  # the actual frames handed to the Node service transport and found the
  # canonical application packet. The private JSON fallback uses a different
  # packet name, so lifecycle success alone cannot satisfy this assertion.
  wait_for_canonical_actor_join "${source_events}.flow"
  if [[ "${multiattempt}" == true ]]; then
    wait_for_line "${source_events}" "canonical-multiattempt-sent|attempt=first" 90
    wait_for_line "${source_events}" "canonical-multiattempt-sent|attempt=second" 90
    wait_for_line "${source_events}" "canonical-multiattempt-terminal|attempt=first" 90
    wait_for_line "${source_events}" "canonical-multiattempt-terminal|attempt=second" 90
    # This experiment records the terminal fate of both real command-28
    # ingress attempts. It intentionally does not prescribe newest-wins,
    # first-wins, eager materialization, or a particular failure code.
    grep -F "canonical-multiattempt-" "${source_events}"
    stop_all
    RESULTS+=("User-Spot Join multi-attempt observation: Node source -> .NET target (two canonical actorJoin(28) ingress attempts)")
    return
  fi
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("User-Spot Join: Node source -> .NET target (canonical actorJoin(28), admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: .NET source -> Node target, canonical actorJoin(28) --
stage_dotnet_source_node_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/dotnet-user-spot-join-source.events"
  target_events="${RUN_DIR}/node-user-spot-target.events"

  start_redis user-spot-join-redis-dotnet-node "${redis_port}"
  start_node_user_spot_join node-user-spot-target user-spot-target \
    --mesh-name cross.user-spot-join \
    --node-rid node-user-spot-join-target \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/node-user-spot-target.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_dotnet dotnet-user-spot-join-source user-spot-source \
    --mesh-name cross.user-spot-join \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-join-source.ready" 180
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 60
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_canonical_actor_join "${target_events}.flow"
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  stop_all
  RESULTS+=("User-Spot Join: .NET source -> Node target (canonical actorJoin(28), admission, joined lifecycle)")
}

# --- User-Spot JoinSpot: Java source -> .NET target, canonical actorJoin(28) --
# Same scenario as stage_node_source_dotnet_target_user_spot_join with the Java
# cross-language host in the source role. NOTE: the canonical-28 wire assertion
# (wait_for_canonical_actor_join) is deliberately absent here -- that line is
# produced only by the Node host's own decode probes, and the .NET
# TestHostMessageFlowListener emits generic flow lines only. This cell asserts
# the lifecycle markers; Java-as-sender canonical-28 wire evidence is not
# available in this matrix cell.
stage_java_source_dotnet_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events start_file
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/java-user-spot-join-source.events"
  target_events="${RUN_DIR}/dotnet-user-spot-target-java.events"
  start_file="${RUN_DIR}/user-spot-join-java-dotnet.start"

  start_redis user-spot-join-redis-java-dotnet "${redis_port}"
  start_dotnet dotnet-user-spot-target-java user-spot-target \
    --mesh-name cross.user-spot-join \
    --peer-rid java-user-spot-join-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-target-java.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_java java-user-spot-join-source user-spot-source \
    --mesh-name cross.user-spot-join \
    --node-rid java-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}" \
    --start-file "${start_file}"

  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"

  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("User-Spot Join: Java source -> .NET target (JoinSpot admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: Node source -> Java target, canonical actorJoin(28) --
# Reuses the Node source mode verbatim; the Java host owns the fixed target
# User Spot (and, unlike the .NET target, also registers an Entry Spot).
stage_node_source_java_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events start_file
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/node-user-spot-join-source-java.events"
  target_events="${RUN_DIR}/java-user-spot-target.events"
  start_file="${RUN_DIR}/user-spot-join-node-java.start"

  start_redis user-spot-join-redis-node-java "${redis_port}"
  start_java java-user-spot-target user-spot-target \
    --mesh-name cross.user-spot-join \
    --node-rid java-user-spot-join-target \
    --peer-rid node-user-spot-join-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/java-user-spot-target.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_node_user_spot_join node-user-spot-join-source-java user-spot-join-source \
    --mesh-name cross.user-spot-join \
    --node-rid node-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}" \
    --start-file "${start_file}"

  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"

  wait_for_canonical_actor_join "${source_events}.flow"
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("User-Spot Join: Node source -> Java target (canonical actorJoin(28), admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: Node source -> C++ target, canonical actorJoin(28) ---
# Reuses the Node source mode verbatim (same mode the node->dotnet and
# node->java cells drive); the C++ cross-language host owns the fixed target
# User Spot. Like the .NET and Node targets -- and unlike the Java one -- the
# C++ target registers a Spot factory and the Actor factory but NO Entry Spot,
# so it can never be an Entry-Spot placement candidate for the source's Actor;
# it additionally drops its placement weight to zero once the fixed Spot
# exists. The canonical-28 wire assertion is kept here because the line is
# produced by the Node source's own decode probe.
stage_node_source_cpp_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events start_file
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/node-user-spot-join-source-cpp.events"
  target_events="${RUN_DIR}/cpp-user-spot-target.events"
  start_file="${RUN_DIR}/user-spot-join-node-cpp.start"

  start_redis user-spot-join-redis-node-cpp "${redis_port}"
  start_cpp cpp-user-spot-target user-spot-target \
    --mesh-name cross.user-spot-join \
    --node-rid cpp-user-spot-join-target \
    --peer-rid node-user-spot-join-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}" \
    --ready-file "${RUN_DIR}/cpp-user-spot-target.ready"
  wait_for_ready "${RUN_DIR}/cpp-user-spot-target.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_node_user_spot_join node-user-spot-join-source-cpp user-spot-join-source \
    --mesh-name cross.user-spot-join \
    --node-rid node-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}" \
    --start-file "${start_file}"

  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"

  wait_for_canonical_actor_join "${source_events}.flow"
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("User-Spot Join: Node source -> C++ target (canonical actorJoin(28), admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: C++ source -> .NET target, canonical actorJoin(28) ---
# Same scenario as stage_node_source_dotnet_target_user_spot_join with the C++
# cross-language host in the source role. NOTE: the canonical-28 wire assertion
# (wait_for_canonical_actor_join) is deliberately absent here -- that line is
# produced only by the Node host's own decode probes, and neither the C++ host
# nor the .NET TestHostMessageFlowListener emits it. This cell asserts the
# lifecycle markers; C++-as-sender canonical-28 wire evidence is not available
# in this matrix cell (same limitation as the Java -> .NET cell).
stage_cpp_source_dotnet_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events start_file
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/cpp-user-spot-join-source.events"
  target_events="${RUN_DIR}/dotnet-user-spot-target-cpp.events"
  start_file="${RUN_DIR}/user-spot-join-cpp-dotnet.start"

  start_redis user-spot-join-redis-cpp-dotnet "${redis_port}"
  start_dotnet dotnet-user-spot-target-cpp user-spot-target \
    --mesh-name cross.user-spot-join \
    --peer-rid cpp-user-spot-join-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-target-cpp.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30

  start_cpp cpp-user-spot-join-source user-spot-source \
    --mesh-name cross.user-spot-join \
    --node-rid cpp-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor \
    --event-file "${source_events}" \
    --start-file "${start_file}" \
    --ready-file "${RUN_DIR}/cpp-user-spot-join-source.ready"

  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"

  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("User-Spot Join: C++ source -> .NET target (JoinSpot admission, joined lifecycle, target-owner probe)")
}

wait_for_user_spot_target_outcome() {
  local target_events="$1"
  wait_for_line "${target_events}" "user-spot-admission|accepted=true" 60
  wait_for_line "${target_events}" "user-spot-joined|actor=cross-lang-user-spot-actor" 90
  wait_for_line "${target_events}" "user-spot-probe|nodeRid=" 90
  if grep -qF "user-spot-probe-timeout" "${target_events}"; then
    echo "User-Spot Join stage: target-side Actor probe did not reach the target RID" >&2
    exit 1
  fi
}

# --- User-Spot JoinSpot: .NET source -> Java target --------------------------
stage_dotnet_source_java_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/dotnet-user-spot-join-source-java.events"
  target_events="${RUN_DIR}/java-user-spot-target-dotnet.events"

  start_redis user-spot-join-redis-dotnet-java "${redis_port}"
  start_java java-user-spot-target-dotnet user-spot-target \
    --mesh-name cross.user-spot-join --node-rid java-user-spot-join-target \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot --actor-id cross-lang-user-spot-actor --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/java-user-spot-target-dotnet.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_dotnet dotnet-user-spot-join-source-java user-spot-source \
    --mesh-name cross.user-spot-join --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-join-source-java.ready" 180
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 60
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: .NET source -> Java target (admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: Java source -> Node target --------------------------
stage_java_source_node_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events start_file
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/java-user-spot-join-source-node.events"
  target_events="${RUN_DIR}/node-user-spot-target-java.events"
  start_file="${RUN_DIR}/user-spot-join-java-node.start"

  start_redis user-spot-join-redis-java-node "${redis_port}"
  start_node_user_spot_join node-user-spot-target-java user-spot-target \
    --mesh-name cross.user-spot-join --node-rid node-user-spot-join-target \
    --bind-endpoint "${target_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/node-user-spot-target-java.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_java java-user-spot-join-source-node user-spot-source \
    --mesh-name cross.user-spot-join --node-rid java-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}" --start-file "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: Java source -> Node target (admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: .NET source -> C++ target ---------------------------
stage_dotnet_source_cpp_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/dotnet-user-spot-join-source-cpp.events"
  target_events="${RUN_DIR}/cpp-user-spot-target-dotnet.events"

  start_redis user-spot-join-redis-dotnet-cpp "${redis_port}"
  start_cpp cpp-user-spot-target-dotnet user-spot-target \
    --mesh-name cross.user-spot-join --node-rid cpp-user-spot-join-target \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}" --ready-file "${RUN_DIR}/cpp-user-spot-target-dotnet.ready"
  wait_for_ready "${RUN_DIR}/cpp-user-spot-target-dotnet.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_dotnet dotnet-user-spot-join-source-cpp user-spot-source \
    --mesh-name cross.user-spot-join --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}"
  wait_for_ready "${RUN_DIR}/dotnet-user-spot-join-source-cpp.ready" 180
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 60
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: .NET source -> C++ target (admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: C++ source -> Node target ---------------------------
stage_cpp_source_node_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events start_file
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/cpp-user-spot-join-source-node.events"
  target_events="${RUN_DIR}/node-user-spot-target-cpp.events"
  start_file="${RUN_DIR}/user-spot-join-cpp-node.start"

  start_redis user-spot-join-redis-cpp-node "${redis_port}"
  start_node_user_spot_join node-user-spot-target-cpp user-spot-target \
    --mesh-name cross.user-spot-join --node-rid node-user-spot-join-target \
    --bind-endpoint "${target_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/node-user-spot-target-cpp.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_cpp cpp-user-spot-join-source-node user-spot-source \
    --mesh-name cross.user-spot-join --node-rid cpp-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}" \
    --start-file "${start_file}" --ready-file "${RUN_DIR}/cpp-user-spot-join-source-node.ready"
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: C++ source -> Node target (admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: Java source -> C++ target ---------------------------
stage_java_source_cpp_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events start_file
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/java-user-spot-join-source-cpp.events"
  target_events="${RUN_DIR}/cpp-user-spot-target-java.events"
  start_file="${RUN_DIR}/user-spot-join-java-cpp.start"

  start_redis user-spot-join-redis-java-cpp "${redis_port}"
  start_cpp cpp-user-spot-target-java user-spot-target \
    --mesh-name cross.user-spot-join --node-rid cpp-user-spot-join-target \
    --peer-rid java-user-spot-join-source --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot --actor-id cross-lang-user-spot-actor \
    --event-file "${target_events}" --ready-file "${RUN_DIR}/cpp-user-spot-target-java.ready"
  wait_for_ready "${RUN_DIR}/cpp-user-spot-target-java.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_java java-user-spot-join-source-cpp user-spot-source \
    --mesh-name cross.user-spot-join --node-rid java-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}" --start-file "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: Java source -> C++ target (admission, joined lifecycle, target-owner probe)")
}

# --- User-Spot JoinSpot: C++ source -> Java target ---------------------------
stage_cpp_source_java_target_user_spot_join() {
  local redis_port source_port target_port source_endpoint target_endpoint source_events target_events start_file
  redis_port="$(free_port)"; source_port="$(free_port)"; target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"; target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/cpp-user-spot-join-source-java.events"
  target_events="${RUN_DIR}/java-user-spot-target-cpp.events"
  start_file="${RUN_DIR}/user-spot-join-cpp-java.start"

  start_redis user-spot-join-redis-cpp-java "${redis_port}"
  start_java java-user-spot-target-cpp user-spot-target \
    --mesh-name cross.user-spot-join --node-rid java-user-spot-join-target \
    --peer-rid cpp-user-spot-join-source --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" --redis-key-prefix zlink-cross-user-spot-join \
    --spot-id cross-lang-user-spot --actor-id cross-lang-user-spot-actor --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/java-user-spot-target-cpp.ready" 180
  wait_for_line "${target_events}" "user-spot-created|spot=cross-lang-user-spot" 30
  start_cpp cpp-user-spot-join-source-java user-spot-source \
    --mesh-name cross.user-spot-join --node-rid cpp-user-spot-join-source \
    --bind-endpoint "${source_endpoint}" --redis-endpoint "127.0.0.1:${redis_port}" \
    --redis-key-prefix zlink-cross-user-spot-join --spot-id cross-lang-user-spot \
    --actor-id cross-lang-user-spot-actor --event-file "${source_events}" \
    --start-file "${start_file}" --ready-file "${RUN_DIR}/cpp-user-spot-join-source-java.ready"
  wait_for_line "${source_events}" "user-spot-source-peer-ready|ready=true" 90
  wait_for_line "${target_events}" "user-spot-source-peer-ready|ready=true" 90
  touch "${start_file}"
  wait_for_line "${source_events}" "user-spot-source-actor-created|status=created" 60
  wait_for_line "${source_events}" "user-spot-join-request-reply|accepted=true" 90
  wait_for_user_spot_target_outcome "${target_events}"
  stop_all
  RESULTS+=("User-Spot Join: C++ source -> Java target (admission, joined lifecycle, target-owner probe)")
}

# --- entry-spot relocation: Java source -> .NET target ------------------------
# Same scenario as stage_node_source_dotnet_target_relocation, but both sides
# now use pure automatic (Location-Store-only) discovery -- confirmed by
# direct repro that .NET's RelocateAsync explicitly refuses a manually
# connected topology (ZLinkFrameworkRelocationReason.ManualTopologyUnsupported)
# -- so neither host calls PeerConnections.connect/peerConnections().connect
# at all; both rely solely on the shared Redis Location/Relocation Store.
# Java (unlike .NET) tolerates a fixed own routing id alongside auto-discovery
# (its own SpotActorTransfer e2e runs the same way), so the Java side keeps a
# known --node-rid for the owner-before/probe assertions.
stage_java_source_dotnet_target_relocation() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/java-relocation-source.events"
  target_events="${RUN_DIR}/dotnet-relocation-target-java.events"
  start_redis relocation-redis-java-dotnet "${redis_port}"
  start_dotnet dotnet-relocation-target-java entry-spot-target \
    --mesh-name cross.relocation \
    --peer-rid java-relocation-source \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/dotnet-relocation-target-java.ready" 180
  start_java java-relocation-source entry-spot-source \
    --mesh-name cross.relocation \
    --node-rid java-relocation-source \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --payload-bytes 100000 \
    --event-file "${source_events}"
  wait_for_line "${source_events}" "entry-spot-owner-before|node=java-relocation-source" 60
  wait_for_line "${source_events}" "relocate-result|outcome=RELOCATED" 150
  wait_for_line "${target_events}" "entry-spot-probe|nodeRid=" 150
  if grep -qF "entry-spot-probe|nodeRid=java-relocation-source" "${target_events}"; then
    echo "relocation stage: target probe still answered by the source -- no owner transition" >&2
    exit 1
  fi
  stop_all
  RESULTS+=("relocation: Java source -> .NET target (JoinEntrySpot create, multi-chunk relocate, owner transition + liveness probe)")
}

# --- entry-spot relocation: .NET source -> Java target ------------------------
# Reverse direction of the stage above, same pure-automatic-discovery config
# on both sides.
stage_dotnet_source_java_target_relocation() {
  local redis_port source_port target_port source_endpoint target_endpoint
  local source_events target_events
  redis_port="$(free_port)"
  source_port="$(free_port)"
  target_port="$(free_port)"
  source_endpoint="tcp://127.0.0.1:${source_port}"
  target_endpoint="tcp://127.0.0.1:${target_port}"
  source_events="${RUN_DIR}/dotnet-relocation-source-java.events"
  target_events="${RUN_DIR}/java-relocation-target.events"
  start_redis relocation-redis-dotnet-java "${redis_port}"
  start_java java-relocation-target entry-spot-target \
    --mesh-name cross.relocation \
    --node-rid java-relocation-target \
    --bind-endpoint "${target_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --event-file "${target_events}"
  wait_for_ready "${RUN_DIR}/java-relocation-target.ready" 60
  start_dotnet dotnet-relocation-source-java entry-spot-source \
    --mesh-name cross.relocation \
    --bind-endpoint "${source_endpoint}" \
    --redis-endpoint "127.0.0.1:${redis_port}" \
    --actor-id cross-lang-relocation-actor \
    --payload-bytes 100000 \
    --event-file "${source_events}"
  wait_for_line "${source_events}" "entry-spot-owner-before|node=" 60
  wait_for_line "${source_events}" "relocate-result|outcome=Relocated" 150
  wait_for_line "${target_events}" "entry-spot-probe|nodeRid=java-relocation-target" 150
  stop_all
  RESULTS+=("relocation: .NET source -> Java target (JoinEntrySpot create, multi-chunk relocate, owner transition + liveness probe)")
}

run_relocation_stages() {
  stage_node_source_dotnet_target_relocation
  stage_java_source_dotnet_target_relocation
  stage_dotnet_source_java_target_relocation
}

run_java_dotnet_relocation_stages() {
  stage_java_source_dotnet_target_relocation
  stage_dotnet_source_java_target_relocation
}

run_spot_route_stages() {
  stage_cpp_spot_route_client_dotnet_host
  stage_dotnet_spot_route_client_cpp_host
  stage_cpp_spot_route_client_node_host
  stage_node_spot_route_client_cpp_host
  stage_java_spot_route_client_cpp_host
  stage_cpp_spot_route_client_java_host
  stage_cpp_spot_route_client_cpp_host
}

run_java_cross_stages() {
  stage_java_spot_route_client_node_host
  stage_node_spot_route_client_java_host
  stage_java_spot_route_client_dotnet_host
  stage_dotnet_spot_route_client_java_host
}

case "${ZLINK_CPP_CROSS_LANGUAGE_STAGE:-all}" in
  message-follow)
    stage_cpp_node_message_follow
    echo "cross-language smoke stage=message-follow result=passed"
    exit 0
    ;;
  spot-route)
    run_spot_route_stages
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=spot-route result=passed"
    exit 0
    ;;
  java-cross)
    run_java_cross_stages
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=java-cross result=passed"
    exit 0
    ;;
  relocation)
    run_relocation_stages
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=relocation result=passed"
    exit 0
    ;;
  relocation-java-dotnet)
    run_java_dotnet_relocation_stages
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=relocation-java-dotnet result=passed"
    exit 0
    ;;
  relocation-dotnet-java)
    stage_dotnet_source_java_target_relocation
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=relocation-dotnet-java result=passed"
    exit 0
    ;;
  relocation-node-dotnet)
    stage_node_source_dotnet_target_relocation
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=relocation-node-dotnet result=passed"
    exit 0
    ;;
  user-spot-join-node-dotnet)
    stage_node_source_dotnet_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-node-dotnet result=passed"
    exit 0
    ;;
  user-spot-join-node-dotnet-multiattempt)
    stage_node_source_dotnet_target_user_spot_join user-spot-join-source-multiattempt
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-node-dotnet-multiattempt result=observed"
    exit 0
    ;;
  user-spot-join-dotnet-node)
    stage_dotnet_source_node_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-dotnet-node result=passed"
    exit 0
    ;;
  user-spot-join-java-dotnet)
    stage_java_source_dotnet_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-java-dotnet result=passed"
    exit 0
    ;;
  user-spot-join-node-java)
    stage_node_source_java_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-node-java result=passed"
    exit 0
    ;;
  user-spot-join-node-cpp)
    stage_node_source_cpp_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-node-cpp result=passed"
    exit 0
    ;;
  user-spot-join-cpp-dotnet)
    stage_cpp_source_dotnet_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-cpp-dotnet result=passed"
    exit 0
    ;;
  user-spot-join-dotnet-java)
    stage_dotnet_source_java_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-dotnet-java result=passed"
    exit 0
    ;;
  user-spot-join-java-node)
    stage_java_source_node_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-java-node result=passed"
    exit 0
    ;;
  user-spot-join-dotnet-cpp)
    stage_dotnet_source_cpp_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-dotnet-cpp result=passed"
    exit 0
    ;;
  user-spot-join-cpp-node)
    stage_cpp_source_node_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-cpp-node result=passed"
    exit 0
    ;;
  user-spot-join-java-cpp)
    stage_java_source_cpp_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-java-cpp result=passed"
    exit 0
    ;;
  user-spot-join-cpp-java)
    stage_cpp_source_java_target_user_spot_join
    for result in "${RESULTS[@]}"; do
      echo "ok - ${result}"
    done
    echo "cross-language smoke stage=user-spot-join-cpp-java result=passed"
    exit 0
    ;;
  all)
    ;;
  *)
    echo "unknown ZLINK_CPP_CROSS_LANGUAGE_STAGE '${ZLINK_CPP_CROSS_LANGUAGE_STAGE}'" >&2
    exit 2
    ;;
esac

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
run_spot_route_stages
# H-10: keep the .NET -> Java automatic-discovery relocation handoff in the
# default gate.  Run it explicitly so its result is reported alongside the
# other default smoke stages rather than being silently omitted.
stage_dotnet_source_java_target_relocation

for result in "${RESULTS[@]}"; do
  echo "ok - ${result}"
done
echo "cross-language smoke result=passed"
