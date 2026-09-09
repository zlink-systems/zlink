#!/usr/bin/env bash

# Java binding perf uses the Java target from the shared version catalog. installDist launchers resolve
# JAVA_HOME (or java on PATH) when they start, independently of Gradle's
# compile toolchain. Validate that runtime before a measurement begins.
require_java25() {
  local required_java
  required_java="$(sed -n 's/^java = "\([0-9]*\)"$/\1/p' "$(dirname "${BASH_SOURCE[0]}")/../gradle/libs.versions.toml")"
  if [[ ! "${required_java}" =~ ^[0-9]+$ ]]; then
    echo "Java target is missing from gradle/libs.versions.toml." >&2
    return 1
  fi
  local java_cmd
  local java_version_line
  local java_major

  if [[ -n "${JAVA_HOME:-}" ]]; then
    java_cmd="${JAVA_HOME}/bin/java"
  else
    java_cmd="$(command -v java || true)"
  fi
  if [[ ! -x "${java_cmd}" ]]; then
    echo "Java ${required_java} runtime not found. Set JAVA_HOME to a JDK ${required_java} installation." >&2
    return 1
  fi

  java_version_line="$("${java_cmd}" -version 2>&1 | sed -n '1p')"
  if [[ "${java_version_line}" =~ \"([0-9]+)\. ]]; then
    java_major="${BASH_REMATCH[1]}"
  else
    echo "Unable to determine Java runtime version: ${java_version_line}" >&2
    return 1
  fi
  if (( java_major < required_java )); then
    echo "Java perf requires Java ${required_java} or newer (found: ${java_version_line})." >&2
    echo "Set JAVA_HOME to the JDK used to build the Java binding." >&2
    return 1
  fi
}
