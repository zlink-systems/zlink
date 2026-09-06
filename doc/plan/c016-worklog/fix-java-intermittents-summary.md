# Java Framework 간헐 gate 2건 근본 수정 결과

## 결론

- M6A same-process ClientServer의 `dealer == null`은 runtime 결함이 아니라 startup과
  admission-ready를 같은 완료점으로 본 fixture의 계약 불일치였다. Production의 공개 호출
  경로는 이미 operation timeout과 5초 cap 안에서 ready target을 기다린다. 테스트가 그 단일
  readiness owner를 사용하도록 고쳤다.
- Canonical relocation의 `target rejected canonical relocation: 17`은 Java 상태기계 결함이었다.
  Source가 typed `NotConnected` terminal을 즉시 소비하지 않고 1초 timer까지 버렸으며, 그 1초는
  target의 cutover fallback과 같았다. 그 사이 terminal이 된 target은 동일 PREPARE request를
  READY로 replay하지 않고 일반 예외로 거절했다. Source의 operation deadline은 다시 시작되지
  않았지만, 그 deadline 안에서 해야 할 즉시 replay 규칙과 target 멱등 terminal 규칙이 모두
  깨져 있었다.

## 진단 근거

### M6A same-process ClientServer

- 최초 gate 증거:
  `/tmp/zlink-java-cs-server-ready-20260906/full-gate.log:46`에서
  `expected: <dealer> but was: <null>`이었다.
- 기존 `ZLINK_JAVA_STREAM_TRACE=1` 진단을 켜고 `yes > /dev/null` 20개 부하 아래 문제 method를
  독립 JUnit Launcher로 100회 실행했다. 결과는
  `/tmp/zlink-java-gate-intermittents-clientserver-launcher-100.log`의 **27/100 실패**다.
- 실패 trace는 `start → publish-result → list-page → reconcile → connect`까지 기록된 뒤 assertion이
  먼저 실행됐다. 성공 run에서만 그 뒤 `admission-ready`가 assertion 전에 보였다. 즉 descriptor
  탐색이나 same-process dealer 생성 실패가 아니라 monitor lane의 admission callback과 내부
  snapshot 조회 사이의 순서 경쟁이다.
- 원인 위치:
  `ZLinkClientServerLocationRuntime.java:118-152`의 `start`는 초기 publish/reconcile과 physical
  connection open까지만 소유한다. Admission callback 뒤 routing-ready 등록은 별도 비동기 경로다.
  Public call의 readiness 대기 소유자는 `ZLinkChannelSocketRegistry.java:273-301`과
  `ZLinkChannelRuntime.java:1113-1132`다.
- Spec 조항:
  `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:170-181`은
  ClientServer가 호출 시점부터 `min(request timeout, 5초)` 동안 ready 후보를 기다리고,
  Framework startup은 local admission 완료를 기다리지 않는다고 명시한다.
- 교차언어 대조:
  Node `client-server-location-runtime.ts:65-75,284-309,318-364`도 `start()`가 publish/reconcile과
  connection open만 기다리고, transport-ready callback에서 admission과 ready 등록을 별도로
  완료한다. Java만 startup을 readiness barrier로 바꿀 근거가 없다.
- 변경 분류: **A — fixture의 기존 계약 적응**. Production runtime 동작은 바꾸지 않았다.

### Canonical relocation PREPARE

- 기존 진단 로그와 20개 CPU 부하 아래 문제 method 100회 반복 결과는
  `/tmp/zlink-java-gate-intermittents-relocation-launcher-100.log`의 **6/100 실패**다.
- 실패 run은 매번 source의 첫 PREPARE request가 target stage를 성립시킨 뒤 transport
  `NotConnected`로 보이지 않게 된 경우다. 기존 source는 이 terminal을 무시하고 1초 slice timer를
  기다렸다. 같은 시각 target의 1초 cutover fallback이 terminal target을 만들었다.
- run 29는 `cutover_timeout → Late or duplicate canonical PREPARE → terminal canonical relocation
  prepare cannot reply` 순서를 직접 기록했다. run 36은 같은 경쟁에서 wire failure 17을 받았다.
  `framework/runtime/protocol/generated/jvm/ServiceWireConstants.java:72`에서 17은
  `FRAMEWORK_ERROR_REQUEST_FAILED`이며 stale generation/seal 코드가 아니다.
  `ZLinkCanonicalRelocationStateMachine.java:744-750`의 mapper상 분류되지 않은 target 예외가 이
  generic code가 된다.
- 원인 위치:
  source의 `awaitReadyWithPrepareResend`가 retryable request failure를 관찰하고도 readiness waiter를
  깨우지 않던 구간과, target의 terminal PREPARE branch가 exact bytes에도 request reply를
  거절하던 구간이다.
- 소유 계층: **Java canonical relocation state machine**이 source의 동일 PREPARE replay와 target의
  relocation identity별 중복 억제/terminal replay를 함께 소유한다. Transport, Actor fixture,
  Location Store 또는 상위 Framework wrapper에 보상 상태를 추가하지 않았다.
- Spec 조항:
  `02-channel-transport/06-wire-protocol.ko.md:610-625`의 command 40 PREPARE request/command 30 READY
  reply 및 불확정 연결 단절 규칙, 같은 문서 `:698-704`의 `RelocationId` 중복-control identity,
  `03-spot-actor/04-actor-model.ko.md:550-580`의 typed transient replay와 sender 단일 deadline,
  D-093 규칙 2를 적용했다. 경과 시간은 D-095에 따라 monotonic clock 하나로 유지했다.
- 교차언어 대조:
  Node `service-relocation-host-runtime.ts:2435-2450`은 active stage와 terminal target 모두 exact
  PREPARE fingerprint에 READY를 반환한다. .NET `ZLinkManagedMeshNode.cs:890-965`는 linked deadline
  하나에서 request reply와 공유 `pending.Ready` terminal 중 먼저 온 결과를 소비한다. Java의
  terminal exact-request 거절과 retryable terminal 무시는 이 두 구현과 달랐다.
- 변경 분류: **B — 기존 결함**. Spec gap이나 상위 계층 우회가 아니다.

## 변경

- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkClientServerM6ARuntimeTest.java`
  - `start().join()` 뒤 ready snapshot을 즉시 읽지 않고, 기존 단일 readiness owner인
    `awaitClientForOutbound`를 fixture의 1초 operation budget으로 호출한다.
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkCanonicalRelocationStateMachine.java`
  - Per-attempt `readyOrRetry` completion이 target READY/FAILED와 retryable request terminal을 한곳에서
    소비한다. `NotConnected`이면 기존 absolute deadline을 그대로 들고 exact PREPARE를 즉시
    재제출한다.
  - Deadline 차감은 주입 가능한 monotonic nano clock 하나를 쓰며, readiness slice와 cutover
    fallback의 delay scheduling도 같은 주입 seam을 사용한다. 기본 production 구현은
    `System.nanoTime`과 `CompletableFuture.delayedExecutor`다.
  - Terminal target이 source와 encoded PREPARE가 정확히 같으면 active duplicate와 동일하게
    canonical READY bytes를 반환한다. READY encode 규칙은 helper 하나로 통합했다.
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/spots/ZLinkCanonicalRelocationStateMachineTest.java`
  - Timer를 실행하지 않는 deterministic scheduler와 고정 monotonic clock으로 첫
    `NotConnected`가 timer 도움 없이 두 번째 PREPARE를 즉시 유발함을 검증한다.
  - Cutover 뒤 exact PREPARE request가 READY를 replay하고 target stage를 다시 만들지 않음을
    검증한다.

수정 전/후 규칙 수: M6A fixture의 `startup 완료 == ready`와 public call readiness 두 규칙을
public call readiness 하나로 줄였다(2→1). Relocation은 active/terminal exact PREPARE 결과 두 규칙과
request-terminal/timer 진행 두 규칙을, exact identity READY 하나와 per-attempt completion trigger
하나로 줄였다(4→2).

## 검증

- 수정 전 부하 재현:
  - ClientServer 문제 method: 100회 중 27 실패.
  - Relocation 문제 method: 100회 중 6 실패.
- Focused regression 3건:
  - `sameProcessServerUsesStoreDiscoveryAndExactDealerRouterAdmission`
  - `actorJoinPrepareRetriesNotConnectedWithinOriginalDeadline`
  - `exactPrepareRequestAfterTargetCutoverRepliesReady`
  - 결과: `BUILD SUCCESSFUL`.
- 전체 class 반복:
  - `ZLinkClientServerM6ARuntimeTest`: **300/300**, 0 failure.
    로그: `/tmp/zlink-java-clientserver-class-300-fixed.log`.
  - `ZLinkCanonicalRelocationStateMachineTest`: **300/300**, 0 failure.
    로그: `/tmp/zlink-java-relocation-class-300-fixed.log`.
- 최종 gate 전 조건: 1분 load average `2.81`, `lto1=0`, gate lock 획득 가능.
- 최종 gate:
  `flock /tmp/zlink-java-gate.lock ./gradlew --no-daemon test`
  - 결과: **BUILD SUCCESSFUL in 1m 1s**, 54 tasks, 0 failure.
  - 로그: `/tmp/zlink-java-full-test-fixed.log`.
- `git diff --check`: 통과.
- 남은 실패: 없음.
