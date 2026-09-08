#!/usr/bin/env bash
# perf 측정 티켓 제출·대기·조회.
#
#   scripts/perf/perf-ticket.sh submit [-p 1|2|3] [-o owner] [-d "설명"] [--no-wait] -- <command...>
#       티켓을 만들고(기본) 완료까지 기다린 뒤 명령의 rc로 종료한다. log 경로를 stderr에 찍는다.
#       -p: 1 감독자 판정 측정, 2 codex 검증(기본), 3 탐색. 같은 우선순위 안에서는 제출 순서.
#   scripts/perf/perf-ticket.sh status        # 상태판(QUEUE.md) 출력
#   scripts/perf/perf-ticket.sh wait <이름>    # 이미 낸 티켓 완료 대기
#
# 명령은 저장소 루트에서 실행되며 ZLINK_CORE_SOURCE·ZLINK_CORE_PACKAGE_PREFIX 등 환경은 티켓에
# 그대로 기록된다(제출 시점의 값). runner(scripts/perf/perf-queue-runner.sh)가 떠 있어야 한다.
set -u
repo="$(cd "$(dirname "$0")/../.." && pwd)"
root="${ZLINK_PERF_QUEUE:-${repo}/.artifacts/perf-queue}"
cmd="${1:-}"; shift || true

wait_ticket() {
  local name="$1" t
  while :; do
    t="${root}/done/${name}.ticket"
    if [ -e "${t}" ]; then
      echo "ticket ${name}: rc=$(grep -m1 '^# rc:' "${t}" | cut -c7-) log=$(grep -m1 '^# log:' "${t}" | cut -c8-)" >&2
      return "$(grep -m1 '^# rc:' "${t}" | cut -c7-)"
    fi
    if [ ! -e "${root}/pending/${name}.ticket" ] && [ ! -e "${root}/running/${name}.ticket" ]; then
      echo "ticket ${name}: 사라졌다" >&2; return 70
    fi
    rp="$(cat "${root}/runner.pid" 2>/dev/null || true)"
    if [ -z "${rp}" ] || ! kill -0 "${rp}" 2>/dev/null; then
      echo "ticket ${name}: runner가 떠 있지 않다 — 감독자가 scripts/perf/perf-queue-runner.sh를 띄워야 한다" >&2; return 71
    fi
    sleep 3
  done
}

case "${cmd}" in
  submit)
    prio=2; owner="${USER:-agent}"; desc=""; nowait=0
    while [ $# -gt 0 ]; do
      case "$1" in
        -p) prio="$2"; shift 2;; -o) owner="$2"; shift 2;; -d) desc="$2"; shift 2;;
        --no-wait) nowait=1; shift;; --) shift; break;; *) echo "unknown option $1" >&2; exit 2;;
      esac
    done
    [ $# -ge 1 ] || { echo "usage: $0 submit [-p N] [-o owner] [-d desc] [--no-wait] -- <command...>" >&2; exit 2; }
    mkdir -p "${root}/pending"
    slug="$(printf '%s' "${desc:-$1}" | tr -c 'A-Za-z0-9_.-' '_' | cut -c1-40)"
    name="${prio}-$(date +%s)-$$-${owner}-${slug}"
    tmp="${root}/pending/.${name}.tmp"
    {
      echo "#!/usr/bin/env bash"
      echo "# desc: ${desc:-$*}"
      echo "# owner: ${owner}"
      echo "# submitted: $(date '+%Y-%m-%d %H:%M:%S') pid $$ cwd $(pwd)"
      for v in ZLINK_CORE_SOURCE ZLINK_CORE_PACKAGE_PREFIX; do
        [ -n "${!v:-}" ] && printf 'export %s=%q\n' "$v" "${!v}"
      done
      printf 'exec'; printf ' %q' "$@"; echo
    } > "${tmp}"
    mv "${tmp}" "${root}/pending/${name}.ticket"
    echo "ticket ${name} 제출" >&2
    [ "${nowait}" = 1 ] && { echo "${name}"; exit 0; }
    wait_ticket "${name}"
    ;;
  wait) wait_ticket "$1";;
  status) cat "${root}/QUEUE.md" 2>/dev/null || echo "상태판 없음 (runner 미실행?)";;
  *) sed -n 2,12p "$0"; exit 2;;
esac
