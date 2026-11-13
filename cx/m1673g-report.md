# m1673g 완료 보고 — #1673 Java/Kotlin 메시지당 비용

## 결과

승인된 여섯 항목(J2·J3·J11·J1·J4·J7), 다른 job이 넘긴 Java 항목 셋((a)·(b)·(c)), 구현 중 찾은 binding 결함 하나(J13)를 각각 커밋 하나로 고쳤다. main은 두 번 merge했다(`0f973e675a`, `7c240c9672`, 본문 `Refs #1673`). J6은 바뀐 결정대로 원인만 확정했고 코드는 바꾸지 않았다. Java/Kotlin 전체 label은 3,361건 중 실패 0건이고, format과 Java·Kotlin API snapshot도 통과했다. 시간 측정은 하지 않았다.

Framework frame에 귀속된 메시지당 할당(objects/msg)은 다음처럼 줄었다. 기준은 merge 뒤 수정 전(`0f973e675a`), 최종은 `0db1cc211f`와 같은 코드다.

| 셀 | 수정 전 | 최종 | 예산 |
|---|---:|---:|---:|
| request-serial source | 291.3 | 184.8 | 4 |
| request-serial target | 353.2 | 269.2 | 4 |
| send-saturation source | 70.6 | 63.7 | 4 |
| send-saturation target | 195.5 | 171.0 | 4 |

예산(할당 ≤ 4)에는 여전히 크게 못 미친다. 남은 큰 몫은 맨 아래 「남은 예산 초과분」에 적었다.

## 계측 방법과 한계

- harness: `m1673g/harness/measure-cell.sh` alloc 모드. 1024 B, 같은 JVM warmup 30초, active 2초, `-XX:-UseTLAB` + `jdk.ObjectAllocationOutsideTLAB`(stack), `jdk.VirtualThreadStart`, `jdk.ThreadPark`(`counts.jfc`). 집계는 `harness/jfrcount/JfrCount.java`(가장 가까운 Framework frame별), `harness/jfrcount/JfrSite.java`(site별 할당 class)다. 결과는 `m1673g/logs/alloc-<단계>-*.txt`에 있다.
- 모든 셀이 다른 job의 빌드(load 10~60, procwatch FOREIGN 표시)와 겹쳤다. 처리량이 셀마다 크게 달라서(serial 150~1,614건/2초), 타이머로 도는 작업(liveness tick, infrastructure tick)은 처리량이 낮은 셀일수록 메시지당 값이 커진다. 그래서 판정은 해당 항목의 site 합계를 수정 직전 단계와 비교해서 했다. 셀 합계는 참고만 한다.
- uncontended monitor는 JFR에 나오지 않는다. 표의 lock 수는 코드를 읽어 센 값이고, 그렇게 적었다.
- J13의 send-saturation 셀은 load 60에서 warmup settle 상한에 걸려 무효였다. load < 20에서 같은 셀을 다시 쟀다(`retry-send.sh`).

## 항목별 전/후

| 커밋 | 항목 | 측정(셀) | 수정 전 → 후 |
|---|---|---|---|
| `6e7af1ebf1` | J2 Spring handler activation | Spring·instance owner site, serial/send target | 53.0 → 39.0, 48.2 → 37.1 obj/msg; activation monitor 1 → 0/msg |
| `ae55a812c2` | J3 reply envelope header 1회 해석 | decodeHeader site, serial source | 38.0 → 19.0 obj/msg; header JSON 해석 2 → 1회/reply(오류 reply 4 → 1) |
| `093075ff65` | J11 수신 회전 permit 예산 | host queue lock/record(코드 셈) | serial 4 → 3, send target 4 → 약 2.3(record의 0.71이 예산 permit) |
| `346242ca90` | J1 request terminal을 entry로 바로 | serial source CompletableFuture / UniWhenComplete / 요청 경로 site | 24 → 17, 14 → 9, 82 → 63 obj/msg |
| `370b978d5d` | J4 M6A profile·text 해석 | M6A codec site, serial src/tgt, send tgt | 33 → 22, 40 → 29, 11.7 → 6.0 obj/msg |
| `b6c6968494` | J13 binding slot pool(virtual thread) | serial target, virtual thread의 NativeMessageSlot·shared arena 생성 | 4.0 → 0/msg |
| `73613792b4` | (c) send 대상 분류 | classifyNodeSendTarget site, send/serial source | 1.0 → 0 obj/msg; intent 전체 순회 1 → 0/send |
| `d02f8a976b` | (a) record별 poll | target receive-poll site, serial/send | 3.0 → 2.0, 2.0 → 0.3 obj/msg; zero-timeout poll 1/record → 회전당 최대 1 |
| `d7469ecaf1` | (b) record별 hex 파싱·liveness 기록 | dispatch 머리 | record마다 1 → (회전·source당) 1 |
| `0db1cc211f` | J7 pump 관리 작업 | serial source/target: liveness tick, announce, pump 회전 | 7.2/7.2 → 3.1/3.1, 4.1/0 → 0/0, 2.0/2.1 → 1.0/1.0 obj/msg; 할 일이 없는 회전의 liveness lane turn 1 → 0 |

thread 전환(virtual thread 생성/msg, serial·send): source 0.94·0.09 → 0.11·0.04, target 1.60·0.15 → 1.07·0.05. target serial의 1.0은 drain당 virtual thread 하나다(J6 참조). send caller의 owner turn park(1.0/msg, J8)는 보류 항목이라 그대로다.

J10(flow ID): 최종 코드에서 기본 수준(ERRORS)과 Off의 `ZLinkFlowContext` site를 쟀다. serial source/target·send source/target 기준으로 기본 6.0/5.0/4.5/8.0, Off 0/1.0/0/0.1 obj/msg다(`alloc-off-*`, bench 임시 패치 `m1673g/bench-flow-off.patch`는 측정 뒤 되돌렸다). 판정은 기본 수준으로 한다.

## J6 원인 확정(코드 변경 없음)

- drain 하나에 virtual thread가 1개를 넘게 생기는 경로는 없다. serial target에서 이름 없는 virtual thread는 처리한 메시지 수와 같았다(205건에 207개, 710건에 710개). 이것은 `ZLinkFrameworkRegistration.java:52` executor에서 drain마다 생기는 thread다.
- 1.6/msg에서 나머지 0.6은 `zlink-java-channel-infrastructure-N` thread다. `ZLinkChannelRuntime.java:578–582`가 ClientServer liveness tick을 `:1594–1598` `scheduleInfrastructureAtFixedRate`로 등록하고, 주기마다 `:1601–1615` `signalInfrastructure`가 `:141–143` thread-per-task executor에 task를 게시한다. 초당 약 10개로 시간에 비례하므로, 처리량이 낮은 셀에서 메시지당 값이 커졌을 뿐이다. 이 경로는 RouteMesh 메시지와 무관하다. `ZLinkSerialExecutionQueue.java`는 건드리지 않았다.

## 변경 파일

- `framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`
  - `messaging/ZLinkFrameworkErrorReply.java`, `channels/ZLinkChannelRuntime.java`, `channels/ZLinkChannelCallRuntime.java`, `channels/ZLinkChannelReplyDecoder.java`, `channels/ZLinkSpotRouterNodeDispatcher.java`, `channels/ZLinkSpotRouteBridgeDispatcher.java`
  - `internal/dispatch/ZLinkApplicationJobQueue.java`
  - `internal/service/ZLinkServiceOperationRegistry.java`, `internal/service/ZLinkServiceM6AWireCodec.java`, `internal/service/ZLinkServiceLivenessRegistry.java`
  - `binding/ZLinkJavaRawMeshNode.java`, `binding/ZLinkJavaRawServicePort.java`, `binding/ZLinkJavaSocketSupport.java`
- 시험: `ZLinkApplicationJobQueueTest.java`(회전 예산 시험 추가), `ZLinkJavaRawServicePortContractTest.java`(지운 `mapRequestFailure`를 reflection으로 부르던 부분을 같은 실패 투영을 쓰는 `requestMessagesTo` 경로로 옮김. assertion은 바꾸지 않음)
- binding: `bindings/java/src/main/java/systems/zlink/contracts/messaging/Message.java`, 시험 `bindings/java/src/test/java/systems/zlink/contract/MessageMemoryContractTest.java`

## 실행한 시험

- 항목마다 관련 시험을 실행했고 모두 통과했다. 예: Spring starter 60건, Channel·SpotRoute·ErrorReply 283건, 요청 경로(OperationRegistry·RawServicePort·Submission·RawMeshNode·Channel·Durable·Socket) 941건, wire codec, pump·liveness·ingress 묶음.
- J13: 새 binding 시험이 수정 전에 실패하고(shared pool 4094 기대, 실제 4093) 수정 후 통과한다. binding 전체 181건 중 실패 0건, skip 1건이다. local package는 `scripts/local-package/java/build-wsl.sh`로 다시 만들었다(이전 jar는 `m1673g/binding-backup/`에 있다).
- 전체: `test contractTest fakeBackendTest integrationTest`(`m1673g/full/run-tests.sh`) 결과 tests=3361, skipped=28, failed=0이다(`m1673g/full/branch/summary.txt`). 이전 main 기준선(g1673m main) 3,131건·실패 0과 비교해 새 실패는 0건이다. 이 job에서 main을 다시 돌리지는 않았다. branch 실패가 0건이므로 main 대비 새 실패도 0건이다.
- `scripts/format/format.sh --check java`(google-java-format, ktfmt) 통과. `verify_api_snapshot.sh java`·`kotlin` 통과.
- main merge 뒤 관련 시험(StateLane, RawSpotNode, RemoteMulticast, ApplicationJobQueue, StatusPublisher, RawMeshNode, SpotActivation, MeshNodeIsolation, SourceTransportOwner)도 통과했다.

## 남은 실패

없음. 전체 label 실행에서 main STREAM 간헐 실패(ZLinkStreamActorRegistryTest:201, ZLinkStreamLifecycleRaceTest:194)는 나오지 않았다.

## 소유 계층·스펙 조항·교차언어·분류

- J2 — 소유: Spring 통합의 handler 생성 절차(등록 시 HandlerPlan). 조항: 08 §4.3 DI 단락, Framework API 「10」·「11」. 대조: .NET `ZLinkScopedHandlerInstanceOwner.cs:24–49`(등록 때 만든 activation factory), Node `handler-instance-scope.ts:154`, C++ `route_channel_registration.cpp:144`. 분류: B.
- J3 — 소유: RouteMesh reply 완료(`completeReply`). 조항: 08 §4.1 I3(header 해석 1회), 05 「6」. 대조: C++ `channel_runtime.cpp:1839`(reply header 1회), .NET `ZLinkEnvelopeCodec.cs:332`, Node `channel-envelope.ts:841`. 분류: B.
- J11 — 소유: host application job queue(예산 예약·반환), pump(회전). 조항: 08 §4.1 I1, 04 「3」. 대조: .NET `ZLinkManagedMeshNode.cs:5446` `TryAcquireBatch`, C++ `mesh_node_host_service.cpp:2601–2606`, Node `application-ingress-record-owner.ts:49`. 분류: B.
- J1 — 소유: operation registry의 pending entry(terminal 수신·취소 대상). 조항: 08 §3 E3–E5, 01 「10」·「11」. 대조: .NET `ZLinkMeshCompletionTable.cs:37`(handler 등록)·`:161`(취소 등록 1회), C++ `operation_registry.cpp:260`·`:312`, Node `operation-registry.ts:50`·`:131`. 분류: B.
- J4 — 소유: M6A wire codec. 조항: 08 §4.1 I3, 05 「6」. 대조: Node `service-wire-m6a-codec.ts:74–145`, C++·.NET M6A codec(text 해석). 분류: B.
- J13 — 소유: binding Message의 native slot 수명. 조항: binding async/ownership 계약(Message close 뒤 native 자원 반환). 대조: .NET binding도 thread-static Message pool이 있다(`bindings/dotnet/src/Zlink/Runtime/Messaging/Message.State.cs:11–13`). 다만 .NET의 실행 자원은 오래 사는 ThreadPool thread라 pool이 thread와 함께 사라지지 않는다. Java만 실행 자원이 drain마다 끝나는 virtual thread라서 생긴 결함이다. 규칙은 같다(닫은 slot은 재사용되거나 close된다). 분류: B.
- (a) — 소유: pump 수신 회전. 조항: 08 §4.1 I2. 대조: C++ `raw_mesh_node_owner.cpp:2647`(같은 결함, m1672c ③), .NET `ZLinkManagedMeshNode.cs:5435–5455`(회전 단위 batch). 분류: B.
- (b) — 소유: pump 수신 회전(liveness 기록). 조항: 08 §4.2. 대조: .NET `ZLinkManagedMeshNode.cs:5704–5716`(같은 결함, m1674c M3), 수정안 `ReceiveRawBatch` 끝에서 source별 1회. 분류: B.
- (c) — 소유: node send 대상 분류. 조항: 08 §3 E2(상수 시간 선택). 대조: .NET `ZLinkManagedMeshNode.cs:9833–9845`(m1674c M2). 분류: B.
- J7 — 소유: pump(회전 스케줄)와 liveness registry(due 시각). 조항: 08 §4.1 I0·§4.2. 대조: C++ `raw_mesh_node_owner.hpp:467–480`, .NET `ZLinkManagedMeshNode.cs:5435–5455`. 분류: B.

## 판정 위치와 상태 증감

- J1: request terminal 변환 위치 3곳(port completion, `mapRequestFailure`, registry 관찰)을 1곳(port `deliverTerminal` → registry terminal)으로 줄였다. 취소 전달 관찰자는 3곳에서 1곳(registry `Operation`의 취소 대상)으로 줄였다. `mapRequestFailure`는 지웠다. 더한 상태: `Operation.cancellationTarget` 필드 1개.
- J3: 오류 reply 판정 4회 → 1회. `ZLinkChannelRuntime`의 helper 4개를 1개로 바꿨다.
- J11: permit 예약 판정은 1곳(queue lock) 그대로다. 더한 공개 메서드 3개(`reserveRotationBudget`, `takeRotationPermit`, `abandonRotationBudget`). `Permit.state`를 volatile로 바꿨다.
- J2: close 정책 판정이 메시지마다 → 등록 시 1회. `SpringActivation`의 monitor를 지웠다(owner lane이 직렬화한다).
- J7: liveness due 판정은 registry 1곳에 있다. 상태 2개(`nextDueNanos`, `nextDueKnown`)를 더했다. pump task 필드 1개(`pumpRotation`)를 더했다.
- (b): pump 전용 상태 2개(`rotationReceivePeer`, `rotationReceiveGeneration`)를 더했다.
- J13: 판정 위치 수는 같다(pool 선택). 상태 증감 없음.
- 새 lock·queue·timer·retry·pool·cache는 없다. common pool에 새로 의존하는 코드도 없다(이 job 커밋의 `+` 줄에 `*Async(`·`commonPool`·`ForkJoinPool` 0건).

## 커밋

`0f973e675a`(merge), `6e7af1ebf1` J2, `ae55a812c2` J3, `7c240c9672`(merge), `093075ff65` J11, `346242ca90` J1, `370b978d5d` J4, `b6c6968494` J13, `73613792b4` (c), `d02f8a976b` (a), `d7469ecaf1` (b), `0db1cc211f` J7. (a)·(b) 커밋의 formatter 수정 두 줄은 J7 커밋에 함께 들어갔다.

## 남은 예산 초과분(file:line)

- target ingress 소유권 운반: `ZLinkApplicationJobContext.java:17–20,52–64`. record마다 State·QueuedOwnership·scope lambda와 `synchronized` 2회가 생긴다(약 12 obj/msg). 0.30.1(#1805)이 같은 파일을 고치고 있어 건드리지 않았다.
- Spring 내부: `ZLinkSpringHandlerFactory.java:284–290`의 `autowireBean`·`initializeBean`, `:227–228`의 `destroyBean`. dispatch마다 RootBeanDefinition·BeanWrapper·DisposableBeanAdapter가 생긴다(target 약 30 obj/msg). Spring lifecycle 계약을 바꾸지 않고 줄이려면 별도 설계가 필요하다.
- instance owner lane: `ZLinkHandlerInstanceOwner.java:19`(dispatch마다 `ZLinkStateLane`). .NET도 같은 구조라 공통 항목으로 올릴 대상이다.
- binding Message 생성: Framework frame 아래 `Message.from`/`dataBuffer` 1회마다 native segment·buffer view 3~4개가 생긴다(`ZLinkJavaRawMeshNode.java` `encodeApplicationFrames`, `ZLinkServiceM6AWireCodec.java` `writeMultipart`). J5(binding 진입점)와 함께 보류했다.
- source 수신 poller: `ZLinkJavaSocketReceivePoller.java:100` `poller.wait`가 호출마다 binding 객체 13개를 만든다(serial source 13 obj/msg). binding poller 쪽 비용이다.
- 보류 항목 그대로: J5(target port·binding 객체), J8(send caller owner turn park 1.0/msg, 스펙 개정), J10(flow ID, 스펙 후보), J12(§4.4 인계, serial 전환 1회), §6 표 Java 행.
- 시간 구동 virtual thread: `ZLinkChannelRuntime.java:578–582,1594–1615`. ClientServer channel이 없어도 주기마다 virtual thread 하나가 생긴다. 메시지당 비용은 아니다.

## 스펙 개정 필요

새 항목은 없다. 계획서의 J8·J12(§6 표)는 감독자 작업으로 넘어가 있다.

리팩토링 점검: 성능 11건·POSDDD 3건·불필요 코드 3건 발견. 고친 항목은 위 표의 열 항목이다. 불필요 코드로는 `mapRequestFailure`, `ZLinkChannelRuntime`의 오류 reply helper 4개, 대기 없이 완료되는 admission 관찰을 지웠다. POSDDD로는 terminal 변환·취소 전달·close 정책 판정을 각각 한 곳으로 모았다. 넘긴 항목: `ZLinkApplicationJobContext.java:17–64`, `ZLinkSpringHandlerFactory.java:284–290`, `ZLinkHandlerInstanceOwner.java:19`, `ZLinkJavaSocketReceivePoller.java:100`, `ZLinkChannelRuntime.java:578–582`.

## 감독자 리뷰 반영(J-F1·J-F2)

- `bff894b43c` J-F1: 대기 없이 바로 permit을 줄지("queue가 열려 있고, 먼저 온 waiter가 없고, 지금 빈 capacity가 있음")를 다섯 곳이 따로 판정했다. 리뷰가 짚은 `acquire`·`reserveAvailable`·`reserveRotationBudget` 셋에, 같은 판정을 하던 FIFO 합류의 대기 판단과 `acquireBatchBlocking`을 더한 다섯 곳이다. 이제 lock 안의 `admitsNowUnderLock` 하나가 판정하고, 예약 계수는 `reserveFreeUnderLock`이 한다. 판정 위치 5 → 1. 새 상태·lock은 없고 동작도 같다. waiter grant 경로의 과다 예약 방지와 예산 반환 시의 grant 판정은 다른 결정이라 그대로 두었다. 관련 시험(ApplicationJob*, RawMeshNode*, RemoteMulticast*)은 통과했다.
- `1ac52f3108` J-F2: `classifyNodeSendTarget`의 admitted fast path에 `!automaticNotRequiredPeers.containsKey(target)` 조건을 더해 이전 결정표로 되돌렸다. intent가 있으면 순회가 결정하고, intent 없이 not-required로 표시된 peer는 TARGET_NOT_FOUND다. 새 시험 `ZLinkJavaRawMeshNodeM6ATest.admittedInboundPeerMarkedNotRequiredIsNotFoundForNodeDirect`를 넣었다. local intent 없이 inbound로 admit된 peer를 not-required로 표시하면 TARGET_NOT_FOUND여야 한다. 수정 전에는 분류가 비어 `NoSuchElementException`으로 실패했고, 수정 후 통과한다.
- 전체 label 1차(`full/branch2`, 953초, load 20 이상)에서 3건이 실패했다.
  - `SpotActorTransferContractTest#[1] false`: `crossNodeDeferredJoinPreservesEmptyApplicationState`의 `:104 joinCompleted.await(3s)`.
  - `ZLinkStreamConnectorTest`의 TLS 2건: connect timeout.
- 조사 결과:
  - Spot 시험을 HEAD에서 따로 5회 돌리면 1회 실패한다. 수정 전 기준선(`7c240c9672`, J13 이전 binding jar)에서도 12회 중 1회 실패했다(65초 뒤 실패). 첫 parameter case는 통과할 때도 1.7~4.6초가 걸려 3초 대기에 근접하는 것이 원인이다. 두 시점의 소요 시간 분포는 같다. 따라서 이 job이 만든 새 실패는 아니지만, main에도 있을 간헐 결함이다. 근본 원인은 확정하지 못했고 재실행 통과로 결함을 취소하지 않는다.
  - Stream connector TLS 2건은 따로 3회 돌리면 64/64 통과한다. 이 모듈의 production 코드는 core·binding에 의존하지 않고 `zlink-framework-json-internal`만 쓴다. 원인은 확정하지 못했다.
- 전체 label 2차(`full/branch3`, 643초)는 tests=3362, failed=0이다. format(`--check java`)과 Java·Kotlin API snapshot도 통과했다.
- 기준선 비교용 임시 worktree(`/root/worktree/m1673g-base`)는 지웠다.

## PR 전 최종 수정·main merge

- `b71c6d3280` J-O1: `ZLinkStateLane.publishOutsideLane`의 executor 없는 `whenCompleteAsync`를 같은 class의 `DEFAULT_EXECUTOR`로 바꿨다. 새 helper나 executor는 없다. 이 브랜치가 main 대비 추가한 main 소스의 executor 없는 `*Async(` 호출은 이것 하나였다(`git diff origin/main...HEAD`로 확인).
- `b01df5ea00` J-T1: `ZLinkServiceOperationRegistryTest.concurrentTerminalProducersKeepTheirPublicationOrder`의 두 대기를 1초에서 5초로 늘렸다.
- `1d8712b619` merge origin/main(d2489ac50f, #1807·#1810·#1811). 텍스트 충돌은 없었다. 양쪽이 모두 고친 파일별 해소 내용:
  - `bindings/java/.../Message.java`, `MessageMemoryContractTest.java`: main #1811(#1808)이 이 브랜치의 b6c6968494와 같은 수정이라, merge 뒤 내용에는 차이가 없다.
  - `ZLinkJavaRawMeshNode.java`: main hunk(pump close를 `close()`로 바꾸고 `awaitExecutorTermination`를 삭제)만 들어왔다. 이 브랜치의 hunk(drain·liveness·announce·분류)와 겹치지 않는다.
  - `ZLinkJavaRawSpotNode.java`: main hunk(어느 Actor도 받지 않은 dispatch를 NotFound admission으로 처리)만 들어왔다.
  - #1810의 send admission 판정 위치(`ZLinkActorDispatchSerials`, queue commit)는 이 브랜치가 건드리지 않아 여전히 한 곳이다.
  - binding은 main pin 1.19.1(Maven Central)로 빌드·시험했다.
- merge 뒤 전체 시험(load 4.92에서 시작): tests=3376, failed=0(`/root/jobs/fx/cx/m1673g/full/merged/summary.txt`). format과 Java·Kotlin API snapshot 통과.
- 범위 밖 `framework/languages/unity/.../zlink-stream-connector.jspre` 변경은 되돌렸다(커밋하지 않음).
- push 했고, PR은 #1820(base main)이다.
