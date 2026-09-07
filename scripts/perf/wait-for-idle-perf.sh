#!/usr/bin/env bash
# 판정용 perf 측정을 시작하기 전에 다른 perf process가 없어질 때까지 기다린다.
#
# 계획서 §7.0의 직렬화 요건은 "측정"에 걸린다 — 서로의 CPU·memory·I/O 부하가 섞이면
# 수치가 오염되기 때문이다. 빌드·컴파일·단위 테스트, 그리고 status만 확인하는 smoke는
# 병렬로 돌아도 된다. 처리량의 자릿수를 판단 근거로 쓰는 순간부터 직렬이어야 한다.
#
# 사용:
#   scripts/perf/wait-for-idle-perf.sh && <measurement command>
#   scripts/perf/wait-for-idle-perf.sh 600   # 최대 대기 초 (기본 1800)

set -euo pipefail
max_wait="${1:-1800}"
interval=10
waited=0

# perf 바이너리와 공식 러너 스크립트. 자기 자신(이 스크립트)은 제외한다.
pattern='(perf_multi|perf_single|run_benchmarks(_multi)?\.(sh|ps1)|Zlink\.BindingBench)'

while :; do
  running="$(pgrep -af "${pattern}" 2>/dev/null | grep -v "wait-for-idle-perf" || true)"
  [ -z "${running}" ] && break
  if [ "${waited}" -ge "${max_wait}" ]; then
    echo "perf가 ${max_wait}초 동안 idle이 되지 않았다:" >&2
    printf '%s\n' "${running}" | head -5 >&2
    exit 1
  fi
  if [ "${waited}" -eq 0 ]; then
    echo "다른 perf process가 실행 중이다. 끝날 때까지 기다린다:" >&2
    printf '%s\n' "${running}" | head -3 | cut -c1-120 >&2
  fi
  sleep "${interval}"
  waited=$((waited + interval))
done

load="$(cut -d' ' -f1 /proc/loadavg)"
echo "perf idle 확인 (대기 ${waited}초, load ${load})"
