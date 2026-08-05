#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAMPLES=(TicTacToe Bingo SupportChat ShoppingMall DeliveryDispatch GameQuest ZoneWorld)

if (( $# > 0 )); then
  SAMPLES=("$@")
fi

for sample in "${SAMPLES[@]}"; do
  case "${sample}" in
    TicTacToe|Bingo|SupportChat|ShoppingMall|DeliveryDispatch|GameQuest|ZoneWorld)
      "${SCRIPT_DIR}/${sample}/run_sample.sh"
      ;;
    *)
      echo "Unknown .NET sample '${sample}'." >&2
      exit 2
      ;;
  esac
done
