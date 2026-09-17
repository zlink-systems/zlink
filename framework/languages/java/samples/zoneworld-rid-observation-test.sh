#!/usr/bin/env bash

# Regression for #545. ZW-G1, ZW-G2-rid and ZW-G3 judge a ZoneNode by its transport RID,
# and the ZoneWorld runners used to look for that RID in the ZoneNode's own log. A ZoneNode
# never prints it there: the RID reaches an observer only through the Ops node status report
# ("node status observed. node=<id>, rid=<rid>"), which the ops role alone writes. The
# runners therefore read the empty string for every node and the three scenarios failed on
# every platform and in both languages.
#
# The bash runners are exercised for real: wait_log, routing_id and is_zone_rid are lifted
# out of the shipped run_sample.sh and run against fixture logs. The PowerShell runners are
# checked for the same observation source, because Windows runs Get-RoutingId instead.
#
# Both directions are asserted. A canonical RID in ops.log is observed and returned, and the
# four inputs that must not pass are each refused: a node Ops never observed, the ZoneNode's
# own log (the pre-fix source, whose only RID is the source_rid= trace field), a
# non-canonical RID, and a replacement whose RID did not change.
#
# Run: bash framework/languages/java/samples/zoneworld-rid-observation-test.sh [samples-dir]

set -uo pipefail

SAMPLES_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
TEST_ROOT="$(mktemp -d)"
PASS=0
FAILED=0

cleanup() {
  rm -rf -- "${TEST_ROOT}"
}
trap cleanup EXIT

pass() {
  PASS=$((PASS + 1))
  printf 'ok %d - %s\n' "${PASS}" "$1"
}

fail() {
  FAILED=$((FAILED + 1))
  printf 'FAIL - %s\n' "$1" >&2
}

check() {
  local description="$1"
  local expected="$2"
  local actual="$3"
  if [[ "${expected}" == "${actual}" ]]; then
    pass "${description}"
  else
    fail "${description}: expected [${expected}], got [${actual}]"
  fi
}

check_contains() {
  local description="$1"
  local haystack="$2"
  local needle="$3"
  if [[ "${haystack}" == *"${needle}"* ]]; then
    pass "${description}"
  else
    fail "${description}: [${needle}] is missing"
  fi
}

check_absent() {
  local description="$1"
  local haystack="$2"
  local needle="$3"
  if [[ "${haystack}" != *"${needle}"* ]]; then
    pass "${description}"
  else
    fail "${description}: [${needle}] is still present"
  fi
}

# Canonical zn-UUIDv4 values. NODE2_NEW is what a graceful replacement of zone-node-2
# publishes under the same NodeId (ZW-G3), and BAD is the shape 9.3 refuses - a v1 UUID.
NODE1_RID="zn-be739ca6-0f12-4b45-8e58-b890b415f612"
NODE2_RID="zn-dfcf9a5b-1ce4-4b19-b038-dd20a8826301"
NODE2_NEW_RID="zn-4c1a9f77-2b05-4d61-9a3c-1f0e8b6d4a25"
NONCANONICAL_RID="zn-dfcf9a5b-1ce4-1b19-b038-dd20a8826301"

status_line() {
  printf 'node status observed. node=%s, rid=%s, registered=true, connected=true\n' "$1" "$2"
}

# The ZoneNode's own stdout. Its only RID is the message-flow trace field source_rid=, which
# is what the pre-fix runners were reading past; the replacement node emits no flow line at
# all because it comes up carrying no traffic.
trace_line() {
  printf 'INFO s.z.f.r.d.ZLinkMessageFlowTracer : zlink flow: event_id=zlink.message_flow phase=sent surface=spot kind=send source_rid=%s packet=UpdatePositionMsg spot=zone-se outcome=succeeded\n' "$1"
}

make_logs() {
  local dir="${TEST_ROOT}/$1"
  mkdir -p "${dir}"
  {
    status_line zone-node-1 "${NODE1_RID}"
    status_line zone-node-2 "${NODE2_RID}"
    status_line zone-node-1 "${NODE1_RID}"
    status_line zone-node-4 "${NONCANONICAL_RID}"
  } >"${dir}/ops.log"
  {
    trace_line "${NODE1_RID}"
    printf 'topology=ready node=zone-node-1 zones=zone-nw,zone-sw\n'
    printf 'node status report submitted. node=zone-node-1\n'
  } >"${dir}/zone-node-1.log"
  {
    trace_line "${NODE2_RID}"
    printf 'topology=ready node=zone-node-2 zones=zone-ne,zone-se\n'
  } >"${dir}/zone-node-2.log"
  printf 'topology=ready node=zone-node-2 zones=\n' >"${dir}/zone-node-replacement.log"
  echo "${dir}"
}

# Runs the shipped runner helpers, not a copy of them: wait_log, routing_id and is_zone_rid
# are lifted verbatim out of run_sample.sh and evaluated against the fixture log directory.
extract_helpers() {
  local runner="$1"
  sed -n '/^wait_log() {$/,/^}$/p;/^routing_id() {$/,/^}$/p;/^is_zone_rid() /p' "${runner}"
}

run_helper() {
  local runner="$1" log_dir="$2"
  shift 2
  local helpers
  helpers="$(extract_helpers "${runner}")"
  LOG_DIR="${log_dir}" bash -c "
set -uo pipefail
${helpers}
$*
" 2>/dev/null
}

for language in java kotlin; do
  runner="${SAMPLES_DIR}/${language}/ZoneWorld/run_sample.sh"
  if [[ ! -f "${runner}" ]]; then
    fail "${language}: ${runner} is missing"
    continue
  fi
  helpers="$(extract_helpers "${runner}")"
  if [[ "${helpers}" != *"routing_id()"* || "${helpers}" != *"wait_log()"* \
      || "${helpers}" != *"is_zone_rid()"* ]]; then
    fail "${language}: run_sample.sh no longer defines wait_log/routing_id/is_zone_rid"
    continue
  fi

  logs="$(make_logs "${language}")"

  # Positive: each ZoneNode's RID comes back from the Ops report, canonical and distinct.
  rid1="$(run_helper "${runner}" "${logs}" 'routing_id zone-node-1 1 1')"
  rid2="$(run_helper "${runner}" "${logs}" 'routing_id zone-node-2 1 1')"
  check "${language}: zone-node-1 RID read from the Ops report" "${NODE1_RID}" "${rid1}"
  check "${language}: zone-node-2 RID read from the Ops report" "${NODE2_RID}" "${rid2}"
  check "${language}: ZW-G1 accepts two distinct canonical RIDs" "accepted" \
    "$(run_helper "${runner}" "${logs}" \
      "if is_zone_rid ${rid1} && is_zone_rid ${rid2} && [[ ${rid1} != ${rid2} ]]; then echo accepted; else echo refused; fi")"

  # Positive: ZW-G3 reads the replacement's RID from the Ops report that arrives after the
  # stop, under the same NodeId, and sees that it changed.
  ops_first=$(( $(wc -l <"${logs}/ops.log") + 1 ))
  status_line zone-node-2 "${NODE2_NEW_RID}" >>"${logs}/ops.log"
  replacement="$(run_helper "${runner}" "${logs}" "routing_id zone-node-2 ${ops_first} 1")"
  check "${language}: ZW-G3 reads the replacement RID after the stop" \
    "${NODE2_NEW_RID}" "${replacement}"
  check "${language}: ZW-G3 accepts a changed canonical replacement RID" "accepted" \
    "$(run_helper "${runner}" "${logs}" \
      "if is_zone_rid ${replacement} && [[ ${replacement} != ${NODE2_RID} ]]; then echo accepted; else echo refused; fi")"

  # Negative: a node Ops never observed yields nothing, and the verdict refuses it. This is
  # the exact shape the defect produced for every node.
  missing="$(run_helper "${runner}" "${logs}" 'routing_id zone-node-3 1 1')"
  check "${language}: an unobserved node yields no RID" "" "${missing}"
  check "${language}: ZW-G2-rid refuses an unobserved node" "refused" \
    "$(run_helper "${runner}" "${logs}" \
      "if is_zone_rid '${missing}'; then echo accepted; else echo refused; fi")"

  # Negative: the ZoneNode's own log is not an observation source. Presented as ops.log, its
  # source_rid= trace field must still yield nothing.
  wrong_source="${TEST_ROOT}/${language}-wrong-source"
  mkdir -p "${wrong_source}"
  cp "${logs}/zone-node-1.log" "${wrong_source}/ops.log"
  check "${language}: a ZoneNode's own log carries no observable RID" "" \
    "$(run_helper "${runner}" "${wrong_source}" 'routing_id zone-node-1 1 1')"

  # Negative: a non-canonical RID is read but refused by the 9.3 format gate.
  bad="$(run_helper "${runner}" "${logs}" 'routing_id zone-node-4 1 1')"
  check "${language}: a non-canonical RID is still read from the report" \
    "${NONCANONICAL_RID}" "${bad}"
  check "${language}: ZW-G1 refuses a non-canonical RID" "refused" \
    "$(run_helper "${runner}" "${logs}" \
      "if is_zone_rid ${bad}; then echo accepted; else echo refused; fi")"

  # Negative: a replacement that reports the previous incarnation's RID is refused, so the
  # observation cannot be satisfied by a stale report from the node that was stopped.
  stale="${TEST_ROOT}/${language}-stale-replacement"
  mkdir -p "${stale}"
  cp "${logs}/zone-node-1.log" "${stale}/zone-node-1.log"
  {
    status_line zone-node-1 "${NODE1_RID}"
    status_line zone-node-2 "${NODE2_RID}"
  } >"${stale}/ops.log"
  stale_first=$(( $(wc -l <"${stale}/ops.log") + 1 ))
  status_line zone-node-2 "${NODE2_RID}" >>"${stale}/ops.log"
  stale_rid="$(run_helper "${runner}" "${stale}" "routing_id zone-node-2 ${stale_first} 1")"
  check "${language}: a stale report yields the previous RID" "${NODE2_RID}" "${stale_rid}"
  check "${language}: ZW-G3 refuses an unchanged replacement RID" "refused" \
    "$(run_helper "${runner}" "${stale}" \
      "if is_zone_rid ${stale_rid} && [[ ${stale_rid} != ${NODE2_RID} ]]; then echo accepted; else echo refused; fi")"

  # The Windows lane runs Get-RoutingId, so it has to observe the same source.
  ps_runner="${SAMPLES_DIR}/${language}/ZoneWorld/run_sample.ps1"
  if [[ ! -f "${ps_runner}" ]]; then
    fail "${language}: ${ps_runner} is missing"
    continue
  fi
  ps_text="$(cat "${ps_runner}")"
  check_contains "${language}: Get-RoutingId reads ops.log" "${ps_text}" \
    'Get-CurrentLogPath "ops"'
  check_contains "${language}: Get-RoutingId matches the Ops report" "${ps_text}" \
    'node status observed\. node=$([regex]::Escape($NodeId)), rid=(zn-[0-9a-f-]+)'
  check_absent "${language}: Get-RoutingId no longer scans a ZoneNode log" "${ps_text}" \
    "'\\brid=(zn-[0-9a-f-]+)\\b'"
  check_absent "${language}: no caller asks for a zone-node-replacement RID" "${ps_text}" \
    'Get-RoutingId "zone-node-replacement"'
done

printf '\n%d passed, %d failed\n' "${PASS}" "${FAILED}"
[[ "${FAILED}" -eq 0 ]]
