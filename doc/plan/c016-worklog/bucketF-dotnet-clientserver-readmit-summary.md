# Bucket F — .NET ClientServer 재승인과 Instance Spot 종료 정지

## 결론

`MalformedPushedControl_ReconnectsAndReadmits`는 ClientServer client가 같은 endpoint를 다시
등록하기 전에 이전 physical connection의 종료를 확인하지 않아 발생할 수 있는 순서 race다.
기존 구현은 `Disconnect()` 뒤 100 ms만 기다렸고, Core가 이전 pipe를 아직 정리하는 중이면 새
`Hello` request가 종료 중인 pipe에 남을 수 있었다. Monitor의 terminal event를 받은 뒤에만
`Connect()`하도록 수정했다.

`InstanceSpotIdleInspectionRotatesWithABoundedBatch`의 정지는 이 race의 결과가 아니다. Hang dump에서
batch assertion은 이미 끝났고 test는 자기 `ZLinkSpotNodeCatalog`를 정리하는 중이었다. 따라서
Instance Spot이나 RouteMesh 코드는 수정하지 않았다.

## 실패 assertion과 trace

Supervisor gate log
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/gate-v4-rest.log:7-19`
에는 두 번째 `Hello`를 5초 안에 받지 못한 실패가 남았다. 당시 diagnostics는 다음 상태였다.

```text
protocol:pushed-control;generation=2;attempt=3;
admissionStarted=True;admissionCompleted=False;reconnect=False;current=False
```

Generation 2의 attempt 3은 시작됐지만 test의 ROUTER에는 request가 도착하지 않았다. 이는 admission
reply 검증 전 단계에서 멈춘 상태다. 단독 사전 반복은 20/20, 수정 전 class 실행은 35/35가
통과했으므로 실패 빈도만으로 원인을 확정할 수는 없다. 그러나 diagnostics가 가리킨 마지막
transition, 종료 관찰 없이 재연결하던 코드와 계약 위반, 수정 후 반복 결과를 함께 근거로
reconnect 순서 race로 판정했다. 별도 monitor trace에서 종료 지연을 직접 관찰한 것은 아니다.

`FindReadyOutboundCandidate`는 원인이 아니다. 이 함수는
`ZLinkMeshPeerAdmission.cs:18-33`에 있으며 `ZLinkManagedMeshNode.cs:8804-8808`의 RouteMesh monitor
경로에서만 호출된다. 실패 test는 `ClientServerChannelRuntimeTests.cs:1714-1773`의
DEALER→ROUTER ClientServer runtime을 사용하며, ClientServer 계약상 client만 outbound connection을
시작한다(`03-client-server-channel.ko.md:157-168`). 따라서 provisional RID,
`RemoteAddr` 문자열 비교와 `hasReadyInboundCandidate`는 이 실패 경로에 관여하지 않는다.

## 원인과 계약

원인은 `ZLinkClientServerClientRuntime.cs`의 `RestartPhysicalConnection`과 `ReconnectAsync`였다.
수정 전 `ReconnectAsync`는 `Socket.Disconnect(_endpoint)` 호출 뒤 100 ms가 지나면 같은 endpoint에
`Socket.Connect(_endpoint)`를 호출했다. 시간 경과는 이전 pipe의 종료를 증명하지 못하므로,
attempt 3이 시작돼도 request가 wire로 나가지 않는 상태가 가능했다.

Transport liveness 계약은 physical pair의 close snapshot 또는 disconnect event를 관찰한 뒤에만
connection을 교체하고, 같은 endpoint의 새 connection도 close 관찰 뒤에 만들도록 규정한다
(`05-transport-liveness.ko.md:239-246`). Reconnect는 service handshake와 identity 확인을 다시
수행하고 이전 ready 상태를 재사용하지 않아야 한다(`05-transport-liveness.ko.md:271-276`).

## 수정과 회귀

변경 파일은
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs`
하나다.

- `:640`: reconnect마다 monitor 종료 관찰을 전달할 `TaskCompletionSource`를 보관한다.
- `:1018-1033`: reconnect 중 `Disconnected` 또는 `Closed`가 도착하면 이전 connection이
  닫혔다는 monitor 관찰 task를 완료한다. Handshake failure는 ready 상태만 내리고 physical close
  관찰로 사용하지 않는다.
- `:1556-1612`: physical generation과 admission attempt를 fence할 때 종료 관찰 task도 함께
  만든다. `Disconnect()`가 요청 자체를 거부하면 monitor event가 보장되지 않으므로 기존 fallback을
  유지한다.
- `:1614-1685`: `ReconnectAsync`는 종료 관찰 task가 완료된 뒤 같은 endpoint를 다시 등록한다.
  `Task.Yield()`로 state lane을 먼저 벗어나고, 종료 대기는 stop token으로 취소되므로 teardown을
  무기한 막지 않는다. `Connect()` 자체의 transient failure만 100 ms 간격으로 다시 시도한다.

기존 `ClientServerChannelRuntimeTests.cs:1714-1773`가 malformed pushed control 뒤 두 번째
`Hello`, `Admission`, `ReadyCount == 1`을 모두 검증하므로 이 test를 회귀로 유지했다. Assertion과
5초 timeout은 변경하지 않았다.

## Instance Spot 정지 판정

Gate dump
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/TestResults/c4a863d8-f935-4480-9206-2fe0a49174fd/dotnet_953_20260905T043252_hangdump.dmp`
를 `dotnet-dump dumpasync --type InstanceSpotIdleInspection --fields`로 확인했다.

- Test state machine은 `total=65`, `index=65`, `distinct=65`였다. 두 batch가 모든 activation을
  순회한다는 본문 검증은 끝난 상태였다.
- Await chain은 `InstanceSpotIdleInspectionRotatesWithABoundedBatch` →
  `ZLinkSpotNodeCatalog.DisposeCoreAsync` → `CloseLifecycleAsync` →
  `ExecuteCloseTransactionAsync`였다.
- Active native/framework 작업은 이 test가 만든 `ZLinkManagedMeshNode.ReceiveLoop`의 poller wait였다.
  ClientServer `Connection`, `RunAdmissionAsync`, `ReconnectAsync`는 해당 await chain에 없었다.

따라서 이 정지는 앞선 ClientServer failure가 남긴 re-admission 작업도, test 본문의 idle batch
무한 대기도 아니다. Instance Spot catalog가 activation을 정리하는 별도 teardown 정지다. 같은 test는
독립 실행 3/3에서 모두 통과했으므로 간헐 teardown 문제로 남긴다.

## 검증 결과

모든 .NET 명령은 local Core `ZLINK_LIBRARY_PATH`, package hash 기반 `NUGET_PACKAGES`, 지정된
`TMPDIR`, shared compilation과 node reuse 비활성화, `/tmp/zlink-dotnet-gate.lock`을 사용했다.
`--artifacts-path`와 `ulimit -v`는 사용하지 않았다.

- `MalformedPushedControl_ReconnectsAndReadmits` 수정 후 반복: **5/5 통과**.
- `ClientServerChannelRuntimeTests`: **35/35 통과**.
- `InstanceSpotIdleInspection` + `--blame-hang-timeout 2m`: **3/3 통과**.
- `git diff --check`: **통과**.

Build에는 기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning 하나가 남았다. Core, binding,
다른 언어, 보호된 spec 문서와 test assertion은 수정하지 않았다.

## BLOCKERS

- ClientServer 재승인 수정에는 blocker가 없다.
- 전체 suite의 Instance Spot cleanup 정지는 별도 조사 범위다. Dump는 catalog activation 종료
  경로를 가리키며, 이번 작업은 “같은 원인일 때만 수정”하도록 제한됐으므로 변경하지 않았다.
