# CP3 JVM 결함 검증·수정 및 발견 10 재감사

## 범위와 결론

- 작업 1: 감사 §4.2·§5의 6건은 모두 **[실증]**이다. 다섯 async source는
  `CompletableFuture`의 완료된 predecessor가 dependent를 inline 실행할 때, 그 dependent의
  synchronous prefix가 publication/chain assignment보다 먼저 재진입할 수 있었다. 여섯째는
  producer가 같은 pending future를 `join()`하는 자기 대기였다.
- 수정은 각 monitor 안에서 정확한 placeholder future를 먼저 claim하고, monitor 밖에서 기존
  chain을 시작하여 그 placeholder만 정산한다. READY retry reset, target terminal retention,
  retire ACK, Redis close identity, connector frame 순서와 기존 오류는 유지한다.
- pending-session의 다른 caller는 기존처럼 동일 pending 결과를 기다린다. 같은 producer
  stack만 즉시 `IllegalStateException`으로 종료한다.
- STOP 조건은 발생하지 않았다. 관측 순서·timeout·오류 코드를 바꾸는 설계 확대나 test
  expectation 변경은 하지 않았다.

## 작업 1 — 건별 정적 판정과 수정

| # | 감사 심각도 | 판정·도달 경로 | 수정 |
|---|---|---|---|
| 1 | [H] | **[실증]** `publishReady`의 caller는 PREPARE 완료 뒤 non-request path에서 호출되고(`ZLinkCanonicalRelocationStateMachine.java:492-498`), 재시도도 동일 fence/attempt를 다시 부른다. 기존에는 `attempt.ready()`가 완료된 경우 L511의 `thenCompose(sendReady)`와 L513의 fallback arm이 `readyPublication` 대입 전에 monitor 안에서 inline 실행될 수 있었다. | `ZLinkCanonicalRelocationStateMachine.java:507-542`: monitor 안에서 `readyPublication` placeholder를 선점하고, 밖에서 ready chain을 연결해 같은 identity만 완료한다. 실패 reset도 `== publication` exact identity를 유지한다. |
| 2 | [H] | **[실증]** CUTOVER 수신은 `publishTarget(..., false)`로(`ZLinkCanonicalRelocationStateMachine.java:916-927`), timeout fallback은 별도 executor에서 같은 attempt로 호출한다(`:930-936`). 기존 completed `prepared`/commit/request/target publish chain이 `attempt.publication` 대입 전에 실행되어 동일 target fence를 재관측할 수 있었다. | `ZLinkCanonicalRelocationStateMachine.java:943-1000`: publication placeholder를 monitor 안에서 설치하고 monitor 밖 체인을 그 placeholder로 정산한다. terminal retention/abort callback은 placeholder completion에 그대로 연결된다. |
| 3 | [H] | **[실증]** `Target.handle`의 PUBLISH command가 `publish(slot)`으로 들어간다(`ZLinkSpotRetireControl.java:192-207`). `slot.staged`가 완료돼 있으면 기존 `thenCompose(endpoint.publish)`의 synchronous prefix가 `slot.published` 대입 전에 재진입해 같은 slot을 다시 publish할 수 있었다. | `ZLinkSpotRetireControl.java:233-259`: `slot.published`를 placeholder로 먼저 설치하고, 밖에서 endpoint publish 후 ACK를 exact placeholder에 완료한다. |
| 4 | [M] | **[실증]** `closeAsync`의 monitor 안에서 `current.handle(...).thenCompose(closeAsync).thenCompose(shutdownAsync)`를 만들면, 이미 완료된 `connection` 및 synchronous close/shutdown completion이 `closeStage` 대입 전에 재진입할 수 있었다. `closed`는 새 connection만 막고 close identity 중복을 막지 못했다. | `ZLinkRedisLocationConnection.java:72-105`: 짧은 synchronized claim 구간에서 `closed`, snapshot, `closeStage` placeholder를 먼저 기록한다. connection close/client shutdown chain은 밖에서 실행하고 placeholder로 정산한다. |
| 5 | [H] | **[실증]** submit request/control이 `sendFrame`으로 합류한다(`DefaultZLinkStreamConnector.java:270-275`, `:342-354`). 완료된 `sendChain`에 붙인 기존 `thenCompose(writeFrame)`가 monitor 안에서 inline 실행되면 transport callback의 재진입 send가 이전 tail을 다시 잡아 frame serialization을 분기할 수 있었다. | `DefaultZLinkStreamConnector.java:294-325`: monitor 안에서 `previous`와 새 tail placeholder를 교체하고, 밖에서 이전 tail에 write를 연결한다. 재진입 send는 이미 새 placeholder를 predecessor로 잡으므로 FIFO tail이 보존된다. |
| 6 | [M] | **[실증]** notification path가 `getOrCreateSessionState`를 호출한다(`ZLinkStreamRuntime.java:1305-1316`). creator가 `createSessionState` synchronous prefix에서 같은 `(streamNode,routingId)`로 재진입하면, 둘째 호출은 claim의 pending branch에서 L1378 `join()`하여 creator 자신의 완료를 기다렸다. | `ZLinkStreamRuntime.java:121-123`, `:1357-1420`: ThreadLocal producer set에 exact pending identity를 등록하고, 같은 identity의 non-creator 재진입은 즉시 `IllegalStateException`으로 실패한다. construction/callback 범위가 끝나면 finally에서 producer mark를 제거한다. |

발견 5에 따라 placeholder의 dependent completion은 state-lane current scope 안에서 실행하지 않았다.
발견 6에 따라 claim은 외부 work 전 monitor turn에 기록됐고, callback/work 결과만 claim에 정확히
정산했다. 발견 9의 반환 전 등록도 각 placeholder가 monitor/`inStateLane` 반환 전 설치되므로
유지된다.

### 변경 파일

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkCanonicalRelocationStateMachine.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRetireControl.java`
- `framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisLocationConnection.java`
- `framework/languages/java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/DefaultZLinkStreamConnector.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java`

Test expectation과 대상 외 source는 수정하지 않았다.

## 작업 2 — 발견 10(찢어진 캡처 블록) 전수 재감사

### 발견 목록: 0건

`framework/languages/java/**/src/main/{java,kotlin}`의 production code를 읽기 전용으로
검사했다. monitor/state-lane의 복수 read, 그 사이의 `thenCompose`/`thenApply`/`handle`/
`whenComplete` 경계, Kotlin의 suspension 경계를 후보로 모은 뒤, 두 값 이상이 하나의 route,
fence, generation, owner, session 또는 derived snapshot을 만드는지를 추적했다.

현재 확인된 복수 read는 다음 중 하나여서 발견 10이 아니었다.

- 하나의 `inStateLane`/monitor turn 안에서 결과 record로 함께 캡처한다. 예를 들어
  `ZLinkLocationRuntime.java:213-225`의 `RenewState(nodeRid, ownerToken)`은 store boundary 전에
  한 turn에서 만들어진다.
- 첫 turn에서 immutable page/record를 완성한 뒤 뒤 단계는 그 record만 projection한다. 예를 들어
  `ZLinkInMemoryLocationStore.java:407-415`의 `stored` page projection은 state field를 다시 읽지
  않는다.
- 이후 state read는 이전 capture와 합쳐 파생 값을 만들지 않는 completion/cleanup predicate다.

따라서 **지금 함께 잡혀야 할 read가 future 단계들 사이에 찢어져 서로 다른 시점의 값을 섞는
파생 값은 찾지 못했다.** `git` 명령은 사용하지 않았으므로 회귀 여부는 판정하지 않았다.

## 검증

Gradle Java/Kotlin 트리는 요청한 `/tmp/zlink-jvm-gate.lock`으로 순차 실행했다.

1. `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test`
   - 실패. 대상 core 변경은 compile 및 test class 생성까지 통과했다.
   - 집계 원문:

     ```text
     1158 tests
     1 failures
     0 skipped
     28.684s duration
     99% successful
     ```

   - 실패 원문 식별: `ZLinkJavaRawSpotNodeM6BTest.remoteSpotSendAndRequestUseTheExactRouteFence()` —
     `java.util.concurrent.ExecutionException: systems.zlink.contracts.errors.ZlinkRequestException`
     (`ZLinkJavaRawSpotNodeM6BTest.java:1078`, `ZLinkJavaRawMeshNode.requestSpot:1812`). 지정된
     기존 flake 두 건에는 해당하지 않으며, 이번 여섯 대상의 호출 스택에도 없다. 따라서 gate는
     **미통과**로 남긴다.
2. `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-kotlin:test`
   - 성공. 이 경로에서 stream connector 변경도 컴파일됐다.
   - 집계 원문:

     ```text
     67 tests
     0 failures
     0 skipped
     2.794s duration
     100% successful
     ```

     ```text
     BUILD SUCCESSFUL in 17s
     23 actionable tasks: 15 executed, 8 up-to-date
     ```
3. `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-locations-redis:compileJava`
   - 성공. Redis close source의 compilation을 별도로 확인했다. 기존 unchecked/unsafe-operation
     warning만 출력됐다.
4. `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test --tests systems.zlink.framework.runtime.spots.ZLinkCanonicalRelocationReadySubmissionTest --tests systems.zlink.framework.runtime.spots.ZLinkCanonicalRelocationStateMachineTest --tests systems.zlink.framework.runtime.streams.ZLinkStreamRuntimeIngressTest`
   - 성공. 집계 원문:

     ```text
     31 tests
     0 failures
     0 skipped
     6.310s duration
     100% successful
     ```

     ```text
     BUILD SUCCESSFUL in 8s
     10 actionable tasks: 1 executed, 9 up-to-date
     ```
5. `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-stream-connector:test --tests systems.zlink.stream.connector.ZLinkStreamConnectorTest`
   - 성공. 집계 원문:

     ```text
     52 tests
     0 failures
     0 skipped
     3.112s duration
     100% successful
     ```

     ```text
     BUILD SUCCESSFUL in 6s
     12 actionable tasks: 3 executed, 9 up-to-date
     ```
