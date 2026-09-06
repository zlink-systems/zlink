#!/usr/bin/env bash
# Serialized framework gate on the current core/build-dev + local packages: 7 samples per language,
# node npm test (incl. M6A), java core/contract tests, dotnet sample-regression + unit tests.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"; cd "$Z"; require_quiet || exit 2
unset ZLINK_LIBRARY_PATH; : > "$LOGS/results.txt"
TMPDIR=/dev/shm/zlink-tmp-gate run cpp-samples framework/languages/cpp bash samples/run_samples.sh
TMPDIR=/dev/shm/zlink-tmp-java run java-samples framework/languages/java bash samples/run_samples.sh
TMPDIR=/dev/shm/zlink-tmp-node run node-samples framework/languages/node bash samples/run_samples.sh
( dotnet_env; run dotnet-samples framework/languages/dotnet bash samples/run_samples.sh )
( dotnet_env; run dotnet-zoneworld-2 framework/languages/dotnet bash samples/ZoneWorld/run_sample.sh )
TMPDIR=/dev/shm/zlink-tmp-node run node-npmtest framework/languages/node npm test
TMPDIR=/dev/shm/zlink-tmp-java run java-coretest framework/languages/java ./gradlew --no-daemon :zlink-framework-core:test contractTest --continue
( dotnet_env; run dotnet-sampleregression framework/languages/dotnet dotnet test tests/Zlink.Framework.SampleRegressionTests )
( dotnet_env; run dotnet-unit-main framework/languages/dotnet dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests' --blame-hang --blame-hang-timeout 10m )
( dotnet_env; run dotnet-unit-join framework/languages/dotnet dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests' --blame-hang --blame-hang-timeout 10m )
echo "GATE_DONE $TAG"; awk '$2!=0{f=1; print "FAILED:", $1} END{exit f}' "$LOGS/results.txt" && echo "ALL GREEN"
touch "$LOGS/gate.done"
