#!/usr/bin/env bash
# C 1024-byte baseline/candidate sweep driver.
#
# Usage:
#   ${ZLINK_WORK}/c016/sweep.sh [--only single|multi] \
#       [--cells PATTERN:TRANSPORT,PATTERN:TRANSPORT] [--retry-failed]
#
# Preconditions: both worktrees already have their own compatible core/build
# runtime and C perf build.  This driver never selects a released Core package;
# each runner resolves the libzlink from its own worktree.  Runner reports and
# this driver's state live only under ${ZLINK_WORK}/c016.

set -uo pipefail

BASELINE_ROOT="/home/hep7/project/zlink-perf-core-0.15.1"
CANDIDATE_ROOT="/home/hep7/project/zlink"
WORK_DIR="${ZLINK_WORK}/c016"
RESULTS_FILE="${WORK_DIR}/sweep-results.md"
LOG_FILE="${WORK_DIR}/sweep.log"
GATE_SCRIPT="${CANDIDATE_ROOT}/bindings/c/perf/perf_regression_gate.py"

ONLY=""
CELLS=""
RETRY_FAILED=0
CELL_FILTER_MATCHED=0

usage() {
  sed -n '2,10{s/^# \{0,1\}//;p}' "$0"
}

die() {
  printf 'ERROR: %s\n' "$*" | tee -a "$LOG_FILE" >&2
  exit 2
}

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

while (($#)); do
  case "$1" in
    --only)
      (($# >= 2)) || die "--only requires single or multi"
      ONLY="$2"
      shift 2
      ;;
    --only=*)
      ONLY="${1#--only=}"
      shift
      ;;
    --cells)
      (($# >= 2)) || die "--cells requires P:T,P:T"
      CELLS="$2"
      shift 2
      ;;
    --cells=*)
      CELLS="${1#--cells=}"
      shift
      ;;
    --retry-failed)
      RETRY_FAILED=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ -z "$ONLY" || "$ONLY" == "single" || "$ONLY" == "multi" ]] || die "--only must be single or multi"

mkdir -p "$WORK_DIR"
touch "$LOG_FILE"

for required in \
  "$BASELINE_ROOT/bindings/c/perf/run_benchmarks.sh" \
  "$BASELINE_ROOT/bindings/c/perf/run_benchmarks_multi.sh" \
  "$CANDIDATE_ROOT/bindings/c/perf/run_benchmarks.sh" \
  "$CANDIDATE_ROOT/bindings/c/perf/run_benchmarks_multi.sh" \
  "$GATE_SCRIPT"; do
  [[ -f "$required" ]] || die "required file is missing: $required"
done

if [[ ! -e "$RESULTS_FILE" ]]; then
  printf '%s\n' \
    '| runner | pattern | transport | verdict(PASS/FAIL/UNSUPPORTED/ERROR) | worst metric ratio | baseline report | candidate report | gate 요약 |' \
    '|---|---|---|---|---|---|---|---|' > "$RESULTS_FILE"
fi

# Read the exact pattern list used when each wrapper expands --pattern ALL.
extract_patterns() {
  local root="$1" suite="$2" script value
  if [[ "$suite" == "single" ]]; then
    script="$root/bindings/c/perf/run_benchmarks.sh"
    value="$(sed -n 's/^STANDARD_PATTERNS="\([^"]*\)".*/\1/p' "$script" | head -n 1)"
  else
    script="$root/bindings/c/perf/run_benchmarks_multi.sh"
    value="$(sed -n 's/^PATTERNS="\([^"]*\)".*/\1/p' "$script" | head -n 1)"
  fi
  [[ -n "$value" ]] || return 1
  printf '%s\n' "$value" | tr ',' '\n' | awk 'NF { print toupper($0) }'
}

has_value() {
  local needle="$1"
  shift
  local item
  for item in "$@"; do
    [[ "$item" == "$needle" ]] && return 0
  done
  return 1
}

cell_selected() {
  local pattern="$1" transport="$2" spec
  [[ -z "$CELLS" ]] && return 0
  IFS=',' read -r -a selected_specs <<< "$CELLS"
  for spec in "${selected_specs[@]}"; do
    [[ "$spec" =~ ^[A-Za-z0-9_]+:(tcp|tls|ws|wss|inproc|ipc)$ ]] || die "invalid --cells entry: $spec"
    if [[ "${spec%%:*}" == "$pattern" && "${spec#*:}" == "$transport" ]]; then
      CELL_FILTER_MATCHED=1
      return 0
    fi
  done
  return 1
}

result_has_verdict() {
  local suite="$1" pattern="$2" transport="$3" wanted="$4"
  awk -F'|' -v suite="$suite" -v pattern="$pattern" -v transport="$transport" -v wanted="$wanted" '
    function clean(v) { gsub(/^[ \t]+|[ \t]+$/, "", v); return v }
    NF >= 6 && clean($2) == suite && clean($3) == pattern && clean($4) == transport && clean($5) == wanted { found=1 }
    END { exit found ? 0 : 1 }
  ' "$RESULTS_FILE"
}

should_run() {
  local suite="$1" pattern="$2" transport="$3"
  if result_has_verdict "$suite" "$pattern" "$transport" PASS || \
     result_has_verdict "$suite" "$pattern" "$transport" UNSUPPORTED; then
    return 1
  fi
  if result_has_verdict "$suite" "$pattern" "$transport" FAIL || \
     result_has_verdict "$suite" "$pattern" "$transport" ERROR; then
    [[ "$RETRY_FAILED" -eq 1 ]]
    return
  fi
  return 0
}

append_result() {
  local suite="$1" pattern="$2" transport="$3" verdict="$4" worst="$5" baseline_report="$6" candidate_report="$7" summary="$8"
  summary="${summary//$'\n'/ }"
  summary="${summary//|/\/}"
  printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$suite" "$pattern" "$transport" "$verdict" "$worst" "$baseline_report" "$candidate_report" "$summary" >> "$RESULTS_FILE"
}

resolve_candidate_runtime() {
  local candidate
  for candidate in \
    "$CANDIDATE_ROOT/core/build/lib/libzlink.so" \
    "$CANDIDATE_ROOT/core/build/bin/libzlink.so"; do
    if [[ -f "$candidate" ]]; then
      realpath -e "$candidate"
      return 0
    fi
  done
  return 1
}

preflight_candidate_runtime() {
  local runtime newer_source
  runtime="$(resolve_candidate_runtime)" || die "candidate core runtime is missing under $CANDIDATE_ROOT/core/build"
  newer_source="$(find "$CANDIDATE_ROOT/core/src" -type f -newer "$runtime" -print -quit 2>/dev/null || true)"
  if [[ -n "$newer_source" ]]; then
    die "candidate libzlink is stale; runtime=$runtime newer-core-src=$newer_source"
  fi
  log "Candidate preflight runtime: $runtime"
}

find_new_report() {
  local report_dir="$1" marker="$2"
  [[ -d "$report_dir" ]] || return 1
  find "$report_dir" -maxdepth 1 -type f -newer "$marker" -printf '%T@ %p\n' 2>/dev/null \
    | sort -nr | head -n 1 | cut -d' ' -f2-
}

# Sets RUN_RC, RUN_REPORT, and RUN_LIBRARY.  Runner output goes to both stdout
# and sweep.log; all runner result files go to WORK_DIR, never repository results/.
run_runner() {
  local suite="$1" label="$2" root="$3" pattern="$4" transport="$5" tag="$6"
  local runner report_root report_dir marker output parsed
  runner="$root/bindings/c/perf/run_benchmarks.sh"
  [[ "$suite" == "multi" ]] && runner="$root/bindings/c/perf/run_benchmarks_multi.sh"
  report_root="$WORK_DIR/runner-results/$label"
  report_dir="$report_root/$suite/report"
  mkdir -p "$report_dir"
  marker="$(mktemp "$WORK_DIR/.sweep-report-marker.XXXXXX")"
  output="$(mktemp "$WORK_DIR/.sweep-runner-output.XXXXXX")"

  log "[$suite $pattern/$transport] $label runner: $runner"
  (
    cd "$root" || exit 125
    timeout 900 "$runner" \
      --pattern "$pattern" \
      --transports "$transport" \
      --msg-sizes 1024 \
      --runs 1 \
      --results-tag "$tag" \
      --results-dir "$report_root"
  ) 2>&1 | tee -a "$LOG_FILE" | tee "$output"
  RUN_RC=${PIPESTATUS[0]}

  RUN_LIBRARY="$(sed -n 's/^Perf runtime libzlink:[[:space:]]*//p' "$output" | tail -n 1)"
  if [[ -n "$RUN_LIBRARY" && -e "$RUN_LIBRARY" ]]; then
    RUN_LIBRARY="$(realpath -e "$RUN_LIBRARY")"
  fi
  RUN_REPORT="$(sed -n 's/^-[[:space:]]*result_file:[[:space:]]*//p' "$output" | tail -n 1)"
  if [[ -z "$RUN_REPORT" || ! -f "$RUN_REPORT" ]]; then
    parsed="$(find_new_report "$report_dir" "$marker" || true)"
    RUN_REPORT="$parsed"
  fi
  rm -f "$marker" "$output"
  log "[$suite $pattern/$transport] $label rc=$RUN_RC runtime=${RUN_LIBRARY:--} report=${RUN_REPORT:--}"
}

worst_ratio() {
  local gate_output="$1"
  awk -F'|' '
    function clean(v) { gsub(/^[ \t]+|[ \t]+$/, "", v); return v }
    /^\|/ && clean($2) ~ /^(single|multi)$/ && clean($5) == "1024" {
      metric=clean($6); ratio=clean($9)
      if (ratio !~ /^[-+]?([0-9]*\.)?[0-9]+$/) next
      score = (metric == "throughput" || metric == "bandwidth") ? ratio + 0 : 1 / (ratio + 0)
      if (!seen || score < worst) { seen=1; worst=score; result=metric "=" ratio }
    }
    END { if (seen) print result; else print "-" }
  ' "$gate_output"
}

run_gate() {
  local suite="$1" baseline_report="$2" candidate_report="$3" output
  output="$(mktemp "$WORK_DIR/.sweep-gate-output.XXXXXX")"
  if [[ "$suite" == "single" ]]; then
    python3 "$GATE_SCRIPT" --baseline-single "$baseline_report" --candidate-single "$candidate_report" 2>&1 | tee -a "$LOG_FILE" | tee "$output"
  else
    python3 "$GATE_SCRIPT" --baseline-multi "$baseline_report" --candidate-multi "$candidate_report" 2>&1 | tee -a "$LOG_FILE" | tee "$output"
  fi
  GATE_RC=${PIPESTATUS[0]}
  GATE_WORST="$(worst_ratio "$output")"
  GATE_SUMMARY="$(sed -n 's/^Final:[[:space:]]*//p' "$output" | tail -n 1)"
  [[ -n "$GATE_SUMMARY" ]] || GATE_SUMMARY="gate exit=$GATE_RC"
  rm -f "$output"
}

run_cell() {
  local suite="$1" pattern="$2" transport="$3"
  local baseline_tag="sweep-b-${pattern}-${transport}"
  local candidate_tag="sweep-c-${pattern}-${transport}"
  local baseline_report candidate_report summary

  if ! should_run "$suite" "$pattern" "$transport"; then
    log "[$suite $pattern/$transport] resume skip"
    return
  fi

  run_runner "$suite" baseline "$BASELINE_ROOT" "$pattern" "$transport" "$baseline_tag"
  baseline_report="$RUN_REPORT"
  local baseline_library="$RUN_LIBRARY"
  if [[ "$RUN_RC" -ne 0 || -z "$baseline_report" || ! -s "$baseline_report" ]]; then
    append_result "$suite" "$pattern" "$transport" ERROR - "${baseline_report:--}" - \
      "baseline rc=$RUN_RC runtime=${baseline_library:--}; report missing or runner failed"
    sleep 2
    return
  fi

  run_runner "$suite" candidate "$CANDIDATE_ROOT" "$pattern" "$transport" "$candidate_tag"
  candidate_report="$RUN_REPORT"
  local candidate_library="$RUN_LIBRARY"
  if [[ "$RUN_RC" -ne 0 || -z "$candidate_report" || ! -s "$candidate_report" ]]; then
    append_result "$suite" "$pattern" "$transport" ERROR - "$baseline_report" "${candidate_report:--}" \
      "candidate rc=$RUN_RC runtime=${candidate_library:--}; report missing or runner failed"
    sleep 2
    return
  fi

  run_gate "$suite" "$baseline_report" "$candidate_report"
  summary="${GATE_SUMMARY}; runtime baseline=${baseline_library:--}, candidate=${candidate_library:--}"
  if [[ "$GATE_RC" -eq 0 ]]; then
    append_result "$suite" "$pattern" "$transport" PASS "$GATE_WORST" "$baseline_report" "$candidate_report" "$summary"
  else
    append_result "$suite" "$pattern" "$transport" FAIL "$GATE_WORST" "$baseline_report" "$candidate_report" "$summary"
  fi
  sleep 2
}

record_unsupported() {
  local suite="$1" pattern="$2" transport="$3" missing_side="$4"
  should_run "$suite" "$pattern" "$transport" || return
  append_result "$suite" "$pattern" "$transport" UNSUPPORTED - - - "expected unsupported: absent from $missing_side --pattern ALL"
  log "[$suite $pattern/$transport] expected unsupported ($missing_side)"
  sleep 2
}

preflight_candidate_runtime

for suite in single multi; do
  [[ -n "$ONLY" && "$ONLY" != "$suite" ]] && continue
  mapfile -t baseline_patterns < <(extract_patterns "$BASELINE_ROOT" "$suite") || die "could not parse baseline $suite ALL patterns"
  mapfile -t candidate_patterns < <(extract_patterns "$CANDIDATE_ROOT" "$suite") || die "could not parse candidate $suite ALL patterns"
  ((${#baseline_patterns[@]})) || die "baseline $suite ALL pattern list is empty"
  ((${#candidate_patterns[@]})) || die "candidate $suite ALL pattern list is empty"

  if [[ "$suite" == "single" ]]; then
    transports=(tcp tls ws wss inproc ipc)
  else
    transports=(tcp tls ws wss)
  fi

  log "[$suite] baseline ALL=$(IFS=,; echo "${baseline_patterns[*]}")"
  log "[$suite] candidate ALL=$(IFS=,; echo "${candidate_patterns[*]}")"

  for pattern in "${baseline_patterns[@]}"; do
    if ! has_value "$pattern" "${candidate_patterns[@]}"; then
      for transport in "${transports[@]}"; do
        cell_selected "$pattern" "$transport" && record_unsupported "$suite" "$pattern" "$transport" candidate
      done
    fi
  done
  for pattern in "${candidate_patterns[@]}"; do
    if ! has_value "$pattern" "${baseline_patterns[@]}"; then
      for transport in "${transports[@]}"; do
        cell_selected "$pattern" "$transport" && record_unsupported "$suite" "$pattern" "$transport" baseline
      done
    fi
  done
  for pattern in "${baseline_patterns[@]}"; do
    has_value "$pattern" "${candidate_patterns[@]}" || continue
    for transport in "${transports[@]}"; do
      cell_selected "$pattern" "$transport" || continue
      run_cell "$suite" "$pattern" "$transport"
    done
  done
done

[[ -z "$CELLS" || "$CELL_FILTER_MATCHED" -eq 1 ]] || die "--cells did not select a supported suite/pattern/transport cell"
log "Sweep complete: $RESULTS_FILE"
