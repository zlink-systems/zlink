# q1673e / #1673 Java per-message cost

결과: B1·B2 결함과 reply/UTF-8 할당을 수정했다. 기능 검증은 통과했으나 CPU 수락 조건은 충족하지 못했다. 작업 전체의 성능 목표는 미완료다. 스펙 BLOCKERS는 없다.

브랜치: framework-java/1639-per-message-cost. 시작 HEAD: 01d3ed75e7c53d2f2707154e7ecffa79ca9f098f. 비교 main: a7ac59c5bd4fea1539397c565bc59ce8b0e1c62d. 커밋: 4458abe381a00e6d99b51bc0ce7c318daf0d568a. push·PR·merge·GitHub 변경 없음.

## 변경과 소유

- SerialExecutionQueue의 일반/shared Spot worker 제출을 submitWorker 하나로 모았다. 표준 ExecutorService는 worker에 직접 제출한다. 임의 Executor의 inline/blocking 호환에 기존 DRAIN_EXECUTOR를 사용한다. execution gate의 claim·직렬화 권한은 유지했다.
- DurableRequest의 private terminal과 caller future를 분리했다. 기존 terminal cleanup callback에서 StateLane의 기존 common-pool 게시 경로를 재사용한다. 실제 caller 게시 callback 자체를 비동기로 등록해 완료 직후 callback 등록 경쟁도 owner 밖으로 보낸다.
- Channel/Spot reply는 기존 declared-type codec 선택과 OwnedMessageSerializer로 native message를 만든다. payload wrapper·defensive copy를 없앴다.
- M6A text는 기존 canonical UTF-8 round-trip 판정으로 malformed/NUL을 거부한다. 매번 만드는 CharsetDecoder·byte/char buffer scratch를 제거했다.
- 미사용 serializeForDeclaredType forwarding helper 두 개를 제거했다.

소유 계층: Framework execution gate가 handler 실행 권한과 worker 제출을 소유하고, durable terminal은 node state owner가, caller continuation 게시 경계는 StateLane이 소유한다.
스펙 조항: server/01-execution/02-handler-turn-and-execution-gate §2, 08-messaging-hot-path §4.1 I4 및 §3 E5, 06-state-ownership-and-lanes 「완료 신호와 블로킹 호환 경계」.
교차언어 대조: C++ serial_execution_queue.cpp:1306–1335의 직접 worker 제출과 task.hpp:346의 caller scheduler, .NET SerialExecutionQueue.cs:1220과 StateLane.cs:114/160의 RunContinuationsAsynchronously, Node serial-execution-queue.ts:278–291과 state-lane.ts:48–64의 microtask 경계를 읽기 전용 대조했다. Kotlin은 Java backend를 공유한다. 기존 계약 B를 맞췄으며 다른 언어 수정 없음.
변경 분류: B(기존 결함). 보호 스펙 변경 없음.
판정 위치 수: 일반/shared worker 제출 정책 2→1; gate 권한 1→1; durable caller 게시 정책의 소유자 2(Durable inline·StateLane)→1(StateLane). terminal 결과 판정은 변경하지 않았다.
상태 증감: caller CompletableFuture 1개 추가(terminal/caller 분리), 새 flag/queue/lock/timer/retry/pool/cache 0개. 표준 worker 경로의 dispatcher task와 drainScheduled 중간 메서드, forwarding helper 2개, UTF-8 decoder scratch를 제거했다. 기존 임의 Executor 호환 dispatcher는 유지했다.

## 수정 전 실패와 검증

수정 전 B1 normal/shared worker 제출 회귀 2/2 실패, B2 성공/실패 owner continuation 회귀 200/200 실패, owned reply 경로 회귀 1/1 실패를 기록했다. 수정 후 모두 통과했다. UTF-8 정상 Hangul/emoji/U+FFFD와 malformed·NUL·overlong·surrogate·truncated 입력을 양방향 검증했다.

검증은 WSL/JDK25만 사용했다. Java/Kotlin 전체 `test contractTest fakeBackendTest integrationTest`, format, Java/Kotlin API snapshot 검사를 실행했다. 최초 전체 통과 후 StateLane의 기존 완료 fast path 복원 변경 때문에 관련 테스트와 최종 전체 검증을 다시 실행했다. 기대값·timeout·fixture 조건을 완화하지 않았다.

|label|main tests / 실패 / skip|최종 branch tests / 실패 / skip|
|---|---:|---:|
|test|2921 / 3 / 19|3035 / 0 / 19|
|contractTest|91 / 0 / 0|91 / 0 / 0|
|fakeBackendTest|3 / 0 / 0|3 / 0 / 0|
|integrationTest|74 / 0 / 7|74 / 0 / 7|

main/branch format와 Java/Kotlin API snapshot 모두 통과. main 대비 새 실패 0. 로그와 XML은 logs/main-* 및 logs/branch-final-*에 보존했다.

남은 기능 실패: main STREAM 3건을 미해결 결함으로 기록한다. branch 통과를 수정 근거로 사용하지 않는다.
- ZLinkStreamActorRegistryTest.java:201: actors 비어 있음이 관찰된 뒤 unbound 알림이 아직 없다. ZLinkStreamActorRegistry.java:106–113에서 상태 제거 뒤 lock 밖 publishUnbound가 실행되는 경계를 확인했다. 이번 수정 함수 밖이므로 수정하지 않았다.
- ZLinkStreamLifecycleRaceTest.java:194의 receiveFailure/serverClosing 2건: race breakpoint 이전 connect가 timeout. ZLinkStreamConnectionLifecycle.java:280/315/338에서 timer 실패와 CONNECT_TIMEOUT 변환 확인. 연결 정체의 근본 원인은 미확정이며 부하 탓으로 단정하지 않는다. 재실행 통과로 결함을 취소하지 않았다.

## 측정

q1673d harness·binding 1.19.0 동일. binding jar SHA256 aa68fdb1c91dea248d4d2d1a497d916f945fb786fefc083d3d582addd9c79c3b. 1024 B, JVM warmup 30초 + active 5초, 848ce30/reference → 수정 전 HEAD/before → 수정 후/after 교대 3회 중앙값. flock /tmp/zlink-1639-measure.lock 사용. CPU 18셀·serial JFR(source/target) 18셀 유효, 모든 received/completed 일치·오류 0·JFR DataLoss 0.

측정 중 자체 빌드/테스트 없음. 외부 빌드/테스트는 시작 전 검사와 1초 간격 감시로 기록했다. 간섭한 셀은 invalid-environment 또는 contended-first로 별도 보존하고 제외했다. 수치가 아닌 외부 프로세스 탐지로 제외했으며 정상 셀은 다시 고르지 않았다. CPU 분포 변동이 커도 아래 수락 실패 판정은 유지한다.

|pattern/version|source CPU µs/msg|target CPU µs/msg|
|---|---:|---:|
|send/reference|27.539|21.166|
|send/before|24.809|17.144|
|send/after|27.578|17.731|
|serial/reference|175.730|173.813|
|serial/before|175.936|169.000|
|serial/after|263.725|217.666|

serial source/target ≤848ce30, send ≤before 모두 미충족. CPU 개선 완료로 판정하지 않는다.

|serial/version|source objects/msg|source bytes/msg|target objects/msg|target bytes/msg|
|---|---:|---:|---:|---:|
|reference|279.611|14051.291|365.281|18691.911|
|before|285.379|14297.845|371.039|18818.159|
|after|275.019|13579.037|343.742|14724.905|

before 대비 source bytes -5.03%, target bytes -21.75%. source stage/msg는 26.041→26.922(게시 분리 비용), target 15.000→15.001. 할당 감소를 CPU 개선으로 환산하지 않았다.

JFR은 -XX:-UseTLAB 및 ObjectAllocationOutsideTLAB으로 stack·객체 수를 수집했다. JFR 계측값과 CPU값은 별도 실행이다. 순위는 가장 가까운 Framework stack 위치별 객체 수/msg 3회 중앙값이며 binding 자체 할당 stack은 제외했다. serializer가 호출한 Jackson/protobuf 비용은 포함한다. line은 수정 전 HEAD 기준이다.

### source Framework 소유 상위 10곳

|순위|file:line (function)|objects/msg|bytes/msg|
|---|---|---:|---:|
|1|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/ZLinkCompletionBridge.java:39 (forwardCancellation)|19.999|575.952|
|2|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:340 (decodeHeader)|16.000|1200.000|
|3|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:938 (decodeText)|6.000|368.000|
|4|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/ZLinkCompletionBridge.java:24 (forwardCancellation)|6.000|95.997|
|5|framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:97 (parseProtobuf)|5.000|1248.000|
|6|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:568 (decodeHeader)|4.000|128.000|
|7|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:558 (decodeHeader)|4.000|96.000|
|8|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:563 (decodeHeader)|4.000|112.000|
|9|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:939 (decodeText)|4.000|136.000|
|10|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:573 (decodeHeader)|4.000|144.000|

### target Framework 소유 상위 10곳

|순위|file:line (function)|objects/msg|bytes/msg|
|---|---|---:|---:|
|1|framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:288 (create)|14.000|576.000|
|2|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:1282 (scheduleDrainIfNeeded)|9.000|376.000|
|3|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:1290 (drainScheduled)|9.000|368.000|
|4|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:340 (decodeHeader)|8.000|600.000|
|5|framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:289 (create)|7.000|184.000|
|6|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:938 (decodeText)|6.000|368.000|
|7|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/handlers/ZLinkHandlerMethodInvoker.java:195 (invoke)|5.022|214.049|
|8|framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:249 (springInvokesClose)|5.000|384.000|
|9|framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:97 (parseProtobuf)|5.000|1248.000|
|10|framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:939 (decodeText)|4.000|136.000|

남은 비용 후보: CompletionBridge.java:39/24의 cancellation observer, ChannelEnvelope.java:340의 header decode, SpringHandlerFactory.java:288/289의 activation, SerialExecutionQueue.java:1284의 worker closure. JFR 실행 표본이 적어 이 위치들을 CPU 회귀 원인으로 확정하지 않았다. 기존 owned protobuf writer와 heap writer를 분리 측정했고 direct writer가 약 2배 빨랐으므로 더 느린 heap 대안을 적용하지 않았다(로그 writer-costs.log). CPU 회귀의 지배 원인은 미확정이다. 추가 CPU 목표 수정·수락은 남아 있다.

## 변경 파일

- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelHandlerInvoker.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/configuration/ZLinkCodecRegistration.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkMessagePayloads.java
- framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotHandlerInvoker.java
- framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueueTest.java
- framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequestOwnerTest.java
- framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkChannelHandlerFilterContractTest.java
- framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/configuration/ZLinkCodecSelectionConformanceTest.java
- framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodecTest.java

## 보존과 정리

D:/t/jobs/fx/cx/q1673e/evidence.tar.gz에 로그·XML·CPU/JFR 원본·외부 간섭 기록·harness/scripts·계측 benchmark source를 보존한다. 838개 파일의 manifest SHA256과 압축본 Windows SHA256을 대조했다. 자체 실행 프로세스 0을 확인한 뒤 지정 WSL 사본을 삭제했다. 압축본 SHA256: 8bcbab174449ae9e978d92f258a26987bd0eb46cbb2c7fa3140094ddba00051b. Windows worktree는 커밋 뒤 clean이다.

리팩토링 점검: 성능 3건·POSDDD 2건·불필요 코드 2건 발견, worker 추가 전환·reply 사본·UTF-8 scratch 및 terminal/caller 게시 소유를 고치고 forwarding helper 두 개를 제거함, 넘긴 항목 main STREAM ZLinkStreamActorRegistry.java:113와 ZLinkStreamLifecycleRaceTest.java:194(근본 원인 미확정), CPU 목표 및 비용 후보 CompletionBridge.java:39·ChannelEnvelope.java:340·SpringHandlerFactory.java:288. CPU 수락 미완료.


