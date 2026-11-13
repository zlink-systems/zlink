#!/usr/bin/env bash
# Builds the Java and Kotlin with-grpc benches (one Gradle build). Owner of the Java/Kotlin build
# command; run_local.sh, run_local_kotlin.sh and build_all.sh call it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
cd "${HERE}"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

bench_require_low_load
"${HERE}/gradlew" --no-daemon --max-workers=1 assemble installDist
