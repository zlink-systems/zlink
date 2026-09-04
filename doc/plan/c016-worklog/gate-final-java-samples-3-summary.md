# Final Java sample gate, round 3

실행일: 2026-09-05. committed `main`에서 실행했다. 시작 전 작업 트리에는 Node 작업의 변경과 기존 `zlink-work/`가 있었으며 이 작업은 Java 샘플 실행 산출물, 로그 및 이 요약만 작성했다. `TMPDIR=/dev/shm/zlink-tmp-java`를 사용했고, 모든 Gradle/sample 호출은 `flock -w7200 /tmp/zlink-jvm-gate.lock`으로 감쌌다. 호출 환경의 `ZLINK_LIBRARY_PATH`는 unset했다. Core 및 local package를 재빌드하지 않았다.

## Aggregate runner (Java + Kotlin)

`bash framework/languages/java/samples/run_samples.sh`는 999초 후 exit 1이었다. Java 7개와 Kotlin 7개는 모두 completion marker까지 도달했다. 실패는 샘플 실행 중이 아니라 모든 marker 뒤의 aggregate static scan에서 발생했다. runner는 개별 sample duration을 출력하지 않으므로, 아래 duration은 `n/a`이며 측정된 전체 duration은 999초다.

| Order | Language | Sample | Exit / duration | Completion marker | Result |
|---:|---|---|---|---|---|
| 1 | Java | TicTacToe | completed / n/a | `tictactoe-placement=completed` | pass |
| 2 | Java | Bingo | completed / n/a | `bingo-placement=completed` | pass |
| 3 | Java | DeliveryDispatch | completed / n/a | `deliverydispatch-placement=completed` | pass |
| 4 | Java | GameQuest | completed / n/a | `gamequest-placement=completed` | pass |
| 5 | Java | ShoppingMall | completed / n/a | `shoppingmall-placement=completed` | pass |
| 6 | Java | SupportChat | completed / n/a | `supportchat-placement=completed` | pass |
| 7 | Java | ZoneWorld | completed / n/a | `zoneworld=completed` | pass |
| 8 | Kotlin | TicTacToe | completed / n/a | `tictactoe-placement=completed` | pass |
| 9 | Kotlin | Bingo | completed / n/a | `bingo-placement=completed` | pass |
| 10 | Kotlin | GameQuest | completed / n/a | `gamequest-placement=completed` | pass |
| 11 | Kotlin | ShoppingMall | completed / n/a | `shoppingmall-placement=completed` | pass |
| 12 | Kotlin | DeliveryDispatch | completed / n/a | `deliverydispatch-placement=completed` | pass |
| 13 | Kotlin | SupportChat | completed / n/a | `supportchat-placement=completed` | pass |
| 14 | Kotlin | ZoneWorld | completed / n/a | `zoneworld=completed` | pass |

Aggregate final failure: `sample gate failed: forbidden sample pattern found` at `zlink-work/c016/logs/gate-final-java-3-aggregate.log:1064`. First matches are `framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/test/java/systems/zlink/samples/deliverydispatch/server/dispatch/DispatchWorkerDeadlineTest.java:59` (`TimeUnit.MILLISECONDS.sleep(...)`) and `framework/languages/java/samples/java/TicTacToe/Client/src/main/java/systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java:306` (`Thread.sleep(100)`). The scan is at `framework/languages/java/samples/run_samples.sh:102`; it includes all `*.java` below `samples`, including the DeliveryDispatch test source.

## Retries and evidence

No sample failed before its completion marker, so no individual `run_sample.sh` re-run was authorized or performed. Consequently no sample role-log/tmp evidence copy was required. The aggregate evidence is retained at `zlink-work/c016/logs/gate-final-java-3-aggregate.log`; Kotlin ZoneWorld's final run directory was `/dev/shm/zlink-tmp-java/tmp.6sTE5Dx6OG` and completed successfully.

## contractTest

| Command | Exit | Duration | Counts | Result |
|---|---:|---:|---|---|
| `./gradlew --no-daemon contractTest` (from `framework/languages/java`) | 1 | 16s | 27 total; 26 passed; 1 failed | C |

The sole failure is `JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts`, `framework/languages/java/zlink-framework-core/src/contractTest/java/systems/zlink/framework/JavaDocumentationRegressionTest.java:37`. This is the known C failure recorded in round 2. The aggregate prerequisite `:zlink-framework-testkit:contractTest --tests '*SampleReleaseGateContractTest*'` completed successfully before samples, so the previously blocking `SampleReleaseGateContractTest.requiredSamplesExposeExecutableEntryPoints` did not recur.

## Failure classification

| Bucket | Evidence | Classification |
|---|---|---|
| A — DONTWAIT/backpressure | No assertion or log evidence. | none |
| B — terminal/error | No sample terminal failure; Bingo and both ZoneWorld runs completed. | none |
| C — known pre-existing | `JavaDocumentationRegressionTest.java:37`, 27 total / 1 failed; exact prior evidence in `gate-final-java-samples-2-summary.md`. | known documentation-contract failure |
| D — environment/runner | `run_samples.sh:102` scans test source as well as sample entry-point source; first match `DispatchWorkerDeadlineTest.java:59`, then `TicTacToeClientScenario.java:306`; terminal line `gate-final-java-3-aggregate.log:1064`. | deterministic aggregate runner/policy-scope failure after 14/14 sample completions |
| E — binding-port dependency | No evidence. | none |

## Commands and logs

```bash
mkdir -p /dev/shm/zlink-tmp-java zlink-work/c016/logs
env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  flock -w7200 /tmp/zlink-jvm-gate.lock \
  bash framework/languages/java/samples/run_samples.sh
(cd framework/languages/java && \
  env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon contractTest)
```

Logs: `zlink-work/c016/logs/gate-final-java-3-aggregate.log` and `.meta`, plus `zlink-work/c016/logs/gate-final-java-3-contractTest.log` and `.meta`. `gate-final-java-3-aggregate-empty-after-tool-detach.log` is an empty harness-detach artifact, not a test run log.

## BLOCKERS

- Aggregate Java/Kotlin gate is blocked after all 14 samples pass by the runner's forbidden-pattern scan scope at `run_samples.sh:102`.
- `contractTest` remains blocked solely by the known `JavaDocumentationRegressionTest.java:37` failure.
