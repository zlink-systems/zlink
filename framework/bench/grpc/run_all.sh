#!/usr/bin/env bash
# Run the with-grpc bench for every language through the perf ticket queue and
# aggregate the result. Builds nothing — run `build_all.sh` first.
#
# 모든 runner는 같은 인자를 받고 같은 배치로 결과를 쓴다(README §3.1). 이 스크립트가 하는
# 일은 그 한 벌을 언어마다·run마다 큐에 넣고, 끝나면 §7.2 판정을 찍는 것뿐이다.
#
# Inputs (all optional):
#   BENCH_LANGS   측정할 언어 (기본: c cpp dotnet java node) — 판정 분모인 c는 빼지 마라
#   RUNS          언어별 run 수 (기본: 3 — G5 재현성 판정의 최소치)
#   PAYLOAD_SIZES payload 크기 (기본: 4096)
#   SCENARIO      all·request·send·패턴 이름 하나 (기본: all)
#   LABEL         결과 디렉터리 이름 (기본: 실행 시각)
#   PRIORITY      perf 티켓 우선순위 1|2|3 (기본: 1 = 감독자 판정 측정)
#   WAIT          1이면 전부 끝날 때까지 기다렸다가 집계한다 (기본: 1)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"

LANGS="${BENCH_LANGS:-c cpp dotnet java node}"
RUNS="${RUNS:-3}"
PAYLOAD_SIZES="${PAYLOAD_SIZES:-4096}"
SCENARIO="${SCENARIO:-all}"
LABEL="${LABEL:-$(date +%Y%m%d_%H%M%S)}"
PRIORITY="${PRIORITY:-1}"
WAIT="${WAIT:-1}"
JUDGE_PATTERN="${SCENARIO}"
case "${JUDGE_PATTERN}" in all|request) JUDGE_PATTERN=request-backpressure ;; send) JUDGE_PATTERN=send-saturation ;; esac

TICKET="${ROOT}/scripts/perf/perf-ticket.sh"
OUTBASE="${HERE}/log/runall/${LABEL}"

for lang in ${LANGS}; do
  case "${lang}" in c|cpp|dotnet|java|node|kotlin) ;; *) echo "알 수 없는 언어: ${lang}" >&2; exit 2 ;; esac
done

# 러너가 없으면 티켓은 pending에 쌓이기만 한다. status는 늘 rc 0이라 판정에 못 쓴다.
# shellcheck source=../../../scripts/perf/perf-queue-root.sh
source "${ROOT}/scripts/perf/perf-queue-root.sh"
queue_root="$(perf_queue_root)"
runner_pid="$(cat "${queue_root}/runner.pid" 2>/dev/null || true)"
if [[ -z "${runner_pid}" ]] || ! kill -0 "${runner_pid}" 2>/dev/null; then
  echo "perf 큐 러너가 떠 있지 않다. 먼저 띄워라:" >&2
  echo "  nohup bash ${ROOT}/scripts/perf/perf-queue-runner.sh > /tmp/perf-runner.log 2>&1 &" >&2
  exit 1
fi

runner_for() {
  case "$1" in kotlin) echo "${HERE}/java/run_local_kotlin.sh" ;; *) echo "${HERE}/$1/run_local.sh" ;; esac
}

tickets=()
for i in $(seq 1 "${RUNS}"); do
  # run 사이에 다른 언어를 끼운다. 한 언어를 연달아 돌리면 그 언어만 따뜻한 기계에서
  # 재고, §7.2의 비율이 계층 비용이 아니라 실행 순서를 재게 된다.
  for lang in ${LANGS}; do
    tickets+=("$(bash "${TICKET}" submit -p "${PRIORITY}" -o "${USER:-supervisor}" --no-wait \
      -d "runall ${LABEL} ${lang} r${i}" -- \
      env SKIP_BUILD=1 bash "$(runner_for "${lang}")" \
        --output "${OUTBASE}/${lang}/r${i}" \
        --scenario "${SCENARIO}" \
        --payload-sizes "${PAYLOAD_SIZES}")")
    sleep 1   # 티켓 이름에 초 단위 timestamp가 들어가 같은 초에 여러 건을 내면 충돌한다
  done
done
echo "[run_all] ${LANGS} × ${RUNS} run 제출 — label=${LABEL}"
echo "[run_all] 결과=${OUTBASE}"
echo "[run_all] 진행: bash ${TICKET} status"

if [[ "${WAIT}" != 1 ]]; then
  printf '%s\n' "${tickets[@]}"
  exit 0
fi

rc=0
for t in "${tickets[@]}"; do
  bash "${TICKET}" wait "${t}" || { echo "[run_all] 실패한 티켓: ${t}" >&2; rc=1; }
done

echo
for lang in ${LANGS}; do
  [[ "${lang}" == c ]] && continue
  echo "===== ${lang} ====="
  python3 "${HERE}/tools/bench_aggregate.py" \
    --lang "${lang}" --min-runs "${RUNS}" \
    --payload-sizes "${PAYLOAD_SIZES}" \
    --judgement-pattern "${JUDGE_PATTERN}" \
    --runs-glob "${OUTBASE}/c/r*" \
    --runs-glob "${OUTBASE}/${lang}/r*" \
    --format judgement || rc=1
done
exit "${rc}"
