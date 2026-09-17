#!/usr/bin/env bash

# Single owner of "which JDK runs what Gradle built" for the POSIX runners.
#
# zlink-jvm-baseline.settings.gradle.kts (next to this file) pins the Gradle
# compile toolchain. The installDist start scripts pick their own runtime at
# launch time: JAVA_HOME if set, otherwise java on PATH. On a machine with more
# than one JDK the two disagree and every role process dies with
# UnsupportedClassVersionError inside its own log file (#517).
#
# So the runners call zlink_jvm_require_toolchain_runtime before they build or
# start anything: it points JAVA_HOME at the toolchain JDK, or fails with one
# message that names the missing version. The required version is read from the
# baseline settings file, never repeated here.
#
# The PowerShell sibling is Set-ZlinkSampleJavaRuntime in redis-common.ps1; both
# read the same constant and search the same places.

ZLINK_JVM_BASELINE_SETTINGS="${BASH_SOURCE[0]%/*}/zlink-jvm-baseline.settings.gradle.kts"

# Prints the Java language version the Gradle toolchain is pinned to.
zlink_jvm_toolchain_version() {
  local version
  version="$(sed -n 's/^val zlinkJavaLanguageVersion = \([0-9][0-9]*\)[[:space:]]*$/\1/p' \
    "${ZLINK_JVM_BASELINE_SETTINGS}" 2>/dev/null | head -n 1)"
  if [[ ! "${version}" =~ ^[0-9]+$ ]]; then
    echo "Java toolchain version is missing from ${ZLINK_JVM_BASELINE_SETTINGS}" >&2
    return 1
  fi
  printf '%s\n' "${version}"
}

# True when <java-home> is a JDK of exactly <major>. The release file is the
# only version source, so no JVM has to be started to answer this.
zlink_jvm_home_is_version() {
  local java_home="$1"
  local major="$2"
  [[ -n "${java_home}" && -f "${java_home}/release" ]] || return 1
  grep -Eq "^JAVA_VERSION=\"${major}([.\"])" "${java_home}/release"
}

# The JDK directories this machine can offer, most specific first: JAVA_HOME,
# the java on PATH, the installations Gradle was configured with, and the
# directories Gradle auto-detects.
zlink_jvm_candidate_homes() {
  local gradle_home="${HOME}/.gradle"
  local path_java resolved_java line entry root program_files
  local -a roots

  [[ -n "${JAVA_HOME:-}" ]] && zlink_jvm_shell_path "${JAVA_HOME}"

  path_java="$(command -v java 2>/dev/null || true)"
  if [[ -n "${path_java}" ]]; then
    resolved_java="$(readlink -f "${path_java}" 2>/dev/null || printf '%s' "${path_java}")"
    printf '%s\n' "${resolved_java%/bin/java}"
  fi

  if [[ -f "${gradle_home}/gradle.properties" ]]; then
    line="$(sed -n 's/^org\.gradle\.java\.installations\.paths=//p' \
      "${gradle_home}/gradle.properties" | head -n 1)"
    while IFS= read -r entry; do
      entry="${entry#"${entry%%[![:space:]]*}"}"
      entry="${entry%"${entry##*[![:space:]]}"}"
      [[ -n "${entry}" ]] || continue
      # Gradle escapes Windows separators in .properties files.
      entry="${entry//\\\\/\\}"
      zlink_jvm_shell_path "${entry}"
    done <<<"${line//,/$'\n'}"
  fi

  roots=(
    "${gradle_home}/jdks"
    "${HOME}/.sdkman/candidates/java"
    /usr/lib/jvm
    /usr/java
    /Library/Java/JavaVirtualMachines
  )
  if [[ -n "${LOCALAPPDATA:-}" ]]; then
    roots+=("$(zlink_jvm_shell_path "${LOCALAPPDATA}")/Programs/jdk")
  fi
  if [[ -n "${PROGRAMFILES:-}" ]]; then
    program_files="$(zlink_jvm_shell_path "${PROGRAMFILES}")"
    roots+=("${program_files}/Java" "${program_files}/Eclipse Adoptium" "${program_files}/Microsoft")
  fi
  for root in "${roots[@]}"; do
    [[ -d "${root}" ]] || continue
    for entry in "${root}"/*; do
      [[ -d "${entry}" ]] || continue
      printf '%s\n' "${entry}"
      # macOS bundles keep the JDK one level down.
      [[ -d "${entry}/Contents/Home" ]] && printf '%s\n' "${entry}/Contents/Home"
    done
  done
  return 0
}

# Converts a native path to the form this shell can open. Only Windows bash
# (MSYS/Cygwin) has a difference to convert.
zlink_jvm_shell_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s\n' "$1"
  fi
}

# The JAVA_HOME value a launcher started from this shell must see. Windows .bat
# launchers need a native path; forward slashes keep it usable from both.
zlink_jvm_launcher_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -m "$1"
  else
    printf '%s\n' "$1"
  fi
}

# Points JAVA_HOME and PATH at the JDK the Gradle toolchain compiles with, so
# the build and the installDist launchers it produces run on the same JDK.
zlink_jvm_require_toolchain_runtime() {
  local required candidate
  required="$(zlink_jvm_toolchain_version)" || return 1

  while IFS= read -r candidate; do
    if zlink_jvm_home_is_version "${candidate}" "${required}"; then
      JAVA_HOME="$(zlink_jvm_launcher_path "${candidate}")"
      export JAVA_HOME
      case ":${PATH}:" in
        *":${candidate}/bin:"*) ;;
        *) PATH="${candidate}/bin:${PATH}"; export PATH ;;
      esac
      return 0
    fi
  done < <(zlink_jvm_candidate_homes)

  echo "JDK ${required} was not found on this machine." >&2
  echo "Gradle compiles these projects with the Java ${required} toolchain pinned in" >&2
  echo "${ZLINK_JVM_BASELINE_SETTINGS}, so the installDist launchers need a JDK ${required} runtime." >&2
  echo "Install JDK ${required}, or set JAVA_HOME to an existing JDK ${required} installation." >&2
  return 1
}
