#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
export CGO_CFLAGS="${CGO_CFLAGS:-} -I${ZLINK_CORE_INCLUDE_DIR}"
export CGO_LDFLAGS="${CGO_LDFLAGS:-} -L${ZLINK_CORE_LIB_DIR} -Wl,-rpath,${ZLINK_CORE_LIB_DIR}"
export LD_LIBRARY_PATH="${ZLINK_CORE_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
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
