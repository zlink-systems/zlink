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
#
# 새 측정은 scripts/perf/with-perf-lock.sh -- <command>를 쓴다 — lock 수명이 명령 수명과 같아
# 아래 holder 추정(패턴·grace·max_hold·호출자 생존)이 필요 없다. 이 스크립트는 호환용.

set -euo pipefail
max_wait="${1:-1800}"
interval=10
waited=0

# 대기자 여럿이 같은 순간 "idle"을 보고 함께 출발하는 경합을 막는다. idle 확인 전에
# lock을 잡고, 확인이 끝나면 lock을 백그라운드 보유자에게 넘긴다. 보유자는 호출자의
# 측정 프로세스가 나타나기를 최대 grace초 기다린 뒤 그 프로세스가 끝날 때까지 lock을
# 쥔다. flock은 open file description 단위라 이 스크립트가 종료해도 자식이 이어받는다.
# (2026-09-08: C++ latency 조사와 wss DD 재현이 1초 차이로 동시에 돌아 3회분이 오염됐다.)
lock_file="${ZLINK_PERF_LOCK:-/tmp/zlink-perf.lock}"
grace="${ZLINK_PERF_LOCK_GRACE:-20}"
exec 9>>"${lock_file}"
if ! flock -w "${max_wait}" 9; then
  echo "perf lock을 ${max_wait}초 동안 얻지 못했다: ${lock_file}" >&2
  exit 1
fi

# 실제 perf 프로세스만 센다. `pgrep -f`는 명령줄 전체를 보므로, 이 패턴을 인자로 들고 있는
# 셸(감시 스크립트, 이 스크립트 자신, 에디터의 grep 등)까지 잡힌다. 그래서 (1) 실행 파일
# 경로나 러너가 실제로 붙이는 인자로 좁히고, (2) 셸 프로세스를 명시적으로 걸러낸다.
pattern='(/perf/build/.*(perf_multi|perf_single)|(perf_multi|perf_single)[^|]* --role |run_benchmarks(_multi)?\.sh .*--pattern|Zlink\.BindingBench[A-Za-z.]*\.dll)'

while :; do
  running="$(pgrep -af "${pattern}" 2>/dev/null \
    | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' \
    | grep -v -e 'wait-for-idle-perf' -e 'with-perf-lock' || true)"
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

# lock 보유자. 호출자의 측정이 시작될 때까지 grace초 기다렸다가 끝날 때까지 쥔다.
# 어떤 경우에도 max_hold초를 넘기면 놓는다 — 2026-09-08 09:40 고아가 된 holder 하나가
# perf 없이 lock을 쥔 채 남아 큐 전체(16개 대기)가 10분 넘게 멈췄다.
max_hold="${ZLINK_PERF_LOCK_MAX_HOLD:-1800}"
case "${grace}" in ''|*[!0-9]*) grace=60;; esac
caller_pid=$PPID
(
  set +e
  hold_start=$(date +%s)
  # 호출자 shell이 살아 있는 동안은 perf가 아직 안 보여도 잡는다(.NET 기동은 20초를 넘긴다 —
  # 2026-09-08 10:03 그 창에서 Go 측정이 겹쳐 출발했다). grace는 호출자가 이미 죽었을 때의 상한.
  for _ in $(seq 1 "${grace}"); do
    r="$(pgrep -af "${pattern}" 2>/dev/null | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' | grep -v -e 'wait-for-idle-perf' -e 'with-perf-lock' || true)"
    [ -n "${r}" ] && break
    kill -0 "${caller_pid}" 2>/dev/null || break
    sleep 1
  done
  # 호출자가 아직 살아 있고 perf가 아직 없으면 계속 기다린다(최대 max_hold).
  while [ -z "${r}" ] && kill -0 "${caller_pid}" 2>/dev/null; do
    [ $(( $(date +%s) - hold_start )) -ge "${max_hold}" ] && break
    sleep 2
    r="$(pgrep -af "${pattern}" 2>/dev/null | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' | grep -v -e 'wait-for-idle-perf' -e 'with-perf-lock' || true)"
  done
  # perf가 보이는 동안 잡되, 호출자가 죽으면 놓는다 — 호출자 없는 holder는 남의 perf를 보고
  # max_hold까지 lock을 쥔다(2026-09-08 10:50 codex 호출 wrapper가 죽은 뒤 12개 대기가 멈췄다).
  while :; do
    r="$(pgrep -af "${pattern}" 2>/dev/null | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' | grep -v -e 'wait-for-idle-perf' -e 'with-perf-lock' || true)"
    [ -z "${r}" ] && break
    kill -0 "${caller_pid}" 2>/dev/null || break
    [ $(( $(date +%s) - hold_start )) -ge "${max_hold}" ] && break
    sleep 5
  done
) >/dev/null 2>&1 &
disown
