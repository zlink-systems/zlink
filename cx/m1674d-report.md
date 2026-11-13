# m1674d 완료 보고 — #1674 .NET 메시지당 비용 (2회차)

- **브랜치**: `framework-dotnet/1674-per-message-cost-2`
- **worktree**: `/root/worktree/zlink-1674-dotnet-2`
- **시작점**: `origin/main`(55f3dc90cf)을 merge한 `7399a5ce47`. merge는 Java 파일만 바꿨고 충돌은 없었다.
- **커밋**: 항목 1~4에 하나씩, 그리고 작업 중 드러난 교착을 고친 커밋 1개로 모두 5개다. 커밋 본문은 모두 `Refs #1674`와 Co-Authored-By 줄로 끝난다.
- push·PR·GitHub 사용은 없다. 시간은 측정하지 않았고 횟수만 셌다.

## 1. 커밋

| 커밋 | 항목 | 요약 |
|---|---|---|
| 1e896b5dea | 1 M1b | 즉시 admission된 send는 completion table 항목 없이 완료한다 |
| a155956f15 | 2 S2 | flow ID는 Normal·Detailed일 때만 만든다 |
| 89d262b96e | 3 | 수신 loop 전용 Core 호출에서 `_socketGate`를 뺐다 |
| 0ced4f5ca4 | 4 | 수신 회전의 관리 작업을 node turn 하나로 처리한다 |
| 6efeba1585 | 수정 | durable request lifetime 종료가 node turn 안에서 caller를 재개하던 교착을 고쳤다 |

## 2. 횟수 (수정 전 → 후)

**계측**
- 계측 사본: `/root/jobs/fx/cx/m1674d/probe/`(커밋하지 않음). m1674c harness에 다음을 더했다.
  - 1 KiB payload 시나리오 두 개(`remote-request-1k` 순차 request, `remote-send-window-1k` 창 64 포화 send)
  - 호출 지점별 집계(`M1674D_SITES=1`)
- 수정 전: `base` = `7399a5ce47`, 5회 중앙값.
- 수정 후: `headall` = HEAD에서 실행한 final3·final4·head-rep2·head-rep3 회차의 중앙값.
- 할당은 개수를 직접 셀 API가 없어 이전 회차처럼 `GC.GetTotalAllocatedBytes`의 bytes/msg로 적었다. 이 값은 source와 target을 합한 process 전체 값이다.
- 요약 스크립트: `probe/summarize.py <label>...`. 원자료: `counts-*.jsonl`. 호출 지점별 원자료: `counts-*-sites.jsonl`.

| 시나리오 | bytes | lane_run | lane_post | table_gate | sock_gate | disp_post | detached | tswitch |
|---|---|---|---|---|---|---|---|---|
| remote-send 순차 | 8667→7697 | 6.60→6.19 | 3.02→3.02 | **3→0.003** | **2.36→1** | 0.001 | 0 | 0.001 |
| remote-send 창 64 | 8315→7500 | 6.14→5.98 | 3.02 | **3→0.04** | **2.12→1** | 0.03→0.01 | 0 | — |
| remote-send 창 64, 1 KiB | 11454→10504 | 6.18→5.95 | 3.02 | **3→0.05** | **2.12→1** | 0.03→0.02 | 0 | — |
| remote-request 순차 | 18571→17188 | **14→12** | 4 | 0 | **8→2** | 0 | 0 | 0.04→0.05 |
| remote-request 순차, 1 KiB | 21463→20089 | **14→12** | 4 | 0 | **8→2** | 0 | 0 | 0.07→0.10 |
| local node send | 3340→2994 | 2 | 1 | 0 | 0 | 0 | 1 | 0 |
| local actor request | 42315→39971 | 33.1 | 5 | 4 | 0 | 1 | 2 | 0.73 |

**항목별 효과**
- 항목 1: send의 table gate가 3에서 0이 됐다. 남은 0.04는 BACKPRESSURED send다. send source의 monitor 획득은 table 3 + socket 1 = 4회에서 socket 1회로 줄었다.
- 항목 2: bytes가 줄었다(request −826, local actor −2577, local node −438).
- 항목 3: request당 socket gate가 8에서 2로 줄었다. 남은 2회는 source submit 1회와 target reply 1회다. send는 submit 1회만 남았다.
- 항목 4: request당 lane turn이 14에서 12로 줄었다(`ProcessInfrastructure` 4→2).

**tswitch**
- request의 tswitch는 회차마다 0.02~0.17로 흔들렸다. 같은 HEAD에서도 그랬다.
- 측정 중 머신 load는 13~17이었다(Java job이 같이 돌았다). 이 값은 판정용 수치가 아니다.

**예산 대비 (alloc ≤4, mutex ≤3, 전환 ≤0.1)**
- send source의 mutex는 1이다.
- request 한 건(양쪽 합)의 monitor는 socket 2이고 table gate는 0이다. lane turn 12는 lane 내부 lock과 별개다.
- 할당 개수 예산은 bytes로만 볼 수 있어 판정하지 못했다.

## 3. 항목별

### 항목 1 — M1b `1e896b5dea`
- **소유**: `ZLinkManagedMeshNode.SendDirectWireAsync`(submitter)와 `ZLinkMeshCompletionTable`(operation 소유자).
- **spec**:
  - 01-submit-and-completion.ko.md §10 :352-358(spec 브랜치): 즉시 admission된 send는 결과로 완료하고 표에 항목을 만들지 않는다.
  - 08 §3 E3 :98: send는 BACKPRESSURED일 때만 등록하고, `admitted`를 소비하기 전에 등록한다.
  - §11 새 execution turn: caller continuation은 submit 호출이 반환된 뒤 같은 자원에서 실행된다. M1a 승인과 같은 조건이다.
- **교차언어**: 세 언어 모두 즉시 admission에는 등록이 없다.
  - C++ `raw_mesh_node_owner.cpp:1236-1253`: submit 뒤 immediate면 바로 반환하고, pending일 때만 `register_operation`한다. 등록이 실패하면 terminated다.
  - Java `ZLinkChannelRouteCalls.java:193-196`(`isImmediateAdmission`): Java의 send는 표를 쓰지 않는다.
  - Node `raw-service-mesh-runtime.ts:1578-1583`: `result === Ok`이면 true를 반환한다.
- **분류**: A(계약 적응, 스펙 개정 S1).
- **판정 위치**: "admission이 이미 결정됐나"는 2곳(submitter, `PendingSend.Observe`)에서 1곳(submitter)으로 줄었다. Observe의 즉시 분기, `CompleteSendBeforeReturn`, `Reject`를 지웠고, `WorkItem.Run`은 private으로 돌렸다.
- **분기 기준**: 분기는 `submission.Result`가 아니라 `EnsureAcceptedAsync()`가 돌려준 Task의 `IsCompleted`로 한다. 결과 분류(`RequiresAdmission`)는 기존 한 곳에 그대로 둔다.
  - 차이는 BACKPRESSURED인데 binding의 `admitted`가 반환 시점에 이미 끝난 경우뿐이다. 이 경우도 등록하지 않는다. §10의 근거("나중에 도착하는 완료"가 없음)와 같다.
- **시험**:
  - 표 수준 시험 `ImmediateSendCompletesBeforeTheSubmitterReturnsItsCompletion`은 node 수준 시험 `CanonicalActorJoinIngressReplyTests.ImmediatelyAdmittedSend_CompletesFromTheSubmitResultWithoutACompletionEntry`로 옮겼다. 계약이 표에서 node로 옮겨갔기 때문이다.
  - 새 시험은 표를 닫은 node에서 send 16회가 모두 반환 시점에 완료됐는지 단언한다. 수정 전에는 ShuttingDown으로 실패한다(`test-m1b-before.log`).
- **동작 변화(감독자 확인 필요)**:
  - 수정 전에는 pump dispose(`FailAll`) 뒤 node socket이 닫히기 전까지 send가 table close 때문에 ShuttingDown으로 실패했다.
  - 이제 즉시 admission된 send는 성공한다. C++와 같은 동작이고, 공개 계약에는 "표가 닫혔으면 send 거부"가 없다.
  - 이 변화로 바뀐 타이밍이 아래 교착(§3 수정 커밋)을 드러냈다.

### 항목 2 — S2 `a155956f15`
- **소유**: `ZLinkMessageFlowTracer.CaptureEnabled`. flow on/off를 판정하는 유일한 위치다.
- **spec**: 04-flow-correlation.ko.md §4 :91-93, 03-message-flow-tracing.ko.md :201(Errors 행)(spec 브랜치).
- **변경**:
  - `CaptureEnabled`는 `>= Normal`일 때만 참이다.
  - dispatch error 기록 판정은 `DispatchErrorsEnabled`(`!= Off`) 하나로 모았다. 지금까지 `ZLinkDispatchErrorReporter.Enabled`가 flow gate(`CaptureEnabled`)를 빌려 썼다. 이를 그대로 두면 Errors 수준에서 오류 기록이 꺼지므로 분리했다.
  - 판정 위치: flow 1→1, 오류 기록 2→1(`TraceDispatchError`의 `== Off`와 reporter의 `Enabled`가 한 property를 쓴다).
- **교차언어**:
  - C++ `f4f2255174`(`flow_tracing()`)
  - Java 1673-java-2 worktree의 미커밋 변경: `captureEnabled() >= NORMAL`, `dispatchErrorsEnabled()` 분리. .NET과 같은 구조다.
  - Node `message-flow.ts:203`(`flowCreationEnabled !== 'off'`)은 아직 바뀌지 않았다.
- **분류**: A.
- **시험 3건 이동**: `Retained_BoundSession_Preserves_The_Ambient_Lifecycle_Flow`, `ActorJoinDispatcher_Preserves_Wire_Flow_Through_Handler_And_Reply`, `ClientServerRequestPreservesInboundFlowIntoHandlerContext`.
  - 셋 다 기본 Errors에서 flow 전파를 단언하던 시험이다.
  - Normal을 설정하도록 바꾸고 §4를 인용하는 주석을 달았다. 단언은 바꾸지 않았다.

### 항목 3 — `_socketGate` `89d262b96e`
- **진단** (request 한 건 8회, `counts-base-sites.jsonl`):

| 위치 | 횟수 | 처리 |
|---|---|---|
| `DrainSocketMonitorEvents`의 monitor 필드 읽기 | 2 | gate 제거 |
| 같은 함수의 `monitor.Recv` | 2 | gate 제거 |
| `ReceiveRawBatch`의 `_socket.Recv` | 2 | gate 제거 |
| `RequestDirectWireAsync` submit | 1 | 유지 |
| `SubmitCapturedNativeReply` | 1 | 유지 |

  - ReceiveLoop의 timer Recv는 wake가 있을 때만 일어나고 send에서 0.2~0.4회다.
- **Core 계약**:
  - `core/doc/spec/core/socket/README.ko.md:49-53`: send는 여러 thread가 동시에 호출할 수 있고, 수신 한 번은 record 하나를 원자적으로 소비한다.
  - 같은 파일 :57-59: close는 다른 호출이 진행 중이면 `EBUSY`다.
  - 같은 파일 :94-98: data 수신은 한 번에 호출 하나이고, 소비자는 특정 thread에 고정되지 않는다.
  - `06-monitoring.ko.md:142-144`: monitor의 recv와 close는 caller가 직렬화한다. Core가 대신 직렬화하지 않는다.
  - `socket/07-router.ko.md:298`: socket마다 route 관찰자는 하나다.
  - `07-utilities.ko.md:231,250,273`: timer의 start·stop·recv는 같은 timer의 다른 작업과 동시에 호출하면 안 된다.
- **판단**:
  - 수신 loop가 socket Recv, monitor Recv, routes snapshot의 단일 소비자다.
  - `DisposeCoreAsync`는 receive loop가 반환된 뒤에야 socket·monitor·poller를 닫는다(`ZLinkManagedMeshNode.cs` DisposeCoreAsync 첫 부분의 기존 주석과 같은 사실).
  - 따라서 이 호출들의 gate는 이미 loop 수명이 보장하는 순서를 다시 막고 있었다(AGENTS §3 "gate가 이미 직렬화하는 상태를 lock으로 다시 보호").
- **gate를 유지한 곳**:
  - timer Recv: 다른 thread가 `SignalReceiveOwner`로 Start한다.
  - submit·reply: dispose의 generation fence와 순서를 맞춰야 한다.
  - Connect·Disconnect: 제어 경로다.
- **결과**:
  - 회전당 gate는 wake timer 1회뿐이다. per-message gate는 submit·reply 1회다.
  - 이것이 Core 계약상 최소다. submit을 gate 없이 하려면 close fence를 다른 수단으로 바꿔야 한다.
- **교차언어**:
  - C++ `raw_route_port.cpp:402`(receive)과 :463(close)은 같은 mutex로 receive와 close를 직렬화한다. C++의 close는 수신 loop 종료 뒤로 순서가 정해져 있지 않기 때문이다.
  - Java `ZLinkJavaRawServicePort.java:447-459`는 state lane turn 안에서 receive한다.
  - 관찰 가능한 동작 차이는 없다. 내부 동기화 수단만 다르다.
- **분류**: B.
- **규칙 수**: 새 lock·상태는 0이다. gate 지점은 19곳에서 15곳으로 줄었다.

### 항목 4 — lane turn `0ced4f5ca4`
- **진단** (request 14 turn, `counts-base-sites.jsonl`):

| 출처 | turn | 판단 |
|---|---|---|
| `ProcessInfrastructure` | 4 | 회전당 2 turn(snapshot 1 + peer당 1) × 회전 2회 |
| `ProcessReceived`(`HasCurrentSource`) | 2 | M3 대상 |
| permit `ReleaseAsync` | 2 | W3. record마다 반환이 spec이라 유지 |
| `TryAcquireBatchAsync` | 1 | 아래 참고 |
| `ReleaseBatchAsync` | 1 | 아래 참고 |
| runtime admission | 2 | 아래 참고 |
| mailbox claim | 1 | 아래 참고 |
| mailbox drain | 1 | 아래 참고 |
| post | 4 | enqueue 2, Release 1, ExitOperation 1 |

- **변경**: `ProcessInfrastructure`는 이제 node turn 하나에서 다음을 읽는다.
  - Hello가 필요한 intent
  - admitted peer마다의 liveness 활동
  - 만료 처리(EndRouteAdmission과 Disconnect)도 같은 turn 안에서 한다.
  - Hello·probe 전송과 PeerClosed 게시는 turn 밖에서 한다.
  - 정상 상태에서는 할당이 0이다. 배열 2개와 closure가 없어졌다.
  - intent 충족 검사는 만료 처리 전에 같은 turn에서 한다. 그래서 수정 전과 같은 intent 집합을 본다.
- **spec**: 08 §4.2 :198(관리 작업은 수신 회전당 한 번). 소유자는 node state lane 하나 그대로이고, 두 번째 소유자는 없다.
- **교차언어**:
  - Java `ZLinkJavaRawMeshNode.java:7371`(`tickLiveness`): registry tick 한 번이다.
  - C++ `raw_mesh_node_owner.cpp:3645-3649`: `_liveness.due(now)`가 아니면 바로 반환한다.
- **분류**: B.
- **판정 위치**: 1→1.
- **줄이지 못한 것(설계 선택 필요, §5)**:
  - mailbox claim/drain/release 3 turn
  - permit batch 2 turn: 회전 상한 64를 먼저 확보하고 남은 permit을 돌려준다. spec I1의 형태다.
  - `HasCurrentSource`

### 수정 `6efeba1585` — durable request lifetime 교착
- **증상**: `RelocationBehaviorConformanceTests`(force stop·ready 실패 시험)가 host dispose에서 멈췄다. 항목 1~3 적용 뒤 14회 중 2회였고, 수정 전 base에서는 6회 중 0회였다.
- **hang dump 스택**: 아래 순서로 같은 node lane이 같은 thread에서 다시 turn을 기다렸다.
  1. AutoConnect `DisconnectPeer`가 node turn을 연다.
  2. turn 안에서 `NotifyPeerConnectionIntentRemoved`가 호출된다.
  3. `NativeDurableRequestLifetime` handler의 `_lifecycle.Cancel()`이 실행된다.
  4. `PrepareCanonicalRelocationAsync`의 취소 continuation이 inline으로 실행된다.
  5. 그 continuation이 lifetime `Dispose`를 호출하고, Dispose는 `RunState`로 node turn을 요청한다.
  - continuation의 실행 문맥이 달라 `ThrowIfReentrant`가 이 재진입을 잡지 못했다.
- **수정**:
  - handler를 `CancelAsync()`로 바꿨다. token은 handler가 반환하기 전에 취소 상태가 되므로 `Ended`는 그대로다.
  - callback은 turn 밖에서 실행된다(01 §11).
- **결과**: 수정 뒤 해당 class 8/8 통과, 전체 unit 3006/3006 통과.
- **분류**: B. M1b가 타이밍을 바꿔 드러난 결함이다.

## 4. 시험

| 검사 | main 기준(`7399a5ce47`) | HEAD |
|---|---|---|
| .NET unit | 3006(이전 회차 HEAD 기준) | **3006/3006** (`test-final2-unit.log`) |
| .NET contract | 96 | **96/96** (`test-final2-contract.log`) |
| csharpier check, 바뀐 .cs 9개 | — | exit 0 (`format-final.log`) |

- `test-final-unit.log`는 수정 커밋 전의 실행이다. 앞의 hang으로 실행이 중단됐다.

## 5. 결정이 필요한 것

1. **즉시 send와 table close (항목 1)**
   - pump dispose 뒤 node close 전까지 즉시 admission send가 성공한다(C++과 같다).
   - "shutdown 중 send 거부"가 필요하면 그 판정은 table이 아니라 node lifecycle(generation fence)이 소유해야 한다. 지금은 그런 계약 조항이 없다.
2. **mailbox claim·drain (항목 4)**
   - (a) claim 시점에 FIFO prefix를 batch로 옮기는 형태로 claim+drain을 한 turn으로 합친다. 비용: `MeshClaim` Receiver 계약과 pump 쪽 변경.
   - (b) 그대로 둔다.
   - 별도로, `DrainReadyAsync`는 ready 신호마다 모든 mailbox를 LINQ로 정렬하고 mailbox마다 claim turn을 하나씩 쓴다(`ZLinkManagedMeshNode.cs` DrainReadyAsync). spec I4는 "준비된 owner 집합"을 요구한다. owner가 많을수록 비용이 커지므로 준비 집합을 소유하는 쪽으로 바꾸는 설계 결정이 필요하다.
3. **`HasCurrentSource` (M3)**
   - record마다 node turn 1회다. 게시된 peer view로 바꾸려면 수신 판정과 송신 판정이 같은 사실인지 감독자가 판정해야 한다. 이전 리뷰는 "서로 다른 사실"로 판정했다.
4. **local actor request (항목 5, 진단만)**

| 출처 (request 한 건) | 횟수 |
|---|---|
| lane: `ZLinkLocationRuntime.IsOwnerAdmissionOpen` | 3 |
| lane: `ResolveActorHandlerInstancesAsync` | 2 |
| lane: `CaptureMovingFrames` | 2 |
| lane: `SubmitActor` | 2 |
| lane: `ExecuteDispatchAsync` | 2 |
| lane: `DrainReadyAsync` | 2 |
| lane: `BoundSessionDispatchScope.DrainAsync` | 2 |
| lane: location cache 조회 `TryGetCachedAsync` | 1 |
| lane: location cache 조회 `ResolveActorRowCoreAsync` | 1 |
| lane: `ManagedActor.Location` getter | 1 |
| table gate: `RequestToActorAsync` Register | 2 |
| table gate: `EnqueueCompletion` take | 1 |
| table gate: `WorkItem.Run` release | 1 |
| detached task: `serial-queue-drain` | 2 |

   - table gate는 local reply도 completion table과 dispatcher post를 거쳐서 생긴다(dispatcher post 1, tswitch 0.73).
   - detached task 2는 `ZLinkSerialExecutionQueue.cs:1146,1215`에서 생긴다. queue가 비어 있다가 일이 들어올 때마다 detached task를 하나씩 띄운다.
   - 선택지:
     - (a) location·owner admission 조회를 게시된 불변 view로 바꾼다. M2와 같은 방식이며 lane −5 이상이다. location 소유자의 view 게시 지점 검증이 필요하다.
     - (b) serial drain을 `IThreadPoolWorkItem`으로 직접 게시한다. detached 2→0이다. runtime 종료 시 drain 추적(task runner)을 다른 수단으로 바꿔야 해서 drain 계약 확인이 필요하다.
     - (c) local reply를 표 없이 완료한다. table gate 4→2이고 post 1→0이다. §11 새 turn 조건과 같은 자원 재개 여부를 판정해야 한다.
     - 모두 M6·M7 범위다.
   - **DI scope**: dispatch마다 새 scope를 만드는 것은 spec 08 §4.3 :237이 요구한다. 줄이지 않는다.
   - **CancellationToken boxing**(`ZLinkHandlerInvocationEngine.cs:26-31`), dispatch당 24 B:
     - (a) invoker delegate에 typed `CancellationToken` 인자를 추가한다. 모든 invoker 생성기를 바꿔야 한다.
     - (b) `CanBeCanceled == false`인 token에 boxed 상수 하나를 쓴다. dispatch token이 대부분 취소 가능하면 효과가 거의 없다.

## 6. 남은 문제

- **local node send에서 메시지 1건 유실 (기존 결함)**
  - probe의 `local-node-send`(warmup 2000 + 20000건 순차) 측정 단계에서 `received=19999`로 끝난 적이 있다.
  - HEAD 68회 중 3회, base `7399a5ce47` 18회 중 1회 발생했다(`probe-base-rep2.log.5`, `probe-final5.log.8`, `probe-final.log.5`, `probe-final2.log.3`). 모두 load 13~17에서였다.
  - 대기 시간 부족이 아니라 유실이다. 300초 대기에서도 느린 대기는 기록되지 않았고, 60초 실패 때 받은 수가 19999에서 멈춰 있었다.
  - 경로는 detached task `ZLinkMeshNodeRouteDispatcher.cs:314-318`(G8 보류 대상)이다. 원인 조사는 하지 않았다. 이슈 등록 여부는 감독자가 판단한다.
- **Node flow 판정**: Node의 `message-flow.ts:203`은 아직 `!== 'off'`다(S2 교차언어 미반영).
- **시험 환경 정리**
  - 임시 worktree `/root/worktree/zlink-1674d-base`는 지웠다.
  - hang dump와 TestResults도 지웠다.
  - worktree의 `.artifacts`·`core/build-dev/lib`는 이전 1674 worktree에서 복사한 것이다(git ignore 대상). `~/.cache/zlink`는 건드리지 않았다.

## 7. 리팩토링 점검

- **발견**: 성능 5건, POSDDD 2건, 불필요 코드 2건.
- **고친 것**
  - 성능: send의 표 등록, 수신 loop gate, 회전당 peer turn과 배열·closure 할당, Errors 수준의 flow 생성.
  - POSDDD: 즉시 admission 판정 2→1, 오류 기록 판정 2→1.
  - 불필요 코드: `CompleteSendBeforeReturn`·`Reject` 삭제, `WorkItem.Run` private 전환.
- **넘긴 것**
  - `DrainReadyAsync`가 신호마다 전체 mailbox를 LINQ로 정렬하고 claim하는 것(`ZLinkManagedMeshNode.cs` DrainReadyAsync)
  - permit batch 반환
  - `HasCurrentSource`
  - §5-4의 local actor 항목들
- **판정 위치**: 증가 0. 새 lock·queue·상태·cache는 0이다.
