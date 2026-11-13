#!/usr/bin/env bash
set -euo pipefail
AGG=/root/worktree/zlink-v21-s033/agg-main/framework/bench/grpc/tools/bench_aggregate.py
JOB=/root/jobs/fx/cx/v21/s033/r4190w
for lang in cpp dotnet java node; do
  args=(--lang "$lang" --min-runs 3 --baseline zlink-c --json-out "$JOB/raw/aggregates/$lang.json")
  case "$lang" in
    cpp)   args+=(--runs-glob "$JOB/raw/bench/cpp/run*" --runs-glob "$JOB/raw/bench/c/run*") ;;
    dotnet) args+=(--runs-glob "$JOB/raw/bench/dotnet/run*" --runs-glob "$JOB/raw/bench/c/run*") ;;
    java)  args+=(--runs-glob "$JOB/raw/bench/java/run*" --runs-glob "$JOB/raw/bench/c/run*") ;;
    node)  args+=(--runs-glob "$JOB/raw/bench/node/run*" --runs-glob "$JOB/raw/bench/c/run*") ;;
  esac
  python3 "$AGG" "${args[@]}" > "$JOB/raw/aggregates/$lang.md"
done
