#!/usr/bin/env bash
set -euo pipefail

# JavaScript samples share the Node binding runtime (@zlink-systems/zlink).
# No separate native binding exists; these plain-JS samples consume the built
# Node package directly. Build the Node binding first: (cd ../../node && npm run build).
SAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_DIR="$(cd "$SAMPLES_DIR/../../node" && pwd)"
source "$NODE_DIR/scripts/run_node_job.sh"

if [[ ! -f "$NODE_DIR/dist/index.js" || ! -f "$NODE_DIR/build/Release/zlink.node" ]]; then
  echo "node binding is not built; run: (cd $NODE_DIR && npm run build)" >&2
  exit 1
fi

# Link the shared runtime so bare `require('@zlink-systems/zlink')` resolves to
# the sibling Node binding without publishing or copying it.
mkdir -p "$SAMPLES_DIR/node_modules/@zlink-systems"
ln -sfn "$NODE_DIR" "$SAMPLES_DIR/node_modules/@zlink-systems/zlink"

samples=(
  "dealer_router_recv_sample.js"
  "monitor_recv_sample.js"
  "pair_recv_sample.js"
  "pubsub_recv_sample.js"
  "request_reply_async_sample.js"
  "stream_packet_callback_sample.js"
  "stream_recv_sample.js"
)

passed=0
failed=0
for sample in "${samples[@]}"; do
  printf '[sample] %s\n' "$sample"
  if run_node_job 90 node "$SAMPLES_DIR/$sample"; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
  fi
done

printf 'sample summary: passed=%d failed=%d\n' "$passed" "$failed"
[[ "$failed" -eq 0 ]]
