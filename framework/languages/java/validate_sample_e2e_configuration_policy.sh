#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

readonly SOURCE_ROOTS=(
  samples/java
  samples/kotlin
  e2e
  e2e-kotlin
)

failures=0

if rg -n 'System\.(getenv|getProperty)\s*\(' "${SOURCE_ROOTS[@]}" --glob '*.{java,kt}'; then
  echo "sample/E2E application code must not read environment variables or JVM system properties" >&2
  failures=1
fi

if rg -n 'ZLINK_(JAVA|KOTLIN)_(SAMPLE|E2E)_[A-Z0-9_]+' \
    "${SOURCE_ROOTS[@]}" --glob '*.{java,kt}'; then
  echo "sample/E2E application source must not define an environment-variable configuration interface" >&2
  failures=1
fi

if (( failures != 0 )); then
  exit 1
fi

echo "Java/Kotlin sample/E2E configuration policy gate passed"
