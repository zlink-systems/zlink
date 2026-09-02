#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

SAMPLES=(
  "samples/request_reply_async_sample"
  "samples/pair_recv_sample"
  "samples/pubsub_recv_sample"
  "samples/dealer_router_recv_sample"
  "samples/stream_recv_sample"
  "samples/stream_packet_callback_sample"
  "samples/monitor_recv_sample"
)

pass=0
fail=0

for sample in "${SAMPLES[@]}"; do
  echo "==> ${sample}"
  if go run "./${sample}"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
  fi
done

echo "samples: pass=${pass} fail=${fail}"
if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi
