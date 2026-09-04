# Bucket B — Java Bingo teardown과 ZoneWorld runner 경로 조사 결과

## 결과

ZoneWorld의 상대 경로 실행 실패는 수정했다. Java와 Kotlin shell runner가 시작 위치에서
자기 파일의 절대 경로를 한 번 계산하고, G4와 B8 격리 자식을 그 경로로 실행한다. Java
ZoneWorld는 repository root와 sample directory에서 각각 한 번씩 전체 시나리오와
`zoneworld=completed`를 확인하고 exit 0으로 끝났다.

Bingo의 `play-a` `ForceStopped/TeardownFailed`는 원인 stack을 확보하지 못했다. 제공된 gate
log에는 lifecycle marker만 있고 당시 `play-a.log`는 이어진 성공 재시도가 덮어썼다. 실패 때
남은 `/dev/shm/zlink-tmp-java/tmp.FlMhkZ3Mlr`에는 properties 파일만 있다. 기존
message-flow와 `ZLINK_JAVA_STREAM_TRACE=1`을 사용한 허용 범위 6회 중 기능 시나리오가 완료된
5회는 모두 모든 role이 `Stopped/None`으로 끝났다. 나머지 한 번은 teardown 전 client request
timeout이므로 대상 재현이 아니다. 실제 예외를 확인하지 못한 상태에서 다른 terminal을
idempotent로 추가하거나 lane 순서를 바꾸면 계약상 실패를 숨길 수 있으므로 Item 1의 runtime
수정과 회귀 test는 추가하지 않았다.

## Item 1 — Bingo `play-a` teardown

### 남아 있는 원본 증거

- `zlink-work/c016/logs/gate-final-java-2-Bingo.log:51-55`는 기능 marker 뒤
  `play-a.log`에서 `READY=1`, `TERMINATION=1`, `STOPPED_NONE=0`, `FORCE_STOPPED=1`과
  `outcome=FORCE_STOPPED reason=TEARDOWN_FAILED`만 기록한다.
- lifecycle logger는 underlying exception을 marker에 포함하지 않는다. Runtime은
  `ZLinkFrameworkRuntime.java:1467-1500`에서 drain failure 또는 teardown failure를
  `ForceStopped/TeardownFailed`로 변환한다.
- 실패 실행의 config directory `/dev/shm/zlink-tmp-java/tmp.FlMhkZ3Mlr`에는
  `play-a.properties`와 다른 role properties만 남았다. Role log는 sample의 고정 경로
  `framework/languages/java/samples/java/Bingo/build/sample-logs/`에 있었고 06:33 성공 재시도가
  덮어썼다.

### 계약 판정

기준 계약은
`framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md`
§14다.

- `:766-776`은 accepted handler, request completion, relocation unit과 session barrier를
  deadline까지 처리하고 Spot callback 뒤 local Actor, Spot scope, owner, descriptor,
  listener와 transport를 정리하며, 완료하면 `Stopped/None`, 실패하면 bounded teardown 뒤
  `ForceStopped/DeadlineExceeded` 또는 `ForceStopped/TeardownFailed`로 끝내도록 정한다.
- `:778-782`는 listener와 transport를 닫기 전에 accepted read/write operation을 완료하거나
  취소하고 cancellation completion을 관찰하도록 정한다.
- `:823-826`은 callback exception을 `ForceStopped/TeardownFailed`, deadline 만료를
  `ForceStopped/DeadlineExceeded`로 구분한다.

제공된 marker만으로는 callback exception, 예상하지 않은 terminal, synchronous close failure와
lane 안의 exception 가운데 어느 경우인지 구분할 수 없다. 결과가 `TeardownFailed`였다는 사실은
원인 stack이 아니다.

### 재현 결과

모든 실행은 `TMPDIR=/dev/shm/zlink-tmp-java`, unset `ZLINK_LIBRARY_PATH`,
`ZLINK_JAVA_STREAM_TRACE=1`과 `flock -w7200 /tmp/zlink-jvm-gate.lock`을 사용했다. 기존 trace로
원인이 드러나지 않은 뒤 `ZLinkFrameworkShutdown.recordFailure`에 stack 출력 계측을 임시로
추가했으며 조사 뒤 제거했다. 대상 실패가 재현되지 않아 계측 stack은 발생하지 않았다.

| 실행 | 기능 marker | `play-a` termination | 판정 |
|---|---|---|---|
| 1 | `bingo-placement=completed` | `STOPPED/NONE` | 대상 기준 green |
| 2 | `bingo-placement=completed` | `STOPPED/NONE` | 대상 기준 green |
| 3 | `bingo-placement=completed` | `STOPPED/NONE` | 대상 기준 green |
| 4 | `bingo-placement=completed` | `STOPPED/NONE` | 대상 기준 green |
| 5 | 없음 | `STOPPED/NONE` | client request timeout, 대상 재현 아님 |
| 6 | `bingo-placement=completed` | `STOPPED/NONE` | 대상 기준 green |

따라서 현재 revision은 Bingo functional + clean lifecycle 기준 5/5 green이다. 로그는
`zlink-work/c016/logs/bucketB-java-bingo-teardown-2-repro/run-{1..6}/`에 보관했다.

### 수정과 회귀 test

Item 1 runtime 수정은 없다. 원인 stack 없이 `ZLinkJavaStreamSocket`의 terminal 분류나
Actor/Spot state lane을 바꾸는 것은 최소 수정이 아니며, 실제 실패를 정상 종료로 잘못 분류할 수
있다. 같은 이유로 추정한 경로를 고정하는 회귀 test를 추가하지 않았다.

## Item 2 — ZoneWorld self-invocation

### 원인과 수정

- Java runner는 수정 전 `run_sample.sh:10`에서 자기 directory로 이동한 뒤 G4/B8 자식을
  `bash "$0"`로 실행했다. Repository root에서 상대 경로로 시작하면 이동 뒤 `$0`가 존재하지
  않아 자식을 시작하지 못했다.
- `framework/languages/java/samples/java/ZoneWorld/run_sample.sh:6-7`에서 `SCRIPT_PATH`를
  절대 경로로 계산하고, `:27`, `:35`의 두 self-invocation이 이를 사용한다.
- 같은 구조인 Kotlin runner도
  `framework/languages/java/samples/kotlin/ZoneWorld/run_sample.sh:6-7,27,35`에 동일하게
  반영했다.
- Java/Kotlin ZoneWorld에는 `run_sample.ps1`가 없다. 참고 확인한 .NET PowerShell runner는 이미
  `Resolve-Path`로 shell runner의 절대 경로를 구하며, Node PowerShell runner에는 shell
  self-invocation이 없다. 다른 언어 파일은 수정하지 않았다.

### 검증

두 실행 모두 unset `ZLINK_LIBRARY_PATH`, `TMPDIR=/dev/shm/zlink-tmp-java`와 JVM gate lock을
사용했다.

| 호출 위치와 명령 | 결과 |
|---|---|
| repository root: `bash framework/languages/java/samples/java/ZoneWorld/run_sample.sh` | exit 0, G4/B8 통과, `zoneworld=completed` |
| ZoneWorld directory: `bash ./run_sample.sh` | exit 0, G4/B8 통과, `zoneworld=completed` |

로그는 `zlink-work/c016/logs/bucketB-java-zoneworld-runner-fix/repo-root.log`와
`sample-dir.log`에 보관했다. 두 shell runner는 `bash -n`을 통과했고 `git diff --check`도
통과했다.

## Java core test

요청한 전체 core test를 한 번 실행했다.

```text
./gradlew --no-daemon :zlink-framework-core:test
1216 tests completed, 2 failed
```

실패는 이전 gate와 같은 알려진 M6A 두 건이다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
  (`ZLinkJavaRawMeshNodeM6ATest.java:1400`)
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`
  (`ZLinkJavaRawMeshNodeM6ATest.java:1439`)

전체 log는 `zlink-work/c016/logs/bucketB-java-bingo-teardown-2-core-test.log`에 보관했다.

## 변경 파일

- `framework/languages/java/samples/java/ZoneWorld/run_sample.sh`
- `framework/languages/java/samples/kotlin/ZoneWorld/run_sample.sh`
- `doc/plan/c016-worklog/bucketB-java-bingo-teardown-2-summary.md`

## BLOCKERS

- Item 1의 원인 stack, 원인별 runtime 수정과 회귀 test는 확보하지 못했다. 원본 role log가
  재시도로 덮였고 trace + 임시 exception 계측을 사용한 허용 범위 6회에서 대상 failure가
  재현되지 않았다. 같은 failure가 다시 발생하면 실패 runner가 이미 지원하는
  `ZLINK_SAMPLE_FAILURE_LOG_ROOT`로 `build/sample-logs`를 즉시 보존하고, stack 계측을 포함한
  `play-a.log`를 확보해야 원인 모듈을 확정할 수 있다.
- Item 2에는 blocker가 없다.
- Java core 전체 gate green에는 알려진 M6A 두 실패가 남아 있다. 이번 runner 수정과 다른
  test/class이므로 수정하거나 assertion을 낮추지 않았다.
- 지정된 `framework/languages/java/AGENTS.md`는 repository에 존재하지 않아 root와
  `framework/AGENTS.md` 규칙을 적용했다.
