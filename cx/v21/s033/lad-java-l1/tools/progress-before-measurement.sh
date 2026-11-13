#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-java
JOB=/root/jobs/fx/cx/v21/s033/lad-java-l1
cd "$ROOT"
client_jar="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
raw_jar="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar"
client_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchClient.java"
stack_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java"
server_src="$ROOT/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java"
[[ "$client_jar" -nt "$client_src" && "$client_jar" -nt "$stack_src" && "$server_jar" -nt "$server_src" ]]
commit="$(git rev-parse HEAD)"
dirty=clean
[[ -z "$(git status --porcelain)" ]] || dirty=dirty
base="$(git merge-base HEAD origin/main)"
bins="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client $ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server $ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
hashes="$(sha256sum $bins "$client_jar" "$server_jar" "$raw_jar" | awk '{printf "%s=%s;",$2,$1}')"
line="$(date -u +%FT%TZ) cell=$1 built_commit=$commit dirty=$dirty merge_base=$base artifacts=$hashes binding_reply=framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:115-127[Message.from(RESPONSE_ENVELOPE),RawWire.encodeBenchPayloadMessage,reply.message(header).message(body).submit(two parts)] L1_reply=framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:203-205[Message.from(RESPONSE_ENVELOPE),RawWire.encodeBenchPayloadMessage,reply.message(header).message(replyBody).submit(two parts)]"
printf '%s\n' "$line" >> "$JOB/progress.md"
printf '%s\n' "$line"
