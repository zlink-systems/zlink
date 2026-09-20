#!/usr/bin/env bash
# Serialized framework gate on the current core/build-dev + local packages: 7 samples per language,
# node npm test (incl. M6A), java core/contract tests, dotnet sample-regression + unit tests.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"; cd "$Z"; require_quiet || exit 2
unset ZLINK_LIBRARY_PATH; : > "$LOGS/results.txt"
# Formatting first: it is the cheapest step and the guides excerpt these sources as they are
# (doc/principal/dev/source-formatting.ko.md).
run format-check . bash scripts/format/format.sh --check
# One sample per invocation. A batch runner let a stalled sample hold the
# whole language's run and made interference between samples indistinguishable
# from a defect in any one of them (#405).
SAMPLES="Bingo DeliveryDispatch GameQuest ShoppingMall SupportChat TicTacToe ZoneWorld"
for s in $SAMPLES; do
  TMPDIR=/dev/shm/zlink-tmp-gate run "cpp-sample-$s" framework/languages/cpp bash "samples/$s/run_sample.sh"
done
for s in $SAMPLES; do
  TMPDIR=/dev/shm/zlink-tmp-java run "java-sample-$s" framework/languages/java bash "samples/java/$s/run_sample.sh"
  TMPDIR=/dev/shm/zlink-tmp-java run "kotlin-sample-$s" framework/languages/java bash "samples/kotlin/$s/run_sample.sh"
done
# Node names its TypeScript samples with a .Ts suffix; ZoneWorld has none.
for s in $SAMPLES; do
  d="$s.Ts"; [ -d "framework/languages/node/samples/$d" ] || d="$s"
  TMPDIR=/dev/shm/zlink-tmp-node run "node-sample-$s" framework/languages/node bash "samples/$d/run_sample.sh"
done
for s in $SAMPLES; do
  ( dotnet_env; run "dotnet-sample-$s" framework/languages/dotnet bash "samples/$s/run_sample.sh" )
done
( dotnet_env; run dotnet-zoneworld-2 framework/languages/dotnet bash samples/ZoneWorld/run_sample.sh )
TMPDIR=/dev/shm/zlink-tmp-node run node-npmtest framework/languages/node npm test
TMPDIR=/dev/shm/zlink-tmp-node run node-sample-tests framework/languages/node npm run test:samples
TMPDIR=/dev/shm/zlink-tmp-java run java-coretest framework/languages/java ./gradlew --no-daemon :zlink-framework-core:test contractTest --continue
( dotnet_env; run dotnet-sampleregression framework/languages/dotnet dotnet test tests/Zlink.Framework.SampleRegressionTests )
( dotnet_env; run dotnet-unit-main framework/languages/dotnet dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests' --blame-hang --blame-hang-timeout 10m )
( dotnet_env; run dotnet-unit-join framework/languages/dotnet dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests' --blame-hang --blame-hang-timeout 10m )
# Build-only: the with-grpc benches (5 languages) must keep compiling against the current runtime (bench plan S6).
( export ZLINK_CORE_PACKAGE_PREFIX="${ZLINK_GATE_CORE_PREFIX:-$Z/core/build-dev}" ZLINK_LOCAL_PACKAGE_ROOT="$Z/.artifacts/wsl"; run bench-build framework/bench/grpc bash build_all.sh )
echo "GATE_DONE $TAG"; awk '$2!=0{f=1; print "FAILED:", $1} END{exit f}' "$LOGS/results.txt" && echo "ALL GREEN"
touch "$LOGS/gate.done"
