#!/usr/bin/env bash
# Measures every language with its runner and aggregates the results (README §11).
# Inputs: BENCH_LANGS (default "c cpp dotnet java kotlin node"), RUN_STAMP,
# DURATION_SECONDS, RUNS, SKIP_BUILD. The runners validate the last four.
# Patterns, payloads and implementations are each runner's defaults.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALL_LANGS="c cpp dotnet java kotlin node"
BENCH_LANGS="${BENCH_LANGS-${ALL_LANGS}}"
export RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
unset PATTERNS PAYLOAD_SIZES IMPLEMENTATIONS OUTPUT_DIR

read -r -a langs <<<"${BENCH_LANGS}"
[[ ${#langs[@]} -gt 0 ]] || { echo "BENCH_LANGS must not be empty" >&2; exit 2; }
for lang in "${langs[@]}"; do
  [[ " ${ALL_LANGS} " == *" ${lang} "* ]] || {
    echo "BENCH_LANGS entry '${lang}' must be one of: ${ALL_LANGS}" >&2
    exit 2
  }
done

ROOT="${HERE}/log/${RUN_STAMP}"

runner_for() {
  case "$1" in
    kotlin) echo "${HERE}/java/run_local_kotlin.sh" ;;
    *) echo "${HERE}/$1/run_local.sh" ;;
  esac
}

for lang in "${langs[@]}"; do
  echo "== ${lang}: measuring into ${ROOT}/${lang}"
  if ! OUTPUT_DIR="${ROOT}/${lang}" bash "$(runner_for "${lang}")"; then
    echo "FAILED: ${lang} runner failed; stopping (results so far are in ${ROOT})" >&2
    exit 1
  fi
done

if [[ " ${langs[*]} " != *" c "* ]]; then
  echo "== aggregation skipped: BENCH_LANGS has no c, and every report needs the C results"
  exit 0
fi

for lang in "${langs[@]}"; do
  [[ "${lang}" != c && "${lang}" != kotlin ]] || continue
  globs=(--runs-glob "${ROOT}/c/run*" --runs-glob "${ROOT}/${lang}/run*")
  if [[ "${lang}" == java && " ${langs[*]} " == *" kotlin "* ]]; then
    globs+=(--runs-glob "${ROOT}/kotlin/run*")
  fi
  echo "== ${lang}: aggregating into ${ROOT}/${lang}/report.md"
  python3 "${HERE}/tools/bench_aggregate.py" --lang "${lang}" "${globs[@]}" \
    --json-out "${ROOT}/${lang}/aggregate.json" >"${ROOT}/${lang}/report.md"
done
