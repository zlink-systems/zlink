#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_SAMPLES=(
  TicTacToe.Ts
  Bingo.Ts
  DeliveryDispatch.Ts
  SupportChat.Ts
  GameQuest.Ts
  ShoppingMall.Ts
  ZoneWorld
)

if [[ "$#" -eq 0 ]]; then
  samples=("${DEFAULT_SAMPLES[@]}")
else
  samples=("$@")
fi

for sample in "${samples[@]}"; do
  runner="${SCRIPT_DIR}/${sample}/run_sample.sh"
  if [[ ! -x "${runner}" ]]; then
    echo "Unknown Node sample '${sample}'." >&2
    exit 1
  fi
  echo "sample ${sample} start"
  "${runner}"
  echo "sample ${sample} completed"
done
