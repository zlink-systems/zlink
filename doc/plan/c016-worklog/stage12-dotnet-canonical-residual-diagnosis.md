# .NET Canonical 잔여 실패 진단

2026-09-05. 대상은 `CanonicalActorJoinIngressReplyTests`의 두 실패이며, runtime 수정 전 진단이다.
시작 branch는 `main`이다. 기존 사용자 변경과 다른 언어·Core·binding·spec은 수정 범위에서 제외한다.

## 패키지와 재현

- NuGet `Systems.Zlink.0.17.0.nupkg`: SHA-256 `be4ab2bbff665e04886c139dbab712da71b3c7fdcef412ab6b795fa816ad5f3a`.
- Core: SHA-256 `98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb`.
- 지정한 hash별 NuGet cache를 사용했다. UnitTests와 공개 API repro의 output native library가 위 Core hash와 일치한다.
- 수정 전 focused 실행: **0/2**, HANDOVER caller `TimedOut(101)`, lost-reply 두 번째 ingress 7초 timeout.
- 증거: `scratchpad/stage12-dotnet-canonical/{baseline.log,baseline.trx,fixture-repro.log,public-repro.log}`.
- 기존 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`, `ZLINK_ROUTER_DEBUG=1`를 켰다. 이 fixture는 application dispatcher 없이 managed mesh ingress를 직접 꺼내므로 `runtime.Flow`를 생성하지 않는다. 기존 test의 public method를 console에서 호출해 native/control-plane 로그를 보존했다. Runtime에 임시 로그를 추가하지 않았다.

## HANDOVER 테스트

원인은 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:576`의 `SendPriorHelloAsync()` 이후 replacement의 DATA receive를 누락한 것이다. `:593`의 correlation 57 요청은 replacement에서 제출하고, `:598`의 reply는 해당 ingress가 보관한 native reply token과 correlation을 사용한다. Runtime은 `ZLinkManagedMeshNode.cs:5834`의 `PrepareNativeReply`와 `:5870`의 `EncodeActorJoinReply`에서 이 값을 유지한다.

| 순서 | 관찰 |
|---|---|
| 1 | old DEALER pair `9802875915738057847/1`에서 Hello/Admit 뒤 correlation 56 ingress를 받는다. Fixture는 그 reply에 Backpressured를 주입한다. |
| 2 | 같은 RID의 replacement pair `3600897240105869876/1`이 HANDOVER된다. replacement의 첫 Admit은 fixture가 소비한다. |
| 3 | old pair의 추가 Hello를 target이 읽는다. Core trace는 그 Admit DATA를 replacement pair로 보낸다. `SendPriorHelloAsync`는 이 Admit을 소비하지 않는다. |
| 4 | old peer를 닫고 simulated terminal로 pending reply 재제출을 끝낸다. replacement pair의 추가 supersession은 없다. |
| 5 | replacement correlation 57 ingress에 `ReplyJoin(Accepted)`가 Ok를 반환한다. 앞선 Admit DATA가 미수신 상태여서 reply가 이를 추월하지 못하고 caller는 2초 후 101을 받는다. |

공개 binding API만 사용한 ROUTER/DEALER 대조 repro에서는 각 Admit을 소비하고 replacement ingress의 `Reply()`를 제출하면 correlation 57 reply가 즉시 완료된다. 이 대조는 old request의 `NOT_CONNECTED` 여부를 검증하지 않는다. old socket을 닫았을 때 받은 `Terminated(103)`를 HANDOVER completion으로 해석하지 않는다.

- 소유 계층: Core가 pair 선택·reply token·DATA/REPLY FIFO를 소유하고 raw caller fixture가 DATA 수신을 수행한다.
- Spec 조항: Core socket README §4 RID duplicate HANDOVER, §6 Request와 reply의 DEALER DATA/REPLY FIFO 규칙(`:1078`).
- 교차언어 대조: C++ `raw_mesh_node_owner.cpp:3669`도 수신한 reply token을 전달한다. .NET runtime에 별도 reply route 결정을 추가할 근거가 없다.
- 변경 분류: **B — fixture의 DATA 수신 누락**. Core의 replacement request completion 누락으로 분류하지 않는다.

대안은 raw caller가 추가 Admit을 소비하거나, 두 테스트용 socket에 별도 receive pump를 추가하는 것이다. 기존 `ReceiveAsync`를 한 번 사용하면 정확한 원인을 제거하고 새로운 pump·상태·poller가 필요 없다. 기존 성공·correlation·admission assertion과 2초 deadline은 유지한다.

## Lost-reply 테스트

원인은 같은 test 파일 `:141-146`에서 첫 native reply를 보내지 않고 Ok로 처리한 뒤, `:212-214`에서 연결을 유지한 채 7초 안에 retry ingress를 요구한 것이다. 전체 operation budget은 `:201`의 8초다. `ZLinkManagedMeshNode.cs:2277`의 durable sender는 그 operation의 correlation과 남은 deadline을 유지한다.

| 순서 | 수정 전 관찰 | 수정할 조건 |
|---|---|---|
| 1 | target→source 연결 하나로 command 28을 받는다. | 유지. source RID는 target RID보다 사전순으로 작다. |
| 2 | 첫 terminal reply 제출을 fixture가 보류한다. | 유지. |
| 3 | pair가 유지된 채 7초 retry를 기다린다. | source→target 연결을 추가하여 첫 attempt의 방향을 HANDOVER로 대체한다. |
| 4 | 재전송 ingress가 없어 실패한다. | Core `NOT_CONNECTED` 뒤 같은 correlation·OperationId가 원래 8초 budget 안에서 재전송되는지 검증한다. |

- 소유 계층: Core가 HANDOVER와 attempt completion, Framework sender가 durable operation replay, target admission이 중복 실행 방지를 소유한다.
- Spec 조항: Core socket README §4·§6 completion 표(`:1149`); actor-model sender bullets(`:668-680`), attempt마다 남은 deadline 전부 사용 및 terminal envelope 없는 결과만 replay.
- 교차언어 대조: C++ `raw_mesh_node_owner.cpp:195-204`도 남은 deadline을 request에 전달하고 `:225-228`에서 route-unavailable 결과를 replay한다.
- 변경 분류: **B — 제거된 5초 attempt 분할을 전제로 한 fixture**.

대안은 reciprocal HANDOVER 또는 raw peer close다. 12:54 package에 구현된 HANDOVER를 선택한다. 일반 pair 종료 처리는 진행 중이므로 그 미구현 경계에 fixture를 의존시키지 않는다. Timeout 분할 복원·deadline 확대·assertion 완화는 필요 없다.

## 구현 범위와 판정

사용자가 승인한 STAGE 1+2 범위에서 두 B fixture 수정을 진행한다. Framework runtime 변경은 필요하지 않다.
수정 전/후 규칙 수(두 실패를 만든 추가 가정): **2 → 0** — 미수신 DATA 추월 가정과 고정 5초 attempt 분할 가정을 없애고 기존 Core FIFO·HANDOVER 및 sender deadline 규칙을 따른다. Runtime의 규칙·상태·helper 수는 변하지 않는다.

진단 단계의 BLOCKERS: 없음. HANDOVER 후 첫 attempt의 `NOT_CONNECTED`나 replacement의 정상 reply가 공개 API repro에서도 누락되면 Core/binding defect로 보고하고 중단한다. 최종 검증과 이후 발견한 별도 실패는 [수정 결과](./stage12-dotnet-canonical-residual-summary.md)에 기록한다.
