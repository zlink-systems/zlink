#!/usr/bin/env bash
# Phase 7 binding-perf smoke driver.  Run from any directory.
# It never writes under the zlink checkout: reports, logs, and the resume file
# are rooted beside this driver.  This is deliberately a conformance smoke, not
# a performance threshold gate.
set -uo pipefail

readonly REPO=/home/hep7/project/zlink
readonly OUT=${ZLINK_WORK}/c016
readonly RESULTS="${OUT}/phase7-results.md"
readonly LOG="${OUT}/phase7-smoke.log"
readonly RUN_ROOT="${OUT}/phase7-run"
readonly TIMEOUT_SECONDS=1200

ONLY="c,cpp,dotnet,go,java,node,python,rust"
FINAL_STATUS=0
NODE_HELP_DONE=0

usage() {
  cat <<'EOF'
Usage: phase7-smoke.sh [--only c,cpp,dotnet,go,java,node,python,rust]

Runs Phase 7 public-binding perf smoke only.  Each runner is invoked with
--pattern ALL, 1024 bytes, --duration 1, --runs 1; multi also uses --clients 1.
Go, Python, and Rust additionally receive --smoke.  A runner is killed after
1200 seconds; failure in one language does not prevent later languages.

Results append to phase7-results.md and all stdout/stderr append to
phase7-smoke.log.  A language whose last completed block is PASS is resumed as
already complete.
EOF
}

while (($#)); do
  case "$1" in
    --only) ONLY=${2:?--only requires a comma-separated value}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT" "$RUN_ROOT"
touch "$LOG" "$RESULTS"

if [[ ! -s "$RESULTS" ]]; then
  cat >"$RESULTS" <<'EOF'
# Phase 7 binding perf smoke results

Smoke-only verdict (D-026): no performance-number threshold is evaluated.
Every expected cell must either pass or emit an `UNSUPPORTED` result that is
declared by that runner's selected `ALL` manifest.  Any skip, failure, missing
completion accounting, runtime-identity omission, or 1200-second timeout is
unexpected.
EOF
fi

contains_language() { [[ ",$ONLY," == *",$1,"* ]]; }

# The current runner manifests expand ALL to seven patterns.  Single = 7 x six
# requested Linux transports; multi = 7 x four requested transports.
expected_cells() { [[ $1 == single ]] && printf '42' || printf '28'; }

last_language_passed() {
  awk -v wanted="## Language: $1" '
    $0 == wanted { seen=1; value=""; next }
    seen && /^## Language: / { seen=0 }
    seen && /^- overall: PASS$/ { value="PASS" }
    END { exit value == "PASS" ? 0 : 1 }
  ' "$RESULTS"
}

markdown_escape() { sed 's/|/\\|/g; s/`/\\`/g'; }

number_from() {
  # $1=file, $2=Completion key (success, unsupported, skip, fail)
  local value
  value=$(sed -nE "s/^- ${2}: ([0-9]+).*/\\1/p" "$1" | tail -n 1)
  [[ $value =~ ^[0-9]+$ ]] && printf '%s' "$value" || printf '0'
}

runtime_identity_from() {
  local evidence=$1 line path sha version revision identity
  path=$(sed -nE 's/^META,core_runtime,(.*)$/\1/p; s/^META,runtime_libzlink,(.*)$/\1/p; s/^(Perf runtime libzlink|Go package runtime|Rust perf runtime):[[:space:]]*(.*)$/\2/p' "$evidence" | tail -n 1)
  sha=$(sed -nE 's/^META,.*(sha256|SHA-256).*,(.*)$/\2/p; s/^(Perf runtime sha256|Go package runtime sha256|Rust perf runtime sha256):[[:space:]]*(.*)$/\2/p' "$evidence" | tail -n 1)
  version=$(sed -nE 's/^META,core_version,(.*)$/\1/p; s/^(Perf Core version|Core version):[[:space:]]*(.*)$/\2/p' "$evidence" | tail -n 1)
  revision=$(sed -nE 's/^META,core_revision,(.*)$/\1/p' "$evidence" | tail -n 1)
  identity="path=${path:-missing}; version=${version:-not-emitted}; sha256=${sha:-not-emitted}"
  [[ -n $revision ]] && identity+="; revision=$revision"
  printf '%s' "$identity"
}

valid_unsupported_lines() {
  # Runner reports use either UNSUPPORTED,current,PATTERN,transport or
  # UNSUPPORTED,<language>,PATTERN,transport.  All requested manifests are
  # limited to the known Phase-7 patterns and transports below.
  local evidence=$1 line a b c d pattern transport
  local invalid=0 found=0
  while IFS= read -r line; do
    found=1
    IFS=, read -r a b c d _ <<<"$line"
    if [[ $b == current || $b == c || $b == cpp || $b == dotnet || $b == go || $b == java || $b == node || $b == python || $b == rust ]]; then
      pattern=$c; transport=$d
    else
      # Some runners prefix a library identity, then pattern/transport.
      pattern=$b; transport=$c
    fi
    pattern=${pattern#MULTI_}
    case "$pattern" in
      PAIR|PUBSUB|DEALER_DEALER|DEALER_ROUTER|DEALER_ROUTER_SENDSEND|DEALER_ROUTER_REQREP|ROUTER_ROUTER|ROUTER_ROUTER_SENDSEND|ROUTER_ROUTER_REQREP|STREAM) ;;
      *) invalid=1 ;;
    esac
    case "$transport" in tcp|tls|ws|wss|inproc|ipc) ;; *) invalid=1 ;; esac
  done < <(grep -E '^UNSUPPORTED,' "$evidence" || true)
  [[ $found -eq 1 && $invalid -eq 0 ]]
}

run_runner() {
  # language, suite, runner path, then the exact Phase-7 arguments.
  local language=$1 suite=$2 runner=$3
  shift 3
  local expected case_log help_log report evidence exit_code help_exit timeout_hit=0
  local pass unsupported skip fail unexpected identity report_display status_bad=0 help_bad=0
  expected=$(expected_cells "$suite")
  case_log="${RUN_ROOT}/${language}-${suite}.log"
  help_log="${RUN_ROOT}/${language}-${suite}.help.log"
  : >"$case_log"
  : >"$help_log"

  # The real top-level runner help is the option authority.  Node's wrapper
  # may synchronize generated tools before forwarding help, so Phase 7 permits
  # one total Node help preflight (single is representative of common flags);
  # multi's --clients support is source-confirmed in the companion summary.
  if [[ $language == node && $NODE_HELP_DONE -eq 1 ]]; then
    printf '\n[%s] node/%s help preflight: reused single runner preflight\n' "$(date -Is)" "$suite" | tee -a "$LOG"
  else
    printf '\n[%s] %s/%s help preflight: %q --help\n' "$(date -Is)" "$language" "$suite" "$runner" | tee -a "$LOG"
    (cd "$REPO" && timeout "$TIMEOUT_SECONDS" "$runner" --help) 2>&1 | tee -a "$help_log" | tee -a "$LOG"
    help_exit=${PIPESTATUS[0]}
    [[ $language == node ]] && NODE_HELP_DONE=1
    if (( help_exit != 0 )); then
      help_bad=1
    elif ! grep -q -- '--pattern' "$help_log" \
        || ! grep -q -- '--transports' "$help_log" \
        || ! grep -q -- '--msg-sizes' "$help_log" \
        || ! grep -q -- '--duration' "$help_log" \
        || ! grep -q -- '--runs' "$help_log"; then
      help_bad=1
    elif [[ $suite == multi ]] && ! grep -q -- '--clients' "$help_log"; then
      help_bad=1
    elif [[ $language == go || $language == python || $language == rust ]] \
        && ! grep -q -- '--smoke' "$help_log"; then
      help_bad=1
    fi
  fi

  printf '\n[%s] %s/%s: %q' "$(date -Is)" "$language" "$suite" "$runner" | tee -a "$LOG"
  printf ' %q' "$@" | tee -a "$LOG"
  printf '\n' | tee -a "$LOG"

  # timeout is intentionally outside the runner, so all build/runtime/process
  # hangs have the same Phase-7 outcome.  PIPESTATUS preserves timeout's exit.
  (cd "$REPO" && timeout "$TIMEOUT_SECONDS" "$runner" "$@") 2>&1 | tee -a "$case_log" | tee -a "$LOG"
  exit_code=${PIPESTATUS[0]}
  [[ $exit_code -eq 124 || $exit_code -eq 137 ]] && timeout_hit=1

  report=$(sed -nE 's/^Saved result file:[[:space:]]*(.*)[[:space:]]+\(status=.*\)$/\1/p' "$case_log" | tail -n 1)
  evidence=$case_log
  if [[ -n $report && -f $report ]]; then
    evidence=$report
    report_display=$report
  elif [[ -n $report ]]; then
    # Go --smoke writes a temporary smoke-report and removes it at exit.  Its
    # report is echoed to stdout, which remains durable in case_log.
    report_display="${report} (transient; stdout captured)"
  elif grep -q '^SMOKE PASS cases=' "$case_log"; then
    # Rust --smoke exits before the normal --report writer.
    report_display='not emitted by runner in --smoke mode (stdout captured)'
  else
    report_display='not found'
  fi

  pass=$(number_from "$evidence" success)
  unsupported=$(number_from "$evidence" unsupported)
  skip=$(number_from "$evidence" skip)
  fail=$(number_from "$evidence" fail)
  if [[ $pass -eq 0 ]]; then
    pass=$(sed -nE 's/^SMOKE PASS cases=([0-9]+).*$/\1/p' "$case_log" | tail -n 1)
    [[ $pass =~ ^[0-9]+$ ]] || pass=0
  fi
  grep -Eq 'status=(partial|failed|failure)|^.*SMOKE FAIL|^.*FAIL,' "$evidence" && status_bad=1

  unexpected=$((skip + fail))
  (( help_bad )) && unexpected=$((unexpected + 1))
  (( timeout_hit )) && unexpected=$((unexpected + 1))
  (( exit_code != 0 && !timeout_hit )) && unexpected=$((unexpected + 1))
  (( status_bad )) && unexpected=$((unexpected + 1))

  if (( unsupported > 0 )); then
    if valid_unsupported_lines "$case_log"; then :; else unexpected=$((unexpected + unsupported)); unsupported=0; fi
  fi

  identity=$(runtime_identity_from "$case_log")
  # An absolute Core runtime location is the minimum identity emitted by every
  # runner; version/SHA may be represented by META provenance instead.
  if [[ $identity == path=missing* || $identity != path=/* ]]; then
    unexpected=$((unexpected + 1))
    identity+="; IDENTITY_ERROR=absolute-runtime-path-missing"
  fi

  # Completion counts are cell counts for the one-size/one-run smoke.  Do not
  # convert missing rows into passes merely because the shell exited zero.
  if (( pass + unsupported + skip + fail != expected )); then
    unexpected=$((unexpected + 1))
  fi

  printf '| %s | %s | %s | %s | %s | %s | %s |\n' \
    "$suite" "$expected" "$pass" "$unsupported" "$unexpected" \
    "$(printf '%s' "$report_display" | markdown_escape)" \
    "$(printf '%s' "$identity" | markdown_escape)" >>"${language}.rows"
}

run_language() {
  local language=$1
  shift
  if ! contains_language "$language"; then return; fi
  if last_language_passed "$language"; then
    printf '[%s] resume: %s already PASS; skipped\n' "$(date -Is)" "$language" | tee -a "$LOG"
    return
  fi

  : >"${language}.rows"
  "$@"
  local overall=PASS
  awk -F'|' 'NR > 1 { gsub(/[[:space:]]/, "", $6); if ($6 != "0") bad=1 } END { exit bad }' "${language}.rows" || overall=FAIL
  {
    printf '\n## Language: %s\n\n' "$language"
    printf '| runner | expected | pass | unsupported | unexpected | report path | runtime identity |\n'
    printf '| --- | ---: | ---: | ---: | ---: | --- | --- |\n'
    cat "${language}.rows"
    printf '\n- overall: %s\n' "$overall"
  } >>"$RESULTS"
  [[ $overall == PASS ]] || FINAL_STATUS=1
  rm -f "${language}.rows"
}

c() {
  run_runner c single "$REPO/bindings/c/perf/run_benchmarks.sh" --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/c"
  run_runner c multi "$REPO/bindings/c/perf/run_benchmarks_multi.sh" --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/c"
}
cpp() {
  run_runner cpp single "$REPO/bindings/cpp/perf/run_benchmarks.sh" --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/cpp"
  run_runner cpp multi "$REPO/bindings/cpp/perf/run_benchmarks_multi.sh" --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/cpp"
}
dotnet() {
  run_runner dotnet single "$REPO/bindings/dotnet/perf/run_benchmarks.sh" --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/dotnet"
  run_runner dotnet multi "$REPO/bindings/dotnet/perf/run_benchmarks_multi.sh" --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/dotnet"
}
go() {
  run_runner go single "$REPO/bindings/go/perf/run_benchmarks.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/go"
  run_runner go multi "$REPO/bindings/go/perf/run_benchmarks_multi.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/go"
}
java() {
  run_runner java single "$REPO/bindings/java/perf/run_benchmarks.sh" --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/java"
  run_runner java multi "$REPO/bindings/java/perf/run_benchmarks_multi.sh" --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/java"
}
node() {
  # The two node wrappers perform their own generated-tool synchronization.
  # run_runner enforces one total Node --help preflight for this driver run.
  run_runner node single "$REPO/bindings/node/perf/run_benchmarks.sh" --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/node"
  run_runner node multi "$REPO/bindings/node/perf/run_benchmarks_multi.sh" --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/node"
}
python() {
  run_runner python single "$REPO/bindings/python/perf/run_benchmarks.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/python"
  run_runner python multi "$REPO/bindings/python/perf/run_benchmarks_multi.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/python"
}
rust() {
  run_runner rust single "$REPO/bindings/rust/perf/run_benchmarks.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss,inproc,ipc --msg-sizes 1024 --duration 1 --runs 1 --results-dir "$RUN_ROOT/rust"
  run_runner rust multi "$REPO/bindings/rust/perf/run_benchmarks_multi.sh" --smoke --pattern ALL --transports tcp,tls,ws,wss --msg-sizes 1024 --clients 1 --duration 1 --runs 1 --results-dir "$RUN_ROOT/rust"
}

run_language c c
run_language cpp cpp
run_language dotnet dotnet
run_language go go
run_language java java
run_language node node
run_language python python
run_language rust rust

(( FINAL_STATUS == 0 )) || exit 1
printf 'Phase 7 smoke PASS; see %s\n' "$RESULTS"
