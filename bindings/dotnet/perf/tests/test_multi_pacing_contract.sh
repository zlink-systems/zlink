#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -eu
multi="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../multi/Zlink.BindingBench.Multi" && pwd)"
status=0

check() {
  local rule="$1"
  shift
  if "$@"; then
    echo "PASS $rule"
  else
    echo "FAIL $rule"
    status=1
  fi
}

sender_rotation() {
  rg -qF '++_accepted < MaxBurstPerSender' "$multi/common/PerfCommonMulti.cs" &&
    rg -qF 'rotation.Accepted()' "$multi/src/PerfMultiDealerDealerClient.cs" &&
    ! rg -qF 'SendLoopAsync' "$multi/src/PerfMultiDealerDealerClient.cs"
}

reply_interleave() {
  rg -qF '++_received == MaxBurstPerSender' "$multi/common/PerfCommonMulti.cs" &&
    rg -qF 'replyTurn?.Received()' "$multi/src/PerfMultiDealerRouterClient.cs" &&
    rg -qF 'replyTurn?.Received()' "$multi/src/PerfMultiRouterRouterClient.cs"
}

catch_up() {
  rg -qF 'InFlight <= WindowStartInFlight' "$multi/src/PerfMultiPubSubClient.cs" &&
    rg -qF 'minimumDelta + ReceiveQuantum >= scheduled - _windowScheduled' "$multi/src/PerfMultiPubSubClient.cs" &&
    rg -qF 'catchUp.Observe(' "$multi/src/PerfMultiPubSubClient.cs"
}

check '1 sender rotation' sender_rotation
check '4 paced reply interleave' reply_interleave
check '5 PUBSUB catch-up' catch_up
exit "$status"
