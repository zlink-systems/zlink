# Stage 2 — .NET Mesh physical HANDOVER state 제거 결과

## 결론

.NET Mesh가 monitor의 physical pair를 기준으로 survivor를 다시 고르거나 reply route를
무효화하던 상태를 제거했다. Framework는 descriptor와 logical RID로 admission과 duplicate
bookkeeping만 수행하고, Core가 선택한 logical survivor를 결정한 즉시 `Admitted`를 게시한다.
Physical pipe 선택·교체와 opaque reply route의 생명은 Core에 맡긴다.

`ConnectPeer`의 admitted inbound-intent 재사용도 제거했다. Manual/automatic 중복은 새 intent를
ordinary duplicate admission으로 수렴시킨다. TicTacToe의 `JoinGameNotify` 경로를 포함한 sample은
이 재사용 없이 통과했다.

검증에는 rebuilt package
`Systems.Zlink.0.17.0.nupkg` SHA-256
`e9e8cc6d8bfd67915232367ab7e4bf397726401b40937ad7e74353989a2d59c1`과
`core/build-dev/lib`를 사용했다. Assertion을 낮추거나 deadline·timeout·retry 횟수를 늘리지
않았다. `ZLinkClientServerClientRuntime.cs`, `core/**`, `bindings/**`, spec, 다른 언어는 수정하지
않았고 commit도 만들지 않았다. Worktree의 기존 `bindings/node/provenance` 및 Java/Node 변경은
이 작업의 변경이 아니다. 검증 종료 무렵 공유 worktree에 추가로 나타난 `core/**` 변경과 test도
다른 작업 소유이며 이 작업에서 읽거나 수정·build하지 않았다.

## 삭제·유지 inventory

| Site | 판정 | 결과 |
|---|---|---|
| `ZLinkMeshPeer.cs`의 `ReciprocalHandoverPending`, disconnect mask, retired direction/endpoint | **삭제** | Framework physical settlement 상태가 없어졌다. |
| `BeginReciprocalHandoverSettlement`, `ObserveReciprocalHandoverDisconnect`와 settlement drain | **삭제** | losing direction lane disconnect를 기다리지 않는다. |
| `ProcessAdmissionCore` / `CompletePeerAdmissionUnderLock` | **수렴** | logical survivor 선택 직후 `Admitted=true`, `PeerAdmitted` 게시로 수렴한다. |
| `_transportPairsByRid`, ready inbound/outbound RID·endpoint set과 admission re-pin | **삭제** | monitor-derived pair를 logical route/admission 입력으로 사용하지 않는다. |
| `HasCurrentInfrastructureControlSource`, `HasCurrentApplicationSource`의 pair veto | **삭제** | routed receive의 logical source RID와 admission/lifecycle fence만 검사한다. |
| `ZLinkTransportPairIdentity`, `ZLinkNativeReplyPeerEpoch`, `_nativeReplyEpochsByTransportPair` | **삭제** | reply token은 Core의 opaque `ReplyOperation`으로만 유지하고 Core submit 결과와 기존 terminal deadline으로 종결한다. |
| `RetireDuplicatePeer`의 endpoint/RID disconnect 선택과 `SharesNativeHandoverRoute` | **삭제** | duplicate intent/index/state 정리만 남기고 physical survivor를 만들기 위한 disconnect는 하지 않는다. |
| `ConnectPeer`의 admitted inbound-intent 재사용(`ebff5b3e1b`) | **삭제** | outbound exact intent만 idempotent reuse하고 inbound/manual 중복은 ordinary admission을 거친다. |
| `ZLinkMeshConnectionCandidates` | **유지·재표현** | READY/DISCONNECTED 관찰과 Hello/Admit handshake 방향 선택에만 쓴다. connection ID는 routed send/reply 또는 application ingress fence가 아니다. |
| unilateral Hello outbound fallback | **유지** | configured outbound endpoint와 C++ `for_handshake`와 같은 direction-first/newest fallback으로 표현했다. monitor pair index는 읽지 않는다. |
| descriptor/security/lifecycle generation admission fence | **유지** | logical peer identity와 stale generation 차단은 Framework 소유다. |
| liveness generation/deadline | **유지** | READY edge는 새 liveness epoch를 시작하는 관찰로만 사용한다. application route를 pin하지 않는다. |
| `DisconnectTransport` endpoint-vs-RID 규칙 | **유지** | 명시적 lifecycle teardown의 기존 ownership 규칙은 이번 duplicate settlement 제거와 별개다. |
| Canonical fixture의 `_createdSources` DEALER ownership | **유지** | failed handover를 포함해 fixture가 만든 socket을 fixture가 닫는다. |

## 변경군별 ownership 판정

### 1. Reciprocal settlement와 admission 게시

- **Owner:** logical descriptor admission과 duplicate bookkeeping은 Framework, reciprocal HANDOVER의 physical survivor와 standby lane은 Core다.
- **Spec clause:** Core socket spec `README.ko.md` §4 `rid 중복 정책`; D-B96의 reciprocal survivor/standby/losing-request 확인.
- **Parity:** C++ `service_topology_registry.cpp:281-309`와 Node `raw-service-mesh-runtime.ts:740-767`은 logical survivor 결정 즉시 admission을 게시하며 lane disconnect settlement를 두지 않는다.
- **Class:** **A — 기존 계약 적응.** 기존 C/D monitor settlement 우회를 삭제했다.

### 2. Physical pair ingress/reply fence

- **Owner:** routed receive source와 reply capability는 Core가 제공하는 logical RID와 opaque token이 소유하며 Framework는 descriptor/generation만 검사한다.
- **Spec clause:** Framework `05-transport-liveness.ko.md:239-248`은 `connection_id`를 진단·correlation으로만 한정하고 send·reply target 및 physical-pair fence 사용을 금지한다. Core socket spec `README.ko.md:1084-1095,1340-1341`은 reply token을 logical RID 범위의 opaque capability로 정한다.
- **Parity:** C++ raw mesh receive는 source logical RID와 Core reply token을 사용하고, Node 역시 connection ID로 application ingress/reply를 veto하지 않는다.
- **Class:** **A — 기존 계약 적응.** pair re-pin, ingress pair veto와 reply-pair epoch tombstone을 삭제했다.

### 3. Duplicate retirement의 physical disconnect 제거

- **Owner:** 하나의 ready logical peer와 `NotRequired` 처리는 Framework, 어떤 endpoint/RID pipe가 active/standby인지와 승격은 Core다.
- **Spec clause:** Core socket spec §4 HANDOVER와 Framework `01-channel-topology.ko.md:688-700`의 ordinary duplicate-pipe admission.
- **Parity:** C++ topology registry는 loser logical record를 제거하지만 survivor 선택을 유도하려고 endpoint/RID를 disconnect하지 않으며, Node도 logical duplicate만 수렴시킨다.
- **Class:** **A — 기존 계약 적응.** physical retirement 결정을 제거하고 logical bookkeeping만 유지했다.

### 4. `ConnectPeer` inbound-intent reuse 제거

- **Owner:** explicit manual connection intent 생성과 logical duplicate 수렴은 Framework admission이 소유한다. 이미 열린 반대 방향 physical pipe를 대신 재사용하는 결정은 public `ConnectPeer` 계약이 아니다.
- **Spec clause:** Framework `01-channel-topology.ko.md:547-549,699-700`은 manual과 automatic 경합에 동일 handshake/duplicate admission을 적용한다.
- **Parity:** C++ `connect_peer`는 local outbound intent를 만들고 ordinary admission으로 수렴하며 admitted inbound logical record를 반환하지 않는다. Node도 같은 별도 intent 경로다.
- **Class:** **A — 기존 계약 적응.** `ebff5b3e1b`의 mixed-direction shortcut을 삭제했다.

Durable lifecycle replay는 Framework `04-actor-model.ko.md:668-680`을 따른다. Actor
create/join 같은 operation은 terminal envelope가 없으면 같은 `OperationId`로 resend하고,
application request는 자동 resend하지 않는다.

## 검증 결과

공통 실행 환경은 지정된 `/dev/shm/zlink-tmp-dotnet` NuGet cache, local Core library와
`flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다. `--artifacts-path`와 `ulimit -v`는
사용하지 않았다.

| 검증 | 결과 | 비고 |
|---|---:|---|
| Core `ZLINK_TEST_CASE=test_reciprocal_handover_inproc_100ms test_router_reciprocal_handover_lanes` | **1/1 통과** | D-B96 재확인: standby 두 lane을 둔 채 survivor 요청/loser timeout/재전송 완료. |
| `ZLinkMeshPeerAdmissionTests` | **8/8 통과** | direction preference, unilateral fallback, endpoint-based outbound 후보 포함. |
| `ConnectPeerConvergesAdmittedInboundAndManualIntentAsDuplicate` | **통과** | inbound intent를 반환하지 않고 새 manual intent가 logical peer 하나로 수렴. |
| `StatefulServiceRuntimeTests` | **46/51 통과, 5 실패** | 아래 BLOCKERS의 동일 5건. |
| `RelocatedActorReplyCompletesTheOriginalRemoteCallerExactlyOnce` | **1/1 통과** | exactly-once reply 유지. |
| `RemoteActorStaleAuthority*` 3종 | **0/3, 3 실패** | expected stale/success terminal 대신 caller `TimedOut(101)`. |
| `ServiceRuntimeFoundationTests` | **59/59 통과** | foundation admission/lifecycle 회귀 없음. |
| Canonical family, run 1 (`--blame-hang --blame-hang-timeout 5m`) | **15/16, 1 실패** | `CanonicalActorJoinRequest_HandoverLeavesReplyRouteToCore`. |
| Canonical family, run 2 | **실행 완료, 집계 유실** | 반복 command의 중간 output chunk가 종료 요약과 분리되어 per-run pass/fail count를 추정하지 않고 미기록으로 남긴다. |
| Canonical family, run 3 | **14/16, 2 실패** | 위 handover test와 `ManagedSource_...(targetDialsSource: True)` initial setup timeout. Hang/dump 없음. |
| Canonical evidence 보충 1회 | **15/16, 1 실패** | 유실된 중간 집계를 추정하지 않기 위한 추가 실행. TRX `stage2-canonical-evidence.trx`; handover replacement reply timeout만 재현. |
| Canonical 중 pair-fence 기대를 계약으로 바꾼 4종 focused | **3/4 통과, 1 실패** | opaque reply Core-terminal, liveness, ActorCreate replay는 통과; handover replacement reply만 실패. |
| full unit gate `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` | **1917/1922, 5 실패**, 5m38s | Stateful의 동일 5건만 실패. |
| `run_samples.sh TicTacToe` | **통과** | `tictactoe-placement=completed`; inbound-intent reuse 없이 JoinGameNotify 경로 완료. |
| `run_samples.sh SupportChat` | **통과** | `supportchat-placement=completed`. |
| `git diff --check` / 제거 심볼 `rg` | **통과** | whitespace error 및 제거 대상 pair/settlement 심볼 없음. |

Canonical test는 monitor disconnect가 reply token을 무효화한다는 예전 pair-epoch assertion을
Core의 실제 terminal result로 바꿨다. `ActorCreateCompletion_AfterHandoverTimeout_ReplaysOnCurrentRoute`는
losing attempt의 timeout 뒤 같은 durable operation을 current route로 replay해 cached terminal을
받는 계약을 검증한다. 반면 `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`는
두 번째 application resend도 완료되지 않았으므로 “한 번 timeout 후 replay 완료”로 assertion을
바꾸지 않고 red로 남겼다.

## BLOCKERS

### B1. Simultaneous reciprocal .NET topology가 D-B96 survivor 형태로 수렴하지 않음

다음 네 Stateful failure가 같은 경계를 보인다.

- `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor`: expected `Ok(0)`, actual `TimedOut(101)`.
- `RemoteActorStaleAuthorityReturnsOneTerminalForTheOriginalOperation`: expected `StaleAuthority(107)`, actual `101`.
- `RemoteActorStaleAuthorityDisposesRejectedFollowerPartsAndReturnsOneStaleTerminal`: expected `107`, actual `101`.
- `RemoteActorStaleAuthorityUsesTheActiveFollowerBeforeAStaleTerminal`: expected `Ok(0)`, actual `101`.

진단 실행에서 첫 application request뿐 아니라 caller가 명시적으로 다시 보낸 두 번째 request도
`101`로 끝났다. Framework target trace는 stale-authority handler까지 도달했지만
`managed_operation_completion_rejected ... kind=ActorRequest` 뒤 caller reply가 유실됨을 보였다.
`ZLINK_ROUTER_DEBUG=1` Core trace는 reciprocal collision을
`router identify_peer: replace duplicate ... existing_local=0 new_local=0`으로 기록했다. 즉 D-B96
순차 Core repro의 opposite-direction survivor/standby 형태가 아니라 same-direction replacement로
분류됐다. D-B96 Core test 자체는 1/1 green이므로 Framework에서 endpoint/RID disconnect나 pair
state를 다시 넣지 않았다. `core/**`·binding 수정 금지 범위의 lower-layer blocker다.

### B2. Durable user-spot operation이 전체 deadline 안에 첫 handover attempt를 소진

`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`은 expected create count `1`,
actual `0`이다. Test의 전체 2초 operation deadline 안에 첫 losing-route attempt가 끝나지 않아
Framework의 기존 5초 terminal-retry window 전에 caller deadline이 소진된다. Deadline을 늘리거나
retry 간격·횟수를 조절하는 금지 완화는 하지 않았다. `04-actor-model`의 durable replay sender
경로에 대한 별도 후속 결함이다.

### B3. Canonical residual failures

`CanonicalActorJoinRequest_HandoverLeavesReplyRouteToCore`는 stale opaque reply가 simulated Core
terminal 뒤 정확히 멈추는 데까지는 통과하지만, current replacement가 새로 받은 request의 target
ingress에서 `ReplyJoin(Ok)`를 제출한 뒤 caller가 `101`로 끝난다. B1과 같은 replacement reply-route
blocker다.

3회차의 `ManagedSource_Command28Request_ConsumesCommand20TailAndApplicationReply(targetDialsSource:
True)`는 initial canonical actorJoin setup이 2초 안에 완료되지 않은 1회성 admission failure다.
D-B94 package를 사용했는데도 live fixture competition에서 남은 red이며 deadline 확대나 Hello
반복으로 완화하지 않았다.
