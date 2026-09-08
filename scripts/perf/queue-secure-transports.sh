#!/usr/bin/env bash
# Core 0.17.4(ws/tls 64 KiB 왕복·latency spike 수정) 도착 시 tls·ws·wss 6 pattern을 C + 7개 binding에 대해
# 티켓으로 한 번에 낸다(1-run, D-BP32). 사용: scripts/perf/queue-secure-transports.sh <prefix> [tag]
#   prefix 예: ~/.cache/zlink/core/0.17.4/linux-x64 (fetch-release.sh로 설치한 공식 artifact)
# 전제: runner(perf-queue-runner.sh)가 떠 있고, 각 언어를 그 prefix로 한 번 빌드했다(빌드 티켓은 별도, prio 0).
set -euo pipefail
prefix="${1:?prefix}"; tag="${2:-sec174}"
export ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="${prefix}"
pats=MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_DEALER_ROUTER_SENDSEND,MULTI_ROUTER_ROUTER_SENDSEND,MULTI_DEALER_ROUTER_REQREP,MULTI_ROUTER_ROUTER_REQREP
common="--pattern ${pats} --transports tls,ws,wss --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 1 --reuse-build --results-tag ${tag}"
runner() { case "$1" in dotnet|java|node|python) echo "bindings/$1/perf/multi/run_benchmarks.sh";; *) echo "bindings/$1/perf/run_benchmarks_multi.sh";; esac; }
for lang in c cpp dotnet java node go rust python; do
  bash scripts/perf/perf-ticket.sh submit --no-wait -p 1 -o supervisor -d "${tag} tls,ws,wss 6 pattern ${lang}" -- bash "$(runner "${lang}")" ${common} >/dev/null
done
echo "queued 8 tickets (tag ${tag}); 기록은 record.py <lang> <pattern> ${tag} --transport <tls|ws|wss>"
