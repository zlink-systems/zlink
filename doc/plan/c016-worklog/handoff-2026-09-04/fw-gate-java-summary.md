# Java framework gate re-verification — `main` `4d263e66b9`

## Result

The Java binding was rebuilt from the current checkout against supplied
`core/build-dev`; Core was not rebuilt. No Java framework status-mapping or
terminal-classification regression remains, so no source fix was made.

The Java gate is not clean: the known two M6A tests and known documentation
regression remain. The official sample runner also stops after TicTacToe's
functional scenario because `play-a` reports teardown failure. Java cross E2E
is green.

## Before → after

| Gate | Before (DONTWAIT) | Current `main` result | Verdict |
|---|---:|---:|---|
| Java binding native/JAR | completion could remain pending without framework polling | `clean jar`: PASS, 19 tasks, against `core/build-dev` | A |
| Framework unit test | completion path RED/hanging | 1,209 tests: 1,207 pass, 2 fail | A green; two C failures |
| `contractTest` | known doc regression | 27 tests: 26 pass, 1 fail | C |
| Seven common samples | completion regression target | 0/7 successes: TicTacToe reached `tictactoe-placement=completed`, then `play-a` force-stopped and runner stopped | C / not terminal classification |
| Java host distribution | n/a | `:Host:installDist`: PASS, 23 tasks | PASS |
| Java cross-language E2E | RED before fix | 4/4 PASS: Java→Node, Node→Java, Java→.NET, .NET→Java | A |

## Failure classification

### (A) DONTWAIT-cleared

- The current public ROUTER poller subscribes to `POLLIN`, `POLLOUT`, and
  `POLLCOMPLETION` at
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketReceivePoller.java:41-47`.
  `ZLinkJavaRawServicePort.receive` drains it outside the state lane before
  lane-owned `recv(DONT_WAIT)` at `.../ZLinkJavaRawServicePort.java:239-255`.
- The full unit result has no DONTWAIT-specific failure, and real-wire Java
  cross passed all four directions, including reply, framework-not-found, and
  application-rejected terminals.

### (B) terminal/error-classification cluster

No Java B failure was observed: there is no Java returned-status/errno whose
mapping needs a framework change. The C++ and .NET cases listed in the generic
gate are outside this Java-only gate and were not edited or reclassified here.

### (C) pre-existing / do not fix

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`:
  waits for `MeshPeerState.CLOSED` at
  `.../ZLinkJavaRawMeshNodeM6ATest.java:613-635`; XML says `peer state was not
  observed: CLOSED` (the helper assertion is line 1400).
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`:
  the two-second `awaitReplacement` window at `.../ZLinkJavaRawMeshNodeM6ATest.java:555-702`
  expires (XML: `expected: <true> but was: <false>`). These are the two known
  M6A failures, not terminal mappings.
- `JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts`
  at `.../src/contractTest/java/systems/zlink/framework/JavaDocumentationRegressionTest.java:31`:
  expected `true`, got `false`. Protected docs/specs were not changed.
- Samples: `play-a.log` records all application flow through the placement
  marker, then `ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED
  reason=TEARDOWN_FAILED`. `ZLinkFrameworkRuntime.java:1482-1486` maps an
  exceptional teardown completion to this terminal; drain paths are
  `.../ZLinkFrameworkRuntime.java:2106-2136` and `2259-2268`. The lifecycle
  logger deliberately prints only the generic marker at
  `.../ZLinkFrameworkLifecycle.java:250-260`, so the underlying exception is
  absent from the retained existing log. Commit `4d263e66b9` does not touch
  lifecycle/sample files; no speculative lifecycle rewrite was made.

## Changes

No source files changed. No files under `core/**`, `bindings/**`,
`framework/doc/framework/**`, or `core/doc/spec/**` were modified. Final
`git status` shows only the untracked `zlink-work/` deliverables.

## Commands run

All Java commands used:

```bash
export TMPDIR=/dev/shm/zlink-tmp-java
export ZLINK_CORE_SOURCE=local
export ZLINK_CORE_INCLUDE_DIR=/home/hep7/project/zlink/core/include
export ZLINK_CORE_LIB_DIR=/home/hep7/project/zlink/core/build-dev/lib
export ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0
export ZLINK_JAVA_BINDINGS_SOURCE=/home/hep7/project/zlink/bindings/java
```

```bash
cd bindings/java && flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon clean jar
cd framework/languages/java && flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon :zlink-framework-core:clean :zlink-framework-core:test contractTest
cd framework/languages/java && flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon contractTest
cd framework/languages/java && ./samples/run_samples.sh
cd framework/languages/java && ./gradlew --no-daemon -p cross-language :Host:installDist
cd framework/languages/cpp && ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross ./cross-language/run_cross_language_smoke.sh
```

`framework/languages/java/cross-language/run_cross_language_smoke.sh` does
not exist (exit 127); the last command is the repository's actual Java
cross-language runner and owns the `java-cross` stage.
