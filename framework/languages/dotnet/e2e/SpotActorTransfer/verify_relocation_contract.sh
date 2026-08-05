#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROGRAM="$ROOT_DIR/Client/Program.cs"
SCENARIOS="$ROOT_DIR/Client/Scenarios"

implemented_selectors=(
  ST-G5-ENTRY-ACTOR-SMALL
  ST-G5-ENTRY-ACTOR-SLOW-CAPTURE
  ST-G5-ENTRY-ACTOR-SLOW-RESTORE
  ST-G5-ENTRY-ACTOR-SLOW-CLEANUP
  ST-G5-PER-ACTOR-SMALL
  ST-G5-PER-ACTOR-SLOW-CAPTURE
  ST-G5-PER-ACTOR-SLOW-RESTORE
  ST-G5-PER-ACTOR-SPOT-SMALL
  ST-G5-PER-ACTOR-SPOT-SLOW-CAPTURE
  ST-G5-PER-ACTOR-SPOT-SLOW-RESTORE
  ST-G5-INSTANCE-SPOT-SMALL
  ST-G5-INSTANCE-SPOT-SLOW-CAPTURE
  ST-G5-INSTANCE-SPOT-SLOW-RESTORE
  ST-G5-SPOT-WIDE-ACTORS-10
  ST-G5-SPOT-WIDE-ACTORS-100
  ST-G6
  ST-I1-ACTOR-BOUNDARY
  ST-I1-INSTANCE
  ST-I1-SPOTWIDE
  ST-I2-RECREATE
  ST-I2-SNAPSHOT
  ST-I2-RECREATE-ON-RELOCATION
  ST-I2-PRESERVE-STATE-WITH
  ST-I3-INSTANCE
  ST-I3-SPOTWIDE
  MF-AO-FOLLOW
  MF-AR-FOLLOW
  MF-AO-QUEUE
  MF-AR-HOLD
  MF-SO-QUEUE
  MF-SR-HOLD
  MF-SO-FOLLOW
  MF-SR-FOLLOW
  MF-CORR
)

known_gap_selectors=(
  MF-PA-SPLIT
  MF-DUP
  MF-EXP
  MF-GEN
  MF-LOOP
  MF-HOP
  MF-BOUND
)

for selector in "${implemented_selectors[@]}"; do
  grep -Fq "[\"$selector\"]" "$PROGRAM" || {
    echo "implemented selector is not registered: $selector" >&2
    exit 1
  }
  [[ "$(grep -Fc "\"$selector\"" "$PROGRAM")" -ge 2 ]] || {
    echo "specialized selector is not excluded from the default run: $selector" >&2
    exit 1
  }
done

for selector in "${known_gap_selectors[@]}"; do
  grep -Fq "[\"$selector\"]" "$PROGRAM" || {
    echo "known contract gap is not classified: $selector" >&2
    exit 1
  }
done

required_evidence=(
  "unit_kind="
  "\"actor\""
  "\"instance_spot\""
  "\"user_spot\""
  "execution_mode=entry"
  "\"per_actor\""
  "execution_mode=spot_wide"
  "encoded_store_bytes="
  "opaque_store_overhead_bytes="
  "case=MF-AO-FOLLOW phase=started"
  "case=MF-AO-FOLLOW\""
  "case=MF-AR-FOLLOW phase=started"
  "case=MF-AR-FOLLOW\""
  "case=MF-AO-QUEUE"
  "case=MF-AR-HOLD"
  "case=MF-SO-QUEUE"
  "case=MF-SR-HOLD"
  "case=MF-SO-FOLLOW"
  "case=MF-SR-FOLLOW"
  "relocation_ready_completed"
  "SecondDeferRejected"
  "case=MF-CORR phase=started"
  "case=MF-CORR\""
  "previous_owner_handler_count=0"
)

for evidence in "${required_evidence[@]}"; do
  grep -FRq "$evidence" "$SCENARIOS" || {
    echo "required evidence contract is absent: $evidence" >&2
    exit 1
  }
done

if grep -ERq \
  --exclude-dir=bin \
  --exclude-dir=obj \
  'System\.Reflection|BindingFlags\.NonPublic|InternalsVisibleTo' \
  "$ROOT_DIR/Client" "$ROOT_DIR/Server"; then
  echo "E2E uses a non-public runtime access mechanism" >&2
  exit 1
fi

echo "SpotActorTransfer relocation contract registration: passed"
echo "implemented_selectors=${#implemented_selectors[@]}"
echo "known_gap_selectors=${#known_gap_selectors[@]}"
