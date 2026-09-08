#!/usr/bin/env bash
# perf 측정 명령을 lock 아래에서 실행한다. lock 수명 = 명령 수명.
#
#   scripts/perf/with-perf-lock.sh [max_wait_sec] -- <measurement command...>
#
# wait-for-idle-perf.sh는 lock을 백그라운드 holder에게 넘기고 "호출자의 perf 프로세스가
# 보이는 동안" 쥐는 방식이라, 프로세스 패턴 추정·호출자 생존·grace·max_hold라는 네 가지
# 추정에 기대며 2026-09-08 하루에 네 번 고쳐야 했다(고아 holder가 큐를 두 번 멈췄다).
# 이 스크립트는 추정이 없다: flock을 얻고 load가 내려가면 명령을 그대로 실행하고,
# 명령이 끝나면(정상·오류·kill 모두) fd가 닫히며 lock이 풀린다.
set -euo pipefail
max_wait=1800
if [ "${1:-}" != "--" ]; then max_wait="$1"; shift; fi
[ "${1:-}" = "--" ] && shift
[ $# -ge 1 ] || { echo "usage: $0 [max_wait_sec] -- <command...>" >&2; exit 2; }

lock_file="${ZLINK_PERF_LOCK:-/tmp/zlink-perf.lock}"
load_max="${ZLINK_PERF_LOAD_MAX:-5}"
exec 9>>"${lock_file}"
if ! flock -w "${max_wait}" 9; then
  echo "perf lock을 ${max_wait}초 동안 얻지 못했다: ${lock_file}" >&2
  exit 75
fi
# lock 밖에서 도는 perf(사람이 직접 띄운 것)와 load가 가라앉을 때까지 기다린다.
pattern='(/perf/build/.*(perf_multi|perf_single)|(perf_multi|perf_single)[^|]* --role |run_benchmarks(_multi)?\.sh .*--pattern|Zlink\.BindingBench[A-Za-z.]*\.dll)'
waited=0
while :; do
  running="$(pgrep -af "${pattern}" 2>/dev/null | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' | grep -v -e 'with-perf-lock' -e 'wait-for-idle-perf' || true)"
  load="$(cut -d' ' -f1 /proc/loadavg)"
  if [ -z "${running}" ] && awk -v l="${load}" -v m="${load_max}" 'BEGIN{exit !(l<=m)}'; then break; fi
  if [ "${waited}" -ge "${max_wait}" ]; then
    echo "perf idle 대기 초과(${max_wait}s): load=${load} running=${running:-없음}" >&2
    exit 75
  fi
  sleep 5; waited=$((waited + 5))
done
echo "perf lock 확보 (대기 ${waited}초, load ${load})" >&2
exec "$@"
