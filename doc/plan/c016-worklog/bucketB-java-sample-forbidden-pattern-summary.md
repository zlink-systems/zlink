# Java sample forbidden-pattern 정리

실행일: 2026-09-05. `main`에서 Java 샘플의 두 `sleep` 대기를 조건 기반 동기화로 교체했다.
Core와 local package는 다시 빌드하지 않았고, 모든 Gradle 및 sample 호출은
`TMPDIR=/dev/shm/zlink-tmp-java`, `env -u ZLINK_LIBRARY_PATH`,
`flock -w7200 /tmp/zlink-jvm-gate.lock`로 실행했다.

## 적용한 규칙

`samples/sample-manifest.env`의 `FORBIDDEN_SAMPLE_PATTERN`은 Java/Kotlin 샘플에서
`Thread.sleep`과 `sleep(`을 금지한다. Java sample guide의 규칙대로 일정 시간을 기다리지 않고
완료 조건을 timeout이 있는 framework completion 또는 lifecycle 신호로 기다려야 한다.
`run_samples.sh:102`의 scan 규칙은 변경하지 않았다.

## 변경

### DeliveryDispatch deadline test

- `DeliveryOfferStore`가 기본적으로 `Clock.systemUTC()`를 사용하되 같은 package의 test가 `Clock`을
  주입할 수 있게 했다. offer deadline 생성과 만료 판정은 동일한 시계를 사용한다.
- `DispatchWorkerDeadlineTest`는 상태 publish completion을 보류한 상태에서 가변 시계를 courier
  decision timeout보다 100 ms 앞으로 이동한 뒤 `takeExpired()`가 비어 있고 courier send가 시작되지
  않았음을 확인한다. 상태 응답을 완료한 다음에는 send 시각에 courier의 전체 decision interval이
  남아 있음을 기존 범위로 그대로 검사한다.
- 실제 시간을 소비하지 않고도 “상태 publish 시간이 courier timeout을 소비하지 않는다”는 기존
  assertion을 동일하게 검증한다. 기본 생성자의 production 동작은 시스템 UTC 시계를 사용하므로
  바뀌지 않는다.

### TicTacToe lifecycle completion

- Java client의 100 ms 파일 polling loop를 `WatchService`의 bounded timed poll로 교체했다.
- watcher 등록 전후의 파일 존재 여부를 확인해 runner의 `lifecycle-complete` 생성과 등록 사이의
  race를 막고, `ENTRY_CREATE` 신호가 오면 실제 파일 존재를 확인한 뒤 반환한다.
- 전체 제한은 기존과 같은 60초이며, 제한 안에 완료 파일이 생기지 않으면 기존과 같은
  `IllegalStateException("Timed out waiting for runner lifecycle completion.")`을 던진다. runner가
  leave와 Entry Spot destroy 증거를 확인한 뒤 파일을 생성하고 client가 connector를 닫는 순서도
  유지된다.

## 검증 결과

| 검증 | 결과 | 시간 / marker |
|---|---|---|
| forbidden-pattern scan (`run_samples.sh:102`와 동일한 `rg`) | pass | 출력 없음, `rg` exit 1 |
| `:Server:Dispatch:test` | pass | 12초, 2 tests / 0 failures / 0 errors |
| Java DeliveryDispatch `run_sample.sh` 1회 | pass | 33초, `deliverydispatch-placement=completed` |
| Java TicTacToe `run_sample.sh` 1회차 | pass | 16초, `tictactoe-placement=completed` |
| Java TicTacToe `run_sample.sh` 2회차 | pass | 18초, `tictactoe-placement=completed` |
| Java TicTacToe `run_sample.sh` 3회차 | pass | 14초, `tictactoe-placement=completed` |

로그는 `zlink-work/c016/logs/bucketB-java-dispatch-test.log`,
`bucketB-java-deliverydispatch-sample.log`, `bucketB-java-tictactoe-sample-{1,2,3}.log`에 보존했다.

## BLOCKERS

없음.
