#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
client="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
raw="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar"
client_bin="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client"
raw_bin="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
while IFS= read -r source; do [[ "$client" -nt "$source" ]] || { echo "stale client JAR: $source" >&2; exit 1; }; done < <(find "$ROOT/framework/bench/grpc/java/client/src/main/java" "$ROOT/framework/bench/grpc/java/shared/src/main/java" -type f -name '*.java')
raw_src="$ROOT/framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java"
[[ "$raw" -nt "$raw_src" ]] || { echo "stale raw server JAR" >&2; exit 1; }
commit="$(git rev-parse HEAD)"; dirty=clean; [[ -z "$(git status --porcelain)" ]] || dirty=dirty
base="$(git merge-base HEAD origin/main)"
hashes="$(sha256sum "$client_bin" "$raw_bin" "$client" "$raw" | awk '{printf "%s=%s;",$2,$1}')"
line="$(date -u +%FT%TZ) cell=$1 built_commit=$commit dirty=$dirty merge_base=$base artifacts=$hashes binding=zlink-java on both A and B sides"
printf '%s\n' "$line" >> "$ROOT/progress.md"
printf '%s\n' "$line"
