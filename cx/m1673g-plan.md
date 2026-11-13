# m1673g 계획 — #1673 Java/Kotlin 메시지당 비용

## 결론

현재 HEAD(`0f973e675a`, main `90aafafaff` merge 뒤)에서 Java 경로는 메시지당 예산(할당 ≤ 4, thread 전환 ≤ 0.1, lock ≤ 3)을 모든 셀에서 크게 넘는다. Framework frame에 귀속된 할당은 send source 약 71, send target 약 195, serial source 약 291, serial target 약 300 objects/msg다. thread 전환은 target의 virtual thread 생성(serial 1.6/msg)과 send source의 owner turn 대기 park(1.0/msg)가 가장 크다.

비용 항목 12개를 아래 순서로 제안한다. 전환 항목(J6·J8)을 먼저 두고, 나머지는 할당 수 순서다. 전환 1회는 park·unpark와 wake가 따르므로 할당 수십 개보다 비싸다. 승인 대상으로 추천하는 것은 J6(감독 결정 필요)·J2·J1·J3·J4·J11·J5·J7이다. J8·J12는 스펙 개정이 필요하고, J9·J10은 현재 계약 안의 비용이라 수정하지 않는다.

## main merge

- `git merge --no-ff origin/main`(90aafafaff) → 커밋 `0f973e675a`(본문 `Refs #1673`, Co-Authored-By 포함).
- 충돌은 `ZLinkStateLaneTest.java` 한 곳뿐이었다. 양쪽이 각각 시험을 추가한 충돌이다. 브랜치의 시험 3개(idle turn 할당 0, 회전 1회 drain, pending turn 수명)와 main의 #1745 시험(`pendingCompletionProgressesWhileObservationPoolIsBlocked`)을 모두 남겼다. 상수 두 개는 class 머리로 옮겼다.
- 본문 변경 `ZLinkStateLane.java:136`(main의 `handleAsync(..., DEFAULT_EXECUTOR)`)은 자동 병합됐다. 브랜치의 완료 fast path(`:131` `result.isDone()`이면 그대로 반환)와 의미가 충돌하지 않는다. pending 결과만 infrastructure executor에 게시한다는 0.30.1 수정이 그대로 유지된다.
- 확인: `ZLinkStateLaneTest` 27/27 통과(`m1673g/merge-statelane-test.log`). 전체 label 시험은 구현 단계 끝에 한 번 실행한다.

## 횟수를 센 방법과 한계

- harness는 q1673f의 `measure-cell.sh`를 복사해 이 worktree의 bench(`framework/bench/grpc/java`)를 가리키게 했다(`m1673g/harness/`). 1024 B, 같은 JVM warmup 30초 뒤 active 2초다.
- 할당은 `-XX:-UseTLAB`와 `jdk.ObjectAllocationOutsideTLAB`(stack 포함)로 전부 센다. thread 생성은 `jdk.VirtualThreadStart`로, park는 `jdk.ThreadPark`(threshold 0)로 센다. 집계는 stack에서 가장 가까운 Framework frame별 횟수를 완료 메시지 수로 나눈 값이다(`m1673g/harness/jfrcount/JfrCount.java`, 결과 `m1673g/logs/alloc-*.txt`). bench 자체 frame(StatsClient 등)은 뺐다.
- 시간 측정은 하지 않았다. 측정 시점에 다른 job의 C++ 빌드(load 17~20)가 돌았고, 처리량이 serial 205 msg/2s, send 2,969 msg/2s로 낮았다. 이 때문에 두 가지 한계가 있다.
  - JIT: warmup 동안 C2 컴파일이 다 끝나지 않았을 수 있다. escape analysis가 지우는 객체까지 셌을 수 있으므로 표의 값은 코드가 만드는 객체 수의 상한이다. 정수에 가까운 site(1.000, 2.000 …)는 메시지마다 실행되는 코드다.
  - 시간 구동 작업: liveness tick, `signalInfrastructure`, idle pump 회전처럼 타이머로 도는 작업은 처리량이 낮을수록 메시지당 값이 커진다. 이런 항목(J7)은 순위에서 낮게 보고 조용한 머신 값으로 다시 센다.
- context switch 수는 load에 따라 달라져 이 머신 상태로는 비교할 수 없다. q1673f의 조용한 측정(m1) 값을 참고로 쓴다. serial source 5.7/msg·target 4.7/msg, send source 0.37/msg·target 0.14/msg였다.
- uncontended monitor 획득은 JFR에 나오지 않는다. lock 수는 구현 단계에서 계측 패치(커밋하지 않음)로 센다. 아래 표의 lock 수는 코드를 읽어 센 값이다.

## 항목 표

열의 할당은 objects/msg이고 순서는 serial source / serial target / send source / send target이다. "허용 표" 열은 #1639 10-13 댓글의 메시지당 허용 작업 표와 대조한 결과다.

| # | 항목 | 할당·전환 | 허용 표 | 소유 스펙 | 분류 | 스펙 개정 |
|---|---|---|---|---|---|---|
| J6 | target drain마다 virtual thread 1개 | vthread 1.6 / 0.15(target), SEQ 25 / 14, ThreadLocal entry 14.6 / 9.4 | 위반: 메시지별 thread 전환·task | 08 §4.1 I4·§4.3, 02 §2 | B | 아니오. 감독 결정 필요 |
| J8 | send caller의 owner turn 대기 park | park 1.0/msg(send source) | 위반: 메시지별 thread 전환 | 08 §3 E2–E4, 06 「반환 전 완료 보장」 | D | 예 |
| J5 | target 수신·reply 경로의 port·binding 객체 | 23.5+7 / 49.8+17 / 4.1 / 21.1+12 | 위반: 할당 | 08 §4.1 I2–I4·§4.3 W4 | B | 아니오(binding API는 확인) |
| J4 | M6A service wire header의 decode·encode | reader 13 / 13 / 0 / 13, writer 24 / 39 / 21 / 0 | 위반: send header 미리 준비 안 함, header 해석에 문자열 할당 | 08 §3 E1, §4.1 I3 | B | 아니오 |
| J2 | dispatch마다 Spring activation과 instance owner | 0 / 58 / 0 / 53 | 위반: 생성 절차 재사용 안 함 | 08 §4.3 DI 단락, Framework API 「10」·「11」 | B | 아니오 |
| J1 | request마다 겹겹의 future·취소 전달 | 70 / 0 / 9 / 0(CF 24, UniWhenComplete 14 /msg serial source) | 위반: 메시지별 future | 08 §3 E3–E5, 01 「10」·「11」 | B | 아니오 |
| J3 | envelope header JSON decode(reply에서 2회) | 38 / 22 / 0 / 15 | 위반: header 해석 1회 초과 | 08 §4.1 I3, 05 「6」 | B | 아니오 |
| J11 | ingress permit을 record마다 예약 | 0 / 12 / 0 / 13 | 위반: permit 예약·반환 | 08 §4.1 I1, 04 「3」 | B | 아니오 |
| J7 | pump 회전마다 lane 진입 3회·관리 작업 | lane 진입 약 3/회전, 관리 할당 30 / 22 / 2 / 6(시간 구동) | 회전당은 허용, 변경 없는 관리 작업 재실행은 낭비 | 08 §4.1 I0·§4.2 | B | 아니오 |
| J9 | protobuf encode·decode 내부 할당 | 12 / 28 / 6 / 5 | 허용(encode·decode 각 1회) | 05 「6」 | — | 아니오 |
| J10 | flow id 생성·설치 | 10 / 5 / 5 / 8 | 기본 진단 수준은 Off가 아님 | 27 §4 | — | 감독 결정 |
| J12 | §4.4 수신 역할 인계 | target I4 1회, source E5 1회 | 스펙 허용 상한(각 1) 안 | 08 §4.4·§6 | D | 예(§6 표) |

## 항목별 내용

### J6 — target drain마다 virtual thread 1개

- 위치: `ZLinkSerialExecutionQueue.java:1280–1300`(drain closure)와 `:1303–1320` `submitWorker`. executor는 `ZLinkFrameworkRegistration.java:50,52`의 `newVirtualThreadPerTaskExecutor()`다. owner queue가 비었다 차는 serial에서는 메시지마다 새 virtual thread가 생긴다. 새 thread는 ThreadLocalMap과 entry(`CURRENT`·`CURRENT_GATE`·flow·application job context)를 다시 만들어 entry가 serial 14.6/msg 생긴다. `ZLinkStateLane.isOnLane:71`의 2/msg도 같은 원인이다. `ZLinkProcessExecutionLanes.java:22–24`의 `APPLICATION`도 thread-per-task 구조다.
- 위반: 08 §4.3은 "요청마다 task·future·virtual thread를 만들어 … 위반"이고 "공유 worker의 drain을 재사용한다"를 요구한다.
- 교차언어: C++ `serial_execution_queue.cpp:1306–1337`은 고정 worker executor에 `drain_loop`를 제출한다. .NET `ZLinkSerialExecutionQueue.cs:1137–1151`은 ThreadPool에, Node `serial-execution-queue.ts:278–291`은 microtask에 게시한다. 세 언어 모두 기존 worker를 재사용하고 실행 단위를 새로 만들지 않는다.
- 수정안: 준비된 owner 집합을 소비하는 고정 수 application worker로 drain을 실행한다. 기존 `serialExecutor`·`handlerExecutor`를 이 worker 집합 하나로 바꾸고 `submitWorker`의 호환 분기(`DRAIN_EXECUTOR`)는 지운다. 판정 위치는 3(serialExecutor, handlerExecutor, APPLICATION)에서 1로 준다.
- **감독 결정 필요**: thread-per-task executor를 고정 worker 집합으로 바꾸는 것이 공통 원칙의 "새 pool 금지"에 걸리는지 판단이 필요하다. 기존 executor를 대체하는 것이라 자원 수는 늘지 않는다. 다만 worker 수 상한을 정해야 하고, handler가 blocking하면 다른 owner가 그 상한 뒤에서 기다린다. 08 §4.3 "다른 gate의 turn은 여러 worker에서 나란히"를 지킬 상한 값을 정해야 한다.
- **ZLinkSerialExecutionQueue.java 변경 표시**: `:1280–1320`, `:1844–1909`(`invokeInline`). 0.30.1(#1699·#1745 Java)이 이 파일을 계속 고치므로, 구현 전에 그 PR의 머지 상태를 확인하고 다시 merge한다.

### J8 — send caller의 owner turn 대기 park(스펙 개정 필요)

- 위치: `ZLinkChannelSocketRegistry.java:591–599` `nodeSubmission`(`owner.awaitTurn`)과 `:601–640` `selectAndSubmit`(`:628` `stateLane.awaitTurn`). 그 아래 `ZLinkStateLane.awaitTurn:99–105`가 `ZLinkCompletionBridge.await:34`에서 park한다. send-saturation(caller 8개)에서 send마다 1.004회 park한다. serial에서는 0.137회다.
- 원인: 08 §3 E2–E4는 owner turn 안에서 동기로 등록·submit하고, 06 「반환 전 완료 보장」은 caller가 반환을 관찰하기 전에 등록이 끝나 있기를 요구한다. caller가 여럿이면 lane이 바쁜 동안 나머지 caller가 기다려야 하고, 기다리면 park가 생긴다. 구현만으로는 0.1 아래로 내릴 수 없다. lock으로 바꾸어도 대기는 같고 새 lock은 금지다.
- 교차언어: .NET `ZLinkStateLaneWait`(동기 대기), C++ node operation owner의 동기 turn. Node는 실행 자원이 하나라 이 대기가 없다.
- 제안 문안(08 §3 E3 "그래서" 열에 추가): "등록 완료 보장의 기준은 caller가 제출 결과(완료·취소·admission)를 관찰하는 시점이다. owner가 바쁘면 runtime은 E3·E4를 owner의 FIFO에 넣고 pending 결과를 바로 반환할 수 있다. 이 결과의 어떤 관찰도 등록보다 먼저 일어나지 않는다." 06 「반환 전 완료 보장」에도 같은 기준을 적는다.

### J5 — target 수신·reply 경로의 port·binding 객체

- 위치: `ZLinkJavaRawServicePort.java:380–422` `receiveOnLane`. 여기서 `Received`, frames용 `AbstractList`, `Inbound`, `Optional`, routing id lambda가 생기고, 같은 frame을 `toByteArray`로 복사하는 view(`:408–411`, 2/msg)도 있다. `:325–345` `replyMessages`는 binding builder(`router.reply(..).message(..).submit()`) 때문에 18/msg다. 그 밖에 `:171` `claimMessages`, `ZLinkJavaRawMeshNode.java:4941–5031` dispatch record·reply closure(12–17/msg)가 있다. Framework frame 아래 `NativeMemorySegmentImpl` 27/msg, `DirectByteBuffer(R)` 21/msg는 binding Message↔native segment 변환이다.
- 수정안: (a) Framework는 application decoder가 borrowed `received.parts()`만 쓰게 하고 `toByteArray` view를 control 경로로만 한정한다. (b) reply·request submit을 builder 없이 parts 목록을 한 번에 넘기는 binding 진입점으로 바꾼다(`bindings/java`). (c) `Inbound`와 `ZLinkMeshDispatchRecord`의 중복 소유 객체를 하나로 합친다.
- 확인 필요: (b)는 binding 공개 API를 더한다. `bindings/doc/spec`에 같은 진입점이 있는지 구현 전에 확인하고, 없으면 그 부분은 스펙 개정으로 보고한다.
- 교차언어: C++ `raw_mesh_node_owner.hpp:467–480`(node별 port·poller, native message 그대로 이동), .NET `ZLinkManagedMeshNode.cs:5446` 회전 단위 `ReceiveRawBatch`, Node `node-raw-mesh-backend.ts:385`.

### J4 — M6A service wire header의 decode·encode

- 위치: decode는 `ZLinkServiceM6AWireCodec.java`의 `Reader.<init>:801`, `text8:876`, `bytes:893`, `buffer:904`, `decodeText:930,933`이다. channel·message 이름을 메시지마다 `String`으로 만든다(13/msg). encode는 `:342,346` `encodeFrameworkMultipartMessage`, `:419` `writeMultipart`, `:692` `Writer.<init>`이고, `ZLinkJavaRawMeshNode.java:4446` `encodeApplicationFrames`, `ZLinkChannelEnvelope.java:846` `HeaderWriter.encode`도 있다(source 21–24, target reply 39/msg).
- 위반: 허용 표의 send는 "미리 준비한 target·header로 encode 1회"인데, 지금은 header를 메시지마다 처음부터 쓴다.
- 수정안: (a) send·request header의 고정 부분(target·channel·message·content type)을 call 준비 때 bytes로 만들어 두고, 메시지마다 바뀌는 correlation·deadline만 쓴다. (b) decode는 등록 때 준비한 이름 표(UTF-8 bytes → 등록된 `String`)와 비교해 기존 `String`을 돌려준다. 이 표는 등록이 소유하는 불변 표이며 cache를 새로 두는 것이 아니다. malformed 판정(q1673e의 canonical UTF-8 검사)은 표에 없는 이름에만 적용한다.
- 교차언어: Node `service-wire-m6a-codec.ts:74–145`, C++·.NET의 M6A codec. 구현 단계에서 각 언어의 header 준비 위치를 file:line으로 대조한다.

### J2 — dispatch마다 Spring activation과 instance owner

- 위치: `ZLinkSpringHandlerFactory.java:284–290`(`newInstance`, `autowireBean`, `initializeBean`, 14+7+2/msg), `:241–262` `springInvokesClose`(destroy마다 `getDeclaredMethods`로 reflection 재탐색, 3–5/msg), `:227–228` `destroyBean`, `:266–269` `SpringActivation`의 `LinkedHashMap`·`ArrayList`. `ZLinkHandlerInstanceOwner.java:16–19,31–41,48–53`은 dispatch마다 owner와 `ZLinkStateLane`(`:19`)을 만든다(15/msg).
- 위반: 08 §4.3 "생성 절차를 미리 결정해 재사용한다 … reflection metadata로 dependency 연결을 다시 짜지 않는다"에 걸린다. close 판정을 메시지마다 reflection으로 다시 하는 것이 그 경우다.
- 수정안: `HandlerPlan`(`:420–470`)에 close 정책(`springInvokesClose` 결과)과 post-processing 필요 여부를 등록 때 계산해 둔다. constructor 호출은 `MethodHandle`로 준비한다. `SpringActivation`의 map·list는 첫 dependency를 만들 때 만든다. dispatch마다 새 scope·instance를 만드는 계약(Framework API 「10」·「11」)은 바꾸지 않는다.
- instance owner의 lane은 .NET `ZLinkScopedHandlerInstanceOwner.cs:26`도 같은 구조다(dispatch scope마다 lane). Java만 지우면 언어별 예외가 되므로 이번 수정에서 지우지 않는다. 공통으로 줄이려면 별도 항목으로 올린다.
- 교차언어: .NET `ZLinkScopedHandlerInstanceOwner.cs:24–49`(등록 때 만든 activation factory 재사용), Node `handler-instance-scope.ts:154`, C++ `route_channel_registration.cpp:144` `create_handler_registry`.

### J1 — request마다 겹겹의 future·취소 전달

- 위치(source serial). 결과 future는 `ZLinkChannelRouteCalls.java:512–516` `requestResult`(metric `whenComplete`)와 `:449–505` `submitNode`(`result` + `forwardCancellation` + `whenComplete`)에서 생긴다. 이어서 `ZLinkChannelCallRuntime.java:236–257` `preserveCurrentFlow`(flow가 켜지면 future 하나 더), `ZLinkJavaRawMeshNode.java:4373` `mapRequestFailure`, `ZLinkServiceOperationRegistry.java:95–117`(`:116` 취소 전달)을 거친다. 그 아래에 `ZLinkJavaRawServicePort.java:282–297`(`completion` + `whenComplete` + 취소 전달)과 `ZLinkJavaSocketSupport.java:71–93`이 있다. 마지막으로 `ZLinkRequestStages.java:35–44` `observe`(`whenComplete` + 취소 전달 + `manageCurrent`)와 `ZLinkChannelRouteCalls.java:383` `manageCurrent`가 붙는다. `ZLinkCompletionBridge.java:57–61` `forwardCancellation`만으로 20/msg다.
- 위반: 허용 표는 E4 결과 객체 1개와 E5 dispatcher 전달 1회다. 지금은 계층마다 future를 새로 만들고 취소 전달을 다시 건다.
- 수정안: operation registry의 pending entry(`ZLinkServiceOperationRegistry.java:418` `Operation`, 이미 `CompletableFuture`다)를 caller가 받는 결과 하나로 쓴다. 계층들은 entry에 decode·discard 함수를 넘기고 future로 다시 감싸지 않는다. 취소는 caller view에서 entry로 한 번만 전달한다. metric과 flow 복원은 entry의 terminal 처리 한 곳에서 한다. 판정 위치: terminal 결과 변환 5곳(`requestResult`, `submitNode`, `preserveCurrentFlow`, `mapRequestFailure`, `requestMessages`)을 1곳(entry terminal)으로 모은다.
- 교차언어: .NET `ZLinkMeshCompletionTable.cs:37` `Register(correlationId, handler)`와 `:161` 취소 등록 1회, C++ `operation_registry.cpp:260` `register_operation`과 `:312` `complete`, Node `operation-registry.ts:50` `register`와 `:131` `take`. 세 언어 모두 pending entry 하나가 terminal을 받는다.
- 주의: `manageCurrent`(`ZLinkSerialExecutionQueue.java:1912–1945`)는 handler 안의 caller에만 붙는다. bench caller에는 할당이 없다. 이 메서드는 J1에서 바꾸지 않는다.

### J3 — envelope header JSON decode(reply에서 2회)

- 위치: `ZLinkChannelCallRuntime.java:272` `isFrameworkErrorReply`가 `ZLinkFrameworkErrorReply.java:49–52` `tryDecodeHeader`로 header를 한 번 읽는다. 이어서 `ZLinkChannelReplyDecoder.java:23–25`가 같은 header를 또 읽는다. 오류 reply이면 `:274–277`에서 kind·message·metadata를 얻으려고 세 번 더 읽는다. decode 한 번에 `ZLinkChannelEnvelope.java:339–341`의 Jackson parser와 `MessageInputStream`, 필드 `String`(`:534–573`, `:709`)이 생긴다.
- 위반: 허용 표의 "header 해석 1회"를 넘는다.
- 수정안: (a) `completeReply`에서 header를 한 번 decode하고, 그 `Header`를 오류 판정과 `replyDecoder`에 넘긴다. (b) parser를 `byte[]`/segment 위에서 직접 만들어 `MessageInputStream`을 없애고, 이름 필드는 J4(b)와 같은 등록 이름 표로 받는다. JSON wire 형식은 바꾸지 않는다(#1643의 형식 변경은 이 job 범위 밖이다).
- 교차언어: C++ `channel_runtime.cpp:1839`(reply header 1회 decode), `envelope_codec.cpp:804`, .NET `ZLinkEnvelopeCodec.cs:332,646`, Node `channel-envelope.ts:841`.

### J11 — ingress permit을 record마다 예약

- 위치: `ZLinkJavaRawMeshNode.java:4611–4660` `drainIngressBatch`가 record마다 `acquireOrResume`(`:4617–4622`)을 부르고 `abandonReservation`을 한다. `ZLinkApplicationJobQueue.java:180,601`, `ZLinkApplicationJobContext.java:18–19,55–79`에서 12–13/msg가 생긴다. 같은 파일에 회전 단위 `acquireBatchBlocking`(`:331`)이 이미 있다.
- 위반: 08 §4.1 I1은 "이 회전의 permit 예산을 확보"이고, 허용 표는 permit 예약·반환을 금지한다.
- 수정안: 회전 시작에 min(64, 남은 permit)을 한 번에 얻고 record에는 예산을 나누어 준다. 남은 예산은 회전 끝에 한 번 반환한다.
- 교차언어: .NET `ZLinkManagedMeshNode.cs:5446` `TryAcquireBatch`(회전 단위), C++ `mesh_node_host_service.cpp:2601–2606` `application_permit_budget`, Node `application-ingress-record-owner.ts:49`.

### J7 — pump 회전마다 lane 진입 3회·관리 작업

- 위치: `ZLinkJavaRawMeshNode.java:4583–4603` `pumpTurn`. 회전마다 `pump.execute`로 자신을 다시 제출한다(single-thread executor queue lock 2회). `drainPeerCloseRequests`, `announceExpectedPeers`(`:7226`, 3/회전), `tickLiveness`가 회전마다 돈다. port 쪽은 `ZLinkJavaRawServicePort.java:366–378`(`waitForReady`·`receiveNow`·`waitForReadable`)이 회전마다 `inStateLane`(`:477–478`)에 약 3회 들어간다. `ZLinkJavaSocketReceivePoller.java:94–100`은 `synchronized`이고 source serial에서 13/msg를 만든다.
- 판단: 08 §4.2는 관리 작업을 "회전당 한 번"으로 정하므로 위반은 아니다. 하지만 serial은 회전 하나에 record 하나이므로 그대로 메시지당 비용이 된다. 변경이 없는 회전에서 다시 계산하는 관리 작업은 지워도 되는 비용이다.
- 수정안: pump를 한 task 안의 loop로 바꿔 회전마다 다시 제출하지 않는다. 관리 작업은 deadline이 지났거나 변경 알림이 온 회전에서만 한다(I0 대기 상한 = 다음 deadline, §4.2 그대로). 같은 pump가 소유한 poller 조회는 lane 진입을 회전당 1회로 묶는다.
- 교차언어: C++ `raw_mesh_node_owner.hpp:467–480`, .NET `ZLinkManagedMeshNode.cs:5435–5455`, Node `node-raw-mesh-backend.ts:385`.

### J9 — protobuf encode·decode 내부 할당(수정 안 함)

`ZLinkProtobufMessageSerializer.java:35,38`(`serializeOwned`), `:76`(`deserializeBorrowed`), `:97`(`parseProtobuf`). encode·decode 각 1회 안의 할당이고 direct writer가 heap writer보다 빠르다는 q1673e 측정이 있다. 허용 표 안이므로 이번 수정에서 제외한다.

### J10 — flow id 생성·설치(감독 결정)

`ZLinkFlowContext.java:77–90,206–216`. 메시지마다 UUIDv7 문자열을 만들고 ThreadLocal scope를 설치한다. 27 §4는 Off를 뺀 모든 수준에서 capture를 요구하고, bench는 기본 수준(Off 아님)으로 돈다. "trace off = 비용 없음"은 Off에만 적용되므로 지금은 계약 안의 비용이다. 측정을 Off로 맞출지(q1673d는 Off로 맞췄다), 기본 수준의 비용으로 볼지 감독이 정한다.

### J12 — §4.4 수신 역할 인계(스펙 개정 필요: §6 표)

- 현재 Java: target ingress는 전용 platform thread pump(`ZLinkJavaRawMeshNode.java:4569–4579`)이고 I4 turn은 J6의 worker가 실행한다(전환 1회). source E5는 `ZLinkServiceCompletionDispatcher.java:82`의 platform worker가 실행한다(park 1/msg, 전환 1회). 둘 다 §3·§4.1의 상한(각 1회) 안이다.
- §4.4의 인계 성공 조건은 "지금 비어 있는 자원을 다음 수신 역할에 독점 배정"이다. Java의 실행 자원은 thread-per-task라 비어 있는 자원 집합이 없다. 그래서 지금은 "배정을 보장할 수 없으면 인계하지 않는다" 분기에 해당하며 계약을 지킨다. 인계를 실제로 하려면 J6의 고정 worker 집합이 먼저 있어야 하고, pump·completion worker·application worker의 배치 소유자를 하나로 모아야 한다(q1645r의 3→1). 따라서 J6 승인 뒤 별도 설계 항목으로 둔다.
- 스펙 개정 필요: 08 §6 표의 Java ingress owner는 "virtual-thread pump"로 적혀 있다. 구현은 측정 근거(`ZLinkJavaRawMeshNode.java:4571–4576` 주석, Issue #75)를 가진 platform thread다. 제안 문안: "Java | node별 전용 platform thread pump | application worker | 공유 completion dispatcher의 platform worker thread".
- 예산 충돌: 브리프 예산의 thread 전환 ≤ 0.1은 스펙 상한(I4 1회, E5 1회)보다 엄격하다. serial 왕복에서는 인계가 성공해야만 0.1에 다가간다. 이 차이를 예산으로 둘지 스펙에 반영할지 감독이 정한다.

## 스펙 개정 필요 목록

| 항목 | 위치 | 근거 | 제안 |
|---|---|---|---|
| J8 | 08 §3 E2–E4, 06 「반환 전 완료 보장」 | caller가 여럿이면 동기 owner turn이 caller마다 park 1회를 만든다(send source 1.0/msg) | 등록 보장의 기준을 "결과 관찰 시점"으로 옮기는 문안(J8 절) |
| J12 | 08 §6 표 Java 행 | 표는 virtual-thread pump, 구현은 platform thread pump | J12 절 문안 |
| J12 | 브리프 예산 thread 전환 ≤ 0.1 vs 08 §3·§4.1 상한 1 | serial에서 인계 없이는 도달할 수 없다 | 감독 결정 |

## 감독 결정 필요

1. J6: thread-per-task executor를 고정 application worker 집합으로 바꾸는 것을 허용할지와 worker 수 상한.
2. J10: 측정과 예산 판정을 진단 Off로 할지.
3. J5(b): binding에 builder 없는 submit 진입점을 더하는 것(`bindings/java`). bindings spec에 없으면 스펙 개정으로 돌린다.

## 구현 순서 제안(승인 뒤)

J2 → J3 → J11 → J1 → J4 → J5 → J7 → J6 순서로 진행한다. 앞의 네 항목은 한 계층 안에서 끝나는 작은 변경이다. J6은 `ZLinkSerialExecutionQueue.java`를 고치므로 0.30.1 머지 상태를 확인한 뒤 마지막에 한다. 항목마다 수정 전 횟수를 다시 세고(같은 harness·`counts.jfc`, 같은 active 길이), 수정한 뒤 다시 세어 표로 낸다. lock 수는 계측 패치로 함께 센다. 커밋은 항목당 하나다.

## 산출물

- 계획: `/root/jobs/fx/cx/m1673g-plan.md`(이 문서)
- harness·분석 도구: `/root/jobs/fx/cx/m1673g/harness/`(`measure-cell.sh` alloc 모드, `counts.jfc`, `jfrcount/JfrCount.java`)
- 원자료: `/root/jobs/fx/cx/m1673g/counts2/alloc/run1/`(JFR), 집계 `/root/jobs/fx/cx/m1673g/logs/alloc-*.txt`
- 첫 시도(`counts/`)는 JFR 설정이 적용되지 않아 event가 0이었다. 이 결과는 쓰지 않았다.

## 감독 결정 반영(구현 단계)

- 승인 순서: J2 → J3 → J11 → J1 → J4 → J7. 다른 job이 넘긴 Java 항목 셋을 J7 앞에 넣었다. (a) record마다 zero-timeout poll(merge 뒤 `ZLinkJavaRawMeshNode.java` `drainIngressBatch`의 `port.waitForReadable(pumpSocket, Duration.ZERO)`), (b) record마다 hex 파싱과 liveness 기록(`dispatch` 머리), (c) send마다 peer intent 전체 순회(`classifyNodeSendTarget`)다.
- J5·J8·J10·J12, 그리고 §6 표 정정은 보류한다. J10은 기본 수준(ERRORS)으로 판정하고, 횟수는 Off와 기본 둘 다 기록한다.
- J6(변경된 결정): 실행 자원은 virtual thread per task로 유지한다. 할 일은 serial target의 1.6 vthread/msg 원인을 file:line으로 찾는 것이다. 결과: drain 하나에 1개를 넘게 생기는 경로는 없다. merge 뒤 계측에서 이름 없는 virtual thread(serial executor의 drain)는 메시지당 정확히 1.0개다(710/710, 205/207). 나머지는 `zlink-java-channel-infrastructure-N`이다. 이것은 `ZLinkChannelRuntime.java:578–582`의 ClientServer liveness tick이 `:1594–1598` `scheduleInfrastructureAtFixedRate` → `:1601–1615` `signalInfrastructure`를 거쳐 `:141–143` thread-per-task executor에 게시하는 주기 작업이다. 시간에 비례하므로(초당 약 10개) 처리량이 낮은 셀에서 메시지당 값이 커졌을 뿐이다. `ZLinkSerialExecutionQueue.java`는 고치지 않는다.
- 진행 중 추가한 항목 J13(binding): virtual thread에서 닫힌 Message의 native slot이 그 thread의 ThreadLocal pool에 남아, thread가 끝나면 재사용도 close도 되지 않았다. target이 request를 하나 처리할 때마다 shared arena 4개가 새로 생기고 해제되지 않았다(`bindings/java/.../Message.java` `MessageSlotPool`·`MessageWrapperPool`). 공개 API는 바꾸지 않고, virtual thread에서는 thread-local pool을 쓰지 않게 고쳤다.
