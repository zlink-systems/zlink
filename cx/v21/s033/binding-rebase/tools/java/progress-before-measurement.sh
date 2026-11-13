#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-java
JOB=/root/jobs/fx/cx/v21/s033/lad-java-m1
cd "$ROOT"
client_jar="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
raw_jar="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar"
client_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchClient.java"
stack_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java"
raw_stack_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java"
server_src="$ROOT/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java"
raw_server_src="$ROOT/framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java"
[[ "$client_jar" -nt "$client_src" && "$client_jar" -nt "$stack_src" && "$client_jar" -nt "$raw_stack_src" && "$server_jar" -nt "$server_src" && "$raw_jar" -nt "$raw_server_src" ]]
commit="$(git rev-parse HEAD)"
dirty=clean
[[ -z "$(git status --porcelain)" ]] || dirty=dirty
base="$(git merge-base HEAD origin/main)"
bins=(
  "$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client"
  "$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
  "$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
)
hashes="$(sha256sum "${bins[@]}" "$client_jar" "$server_jar" "$raw_jar" | awk '{printf "%s=%s;",$2,$1}')"
common="$(date -u +%FT%TZ) cell=$1 built_commit=$commit dirty=$dirty merge_base=$base artifacts=$hashes"
printf '%s\n' "$common" >> "$JOB/progress.md"
printf '%s\n' "$common"
binding='binding receive=ZLinkRawBenchServer.java:94-109[one blocking recv(NONE) per message; no drain; reuse Received; decode body]; reply=:115-127[new response header Message + protobuf body encode/Message; submit 2 parts; close wrappers]; waits=1 blocking recv/message; metrics=1 same record atomic set/message; copies=protobuf reply body encode once'
ladder='ladder receive=ZLinkFrameworkBenchServer.java:126-174[one blocking recv(NONE) per message; no drain; reuse Received; same body decode]; reply=:215-219[one new response header + protobuf body Message; submit 2 parts; close]; waits=1 blocking recv/message; metrics=1 same record atomic set/message; copies=same protobuf reply body encode once; L1+ one permit allocation/acquire before recv and transfer just before handler at :140-151'
case "$2" in
  L0) features='L0 per-message delta=none in waits/allocations/atomics/copies; fixed direct handler call only' ;;
  L1) features='L1 per-message delta=one permit allocation/acquire/handlerStarted/return; no extra payload copy or metrics atomic' ;;
  L2) features='L2 input=ZLinkFrameworkBenchServer.java:196-209[one Framework envelope decode]; source=FrameworkStack.java:146-162,RawStack.java:98-129,259-290[Framework envelope header encode, same two submitted app parts]; per-message delta=header/view allocations + one decode each direction; waits and metrics atomics unchanged; payload copy unchanged' ;;
  L3) features='L3 source=RawStack.java:134-176,280-318[service header + encoded multipart frame around two L2 parts; submit outer two parts]; server=ZLinkFrameworkBenchServer.java:223-280[decode service header + multipart view + L2 envelope; reply service header + encoded multipart frame]; per-message delta=service header/frame allocations + one service/multipart decode each direction; waits and metrics atomics unchanged; request-frame copy and reply body encode once' ;;
  *) echo "unknown level: $2" >&2; exit 2 ;;
esac
parity="$(date -u +%FT%TZ) cell=$1 $binding || $ladder || $features"
printf '%s\n' "$parity" >> "$JOB/progress.md"
printf '%s\n' "$parity"
