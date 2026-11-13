#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-java
JOB=/root/jobs/fx/cx/v21/s033/lad-java-l2
cd "$ROOT"
client_jar="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
raw_jar="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar"
client_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchClient.java"
stack_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java"
raw_stack_src="$ROOT/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java"
server_src="$ROOT/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java"
[[ "$client_jar" -nt "$client_src" && "$client_jar" -nt "$stack_src" && "$client_jar" -nt "$raw_stack_src" && "$server_jar" -nt "$server_src" ]]
commit="$(git rev-parse HEAD)"
dirty=clean
[[ -z "$(git status --porcelain)" ]] || dirty=dirty
base="$(git merge-base HEAD origin/main)"
bins="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client $ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server $ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
hashes="$(sha256sum $bins "$client_jar" "$server_jar" "$raw_jar" | awk '{printf "%s=%s;",$2,$1}')"
line="$(date -u +%FT%TZ) cell=$1 built_commit=$commit dirty=$dirty merge_base=$base artifacts=$hashes binding_reply=framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:115-127[Message.from(RESPONSE_ENVELOPE),RawWire.encodeBenchPayloadMessage,reply.message(header).message(body).submit(two parts)] L2_reply=framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:203-205[Message.from(RESPONSE_ENVELOPE),RawWire.encodeBenchPayloadMessage,reply.message(header).message(replyBody).submit(two parts)]"
printf '%s\n' "$line" >> "$JOB/progress.md"
printf '%s\n' "$line"
parity="$(date -u +%FT%TZ) cell=$1 receive:binding=framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:94-109[one blocking recv; no drain on request socket; 0 framework permit; protobuf decode + metrics] L2=framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:138-155,200-218[one blocking recv + DONT_WAIT drain; same one L1 permit acquire; one Framework header decode; same protobuf decode + metrics]; reply:binding=ZLinkRawBenchServer.java:115-127[one response envelope Message + one protobuf body Message; two parts; submit; body protobuf copy once] L2=ZLinkFrameworkBenchServer.java:220-223[same response header/body, two parts, same one body protobuf copy]; source allocations:binding=RawStack.java:66-92[raw header Message + body Message per request] L2=FrameworkStack.java:131-149+RawStack.java:93-118,185-210[Framework Header + encoded header Message + body Message per request; command header bytes use Framework simple-header cache]; header parse=exactly one decodeHeader call per inbound L2 message at ZLinkFrameworkBenchServer.java:201; payload copied because of header=no[header parser receives only first-part dataBuffer; payload remains last part and is decoded by RawWire without header coupling]."
printf '%s\n' "$parity" >> "$JOB/progress.md"
printf '%s\n' "$parity"
