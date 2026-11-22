#!/usr/bin/env bash
set -eo pipefail
worker_count=2
if [[ "${ZLINK_SINGLE_WORKER}" == "true" ]]; then
  worker_count=1
fi
dump_dir="${RUNNER_TEMP}/framework-java-thread-dumps/${1}"
mkdir -p "${dump_dir}"
bash "${GITHUB_WORKSPACE}/scripts/ci/gradle-test-watchdog.sh" 3600 600 "${dump_dir}" &
watchdog_pid=$!
stop_watchdog() {
  kill "${watchdog_pid}" 2>/dev/null || true
  wait "${watchdog_pid}" 2>/dev/null || true
}
trap stop_watchdog EXIT
./gradlew --no-daemon --max-workers="${worker_count}" \
  :zlink-framework-core:test \
  :zlink-framework-core:integrationTest \
  :zlink-framework-kotlin:test \
  :zlink-framework-spring-boot-starter:test \
  :zlink-framework-spring-boot-starter:integrationTest \
  :zlink-framework-locations-redis:test \
  :zlink-framework-locations-etcd:test \
  :zlink-framework-locations-etcd:contractTest \
  :zlink-framework-testkit:test \
  -Pzlink.includeSamples=false
