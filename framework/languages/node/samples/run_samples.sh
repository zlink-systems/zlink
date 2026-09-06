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

sample_pid=""
stop_status=0
forward_signal() {
  local signal="$1"
  stop_status="$2"
  trap '' INT TERM
  if [[ -n "$sample_pid" ]] && kill -0 "$sample_pid" 2>/dev/null; then
    kill -"$signal" "$sample_pid" 2>/dev/null || true
  fi
}
trap 'forward_signal INT 130' INT
trap 'forward_signal TERM 143' TERM

for sample in "${samples[@]}"; do
  runner="${SCRIPT_DIR}/${sample}/run_sample.sh"
  if [[ ! -f "${runner}" ]]; then
    echo "Unknown Node sample '${sample}'." >&2
    exit 1
  fi
  echo "sample ${sample} start"
  bash "${runner}" &
  sample_pid=$!
  status=0
  wait "$sample_pid" || status=$?
  if [[ "$stop_status" -ne 0 ]]; then
    # A trapped signal interrupts wait before the sample finishes its cleanup.
    wait "$sample_pid" || true
    exit "$stop_status"
  fi
  sample_pid=""
  if [[ "$status" -ne 0 ]]; then exit "$status"; fi
  echo "sample ${sample} completed"
done
