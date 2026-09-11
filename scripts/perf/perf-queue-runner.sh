#!/usr/bin/env bash
# perf 측정 티켓 runner — 한 프로세스만 띄우고, pending 티켓을 이름순으로 하나씩 실행한다.
#
#   scripts/perf/perf-queue-runner.sh            # 포그라운드 (보통 nohup … & 로 띄운다)
#
# 직렬성은 "runner가 하나"라는 사실에서 나온다(D-BP33 후속, 사용자 제안 "공통 파일에 티켓").
# 티켓은 .artifacts/perf-queue/pending/<이름>.ticket 파일 하나이며, 파일 자체가 bash 스니펫이다
# (`perf-ticket.sh submit`이 만든다). 실행 중 running/, 끝나면 done/으로 옮기고 rc·시각·log를
# 티켓 끝에 덧붙인다. QUEUE.md에 대기·실행·완료 상태판을 쓴다.
#
# 이전 방식(with-perf-lock.sh·wait-for-idle-perf.sh)과 겹치지 않도록 티켓 하나를 실행하는 동안
# 같은 flock(/tmp/zlink-perf.lock)을 쥔다. 새 측정은 모두 티켓으로 낸다.
set -u
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(git -C "${script_dir}" rev-parse --show-toplevel)"
# shellcheck source=perf-queue-root.sh
source "${script_dir}/perf-queue-root.sh"
root="$(perf_queue_root)"
lock_file="${ZLINK_PERF_LOCK:-/tmp/zlink-perf.lock}"
load_max="${ZLINK_PERF_LOAD_MAX:-5}"
pattern='(/perf/build/.*(perf_multi|perf_single)|(perf_multi|perf_single)[^|]* --role |run_benchmarks(_multi)?\.sh .*--pattern|Zlink\.BindingBench[A-Za-z.]*\.dll)'
mkdir -p "${root}/pending" "${root}/running" "${root}/done" "${root}/log"

# runner 단일 인스턴스
exec 8>>"${root}/runner.lock"
if ! flock -n 8; then
  runner_pid=""
  [ -r "${root}/runner.pid" ] && runner_pid="$(<"${root}/runner.pid")"
  if [[ "${runner_pid}" =~ ^[1-9][0-9]*$ ]] && kill -0 "${runner_pid}" 2>/dev/null; then
    echo "runner가 이미 떠 있다: pid ${runner_pid}" >&2
  else
    echo "runner가 이미 떠 있다: runner.lock을 다른 runner가 보유 중 (runner.pid stale 또는 없음)" >&2
  fi
  exit 1
fi
echo $$ > "${root}/runner.pid"
trap 'rm -f "${root}/runner.pid"' EXIT

log() { echo "$(date '+%H:%M:%S') $*" >> "${root}/runner.log"; }
board() {
  # 주의: 호출자의 루프 변수와 이름이 겹치면 안 된다 — t·st·sig를 local로.
  local t st
  {
    echo "# perf queue — $(date '+%Y-%m-%d %H:%M:%S') runner pid $$ load $(cut -d' ' -f1-3 /proc/loadavg)"
    echo; echo "## 실행 중"
    for t in "${root}"/running/*.ticket; do [ -e "$t" ] || continue; st="$(grep -m1 '^# started:' "$t" | cut -c12-)"; echo "- $(basename "$t" .ticket) — $(grep -m1 '^# desc:' "$t" | cut -c9-) (${st:+시작 $st}${st:-lock·load 대기 중})"; done
    echo; echo "## 대기 (이름순 = 우선순위-제출시각)"
    for t in $(ls "${root}"/pending/*.ticket 2>/dev/null | sort); do echo "- $(basename "$t" .ticket) — $(grep -m1 '^# desc:' "$t" | cut -c9-)"; done
    echo; echo "## 최근 완료 (20)"
    for t in $(ls -t "${root}"/done/*.ticket 2>/dev/null | head -20); do echo "- $(basename "$t" .ticket) — $(grep -m1 '^# desc:' "$t" | cut -c9-) → rc=$(grep -m1 '^# rc:' "$t" | cut -c7-) ($(grep -m1 '^# started:' "$t" | cut -c12-)~$(grep -m1 '^# finished:' "$t" | cut -c13-))"; done
  } > "${root}/QUEUE.md.tmp" && mv "${root}/QUEUE.md.tmp" "${root}/QUEUE.md"
}

# 이전 runner가 죽어 running/에 남은 티켓은 다시 pending으로 돌린다(실행 여부는 log로 확인).
for t in "${root}"/running/*.ticket; do
  [ -e "$t" ] || continue
  echo "# requeued: $(date '+%H:%M:%S') 이전 runner 종료" >> "$t"; mv "$t" "${root}/pending/"
done
echo "perf queue runner 시작 pid $$ root ${root}"
board
while :; do
  t="$(ls "${root}"/pending/*.ticket 2>/dev/null | sort | head -1)"
  if [ -z "${t}" ]; then
    sig="$(ls "${root}"/pending "${root}"/running 2>/dev/null | md5sum)"; [ "${sig}" != "${last_sig:-}" ] && { board; last_sig="${sig}"; }
    sleep 3; continue
  fi
  name="$(basename "${t}")"
  log "pick ${name}"
  mv "${t}" "${root}/running/${name}" || { log "mv-to-running 실패 ${name}"; continue; }
  t="${root}/running/${name}"
  echo "# waiting: $(date '+%H:%M:%S') lock·load 대기" >> "${t}"
  board
  # 이전 방식 측정과 겹치지 않게 같은 lock을 쥐고, load가 내려갈 때까지 기다린다.
  exec 9>>"${lock_file}"
  # 블로킹 flock — 폴링(flock -n + sleep)은 블로킹 대기자들에게 항상 져서 runner가 굶는다.
  flock -w 3600 9 8>&- || echo "# warn: perf lock 3600s 초과, 그대로 진행" >> "${t}"
  # 조용한 머신 판정: loadavg는 끝난 벤치의 잔상이 1~2분 남아 큐를 굶기므로, 실제 CPU 사용률(2초 창)과
  # perf 프로세스 부재로 판단하고 loadavg는 상한(load_max×2)으로만 쓴다.
  waited=0
  while [ "${waited}" -lt 600 ]; do
    busy="$(awk 'NR==1{print $2+$3+$4+$5+$6+$7+$8, $5}' /proc/stat)"; sleep 2
    busy2="$(awk 'NR==1{print $2+$3+$4+$5+$6+$7+$8, $5}' /proc/stat)"
    pct="$(awk -v a="${busy}" -v b="${busy2}" 'BEGIN{split(a,x," ");split(b,y," ");t=y[1]-x[1];i=y[2]-x[2]; if(t<=0){print 0}else{printf "%d",(t-i)*100/t}}')"
    running="$(pgrep -af "${pattern}" 2>/dev/null | grep -vE '^[0-9]+ +(/bin/)?(ba)?sh -[lc]' | grep -v -e 'with-perf-lock' -e 'wait-for-idle-perf' -e 'perf-ticket' -e 'perf-queue-runner' || true)"
    load1="$(cut -d' ' -f1 /proc/loadavg)"
    if [ -z "${running}" ] && [ "${pct}" -le 15 ] && awk -v l="${load1}" -v m="${load_max}" 'BEGIN{exit !(l<=m*2)}'; then break; fi
    waited=$((waited+2))
  done
  echo "# started: $(date '+%H:%M:%S') (lock·quiet 대기 ${waited}s, load $(cut -d' ' -f1 /proc/loadavg), cpu ${pct:-?}%)" >> "${t}"
  log "start ${name} (대기 ${waited}s)"
  board
  # 자식에게 runner lock(8)·perf lock(9)을 물려주지 않는다 — 자식이 남으면 lock도 남는다.
  ( exec 8>&- 9>&-; cd "${repo}" && bash "${t}" ) > "${root}/log/${name%.ticket}.log" 2>&1
  rc=$?
  exec 9>&-
  { echo "# finished: $(date '+%H:%M:%S')"; echo "# rc: ${rc}"; echo "# log: ${root}/log/${name%.ticket}.log"; } >> "${t}"
  mv "${t}" "${root}/done/${name}" || log "mv-to-done 실패 ${name}"
  log "done ${name} rc=${rc}"
  board
done
