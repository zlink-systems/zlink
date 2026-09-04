# Final Java gate: samples and framework tests

실행일: 2026-09-05. 실행은 `main`에서 수행했고, `TMPDIR=/dev/shm/zlink-tmp-java`, 지정된 0.17.0 `libzlink.so`, `flock -w7200 /tmp/zlink-jvm-gate.lock`를 사용했다. Core와 local package는 다시 만들지 않았다.

## Java common samples (individual)

| Sample | Exit | Duration | Final marker | Result |
|---|---:|---:|---|---|
| TicTacToe | 126 | 0s | — | D: `./run_sample.sh` permission denied. 파일 mode가 `644`였다. |
| Bingo | 0 | 37s | `bingo-placement=completed` | pass |
| DeliveryDispatch | 0 | 37s | `deliverydispatch-placement=completed` | pass |
| SupportChat | 0 | 17s | `supportchat-placement=completed` | pass |
| GameQuest | 0 | 41s | `gamequest-placement=completed` | pass |
| ShoppingMall | 0 | 13s | `shoppingmall-placement=completed` | pass |
| ZoneWorld | 1 | 265s | — (`zoneworld=completed` withheld) | B: ZW-A3 terminal timeout |

ZoneWorld의 A3은 `CompletionException: TimeoutException`으로 종료했다. 호출 지점은 `ScenarioSupport.java:142`, 시나리오 호출은 `Scenarios.java:77`이며, runner가 `run_sample.sh:326`에서 완료 marker를 보류했다. DONTWAIT/backpressure assertion은 관찰되지 않았다.

## Aggregate runner

`framework/languages/java/samples/run_samples.sh`는 한 번 실행했다. exit 1, 633초였다. Java 7개는 aggregate에서는 모두 완료했고 ZoneWorld도 `zoneworld=completed`를 냈다. 이후 Kotlin GameQuest cleanup에서 `Sample process 6765 exited during cleanup with status 1.`로 멈췄다.

이는 B (terminal/error)로 분류한다. failure emission은 `framework/languages/java/samples/runner-common.sh:235`이고, 해당 assertion은 aggregate log 962행이다.

## Framework tests

| Command | Exit | Duration | Test count | Classification |
|---|---:|---:|---:|---|
| `:zlink-framework-core:test` | 1 | 49s | 1,215 total; 1,213 passed; 2 failed | C (known) |
| `contractTest` | 1 | 11s | 27 total; 26 passed; 1 failed | C (known) |

Known C failures:

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement` — `ZLinkJavaRawMeshNodeM6ATest.java:1400`.
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent` — `ZLinkJavaRawMeshNodeM6ATest.java:1439`.
- `JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts` — `JavaDocumentationRegressionTest.java:37`.

No additional framework core/contract failures were reported.

## Commands and logs

```bash
for sample in TicTacToe Bingo DeliveryDispatch SupportChat GameQuest ShoppingMall ZoneWorld; do
  (cd framework/languages/java/samples/java/$sample && ./run_sample.sh)
done
(cd framework/languages/java/samples && ./run_samples.sh)
(cd framework/languages/java && ./gradlew --no-daemon :zlink-framework-core:test)
(cd framework/languages/java && ./gradlew --no-daemon contractTest)
```

Logs are in `zlink-work/c016/logs/`: `gate-final-java-<sample>.log`, `gate-final-java-aggregate.log`, `gate-final-java-core-test.log`, and `gate-final-java-contract-test.log`. Timing and exit-code TSV files are alongside them.
