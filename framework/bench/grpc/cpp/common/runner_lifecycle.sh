# shellcheck shell=bash
# Shared owner of the existing unified runner settle contract.
settle_and_capture() {
  local source_url="$1" target_url="$2" target_file="$3" target_counter="${4:-received}"
  local bound_ms="${5:-${DRAIN_BOUND_MS}}"
  local started_ms now_ms previous="" stable=0 stable_needed source_body target_body counts in_flight
  started_ms="$(date +%s%3N)"
  stable_needed=$(((COMMAND_SETTLE_MS + 99) / 100))
  while :; do
    source_body="$(curl --silent --show-error --fail --max-time 5 "${source_url}/bench/stats")"
    target_body="$(curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats")"
    counts="$(python3 -c '
import json,sys
a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2])
in_flight=a.get("currentInFlight", a.get("inFlight", 0))
received=(b.get("anyPhaseMessages", 0) if sys.argv[3] == "any"
          else b.get("received", b.get("activeMessages", 0)))
print(a.get("completed",0), in_flight, received, b.get("errors",0), b.get("rejected"))
' "${source_body}" "${target_body}" "${target_counter}")"
    in_flight="$(awk '{print $2}' <<<"${counts}")"
    # Settle = counts unchanged for COMMAND_SETTLE_MS (spec 3). Abandoned operations keep the
    # source in-flight count above zero after the window; they are a recorded result, not a
    # reason to wait for the bound.
    if [[ "${counts}" == "${previous}" ]]; then
      stable=$((stable + 1))
      if ((stable >= stable_needed)); then
        printf '{"snapshot":%s}\n' "${target_body}" >"${target_file}"
        SETTLE_MS=$(($(date +%s%3N) - started_ms))
        SETTLE_BOUND_HIT=false
        return 0
      fi
    else
      previous="${counts}"
      stable=0
    fi
    now_ms="$(date +%s%3N)"
    if ((now_ms - started_ms >= bound_ms)); then break; fi
    sleep 0.1
  done
  target_body="$(curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats")"
  printf '{"snapshot":%s}\n' "${target_body}" >"${target_file}"
  SETTLE_MS=$(($(date +%s%3N) - started_ms))
  SETTLE_BOUND_HIT=true
  return 1
}
