#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SAMPLE_RUNNERS=(
  TicTacToe/run_sample.sh
  Bingo/run_sample.sh
  DeliveryDispatch/run_sample.sh
  SupportChat/run_sample.sh
  GameQuest/run_sample.sh
  ShoppingMall/run_sample.sh
)

SELECTED_RUNNERS=()

POSITIONAL_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --*) echo "Unknown sample runner option '${arg}'." >&2; exit 2 ;;
    *) POSITIONAL_ARGS+=("$arg") ;;
  esac
done
set -- "${POSITIONAL_ARGS[@]}"

if [[ "$#" -eq 0 ]]; then
  SELECTED_RUNNERS=("${SAMPLE_RUNNERS[@]}")
else
  for selector in "$@"; do
    matched=0
    for runner in "${SAMPLE_RUNNERS[@]}"; do
      sample_dir="$(basename "$(dirname "$runner")")"
      runner_key="${runner%/run_sample.sh}"
      if [[ "$selector" == "$runner" || "$selector" == "$runner_key" || "$selector" == "$sample_dir" ]]; then
        SELECTED_RUNNERS+=("$runner")
        matched=1
      fi
    done
    if [[ "$matched" == "0" ]]; then
      echo "Unknown sample selector '${selector}'." >&2
      exit 2
    fi
  done
fi

run_sample() {
  local runner="$1"
  "$SCRIPT_DIR/$runner"
}

for runner in "${SELECTED_RUNNERS[@]}"; do
  run_sample "$runner"
done

echo "sample all result=passed"
