#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAMPLES_DIR="$ROOT_DIR/samples"
CORE_LIB="$ROOT_DIR/../../../core/build/lib/libzlink.so"
MANIFEST="$SAMPLES_DIR/sample-manifest.env"

if [[ -f "$CORE_LIB" ]]; then
  export ZLINK_LIBRARY_PATH="$CORE_LIB"
fi

cd "$ROOT_DIR"
source "$MANIFEST"
source "$SAMPLES_DIR/runner-common.sh"

SAMPLE_FILTER="${ZLINK_SAMPLE_FILTER:-}"
SAMPLE_LANGUAGES="${ZLINK_SAMPLE_LANGUAGES:-java kotlin}"
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"

should_run_language() {
  local language="$1"
  [[ " ${SAMPLE_LANGUAGES} " == *" ${language} "* ]]
}

should_run_sample() {
  local language="$1"
  local sample="$2"
  shift 2
  if [[ "$#" -eq 0 && -z "$SAMPLE_FILTER" ]]; then
    return 0
  fi
  if [[ -n "$SAMPLE_FILTER" && ( "$SAMPLE_FILTER" == "$sample" || "$SAMPLE_FILTER" == "$language/$sample" ) ]]; then
    return 0
  fi
  local selector
  for selector in "$@"; do
    [[ "$selector" == "$sample" || "$selector" == "$language/$sample" ]] && return 0
  done
  return 1
}

./gradlew --no-daemon --no-parallel \
  :zlink-framework-testkit:contractTest \
  --tests '*SampleReleaseGateContractTest*'

./gradlew --no-daemon --no-parallel \
  :zlink-framework-testkit:fakeBackendTest \
  --tests 'systems.zlink.framework.testkit.CurrentManagerFakeBackendTest.actorAndSpotManagersExposeCurrentFluentCallsAgainstFakeBackend'
echo "java fake-backend public-manager gate completed"

run_sample_with_retry() {
  local script="$1"
  local attempt
  local output
  output="$(mktemp)"
  for attempt in 1 2 3; do
    if "$script" >"$output" 2>&1; then
      cat "$output"
      rm -f "$output"
      return 0
    fi
    if ! rg -q "${BIND_RETRY_PATTERN}" "$output"; then
      cat "$output" >&2
      rm -f "$output"
      return 1
    fi
    if [[ "$attempt" == "3" ]]; then
      cat "$output" >&2
      rm -f "$output"
      return 1
    fi
    echo "sample transient port bind failure; retrying ${script} (${attempt}/3)" >&2
  done
}

if should_run_language java; then
for sample in $JAVA_SAMPLES; do
  if should_run_sample java "$sample" "$@"; then
    run_sample_with_retry "$SAMPLES_DIR/java/$sample/run_sample.sh"
  fi
done
fi

if should_run_language kotlin; then
for sample in $KOTLIN_SAMPLES; do
  if should_run_sample kotlin "$sample" "$@"; then
    run_sample_with_retry "$SAMPLES_DIR/kotlin/$sample/run_sample.sh"
  fi
done
fi

if rg -n "$FORBIDDEN_SAMPLE_PATTERN" "$SAMPLES_DIR" -g '*.java' -g '*.kt'; then
  echo "sample gate failed: forbidden sample pattern found" >&2
  exit 1
fi

echo "All Java/Kotlin samples passed"
