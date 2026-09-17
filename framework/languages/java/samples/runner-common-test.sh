#!/usr/bin/env bash

# Tests the two runner mechanisms that only show up on a developer machine:
# picking the JDK the Gradle toolchain compiles with (gradle/zlink-jvm-runtime.sh)
# and staging standalone Gradle settings (runner-common.sh). CI has a single JDK
# and never interrupts a run, so neither is exercised there (#517).
#
# Both directions are asserted: a fixture JDK that satisfies the pinned version
# is selected, and one that does not is refused with the message that names it.
#
# Run: bash framework/languages/java/samples/runner-common-test.sh

set -uo pipefail

SAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
    fail "${description}: [${needle}] is missing from [${haystack}]"
  fi
}

# A JDK layout is a directory with a release file; 77 is a version no real
# installation can have, so only these fixtures can satisfy the fixture baseline.
make_jdk() {
  local home="${TEST_ROOT}/$1"
  local version="$2"
  mkdir -p "${home}/bin"
  printf 'JAVA_VERSION="%s"\nOS_ARCH="x86_64"\n' "${version}" >"${home}/release"
  printf '#!/bin/sh\necho "%s"\n' "${version}" >"${home}/bin/java"
  chmod +x "${home}/bin/java"
  printf '%s\n' "${home}"
}

JDK_TOOLCHAIN="$(make_jdk jdk-77 '77.0.1')"
JDK_OLD="$(make_jdk jdk-22 '22.0.2')"
JDK_NEAR="$(make_jdk jdk-770 '770.1')"

BASELINE_FIXTURE="${TEST_ROOT}/baseline.settings.gradle.kts"
printf 'val zlinkJavaLanguageVersion = 77\n' >"${BASELINE_FIXTURE}"
BASELINE_WITHOUT_VERSION="${TEST_ROOT}/no-version.settings.gradle.kts"
printf 'rootProject.name = "x"\n' >"${BASELINE_WITHOUT_VERSION}"

gradle_properties_home() {
  local home="${TEST_ROOT}/$1"
  shift
  mkdir -p "${home}/.gradle"
  local joined=""
  local path
  for path in "$@"; do
    joined="${joined:+${joined},}${path}"
  done
  printf 'org.gradle.java.installations.paths=%s\n' "${joined}" \
    >"${home}/.gradle/gradle.properties"
  printf '%s\n' "${home}"
}

# --- the version pinned by the real baseline is readable ---------------------

(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  version="$(zlink_jvm_toolchain_version)"
  [[ "${version}" =~ ^[0-9]+$ ]] || exit 1
)
check 'the real baseline settings still expose the toolchain version' '0' "$?"

# --- version matching, both directions --------------------------------------

(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  zlink_jvm_home_is_version "${JDK_TOOLCHAIN}" 77
)
check 'a JDK 77 release file satisfies version 77' '0' "$?"

(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  zlink_jvm_home_is_version "${JDK_NEAR}" 77
)
check 'a JDK 770 release file does not satisfy version 77' '1' "$?"

(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  zlink_jvm_home_is_version "${TEST_ROOT}/absent" 77
)
check 'a directory without a release file satisfies nothing' '1' "$?"

# --- selection: JAVA_HOME already holds the toolchain JDK -------------------

selected="$(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  ZLINK_JVM_BASELINE_SETTINGS="${BASELINE_FIXTURE}"
  HOME="$(gradle_properties_home home-empty)"
  JAVA_HOME="${JDK_TOOLCHAIN}"
  PATH="${JDK_TOOLCHAIN}/bin:${PATH}"
  zlink_jvm_require_toolchain_runtime >/dev/null 2>&1 || exit 1
  # JAVA_HOME is handed to launchers in native form; compare it in shell form.
  zlink_jvm_shell_path "${JAVA_HOME}"
)"
check 'a matching JAVA_HOME is kept' "${JDK_TOOLCHAIN}" "${selected}"

# --- selection: the #517 machine, default java older than the toolchain -----

selected="$(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  ZLINK_JVM_BASELINE_SETTINGS="${BASELINE_FIXTURE}"
  HOME="$(gradle_properties_home home-configured "${JDK_TOOLCHAIN}")"
  unset JAVA_HOME
  PATH="${JDK_OLD}/bin:${PATH}"
  zlink_jvm_require_toolchain_runtime >/dev/null 2>&1 || exit 1
  zlink_jvm_shell_path "${JAVA_HOME}"
)"
check 'an older java on PATH is replaced by the toolchain JDK' "${JDK_TOOLCHAIN}" "${selected}"

first_on_path="$(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  ZLINK_JVM_BASELINE_SETTINGS="${BASELINE_FIXTURE}"
  HOME="$(gradle_properties_home home-configured2 "${JDK_TOOLCHAIN}")"
  unset JAVA_HOME
  PATH="${JDK_OLD}/bin:${PATH}"
  zlink_jvm_require_toolchain_runtime >/dev/null 2>&1 || exit 1
  printf '%s\n' "${PATH%%:*}"
)"
check 'the toolchain JDK also leads PATH' "${JDK_TOOLCHAIN}/bin" "${first_on_path}"

# --- refusal: no installation of the pinned version -------------------------

message="$(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  ZLINK_JVM_BASELINE_SETTINGS="${BASELINE_FIXTURE}"
  HOME="$(gradle_properties_home home-old "${JDK_OLD}" "${JDK_NEAR}")"
  JAVA_HOME="${JDK_OLD}"
  PATH="${JDK_OLD}/bin:${PATH}"
  zlink_jvm_require_toolchain_runtime 2>&1 >/dev/null
  printf 'status=%s\n' "$?"
)"
check_contains 'a machine without the pinned JDK fails' "${message}" 'JDK 77 was not found'
check_contains 'the refusal names the file that pins the version' "${message}" 'baseline.settings.gradle.kts'
check_contains 'the refusal reports failure' "${message}" 'status=1'

message="$(
  source "${SAMPLES_DIR}/gradle/zlink-jvm-runtime.sh"
  ZLINK_JVM_BASELINE_SETTINGS="${BASELINE_WITHOUT_VERSION}"
  zlink_jvm_require_toolchain_runtime 2>&1 >/dev/null
  printf 'status=%s\n' "$?"
)"
check_contains 'a baseline without the version constant fails' "${message}" \
  'Java toolchain version is missing'
check_contains 'the missing constant reports failure' "${message}" 'status=1'

# --- the runners ask the owner before they start a JVM artifact -------------
# The unit checks above cover the decision; these two cover the wiring, which
# only an end-to-end run would otherwise catch.

check 'the sample runners ask for the toolchain JDK' 'asks' \
  "$(grep -q 'zlink_jvm_require_toolchain_runtime' "${SAMPLES_DIR}/runner-common.sh" \
    && echo asks || echo missing)"

SMOKE_RUNNER="${SAMPLES_DIR}/../../cpp/cross-language/run_cross_language_smoke.sh"
check 'the cross-language smoke loads the same owner' 'loads' \
  "$(grep -q 'samples/gradle/zlink-jvm-runtime.sh' "${SMOKE_RUNNER}" \
    && echo loads || echo missing)"
check 'the cross-language smoke asks before it starts the Java host' 'asks' \
  "$(awk '/^start_java\(\)/,/^}/' "${SMOKE_RUNNER}" \
    | grep -q 'zlink_jvm_require_toolchain_runtime' && echo asks || echo missing)"

# --- standalone settings staging, both directions ---------------------------

if ! command -v flock >/dev/null 2>&1; then
  printf 'skip - standalone settings staging needs flock\n'
else
  STAGING_DIR="${TEST_ROOT}/staging"
  mkdir -p "${STAGING_DIR}"
  printf 'rootProject.name = "zlink-standalone-fixture"\n' \
    >"${STAGING_DIR}/standalone.settings.gradle.kts"

  stage() (
    cd "${STAGING_DIR}" || exit 1
    source "${SAMPLES_DIR}/runner-common.sh" || exit 1
    zlink_sample_gradle_standalone standalone.settings.gradle.kts true
  )

  stage >/dev/null 2>&1
  check 'staging succeeds on a clean sample directory' '0' "$?"
  check 'staging removes its own copy' '1' \
    "$(test -e "${STAGING_DIR}/settings.gradle.kts"; echo $?)"

  # An interrupted run leaves the staged copy behind.
  cp -- "${STAGING_DIR}/standalone.settings.gradle.kts" \
    "${STAGING_DIR}/settings.gradle.kts"
  staging_message="$(stage 2>&1 >/dev/null)"
  check 'staging takes over the copy an interrupted run left' '0' "$?"
  check_contains 'the takeover is reported' "${staging_message}" \
    'left by an interrupted run'
  check 'the taken-over copy is removed too' '1' \
    "$(test -e "${STAGING_DIR}/settings.gradle.kts"; echo $?)"

  # A settings file a developer wrote is never replaced.
  printf 'rootProject.name = "my-own-root"\n' >"${STAGING_DIR}/settings.gradle.kts"
  staging_message="$(stage 2>&1 >/dev/null)"
  check 'staging refuses a settings file it did not write' '1' "$?"
  check_contains 'the refusal names the file' "${staging_message}" \
    'Refusing to replace existing settings.gradle.kts'
  check 'the developer settings file survives' 'rootProject.name = "my-own-root"' \
    "$(cat "${STAGING_DIR}/settings.gradle.kts")"
  rm -f -- "${STAGING_DIR}/settings.gradle.kts"
fi

if ((FAILED > 0)); then
  printf '%d check(s) failed\n' "${FAILED}" >&2
  exit 1
fi
printf 'all %d checks passed\n' "${PASS}"
