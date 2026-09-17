#!/usr/bin/env bash
#  Compiles every Java and Kotlin e2e scenario.
#
#  Each scenario under e2e/ and e2e-kotlin/ is its own Gradle build, so neither
#  the root build nor the samples composite ever compiles them. A public API
#  change therefore leaves the scenario sources broken with no build saying so
#  until someone runs the scenario (issue #519; #515 was the same hole in the
#  samples). Compiling is enough to catch that, and it needs no Redis, no ports
#  and no Core runtime, so it runs wherever the unit tests run.
#
#  Extra Gradle arguments pass through: scripts/compile_e2e.sh --max-workers=2
set -euo pipefail

#  Scenarios that do not compile for reasons this guard does not own, tracked by
#  issue #524. The guard also fails when one of these compiles, so the list
#  cannot quietly rot: fixing a scenario forces its removal from here.
known_broken=(
  "e2e-kotlin/DiscoveryRegistryHa"
  "e2e-kotlin/SubmitAdmission"
  "e2e-kotlin/ToActorMessaging"
)

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
java_root="$(cd "${script_dir}/.." && pwd)"
cd "${java_root}"

is_known_broken() {
  local candidate="$1" entry
  for entry in "${known_broken[@]}"; do
    [[ "${entry}" == "${candidate}" ]] && return 0
  done
  return 1
}

scenarios=()
for settings in e2e/*/settings.gradle.kts e2e-kotlin/*/settings.gradle.kts; do
  [[ -f "${settings}" ]] || continue
  scenarios+=("$(dirname "${settings}")")
done

if (( ${#scenarios[@]} == 0 )); then
  echo "No e2e scenario build found under ${java_root}" >&2
  exit 1
fi

for entry in "${known_broken[@]}"; do
  if [[ ! -f "${entry}/settings.gradle.kts" ]]; then
    echo "known_broken lists ${entry}, which is not an e2e scenario build" >&2
    exit 1
  fi
done

failed=()
unexpectedly_green=()
for scenario in "${scenarios[@]}"; do
  echo "===== compiling ${scenario}"
  if ./gradlew "$@" -p "${scenario}" classes; then
    if is_known_broken "${scenario}"; then
      unexpectedly_green+=("${scenario}")
    fi
  elif ! is_known_broken "${scenario}"; then
    failed+=("${scenario}")
  fi
done

status=0
if (( ${#failed[@]} > 0 )); then
  echo "e2e-compile=failed scenarios=${failed[*]}" >&2
  status=1
fi
if (( ${#unexpectedly_green[@]} > 0 )); then
  echo "e2e-compile=stale-quarantine scenarios=${unexpectedly_green[*]}" >&2
  echo "Remove them from known_broken in ${BASH_SOURCE[0]} and close their entry in issue #524." >&2
  status=1
fi
(( status == 0 )) || exit "${status}"

echo "e2e-compile=completed scenarios=${#scenarios[@]} quarantined=${#known_broken[@]}"
