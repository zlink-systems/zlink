#!/usr/bin/env bash

# Java binding perf uses the Java 22 FFM API. installDist launchers resolve
# JAVA_HOME (or java on PATH) when they start, independently of Gradle's
# compile toolchain. Validate that runtime before a measurement begins.
require_java22() {
  local java_cmd
  local java_version_line
  local java_major

  if [[ -n "${JAVA_HOME:-}" ]]; then
    java_cmd="${JAVA_HOME}/bin/java"
  else
    java_cmd="$(command -v java || true)"
  fi
  if [[ ! -x "${java_cmd}" ]]; then
    echo "Java 22 runtime not found. Set JAVA_HOME to a JDK 22 installation." >&2
    return 1
  fi

  java_version_line="$("${java_cmd}" -version 2>&1 | sed -n '1p')"
  if [[ "${java_version_line}" =~ \"([0-9]+)\. ]]; then
    java_major="${BASH_REMATCH[1]}"
  else
    echo "Unable to determine Java runtime version: ${java_version_line}" >&2
    return 1
  fi
  if (( java_major < 22 )); then
    echo "Java perf requires Java 22 or newer (found: ${java_version_line})." >&2
    echo "Set JAVA_HOME to the JDK used to build the Java binding." >&2
    return 1
  fi
}
