# Final Java sample gate, round 2

실행일: 2026-09-05. 기준은 committed `main`의 `d8575e6fbb`이다. 실행 환경은 `TMPDIR=/dev/shm/zlink-tmp-java`이고, 각 Gradle 또는 sample 실행을 `flock -w7200 /tmp/zlink-jvm-gate.lock`으로 감쌌다. `ZLINK_LIBRARY_PATH`는 호출 환경에서 unset했으며, Core 및 local package를 재빌드하지 않았다. 첫 개별 TicTacToe 실행의 `installDist`가 Java host 산출물을 갱신했다.

## Individual Java samples

| Sample | Initial exit | Duration | Final marker / first failure | Result |
|---|---:|---:|---|---|
| TicTacToe | 0 | 18s | `tictactoe-placement=completed` | pass |
| Bingo | 1 | 61s | `bingo-placement=completed`; lifecycle validation then failed | B; retry: exit 0, 40s |
| DeliveryDispatch | 0 | 37s | `deliverydispatch-placement=completed` | pass |
| SupportChat | 0 | 16s | `supportchat-placement=completed` | pass |
| GameQuest | 0 | 37s | `gamequest-placement=completed` | pass |
| ShoppingMall | 0 | 14s | `shoppingmall-placement=completed` | pass |
| ZoneWorld | 1 | 136s | `zoneworld=completed` withheld: ZW-B8 did not pass | D; retry: exit 1, 140s (reproduced) |

The individual scripts were invoked through `bash` because aggregate runner does the same. The committed Java TicTacToe entry point is executable (`100755`), consistent with `133d01c9b2`.

## Aggregate runner (Java + Kotlin)

| Command | Exit | Duration | Stop point |
|---|---:|---:|---|
| `run_samples.sh` | 1 | 16s | `:zlink-framework-testkit:contractTest`, before any Java/Kotlin sample |

The aggregate prerequisite ran 22 tests: 21 passed and 1 failed. First failure: `SampleReleaseGateContractTest.requiredSamplesExposeExecutableEntryPoints`, `framework/languages/java/zlink-framework-testkit/src/contractTest/java/systems/zlink/framework/testkit/SampleReleaseGateContractTest.java:141`. It asserts that Java TicTacToe `run_sample.sh` is non-executable, but the committed file is executable. This contradicts the committed runner-mode repair `133d01c9b2` (“restore the executable bit”) and the current file mode.

## Framework core unit test

| Command | Exit | Duration | Counts | Result |
|---|---:|---:|---|---|
| `./gradlew --no-daemon :zlink-framework-core:test` | 1 | 46s | 1,216 total; 1,214 passed; 2 failed | C (known) |

The two observed failures exactly match the known C failures:

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement` — `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeM6ATest.java:1400`.
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent` — `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeM6ATest.java:1439`.

`JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts` was not part of the requested `:zlink-framework-core:test` invocation. Its previous C evidence remains `framework/languages/java/zlink-framework-core/src/contractTest/java/systems/zlink/framework/JavaDocumentationRegressionTest.java:37` (round-1 `contractTest`: 27 total, 26 passed, 1 failed).

## Failure classification

| Bucket | Evidence | Classification |
|---|---|---|
| A — DONTWAIT/backpressure | No assertion or log evidence observed. | none |
| B — terminal/error | Initial Bingo cleanup: `Framework lifecycle evidence is incomplete: build/sample-logs/play-a.log`; `ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED reason=TEARDOWN_FAILED` in `gate-final-java-2-Bingo.log:53-55`. Retry passed. | intermittent terminal/lifecycle failure |
| C — known pre-existing | M6A failures at `ZLinkJavaRawMeshNodeM6ATest.java:1400`, `:1439`; historical Java documentation contract failure at `JavaDocumentationRegressionTest.java:37`. Aggregate runner also has a stale release-mode assertion at `SampleReleaseGateContractTest.java:141`, contradicted by `133d01c9b2`. | known validation/test debt |
| D — environment/runner | ZoneWorld invokes isolation children through `bash "$0"` at `framework/languages/java/samples/java/ZoneWorld/run_sample.sh:34`. After `cd "$ROOT_DIR"` at line 10, the original relative `$0` resolves incorrectly: log lines 1 and 3 report `bash: framework/languages/java/samples/java/ZoneWorld/run_sample.sh: No such file or directory`; line 37 then blocks ZW-B8. Reproduced once. | deterministic runner path defect |
| E — binding-port dependency | No evidence observed. | none |

## Commands and logs

```bash
mkdir -p /dev/shm/zlink-tmp-java zlink-work/c016/logs
for sample in TicTacToe Bingo DeliveryDispatch SupportChat GameQuest ShoppingMall ZoneWorld; do
  env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
    flock -w7200 /tmp/zlink-jvm-gate.lock \
    bash framework/languages/java/samples/java/$sample/run_sample.sh
done
env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  flock -w7200 /tmp/zlink-jvm-gate.lock \
  bash framework/languages/java/samples/run_samples.sh
(cd framework/languages/java && env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon :zlink-framework-core:test)
```

Logs: `zlink-work/c016/logs/gate-final-java-2-TicTacToe.log`, `-Bingo.log`, `-DeliveryDispatch.log`, `-SupportChat.log`, `-GameQuest.log`, `-ShoppingMall.log`, `-ZoneWorld.log`, retries `-Bingo-retry.log` and `-ZoneWorld-retry.log`, `-aggregate.log`, and `-core-test.log`. Corresponding `*.meta` files contain retry, aggregate, and core-test exit/duration measurements.

## BLOCKERS

- The individual Java ZoneWorld entry point fails deterministically when invoked by the requested relative path because its self-invocation at line 34 uses `$0` after changing directory. The aggregate runner would avoid this particular defect by providing an absolute path, but is currently blocked before samples by the incompatible TicTacToe executable-mode contract assertion.
- Core unit gate remains blocked only by the two explicitly known M6A failures.
