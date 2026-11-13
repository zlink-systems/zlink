aa_bench_run_cell() {
  local trigger_url="$1" source_url="$2" target_url="$3" run_id="$4" cell_id="$5"
  local implementation="$6" pattern="$7" payload="$8" warmup_ms="$9"
  local result_file="${10}" target_file="${11}" reset_url="${12:-}"
  local remaining_drain_ms source_bound_hit warmup_abandoned drain_state
  local env_file="$AA_ENV_DIR/${AA_CELL}-${AA_SIDE}-run${AA_INDEX}.txt"
  printf 'run_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$env_file"
  trigger_phase "$trigger_url" "$run_id" "$cell_id" "$pattern" "$payload" warmup "$warmup_ms"
  wait_for_idle "$source_url"
  warmup_abandoned="$(curl --silent --show-error --fail --max-time 5 "${source_url}/bench/stats" | python3 -c 'import json,sys; a=json.load(sys.stdin); print(a.get("currentInFlight", a.get("inFlight", 0)))')"
  if ((warmup_abandoned > 0)); then bench_record_contaminated_cell "$result_file" "$implementation" "$pattern" "$payload" "warmup drain bound hit" "$warmup_abandoned"; return 0; fi
  settle_and_capture "$source_url" "$target_url" /dev/null allPhaseMessages
  if [[ "$SETTLE_BOUND_HIT" == true ]]; then bench_record_contaminated_cell "$result_file" "$implementation" "$pattern" "$payload" "warmup settle bound hit"; return 0; fi
  if [[ -n "$reset_url" ]]; then curl --silent --show-error --fail --max-time 5 -X POST "${reset_url}/bench/reset" >/dev/null; fi
  trigger_phase "$trigger_url" "$run_id" "$cell_id" "$pattern" "$payload" active "$((DURATION_SECONDS * 1000))"
  "$AA_TOOLS/capture-active.sh" "$env_file" "$DURATION_SECONDS" "${BENCH_SOURCE_PID:-}" "${BENCH_TARGET_PID:-}" "${BENCH_OUTSIDE_PID:-}" &
  local capture_pid=$!
  wait_for_idle "$source_url"
  wait "$capture_pid"
  [[ -s "$result_file" ]] || { echo "missing source result: $result_file" >&2; return 1; }
  drain_state="$(python3 - "$result_file" "$DRAIN_BOUND_MS" <<'PY'
import json, math, sys
with open(sys.argv[1], encoding="utf-8") as handle: cell=json.load(handle)["cells"][0]
print(max(0, int(sys.argv[2])-math.ceil(float(cell.get("source_drain_ms",cell.get("drain_ms",0))))), str(bool(cell.get("drain_bound_hit",False))).lower())
PY
)"
  read -r remaining_drain_ms source_bound_hit <<<"$drain_state"
  settle_and_capture "$source_url" "$target_url" "$target_file" received "$remaining_drain_ms"
  merge_target_stats "$result_file" "$target_file" "$SETTLE_MS" "$SETTLE_BOUND_HIT"
  if [[ "$SETTLE_BOUND_HIT" == true || "$source_bound_hit" == true ]]; then echo "cell settle hit bound: $cell_id" >&2; return 0; fi
  verify_request_counts "$result_file"
  printf 'run_end_utc=%s\n' "$(date -u +%FT%TZ)" >> "$env_file"
}
