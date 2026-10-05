#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_DIR="$(mktemp -d)"
trap 'rm -rf "${RUN_DIR}"' EXIT

# Load the cleanup owner without launching smoke roles.
# shellcheck disable=SC1090
source <(sed -n '/^PIDS=()/,/^cleanup() {/p' \
  "${SCRIPT_DIR}/run_cross_language_smoke.sh" | sed '$d')

kill() { printf 'kill %s\n' "$*" >>"${RUN_DIR}/actual"; }
wait() { printf 'wait %s\n' "$*" >>"${RUN_DIR}/actual"; }
docker() { printf 'docker %s\n' "$*" >>"${RUN_DIR}/actual"; }

PIDS=(role-a role-b)
REDIS_PIDS=(store-a store-b)
REDIS_CONTAINERS=(store-container)
stop_all
cat >"${RUN_DIR}/expected" <<'EXPECTED'
kill role-a
kill role-b
wait role-a
wait role-b
docker rm -f store-container
kill store-a
kill store-b
wait store-a
wait store-b
EXPECTED
diff -u "${RUN_DIR}/expected" "${RUN_DIR}/actual"
[[ ${#PIDS[@]} == 0 && ${#REDIS_PIDS[@]} == 0 && ${#REDIS_CONTAINERS[@]} == 0 ]]

# Repeated cleanup must not signal empty or already reaped PID entries.
stop_all
diff -u "${RUN_DIR}/expected" "${RUN_DIR}/actual"
echo 'stop_all ordering and repeated cleanup: passed'
