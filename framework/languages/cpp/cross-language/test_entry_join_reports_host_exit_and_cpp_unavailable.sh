#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE="${1:-${SMOKE:-${SCRIPT_DIR}/run_cross_language_smoke.sh}}"
FIXTURES="${SCRIPT_DIR}/fixtures/entry-join"
RUN_DIR="$(mktemp -d "${TMPDIR:-${PWD}}/entry-join-report.XXXXXX")"
trap 'rm -rf "${RUN_DIR}"' EXIT

# Load the wait functions without building hosts or starting Redis.
# shellcheck disable=SC1090
source <(sed -n '/^wait_for_ready() {$/,/^}$/p' "${SMOKE}")

entry_join_reports_host_exit() {
  local role=entry-join-cpp-node-target-restarted pid status=0 output
  local log="${RUN_DIR}/${role}.log" ready="${RUN_DIR}/${role}.ready"

  # The process has already exited by the time the harness checks readiness.
  (printf '%s\n' "Node User-Spot Join source failed: ZLinkMeshNodeDescriptorConflictException: MeshNode 'cross.entry-join' descriptor publication failed: rejectedConflict." >&2
   exit 1) >"${log}" 2>&1 &
  pid=$!
  while kill -0 "${pid}" 2>/dev/null; do command sleep 0.01; done
  if wait_for_ready "${ready}" 2 "${pid}" >"${RUN_DIR}/ready-output" 2>&1; then
    echo 'expected an error for exit before ready' >&2
    return 1
  fi
  output="$(cat "${RUN_DIR}/ready-output")"
  printf '%s\n' "${output}"
  [[ "${output}" == *"${role} exited before ready (status 1)"* ]] || status=1
  [[ "${output}" == *'rejectedConflict'* && "${output}" != *'timed out'* ]] || status=1

  # A ready file published before exit still satisfies the readiness check.
  touch "${ready}"
  wait_for_ready "${ready}" 2 "${pid}"
  return "${status}"
}

ready_status=0
entry_join_reports_host_exit || ready_status=$?
# shellcheck disable=SC1090
source <(sed -n '/^PIDS=()$/,/^cleanup() {$/p' "${SMOKE}" | sed '$d')
# shellcheck disable=SC1090
source <(sed -n '
  /^wait_for_line() {$/,/^}$/p
  /^assert_source_actor_create() {$/,/^}$/p
  /^entry_join_rid_args() {$/,/^}$/p
  /^wait_for_entry_join_line() {$/,/^}$/p
  /^stage_entry_join_cell() {$/,/^}$/p
' "${SMOKE}")

# Advance the Bash polling clock only if the stage fails to see the fixture.
sleep() { SECONDS=$((SECONDS + 1)); }
free_port() { echo '1 2 3'; }
start_redis() { :; }
entry_join_store_fence() {
  case "$3:$4" in
    actor:*-restart) echo "${actor_fence_fixture}";;
    actor:*) echo "6e6f6465 ${old_generation_fixture}";;
    descriptor:*) echo "6e6f6465 ${new_generation_fixture}";;
  esac
}

start_outage_host() {
  local name="$2" events suffix
  shift 3
  while [[ "$1" != --event-file ]]; do shift; done
  events="$2"
  suffix="${events#"${RUN_DIR}/${run_id}."}"
  cp "${fixture_dir}/${fixture_id}.${suffix}" "${events}"
  if [[ -n "${terminal_fixture}" && "${suffix}" == target-restarted.events ]]; then
    printf '%s\n' "${terminal_fixture}" >>"${RUN_DIR}/${run_id}.source.events"
  fi
  if [[ "${case_name}" == exit && "${suffix}" == target-restarted.events ]]; then
    (exit 1) >"${RUN_DIR}/${name}.log" 2>&1 &
    PIDS+=("$!")
    cp "${fixture_dir}/${fixture_id}-target-restarted.txt" "${RUN_DIR}/${name}.log"
    while kill -0 "${PIDS[-1]}" 2>/dev/null; do command sleep 0.01; done
  else
    command sleep 30 &
    PIDS+=("$!")
    touch "${RUN_DIR}/${name}.ready"
  fi
}

replay() (
  set -e
  local case_name="$1" fixture_dir="$2" source="$3" target="$4"
  local fixture_id="entry-join-${source}-${target}"
  local actor_fence_fixture="${5:-6e6f6465 ${new_generation_fixture}}"
  local terminal_fixture="${6:-}"
  RUN_DIR="$(mktemp -d "${RUN_DIR}/${case_name}.XXXXXX")"
  trap 'stop_all; rm -rf "${RUN_DIR}"' EXIT
  stage_entry_join_cell "${source}" "${target}"
  printf 'ok - %s\n' "${RESULTS[@]}"
)

expect_failure() {
  local output="$1" expected="$2"
  shift 2
  # Keep errexit active inside the stage so the first failed wait decides.
  set +e
  (set -e; replay "$@") >"${output}" 2>&1
  local status=$?
  set -e
  sed -n '1,4p' "${output}"
  if [[ "${status}" -ne 1 ]] || ! grep -qF "${expected}" "${output}"; then
    failures=1
  fi
}

expected_success="$(sed -n '/^ok - remote Entry Spot join: node source -> node target /p' "${FIXTURES}/success.txt")"
generations='joined at generation ([0-9]+) \(was ([0-9]+)\)'
[[ "${expected_success}" =~ ${generations} ]]
new_generation_fixture="${BASH_REMATCH[1]}"
old_generation_fixture="${BASH_REMATCH[2]}"

failures="${ready_status}"
expect_failure "${RUN_DIR}/initial" 'first join: Unavailable' initial "${FIXTURES}" cpp cpp
expect_failure "${RUN_DIR}/restart" 'entry-join-cpp-node-target-restarted exited before ready (status 1)' exit "${FIXTURES}" cpp node
grep -qF rejectedConflict "${RUN_DIR}/restart" || failures=1
actual_success="$(replay success "${FIXTURES}" node node)"
printf '%s\n' "${actual_success}"
[[ "${actual_success##*$'\n'}" == "${expected_success}" ]] || failures=1
expect_failure "${RUN_DIR}/stale-fence" 'expected restarted fence' stale-fence "${FIXTURES}" node node "6e6f6465 ${old_generation_fixture}"
# Reuse the recorded terminal for the second Actor and omit its success event.
restart_unavailable_fixture="${RUN_DIR}/restart-unavailable-fixture"
mkdir "${restart_unavailable_fixture}"
cp "${FIXTURES}"/entry-join-node-node.{source,target,target-restarted}.events "${restart_unavailable_fixture}"
restart_terminal="$(sed -n 's/^user-spot-join-failed|kind=unavailable|actor=cross-lang-entry-join-actor$/&-restart/p' \
  "${FIXTURES}/entry-join-cpp-cpp.source.events")"
sed '/^entry-spot-joined|/d' "${FIXTURES}/entry-join-node-node.target-restarted.events" \
  >"${restart_unavailable_fixture}/entry-join-node-node.target-restarted.events"
for terminal in "${restart_terminal}" "${restart_terminal/|kind=unavailable|/|kind=Unavailable|}"; do
  expect_failure "${RUN_DIR}/restart-unavailable" 'restart join: Unavailable without evidence of zero binding submissions' \
    restart-unavailable "${restart_unavailable_fixture}" node node '' "${terminal}"
done
if ((failures == 0)); then
  echo 'entry_join_reports_host_exit_and_cpp_unavailable: passed'
fi
exit "${failures}"
