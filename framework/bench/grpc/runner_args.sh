# shellcheck shell=bash
# The runner input contract (README §3.1). Every `run_local*.sh` sources this file
# and calls `bench_runner_args <lang> "$@"`, so the six harnesses take the same
# names and reject the same mistakes. A per-language spelling is not a cosmetic
# difference: it measures one language under conditions the table cannot show.

# Names that used to mean something here. Silently ignoring them would keep an old
# invocation running with defaults, which is exactly how a run gets measured under
# conditions nobody asked for.
declare -A BENCH_RETIRED_INPUTS=(
  [OUTROOT]=OUTPUT
  [OUTPUT_DIR]=OUTPUT
  [PAYLOADS]=PAYLOAD_SIZES
  [DURATION]=DURATION_SECONDS
  [RUNS]="(제거) run 하나가 실행 하나다 — 호출자가 OUTPUT을 바꿔 반복한다"
  [REPORT_FILE]="(제거) run report는 늘 report.txt다"
  [WARMUP]="WARMUP_SECONDS (초 단위 시간이다 — 호출 수가 아니다)"
  [STAMP]=OUTPUT
  [RUN_STAMP]=OUTPUT
)

bench_runner_args() {
  local lang="$1"; shift
  local name replacement bench_root
  bench_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

  for name in "${!BENCH_RETIRED_INPUTS[@]}"; do
    if [[ -n "${!name:-}" ]]; then
      replacement="${BENCH_RETIRED_INPUTS[${name}]}"
      echo "retired runner input: ${name} -> ${replacement}" >&2
      return 2
    fi
  done

  SCENARIO="${SCENARIO:-all}"
  WARMUP_SECONDS="${WARMUP_SECONDS:-2}"
  IMPLEMENTATION="${IMPLEMENTATION:-all}"
  PAYLOAD_SIZES="${PAYLOAD_SIZES:-4096}"
  DURATION_SECONDS="${DURATION_SECONDS:-5}"
  SKIP_BUILD="${SKIP_BUILD:-0}"
  OUTPUT="${OUTPUT:-}"

  while (($# > 0)); do
    case "$1" in
      --output) OUTPUT="${2:?--output requires a value}"; shift 2 ;;
      --scenario) SCENARIO="${2:?--scenario requires a value}"; shift 2 ;;
      --implementation) IMPLEMENTATION="${2:?--implementation requires a value}"; shift 2 ;;
      --payload-sizes) PAYLOAD_SIZES="${2:?--payload-sizes requires a value}"; shift 2 ;;
      --duration-seconds) DURATION_SECONDS="${2:?--duration-seconds requires a value}"; shift 2 ;;
      --warmup-seconds) WARMUP_SECONDS="${2:?--warmup-seconds requires a value}"; shift 2 ;;
      --skip-build) SKIP_BUILD=1; shift ;;
      *) echo "unsupported runner argument: $1" >&2; return 2 ;;
    esac
  done

  case "${SCENARIO}" in
    all) bench_patterns=(request-serial request-backpressure send-saturation) ;;
    request) bench_patterns=(request-serial request-backpressure) ;;
    send) bench_patterns=(send-saturation) ;;
    request-serial|request-backpressure|send-saturation) bench_patterns=("${SCENARIO}") ;;
    *) echo "unknown scenario: ${SCENARIO}" >&2; return 2 ;;
  esac

  case "${IMPLEMENTATION}" in
    all) bench_implementations=("grpc-${lang}" "zlink-${lang}" "zlink-framework-${lang}") ;;
    "grpc-${lang}"|"zlink-${lang}"|"zlink-framework-${lang}") bench_implementations=("${IMPLEMENTATION}") ;;
    *) echo "unknown implementation: ${IMPLEMENTATION}" >&2; return 2 ;;
  esac

  IFS=',' read -r -a bench_payloads <<<"${PAYLOAD_SIZES}"
  local payload
  for payload in "${bench_payloads[@]}"; do
    [[ "${payload}" == 4096 ]] || {
      echo "payload size must be 4096: ${payload}" >&2; return 2; }
  done

  [[ "${DURATION_SECONDS}" =~ ^[1-9][0-9]*$ ]] || {
    echo "DURATION_SECONDS must be a positive integer" >&2; return 2; }
  [[ "${WARMUP_SECONDS}" =~ ^[1-9][0-9]*$ ]] || {
    echo "WARMUP_SECONDS must be a positive integer" >&2; return 2; }
  [[ "${SKIP_BUILD}" == 0 || "${SKIP_BUILD}" == 1 ]] || {
    echo "SKIP_BUILD must be 0 or 1" >&2; return 2; }
  OUTPUT="${OUTPUT:-${bench_root}/log/${lang}/$(date +%Y%m%d_%H%M%S)}"

  # Java와 Node runner는 자기 디렉터리로 cd 한 뒤 실행한다. 상대 경로를 그대로 두면
  # 결과가 호출자가 보는 곳이 아닌 곳에 생긴다.
  [[ "${OUTPUT}" == /* ]] || OUTPUT="$(pwd)/${OUTPUT}"

  export SCENARIO IMPLEMENTATION PAYLOAD_SIZES DURATION_SECONDS WARMUP_SECONDS SKIP_BUILD OUTPUT
}

# The run-level report is never formatted by a runner (README §4).
bench_write_report() {
  local run_dir="$1" here
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  python3 "${here}/tools/bench_report.py" "${run_dir}" --output "${run_dir}/report.txt"
}
