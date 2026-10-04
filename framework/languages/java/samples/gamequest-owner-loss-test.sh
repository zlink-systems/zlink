#!/usr/bin/env bash

# Exercise the shipped owner-loss stage with initialized owners and a client whose Join
# response is either pending or complete. No server process is killed by this test.
set -euo pipefail

SAMPLES_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
RUNNER="$SAMPLES_DIR/java/GameQuest/run_sample.sh"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

awk '
  /^wait_log_count\(\)|^log_count\(\)|^wait_log_total_count\(\)/ { copying = 1 }
  copying { print }
  copying && /^}/ { copying = 0 }
' "$RUNNER" > "$TEST_ROOT/functions.sh"
awk '
  $0 == "pids+=(\"$owner_unavailable_client_pid\")" { copying = 1; next }
  copying { print }
  copying && /^kill -9 / { exit }
' "$RUNNER" > "$TEST_ROOT/stage.sh"
test -s "$TEST_ROOT/stage.sh"

for state in pending complete; do
  for owner in mission-a mission-b; do
    case_dir="$TEST_ROOT/$state-$owner"
    mkdir -p "$case_dir"
    : > "$case_dir/mission-a.log"
    : > "$case_dir/mission-b.log"
    # Both spellings allow the same fixture to exercise the unfixed and fixed runner.
    printf 'gamequest-owner-ready player=player-owner-unavailable node=%s\n' "$owner" \
      > "$case_dir/$owner.log"
    printf 'gamequest-owner-initialized player=player-owner-unavailable node=%s\n' "$owner" \
      >> "$case_dir/$owner.log"
    : > "$case_dir/owner-unavailable-client.log"
    if [[ "$state" == complete ]]; then
      printf 'gamequest-owner-join-completed\n' > "$case_dir/owner-unavailable-client.log"
    fi
    export LOG_DIR="$case_dir"
    if bash -euo pipefail -c '
      source "$1/functions.sh"
      declare -A role_pids=([mission-a]=101 [mission-b]=102)
      kill() { printf "%s\n" "$2" > "$LOG_DIR/killed"; }
      source "$1/stage.sh"
    ' bash "$TEST_ROOT" > "$case_dir/stage.log" 2>&1; then
      status=0
    else
      status=$?
    fi
    if [[ "$state" == pending ]]; then
      if [[ "$status" == 0 || -f "$case_dir/killed" ]]; then
        echo "FAIL: $owner was killed while the Join response was pending" >&2
        exit 1
      fi
      if ! grep -F -q "Timed out waiting for 1 'gamequest-owner-join-completed'" \
        "$case_dir/stage.log"; then
        echo "FAIL: pending Join failed outside the client completion check" >&2
        exit 1
      fi
    else
      expected=101
      [[ "$owner" == mission-b ]] && expected=102
      if [[ "$status" != 0 || "$(cat "$case_dir/killed")" != "$expected" ]]; then
        echo "FAIL: completed Join did not select $owner for termination" >&2
        exit 1
      fi
    fi
    printf 'ok: %s Join, owner=%s\n' "$state" "$owner"
  done
done
