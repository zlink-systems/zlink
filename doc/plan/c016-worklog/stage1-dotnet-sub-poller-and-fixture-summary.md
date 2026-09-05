# Stage 1 — .NET SUB poller 특례 제거와 Canonical 경쟁 복원

## 결과

- Item 1은 완료했다. Framework가 SUB socket을 `ZlinkPoll.Poll`로 분기하던 특례를 제거했고,
  모든 socket이 public `IPoller`의 `POLLIN` 등록 경로를 사용한다.
- Item 2의 fixture 의미를 복원했다. `_createdSources` 소유권과
  `HandoverAdmissionFailure_DisposeClosesUnpublishedReplacement`는 유지하면서, prior DEALER의
  reconnect를 유지하고 replacement보다 먼저 닫지 않으며 Hello를 한 번만 보낸다.
- Canonical 3회 검증은 매회 12/15였다. 복원된 live duplicate 경쟁에서 드러난 세 실패를
  timeout, retry 또는 assertion 변경 없이 남겼다. 세 실행 모두 testhost hang은 없었다.

검증은 package
`Systems.Zlink.0.17.0.nupkg` SHA-256
`e33ef88d6a3392818fcaa152cb6b02f39f9b9bc366b155d11d4da4669b676f12`에 대응하는 독립
NuGet cache와 지정된 `ZLINK_LIBRARY_PATH`를 사용했다. Core와 local package는 재빌드하지 않았다.

## Item 1 — public poller 계약과 SUB 특례 제거

### 계약 test

`BackendAdapterFactoryTests.PublicPoller_SubPollIn_AndDealerCompletion_ProgressTogether`를 추가했다.

1. XPUB가 SUB의 `contract` subscription을 관찰한 뒤에만 test를 진행한다.
2. public `IPoller` 하나에 SUB를 `POLLIN`으로, DEALER를 `POLLCOMPLETION`으로 등록한다.
3. publish 한 번과 DEALER request/reply 한 번을 발생시킨다.
4. SUB slot의 `POLLIN`과 DEALER slot의 `POLLCOMPLETION`을 모두 관찰하고, 실제 publish payload와
   request reply도 각각 소비한다.

연결 설정 뒤 send를 반복하지 않으므로 readiness 또는 completion 소실을 재시도로 숨기지 않는다.
이 test는 `Poller.Add`가 `POLLIN` 전용 SUB에서 completion owner를 요구하지 않는 현재 binding
계약과, 같은 poller가 completion owner인 DEALER도 함께 진행할 수 있음을 고정한다.

### 구현 diff

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendSocketPoller.cs`
  - `socket is ISubSocket` 분기를 삭제했다.
  - `ZLinkBackendSubscriberSocketPoller`, `ZlinkPoll.Poll` 배열과 별도 timeout 변환을 삭제했다.
  - 모든 socket을 `Systems.Zlink.Zlink.CreatePoller()`에 `PollEventFlags.PollIn`으로 등록한다.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/BackendAdapterFactoryTests.cs`
  - 위 public poller 계약 test를 추가했다.

### 검증

- 특례 제거 전 focused contract test: 1/1 통과.
- 특례 제거 후 focused contract test: 1/1 통과.
- `--filter 'FullyQualifiedName~Fanout|FullyQualifiedName~Subscriber'`: 10/10 통과, 656 ms.

기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning만 출력됐으며 이번 변경과 무관하다.

## Item 2 — Canonical live duplicate 경쟁 복원

### 복원한 시나리오

`CanonicalActorJoinIngressReplyTests.ConnectedRuntime`에서 다음 의미를 복원했다.

- `HandoverAsync`는 같은 RID replacement DEALER를 연결하고 Hello를 정확히 한 번 보낸 뒤 기존
  2초 `ReceiveAsync`로 Admit 한 번을 기다린다.
- prior DEALER의 `ReconnectInterval`을 변경하지 않고, replacement admission 전에 prior를 닫지
  않는다. admission 뒤에만 `PriorSource`와 `Source`를 갱신한다.
- `SendPriorHelloAsync`, `SendPriorHelloAndDisconnectAsync`, `SendIdempotentHelloAsync`도 Hello를 한
  번만 보낸다. 삭제한 `SendHelloUntilAdmittedAsync`의 2초 반복 전송은 남기지 않았다.
- `RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`,
  `RouteAdmission_HandoverStartsFreshLivenessDeadline`,
  `CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`가 live prior와
  replacement의 실제 same-RID 경쟁을 다시 실행한다.

소유권 수정은 유지했다. initial source와 생성 즉시의 모든 replacement를 `_createdSources`에 넣고,
fixture 종료 때 전부 닫은 뒤 target과 context를 닫는다. admission 실패 주입 overload는 teardown
회귀에만 사용하며 정상 handover의 전송 순서에는 관여하지 않는다.

### Canonical family 3회 결과

공통 명령 범위:

```text
--filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests&FullyQualifiedName!~ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute'
--blame-hang --blame-hang-timeout 5m
```

| 실행 | 결과 | 관찰한 실패 timeline |
|---|---:|---|
| 1 | 12/15, 24 s | liveness: initial admission → 13초 대기 → live prior를 둔 same-RID replacement connect → Hello 1회 → 2초 동안 Admit 없음(`ReceiveAsync:1061`, 약 15초). prior-reply와 prior-Hello test도 initial admission 뒤 첫 handover Hello의 Admit이 2초 안에 없음. |
| 2 | 12/15, 25 s | liveness: initial admission → 13초 대기 → replacement Hello/Admit 완료 → 3초 대기 → `AdmittedPeerCount`가 1에서 0으로 감소(`:671`, 16초). prior-reply와 prior-Hello test는 실행 1과 같이 첫 replacement Admit이 2초 안에 없음. |
| 3 | 12/15, 24 s | liveness, prior-reply, prior-Hello test 모두 live prior를 둔 첫 replacement Hello 뒤 2초 안에 Admit이 없어 `ReceiveAsync:1061`에서 실패. |

매회 실패한 test는 다음 세 개다.

- `CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`
- `RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer`
- `RouteAdmission_HandoverStartsFreshLivenessDeadline`

`HandoverAdmissionFailure_DisposeClosesUnpublishedReplacement`는 세 실행 모두 통과했고 teardown
hang은 재현되지 않았다. 실행 2/3의 TRX에서도 각각 약 105 ms와 106 ms에 통과했다. Blame
collector는 모든 test가 종료되어 sequence/dump를 만들지 않았다고 보고했다.

### 실패 귀속

replacement Admit이 2초 안에 없는 실패는 B session의 D-086과 같은 public-API 경계다. D-086
repro는 HANDOVER ROUTER에 fixed-RID DEALER A를 admitted 상태와 reconnect intent가 유지된 채 두고,
같은 RID의 DEALER B를 TCP로 연결했을 때 admission이 0.1–2.9초, 간헐적으로 5초 이상 걸렸으며,
reconnect interval을 줄이면 더 악화되고 inproc에서는 즉시 진행됨을 측정했다. 이번 실패도
`ConnectedRuntime`의 TCP endpoint에서 A가 유지된 동안 B가 단발 Hello를 보낸 뒤 2초 경계를
넘었다. 따라서 timeout을 늘리거나 Hello를 반복하지 않고 D-086의 Core same-RID TCP admission
latency/churn blocker로 남긴다. 공개 API repro 설명과 측정은
[`core-ctx-term-teardown-hang-summary.md`](./core-ctx-term-teardown-hang-summary.md)의
“Secondary finding” 및 [`decisions.ko.md`](./decisions.ko.md)의 D-086에 있다.

실행 2의 liveness 실패는 Admit 뒤 current peer가 제거되는 별도 증거다. 동일 test의 기존 timestamp
trace는 replacement와 prior pair가 모두 disconnect되고, 새 peer가 Admit된 뒤 약 0.1초 만에 같은
RID의 두 disconnect가 다시 발생해 새 peer가 제거됐으며 reciprocal settlement는 시작되지 않았음을
기록했다. 이 실행의 “Admit 완료 뒤 3초에 count 0”과 같은 전이다. 이 결과는 raw receive가 RID만
제공하는데 .NET runtime이 최신 monitor-derived pair를 RID에 붙여 admission/current-source/liveness를
판정하는 문제와 연결된다. 근거는
[`bucketF-dotnet-handover-liveness-hang-summary.md`](./bucketF-dotnet-handover-liveness-hang-summary.md)의
“재현 stack과 trace” 및
[`review-campaign-dotnet.md`](./review-campaign-dotnet.md)의
“`ZLinkManagedMeshNode.cs`의 C++ 비대칭 monitor 기반 결정”이다.

## BLOCKERS

1. **D-086 / B session — Core same-RID TCP admission latency.** Public-API repro는 HANDOVER ROUTER에
   fixed-RID DEALER A를 admitted/reconnect-enabled 상태로 유지하고 같은 RID DEALER B를 TCP로
   연결한 뒤, B가 보낸 단발 Hello의 Admit latency를 측정한다. 이번 세 실행에서 8개의 handover가
   기존 2초 budget을 넘겼다. Core/binding은 이 작업에서 수정하거나 재빌드하지 않았다.
2. **.NET physical-pair attribution.** 한 liveness 실행은 replacement Admit 이후 current peer가
   제거됐다. raw receive에 physical pair identity가 없는데 monitor의 최신 RID→pair를 admission,
   application source와 liveness fence로 쓰는 runtime root를 재설계해야 한다. 지시된
   `ZLinkManagedMeshNode.cs`/`ZLinkMeshPeerAdmission.cs` 범위는 수정하지 않았다.

assertion, 2초 admission wait, liveness timing과 5분 blame timeout은 변경하지 않았고, 안전용 retry를
추가하지 않았다.
