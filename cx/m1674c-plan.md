# m1674c 계획 — #1674 .NET 메시지당 비용

대상은 worktree `/root/worktree/zlink-1674-dotnet`, 브랜치 `framework-dotnet/1674-per-message-cost`, HEAD `90aafafaff`다.
이 문서는 진행 단계 1(계획)의 결과다. 코드는 바꾸지 않았다. 아래 경로의 접두사는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/`이다.

## 0. 범위와 근거

- 읽은 근거: `perf-common-brief.md`, `perf-HANDOFF.md` §0, `m1639d2-report.md`(남은 비용 순위·넘긴 항목),
  spec `08-messaging-hot-path.ko.md` §3·§4·§6, `01-submit-and-completion.ko.md` §4·§10·§11·§15.
- 추적한 경로는 benchmark가 쓰는 공개 API다(`framework/bench/grpc/dotnet/Client/Program.cs:327–342`).
  send는 `IZLinkRouteClient.SendToNode(...).Async()`, request는 `RequestToNode(...).Async<T>()`이며 target handler는
  `ZLinkServer/Program.cs:62–88`의 node route handler다.
- 아래 "메시지당 비용"은 **코드 읽기로 센 횟수**다. 할당 수의 확정값은 구현 단계의 수정 전 계측으로 낸다(§3).
  lane turn 하나는 비경합일 때 closure·delegate 2개와 `CurrentLane` AsyncLocal 설정·복원(ExecutionContext 2개 안팎)을
  만든다(`Execution/ZLinkStateLane.cs:65–110`). 경합이면 TaskCompletionSource와 mailbox 항목이 더해지고,
  `AwaitStateLane`(`ZLinkStateLane.cs:317`)으로 부른 곳은 호출 thread가 blocking 대기한다.
- 분류는 AGENTS.md의 A 계약 적응 / B 기존 결함 / C 우회 / D spec gap이다. "스펙 개정 필요" 항목은 D로 적는다.
- 0.30.1 겹침 표시: **#1769** = `Execution/ZLinkRuntimeTaskRunner.cs:115 RunLongRunning`과 그 호출자
  (`Channels/ZLinkChannelRuntimeManager.cs:112,176`의 ClientServer·subscriber 수신 루프, `Streams/ZLinkStreamNodeRuntime.cs:271,279`).
  **#1712** = relocation, `Service/ZLinkManagedMeshNode.cs`. 두 작업의 브랜치는 원격에 없어(`git ls-remote`에 spec 브랜치만 있음)
  겹침은 파일·함수 이름으로 판정했다. RouteMesh 수신 루프(`ZLinkManagedMeshNode.cs:5320 ReceiveLoop`)는
  `Task.Factory.StartNew`로 시작하며 `RunLongRunning`을 쓰지 않는다(`:438–443`).

## 1. 메시지당 경로 요약 (수정 전, 코드 읽기)

| 구간 | send 1건 | request 1건 | 예산 |
|---|---|---|---|
| runtime state lane turn (`GetMeshNodeRuntime`) | 2 | 2 | 0 (E2 선택 소유자 turn만 허용) |
| node `_lane` turn (peer·liveness 조회) | 1 | 2 | 0 |
| completion table `_gate` monitor | 3 | 0 | — |
| `_socketGate` monitor | 1 | 1 | — |
| Framework ThreadPool 게시(전환) | 1 | 0 (binding TCS 게시는 binding 몫) | ≤0.1 |
| async state machine box | 3 | 5 | — |
| 문자열 생성(`$"packet '…'"`) | 1 | 1 | 0 |
| target: node `_lane` turn / record | 2 | 2 | 0 (§4.2) |
| target: runtime state lane turn / record (inbound admission) | 1 | 1 | 0 |
| target: permit lane 작업 / record (MarkQueued·Release) | 2 | 2 | 허용 작업 표는 0, spec W3는 record마다 반환 |
| target: reply closure / request | — | 2쌍(4개) | 0 |

monitor 예산 3은 send source에서 table 3 + socket 1 = 4로 이미 넘고, thread 전환은 send마다 1로 예산 0.1을 넘는다.

## 2. 항목 (겹치지 않는 묶음을 앞에 두고, 묶음 안은 비용 순)

### 묶음 1 — #1769·#1712와 파일이 겹치지 않음

#### G1. Route client가 메시지마다 mesh node를 runtime state lane에서 두 번 찾는다
- 위치: `Channels/ZLinkRouteClient.cs:298`(send `GetMeshNodeRuntime`), `:303`(`EnsureKnownRouteMeshPeer`가 `Host/ZLinkFrameworkRuntimeChannels.cs:243–252`에서 다시 `GetMeshNodeRuntime`), request는 `:403`, `:421`.
  `GetMeshNodeRuntime`은 `Host/ZLinkFrameworkRuntimeSpots.cs:265–275`에서 `AwaitStateLane(state.RunStateAsync(...))`다.
  `:303`·`:421`의 `$"packet '{name}'"`는 실패 메시지용 문자열을 메시지마다 만든다.
- 비용/건: runtime state lane turn 2(비경합 할당 약 8, CAS 4), 경합 시 caller thread blocking 대기, 문자열 1.
- 위반: 08 §3 E2(:95, :126–127 "요청마다 … lane을 차례로 기다리는 구현은 위반"), 허용 작업 표 "메시지별 lane 합류".
- 다른 언어: Node `channels/channel-clients.ts:190–205`(mesh·target별 `PreparedNodeSend`를 한 번 만들어 재사용),
  Java `channels/ZLinkChannelRouteCalls.java:187`(`classifyNodeSendTarget`을 lane 없이 직접 호출),
  C++ `runtime/mesh/raw_mesh_node_owner.cpp:1302`(`_topology.peer` 직접 조회).
- 수정안: route client가 mesh 이름으로 찾은 `ZLinkSpotNodeRuntime`을 호출 객체 생성 때 한 번 얻는다(등록이 끝난 뒤 바뀌지 않는 SpotNodes를
  게시된 읽기 전용 보기로 읽는다). `EnsureKnownRouteMeshPeer`는 이미 있는 private overload(`ZLinkFrameworkRuntimeChannels.cs:268`)에
  그 node를 넘기고, 설명 문자열은 예외를 만들 때만 만든다. 판정 위치는 target 분류 1→1, mesh 해석 2→1이다.
- 분류 B. 스펙 개정 불필요. 겹침 없음.

#### G2. 수신 record마다 runtime state lane에서 inbound operation admission을 받는다
- 위치: `Spots/ZLinkMeshNodeRouteDispatcher.cs:540–544` → `Host/ZLinkFrameworkRuntime.cs:1059–1118`
  (`AdmitUnnestedInboundOperationAsync`가 `_stateLane.RunAsync` 한 turn, 성공 시 `ZLinkRuntimeOperationAdmission` 생성),
  `EnterInbound`(`:968–971`)가 lease를 만들고 `AmbientOperation` AsyncLocal을 설정·복원한다.
  같은 곳 `:488–490`의 `ZLinkApplicationJobQueueInvocation.Enter`(`Dispatch/ZLinkApplicationJobQueue.cs:1206–1212`)도 record마다 Scope 객체와 AsyncLocal 설정을 한다.
- 비용/record: state lane turn 1(할당 약 4), admission·lease 객체 2, AsyncLocal 설정·복원 2쌍(ExecutionContext 약 4).
- 위반: 허용 작업 표 "메시지별 lane 합류"; 08 §4.3 "record마다 별도 … supervisor 등록이 없다"(:223–227, :271–272)의 취지.
- 다른 언어: Java·C++·Node의 node route dispatch에서 같은 이름의 per-record runtime admission은 검색되지 않았다
  (Java `ZLinkMeshApplicationDispatcher.java`, C++ `runtime/channels`, Node `runtime/channels` 이름 검색). 구현 전에 drain seal 판정 위치를 세 언어에서 다시 대조한다.
- 수정안: drain seal·lifecycle 판정을 claim batch 하나(`DispatchBatchAsync`, `:269`)에 한 번 받고, 그 lease를 batch의 record들이 함께 쓴다.
  shutdown drain이 기다리는 대상은 batch 단위 lease가 된다. 판정 위치 1→1.
- 분류 B. 스펙 개정 불필요(08 §4.1 회전 단위와 같은 방향). 겹침 없음.

#### G3. Permit 상태 전이마다 lane closure와 async Task를 만든다
- 위치: lease 생성 `Dispatch/ZLinkApplicationJobQueue.cs:361`(record마다 1), `MarkQueued` `:452–453`(record마다 `_lane.TryPost` closure; 호출은 `Service/ZLinkManagedMeshNode.cs:11467`),
  handler 시작 반환 `:481–489`(`ReleaseAsync`가 `_lane.RunAsync` closure, 비동기 완료 시 Task box).
  같은 계열로 owner mailbox 넣기 `Service/ZLinkMeshOwnedMailbox.cs:76–90`(record마다 `_lane.TryPost` closure).
- 비용/record: lane 작업 3(MarkQueued·Release·mailbox enqueue), closure·delegate 약 6, lease 1.
- 위반: 허용 작업 표의 "permit 예약/반환" 금지. 다만 spec 08 W3(:219)와 04 §3은 permit을 handler 첫 instruction 직전에 record마다 반환하라고 정한다.
- 다른 언어: Java `internal/dispatch/ZLinkApplicationJobQueue.java:162–605`(record마다 `synchronized` 1회, closure 없음),
  Node `host/application-job-queue.ts:171–174`(동기 호출), C++ `runtime/dispatch/application_job_queue.hpp:65`.
- 수정안(B 부분): 반환·표시·mailbox 넣기를 상태 인자를 받는 lane 작업으로 바꿔 closure를 없앤다. 새 lock·queue를 더하지 않는다.
  MarkQueued와 Release가 같은 lease의 상태 CAS로 이미 경쟁을 판정하므로(`:473`, `:524`) 별도 turn 하나로 합칠 수 있는지 구현 전 확인한다.
- **스펙 개정 필요(D 부분)**: record마다 permit 반환을 없애려면 08 W3·04 §3을 바꿔야 한다. 근거: 허용 작업 표의 "permit 예약/반환 금지"와 W3의
  "permit은 handler의 첫 instruction 직전에 반환"이 충돌한다. 제안 문안: "W3 permit 반환은 같은 owner turn에서 연속 실행한 record 묶음의 마지막
  handler 시작 전에 한 번에 한다. 반환을 미룬 permit 수는 그 turn의 W5 시간 예산 안의 record 수를 넘지 않는다."
- 겹침 없음(04 permit은 0.30.1 #1761이 spec gap으로 0.30.2로 넘김 — HANDOFF §0.4).

#### G4. Request 제출 객체와 async 층
- 위치: `Messaging/ZLinkRequestAdmission.cs:7–9`(request마다 RCA TaskCompletionSource), `:16–24`(`ObserveAsync` box),
  `Channels/ZLinkRouteClient.cs:383–388`(`admission => ExecuteAsync(...)` closure·delegate), `:398`(`ExecuteAsync` box),
  `Spots/ZLinkSpotNodeRuntime.cs:728`(예외 변환만 하는 async 층), 나머지 두 층은 묶음 2의 M2에서 다룬다.
- 비용/request: TCS 1, closure·delegate 2, async box 3, `ZLinkRequest` 1.
- 위반: 허용 작업 표 "메시지별 task/Promise/future".
- 다른 언어: Java `ZLinkChannelRouteCalls.java:233`(`requestAdmission` CompletableFuture를 호출 객체가 보유), Node `channel-clients.ts:211–223`(`onAdmitted` callback, Promise 없음).
- 수정안: admission 완료 자리는 공개 `Admitted`를 관찰한 경우에만 Task를 만든다(관찰 전 완료면 완료된 값을 돌려준다). closure 대신 호출 객체를 상태로 넘긴다.
  `ZLinkSpotNodeRuntime.RequestToNodeAsync`의 예외 변환을 그 아래 async 메서드의 catch로 옮겨 층 하나를 없앤다. 판정 위치 변화 없음.
- 분류 B. 스펙 개정 불필요(01 §2 admitted 단계 의미 유지). 겹침 없음.

#### G5. Handler dispatch마다 DI factory 조회와 boxing
- 위치: `Handlers/ZLinkHandlerDispatcher.cs:85–89`, `Handlers/ZLinkScopedHandlerInstanceOwner.cs:95–123`(dispatch마다 `GetService<IServiceProviderIsService>()`와 `ConcurrentDictionary.GetOrAdd`),
  `Handlers/ZLinkHandlerInvocationEngine.cs:26–31`(CancellationToken을 object로 boxing), `Channels/ZLinkRouteHandlerInvoker.cs:63–71`(context 객체).
- 비용/dispatch: DI 서비스 조회 1, dictionary 조회 1, boxing 1~2. DI scope 1개는 spec이 요구한다(08 §4.3 :236–237).
- 위반: 08 §4.3 "생성 절차를 미리 결정해 재사용"(:239–245) — 조회는 허용이나 provider 기능 조회를 dispatch마다 반복한다.
- 다른 언어: Java `channels/ZLinkMeshApplicationDispatcher.java`, C++·Node handler registry의 대응 위치는 구현 단계에서 file:line으로 대조한다.
- 수정안: handler descriptor에 등록 때 정한 factory를 둔다. CancellationToken 인자는 invoker 서명에서 값으로 넘긴다.
- 분류 B. 스펙 개정 불필요. 겹침 없음.

#### G6. Serial work item마다 TCS와 AsyncLocal 두 개 설치·복원 (요청 P3)
- 위치: `Execution/ZLinkSerialWorkItem.cs:31`(RCA TCS), `:33`(`CurrentOperation` 읽기), `:165–166`(`ZLinkSerialTurn.Push` `Execution/ZLinkSerialTurn.cs:79–84`, `EnterAcceptedOperation` `Host/ZLinkFrameworkRuntime.cs:51–63`).
  trace 설정과 무관하게 실행된다. node route(benchmark) 경로가 아니라 Spot·Actor handler 경로다(08 §5는 Spot direct도 같은 단계라고 정한다).
- 비용/work item: TCS 1, AsyncLocal 설정·복원 2쌍(ExecutionContext 약 4).
- 위반: 허용 작업 표 "메시지별 task/Promise/future".
- 다른 언어: Java `ZLinkSerialExecutionQueue.java`(#1743이 바꾼 파일, ThreadLocal 사용 여부 구현 전 대조), Node AsyncLocalStorage 1회, C++ thread_local.
- 수정안: 완료 TCS는 `Completion`을 관찰한 호출자에게만 만든다. turn과 operation ambient를 값 하나로 묶어 AsyncLocal 설정을 1쌍으로 줄인다.
- 분류 B. 스펙 개정 불필요. 겹침: `ReleaseForRelocation`(`:131–146`)은 relocation 경로다. #1712 diff에 이 파일이 들어가는지 머지 뒤 확인한다.

#### G7. State lane turn 자체의 AsyncLocal 비용
- 위치: `Execution/ZLinkStateLane.cs:82–95`(동기 turn도 `CurrentLane.Value` 설정·복원).
- 비용: 남은 모든 lane turn마다 ExecutionContext 약 2.
- 수정안 두 가지: (a) 위 항목들로 메시지당 lane turn을 없애 이 비용을 경로에서 뺀다. (b) 동기 turn은 thread-static 표시를 쓴다 — `IsOnLane` 판정 위치가 1→2로 늘어난다.
  (a)를 먼저 하고 (b)는 감독자 판단으로 미룬다.
- 분류 B(a). 겹침 없음.

#### G8. Local node send가 record마다 detached task를 만든다
- 위치: `Spots/ZLinkMeshNodeRouteDispatcher.cs:314–318`(`TryRunDetached`), 호출 `Spots/ZLinkSpotNodeRuntime.cs:670`.
- 위반: 08 §4.3 "record마다 별도 dispatch task를 만들지 않는다"(:223–224).
- 다른 언어: Java `ZLinkChannelRouteCalls.java:226–231`(`submitLocalNodeSend`), C++·Node local 경로는 구현 단계에서 대조.
- 수정안: local record도 remote와 같은 owner mailbox(`EnqueueOwned`)로 넣는다. 판정 위치 2(local·remote dispatch)→1.
- 분류 B. 겹침 없음.

### 묶음 2 — `ZLinkManagedMeshNode.cs`(#1712와 같은 파일, relocation이 아닌 함수)

#### M1. Direct send 완료를 table 등록·monitor 3회·ThreadPool 게시로 전달한다 (요청 P1, P2의 send 몫)
- 위치: `Service/ZLinkManagedMeshNode.cs:9890–9925`(`SendDirectWireAsync`), `Backend/DotNet/ZLinkMeshCompletionTable.cs:46–73`(등록, `_gate` 1),
  `:174–183`·`:226–230`(`TryTake`, `_gate` 1, `ZLinkCompletionDispatcher.Post`), `:231–245`(`ReleaseReservation`, `_gate` 1), `:286–312`(즉시 admission도 `CompleteAdmission`→게시).
  즉시 admission(`Messaging/ZLinkOneWaySubmitOutcome.cs:28–31`이 `Task.CompletedTask`를 돌려줌)에서도 caller continuation이 ThreadPool로 넘어가므로
  `ZLinkRouteClient.cs:293`, `Spots/ZLinkSpotNodeRuntime.cs:597`, `ZLinkManagedMeshNode.cs:9663`의 async 세 층이 매번 box를 만든다.
- 비용/send: `PendingSend` 1, `_gate` monitor 3, dictionary 추가·제거 1, ThreadPool 게시 1(전환 1), async box 3.
- 위반: 예산 thread 전환 ≤0.1, monitor ≤3. 허용 작업 표 send = "encode 1회 + binding submit 1회".
- 다른 언어(세 언어 모두 즉시 admission은 등록·게시 없이 완료):
  C++ `runtime/mesh/raw_mesh_node_owner.cpp:1225–1243`(즉시면 결과를 바로 반환, pending일 때만 `register_operation`),
  Java `ZLinkChannelRouteCalls.java:193–196`·`internal/calls/ZLinkOneWayCalls.java:91–93`(`IMMEDIATE_ADMISSION` 상수 반환),
  Node `foundation/raw-service-mesh-runtime.ts:1578–1579`(`Ok`면 동기 `true`).
  .NET만 `9333cc11a6`(Refs #1674)에서 spec E3 문구대로 즉시 admission도 등록·게시하게 바꿨다.
- 수정안 M1a(스펙 안): 즉시 admission이면 table entry를 꺼낸 그 호출 안에서 ValueTask source를 완료하고 게시하지 않는다. caller가 아직 await하지 않았으므로
  callback이 같은 stack에서 실행되지 않으며, caller는 제출 호출이 반환된 뒤 같은 실행 자원의 다음 turn에서 이어간다(01 §11 :406–411, 08 §4.4 :281–283).
  결과: 전환 1→0, async box 3→0, 게시 1→0. monitor 3과 `PendingSend` 1은 남는다.
- 수정안 M1b — **스펙 개정 필요(D)**: send는 `result == BACKPRESSURED`일 때만 E3 entry를 등록한다. 근거: 08 §3 E3(:96 "request와 send 둘 다 … transport submit 전에 등록")와
  :113–116("admission이 즉시 성공하면 E5가 같은 호출 안에서 E3의 entry를 꺼내 … dispatcher 자리에 전달")이 즉시 admission에도 등록·게시를 요구하고,
  C++·Java·Node는 이미 등록하지 않는다. send의 나중 완료는 submit이 돌려준 `admitted` 객체로만 도착하므로 등록 전에 완료가 처리되는 경쟁이 없다.
  제안 문안(E3): "terminal 완료가 나중에 도착하는 operation에 등록한다. request는 submit 전에 등록한다. send는 E4의 `result`가 `BACKPRESSURED`일 때 `admitted`를
  관찰하기 전에 등록하며, 즉시 admission은 등록하지 않고 E4를 반환한 호출이 완료를 확정한다." 결과: monitor 3→0, 할당 1→0, 판정 위치(즉시/대기) 1→1.
- 분류 M1a B, M1b D. 겹침: 함수는 relocation과 무관하다. table 파일은 #1712·#1769 범위 밖이다.
- 요청 P2(`ZLinkCompletionDispatcher.cs:16`, 완료마다 ThreadPool 게시)의 request·table 완료 몫은 바꾸지 않는다. 허용 작업 표의 "turn당 묶은 completion 전달"은
  감독자 2차 리뷰(`a5f7a5b5fe`)가 고친 단일 drain의 head-of-line 결함과 08 §4.4 "완료 callback 사이의 독립"(:299)과 충돌한다. **스펙 개정 필요(D)**:
  허용 작업 표 문구를 "완료마다 Framework 전환 최대 1, 서로 무관한 완료를 한 drain에 묶지 않는다"로 맞출지 감독자가 정한다.

#### M2. Source가 peer·liveness를 node lane에서 메시지마다 찾는다
- 위치: `Service/ZLinkManagedMeshNode.cs:9833–9845`(`RequireDirectPeerAsync`, send·request 각 1 turn), `:9996–9998`(request가 이미 찾은 peer의 `Liveness`를 `RunState`로 다시 찾음, blocking).
  async 층 `:9687`·`:9985`(request 두 층).
- 비용/건: send lane turn 1, request lane turn 2(그중 1은 blocking `AwaitStateLane`), request async box 2.
- 위반: 08 §3 :126–127("topology·liveness·port의 lane을 차례로 기다리는 구현은 위반").
- 다른 언어: Java `binding/ZLinkJavaRawMeshNode.java:1813`(`topology.peer` 직접 읽기), C++ `raw_mesh_node_owner.cpp:1302`, Node는 binding submit만 호출(`raw-service-mesh-runtime.ts:1578`).
- 수정안: admitted peer를 router target 선택 보기와 같은 게시 지점(`PrepareRouterTargets`, m1639d2의 공통 rebuild owner)에서 불변 보기로 함께 준비하고 메시지 경로는 그 보기만 읽는다.
  request는 찾은 peer의 `Liveness`를 그대로 넘긴다. 판정 위치: peer 승인 판정 1→1, 게시 지점 1→1(기존 rebuild 재사용).
- 분류 B. 겹침: `_peersByRid` 변경 지점에 relocation 경로가 있을 수 있어 #1712 머지 뒤 rebase하고 변경 지점 전수 검색을 다시 한다.

#### M3. 수신 record마다 node lane turn 2회와 liveness 기록
- 위치: `Service/ZLinkManagedMeshNode.cs:6175`→`:5704–5716`(`HasCurrentApplicationSource`, `RunState` + `Liveness.RecordReceived`), `:6499`→`:12400–12405`(`ResolvePeerGeneration`, `RunState`).
  둘 다 ingress owner(receive loop)에서 blocking `AwaitStateLane`이다. 같은 `_lane`을 sender의 `RequireDirectPeerAsync`가 쓰므로 경합하면 수신 thread가 멈춘다.
- 비용/record: lane turn 2(할당 약 8), 경합 시 ingress owner blocking.
- 위반: 08 §4.2 :201–202("record마다 liveness tick과 claim 조회를 반복하는 구현은 위반"), §4.1 I3(:184 header만 해석).
- 다른 언어: Java `ZLinkJavaRawMeshNode.java:4761–4767`(lock 없는 `topology.peer` 읽기, 단 record마다 `recordReceived`와 hex 파싱 — Java 쪽 위반으로 따로 보고),
  Node `channels/channel-socket-registry.ts:909`, C++ `raw_mesh_node_owner.cpp`의 수신 dispatch는 구현 단계에서 대조.
- 수정안: M2의 불변 peer 보기에서 source 승인과 generation을 한 번에 읽는다. liveness 수신 기록은 수신 회전마다 source별 한 번 한다(`ReceiveRawBatch` `:5470–5510` 끝).
- 분류 B. 겹침: `ProcessReceived`(`:6134`)는 stateful·relocation record도 분기하므로 #1712와 같은 함수다. application 분기(`:6175–6512`)만 고치고 #1712 머지 뒤 진행한다.

#### M4. Request record마다 reply closure 두 쌍
- 위치: `Service/ZLinkManagedMeshNode.cs:6466–6489`(`replyHandler` closure·delegate, `replied` 캡처), `Backend/DotNet/ZLinkMeshDispatchPump.cs:590–593`(그 위에 다시 closure·delegate), `ZLinkManagedMeshNode.cs:6441`(metadata `ToArray`).
- 비용/request: closure·delegate 4.
- 위반: 허용 작업 표 reply = "binding reply 1회".
- 다른 언어: C++ `raw_mesh_node_owner.cpp`의 reply 경로, Java `ZLinkJavaRawMeshNode.java:4895–4965`, Node `mesh-record-dispatcher.ts`를 구현 단계에서 대조.
- 수정안: native reply 능력을 record가 가진 객체 하나로 두고 pump는 감싸지 않고 넘긴다. 판정 위치(1회 reply 판정) 1→1.
- 분류 B. 겹침: M3과 같은 함수(`ProcessReceived`) → #1712 머지 뒤.

### 묶음 3 — 0.30.1 작업과 함수 또는 의미가 겹침 (두 PR 머지 뒤)

#### M5. Permit 재개마다 `_socketGate`·wake timer·poller 변경 (요청 P6)
- 위치: `Service/ZLinkManagedMeshNode.cs:5452–5464`(permit 0이면 poller `Modify` + 비동기 대기 시작), `:5555–5580`(`WaitForRawApplicationAdmissionAsync`: `AcquireAsync` waiter, `_socketGate`, `SignalReceiveOwnerUnderLock`→`_receiveWakeTimer.Start`),
  `:5345–5346`(timer `Recv`에 `_socketGate`), `:5440–5442`(재개 뒤 poller `Modify`).
- 비용: permit이 0인 동안 permit 하나가 돌아올 때마다 waiter 할당 약 4, monitor 2, timer start 1, poller modify 2, 수신 thread 깨우기 1. backpressure·saturation 셀에서는 record마다 일어날 수 있다.
- 위반 후보: 08 I1(:182) "예산은 회전 상한 64건과 남은 permit 중 작은 값". permit 1개로 재개하면 회전당 1건이 된다.
- 다른 언어: C++ `raw_mesh_node_owner` permit 대기, Java pump, Node permit waiter의 재개 단위를 대조해야 한다.
- 수정안: 먼저 수정 전 계측으로 재개 1회당 claim 건수를 센다. 재개 단위를 늘리려면 04 §3·08 I1의 재개 조건을 정해야 하므로 **스펙 개정 필요(D) 후보**다.
  제안 문안 초안: "permit 0에서 재개하는 수신 회전은 반환된 permit이 회전 상한 또는 직전 회전의 claim 수에 닿거나 가장 가까운 관리 deadline이 올 때 시작한다." 확정은 계측 뒤 감독자에게 넘긴다.
- 분류 D 후보. 겹침: HANDOFF §0 "0.30.1 수정(#1712·#1745·#1761·#1769)이 수신 경로·permit·수신 gate를 바꾼다". 수신 루프 함수(`ReceiveLoop`·`DrainRawSocket`)는 #1712 파일 안이다.

#### M6. #1723 이후 local Actor send 비용 (HANDOFF §0.2)
- 위치: `Actors/ZLinkActorClient.cs:64`(runtime state lane `GetMeshNodeRuntime`), `Service/ZLinkManagedMeshNode.cs:4509–4521`→`:4563–4700`(`SubmitActor` local 분기: `ZLinkActorId.FromBoundary`, authority 조회, `MeshReceiveRecord`)
  →`:11334–11410`(`EnqueueLocalApplication`: 7개 변수를 캡처한 closure, `AcquireAndPostAsync`, `FinishAdmissionAsync` async box, record owner).
  `d9dd2f7a4d`가 header를 직접 해석해 넣던 우회(`TrySubmitLocalActorSend`)를 지웠다. 정확성 변경이므로 되돌리지 않는다.
- 비용/send: lane turn 1(G1과 같은 원인), closure·delegate 2, async box 1~2, owner 객체 1, permit 대기 객체. `tests/Zlink.Framework.UnitTests/Runtime/JoinCallbackSendTests.cs`의 비교 probe로 수정 전 값을 잰다.
- 다른 언어: Java·C++·Node의 local Actor send가 managed mailbox에 넣는 위치를 구현 단계에서 대조한다.
- 수정안: G1의 mesh 해석 재사용, local admission 게시를 상태 인자 작업으로 바꿔 closure를 없앤다. 판정 위치 변화 없음.
- 분류 B. 겹침: `SubmitActor`는 authority generation(relocation fencing)을 판정하므로 #1712와 같은 함수일 가능성이 높다 → #1712 머지 뒤.

#### M7. Managed terminal owner 왕복 (m1639d2가 넘긴 q1645e B3)
- 위치: `Service/ZLinkManagedMeshNode.cs:5012–5026`(`CompleteManagedOperationAsync`가 `TryRemoveOperationAsync` `:4974`의 `_operationLane` turn을 기다림), 호출 `:4357`, `:4661`, `:5001`.
- 비용/local request: operation lane turn 1, async box 1.
- 위반: 08 §3 E3 "terminal 권한 꺼내기가 완료 알림을 받은 자리에서 … 소유 turn으로의 추가 왕복을 만들지 않는다"(:96).
- 다른 언어: C++ `foundation/operation_registry.cpp:104–120`(완료 자리에서 꺼내기), Java `ZLinkServiceCompletionDispatcher.java:13`.
- 수정안: 꺼내기를 `ZLinkMeshCompletionTable`의 원자적 꺼내기와 같은 방식으로 완료 알림 자리에서 한다. 판정 위치 1→1.
- 분류 B. 겹침: managed operation은 relocation reply relay도 쓴다 → #1712 머지 뒤.

#### M8. BACKPRESSURED request마다 deadline timer와 linked CTS (m1639d2가 넘긴 `ZLinkRequestCompletion.cs:15`)
- 위치: `Messaging/ZLinkRequestSubmissionOutcome.cs:20–38`(`CreateLinkedTokenSource`, `CancelAtDeadlineAsync` timer, `ZLinkRequestCompletion` TCS, `ObserveCompletionAsync`).
- 비용/backpressured request: timer 1, CTS 1, TCS 1, async box 3. Ok submission에는 없다.
- 위반: 08 §3 E3 "별도 timer 객체를 요청마다 만들지 않는다"(:96).
- 다른 언어: Java `RouteRequestCall`의 admission 대기, Node `bindingRequestReply`(`node-raw-binding-port.ts:339`), C++ `request_with_header` `raw_mesh_node_owner.cpp:1095`.
- 수정안: 진단 먼저. binding request에 이미 넘긴 `Timeout(timeout)`(`ZLinkManagedMeshNode.cs:10004`)이 admission 대기도 덮는지 binding spec과 대조한 뒤 Framework timer를 지울 수 있는지 정한다.
- 분류 진단 필요(B 또는 D). 겹침: #1761·#1769 spec 브랜치 이름이 "admission-identity-reply-linger"다 → 머지 뒤.

## 3. 계측 방법 (구현 단계, 계측 코드는 커밋하지 않음)

- 위치: `/root/jobs/fx/cx/m1674c/`에 패치 사본으로 둔다. 시간 측정은 하지 않는다.
- 할당: m1639d2가 main에 남긴 benchmark 할당 snapshot(`framework/bench/grpc/dotnet/Shared/BenchServerMetrics.cs:13`, `Client/Program.cs:861–871`, `GC.GetTotalAllocatedBytes(precise:true)`)을
  고정 건수(예: warmup 뒤 send 100k·request 20k)로 실행해 byte/메시지와, 같은 셀의 binding 직접 경로를 함께 낸다. 시간 창이 아니라 건수로 나눈다.
  객체 수는 계측 패치의 site 카운터(lane `RunAsync`·`TryPost` 호출, TCS 생성, closure를 만드는 site)로 센다.
- lock: `_gate`·`_socketGate`·`SemaphoreSlim.Release` 진입 site에 `Interlocked` 카운터를 넣은 패치와 `Monitor.LockContentionCount` 전후 차이.
- thread 전환: E1 진입과 caller continuation 첫 instruction, claim과 handler 첫 instruction의 `Environment.CurrentManagedThreadId`를 기록해 다른 값의 비율을 낸다(m1639d2의 dispatcher 전환 검사와 같은 방식).
- task·future: 계측 패치의 async box·TCS 생성 site 카운터.
- 단계별: E1–E5, I0–I4, W1–W4 경계에 카운터를 두고 메시지당 값으로 표를 만든다.
- 수정 전·후를 같은 패치·같은 건수로 세고, 기준선은 `/root/worktree/zlink-main-base`(읽기·빌드만)다.

## 4. 순서 제안

1. 묶음 1: G1 → G2 → G4 → G3(B 부분) → G5 → G8 → G6 → G7(a 확인).
2. 묶음 2: M1a(M1b 승인 시 M1b로 대체) → M2 → (#1712 머지 뒤) M3 → M4.
3. 묶음 3: #1712·#1769 머지 뒤 main으로 rebase하고 M7 → M6 → M8 진단 → M5 계측.

## 5. 스펙 개정 필요 목록 (D)

| 항목 | 조항 | 요지 |
|---|---|---|
| M1b | 08 §3 E3(:96), :113–116 | 즉시 admission send는 E3 등록·dispatcher 게시 없이 완료. C++·Java·Node 현재 동작과 같음 |
| P2 | 허용 작업 표 "turn당 묶은 completion 전달" vs 08 §4.4(:299) | 서로 무관한 완료를 한 drain에 묶지 않는 현재 설계(`a5f7a5b5fe`)와 문구 정합 |
| G3(D) | 08 W3(:219), 04 §3 | record마다 permit 반환을 owner turn 묶음 반환으로 |
| M5 | 08 I1(:182), 04 §3 | permit 0에서 재개하는 회전의 시작 조건 (계측 뒤 확정) |

## 6. 준비 상태

- `rebuild-dev.sh m1674c`를 시작했다(로그 `/root/jobs/fx/cx/m1674c/rebuild-dev.log`). 계획 작성 시점에는 Core 재빌드 중이었고 결과는 구현 단계 시작 때 확인한다.
- 코드 변경·커밋 0. GitHub·보드 사용 0.
- 다른 언어에서 본 위반(Java `ZLinkJavaRawMeshNode.java:4761–4767` record마다 hex 파싱과 liveness 기록, `:1384–1404` send마다 peer intent 전체 순회)은 이 job에서 고치지 않고 보고만 한다.
