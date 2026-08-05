#!/usr/bin/env bash
set -euo pipefail

java_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_roots=("${java_root}/samples/java" "${java_root}/e2e")

if rg -n 'System\.(getenv|getProperty)\s*\(' "${source_roots[@]}" \
    --glob '*.java' --glob '!**/build/**'; then
  echo "Java sample/E2E application code must not read global configuration" >&2
  exit 1
fi

if rg -n 'environmentVariable\s*\(' "${source_roots[@]}" \
    --glob '*.gradle' --glob '*.gradle.kts' --glob '!**/build/**'; then
  echo "Java sample/E2E builds must not provide application configuration from the environment" >&2
  exit 1
fi

custom_runner_environment="$({
  rg -n '\$\{ZLINK_[A-Z0-9_]+' "${source_roots[@]}" \
    --glob '*.sh' --glob '!**/build/**' || true
} | rg -v 'ZLINK_LIBRARY_PATH' || true)"
if [[ -n "${custom_runner_environment}" ]]; then
  printf '%s\n' "${custom_runner_environment}"
  echo "Java sample/E2E runners must use options or repository settings, not custom environment interfaces" >&2
  exit 1
fi

echo "java sample/E2E configuration policy gate passed"
