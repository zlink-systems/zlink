#!/usr/bin/env bash
# Source from the perf entry scripts. Points JAVA_HOME at the JDK the Gradle toolchain compiles with (the samples' single
# owner of that decision), so the installDist launchers run on the JDK they were built for. Packages resolve from Maven
# Central through the samples' user mode (perf/gradle.properties).
_perf_java_scripts="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${_perf_java_scripts}/../../samples/gradle/zlink-jvm-runtime.sh"
zlink_jvm_require_toolchain_runtime
