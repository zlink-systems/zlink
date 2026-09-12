# Issue #5 — .NET send lock contention 표본

## 표본 조건

- perf queue가 비어 있는 것을 확인한 뒤, `framework/bench/grpc/dotnet`의
  `zlink-framework-dotnet send-saturation`을 payload 1024, `send-concurrency=8`,
  warmup 1000으로 실행했다. 이것은 throughput 판정이 아니라 EventPipe 표본이다.
- `dotnet run` 부모가 아니라 실제 Client child PID에 붙어 `dotnet-trace collect
  --clrevents contention+stack --clreventlevel informational --duration 00:00:12`를
  수집했다. trace 시작 약 1초 뒤 active 12초를 시작했으므로 idle 시작부를 제외한
  약 11초 active 구간이 표본이다.
- 원본: `/tmp/zlink-lock-profile-child-1789246284/contention.nettrace`.
  `ContentionStart_V2`의 stack과 `LockID`를 EventPipe/TraceEvent로 집계했다.

## 확인한 contention 지점

12초 trace에 `ContentionStart_V2` 42,784건이 있었다. 이 중 41,360건(96.7%)은
동일한 lock object (`LockID=110401529758104`)에서 아래 stack으로 발생했다.

```
Systems.Zlink.CompletionOwner.SendAsync
  <- SocketSendOperation.Async
  <- ZLinkManagedMeshNode.SendDirectWireAsync
  <- ZLinkManagedMeshNode.SendToNodeDirectAsync
  <- ZLinkSpotNodeRuntime.SendToNodeAsync
  <- ZLinkRouteSendCall.Async
```

이 stack의 진입 lock은
[`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:39`](bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:39)의
`_submitSync`다. 같은 method 안의 `_sync`/`_runtimeSync`가 아니라 이 lock으로
판정한 근거는 41,360건이 하나의 `LockID`에 집중되고, `SendAsync`의 첫 lock이
`_submitSync`이며 이 method의 DONT_WAIT admission과 completion-drain fence가 이
lock을 공유한다는 코드 주석과 drain 경로
([CompletionOwner.cs:268](bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:268),
[CompletionOwner.cs:737](bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:737))다.

다른 관련 표본은 `_socketGate`를 잡은 framework
[`ZLinkManagedMeshNode.SendDirectWireAsync`](framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9064)에서
4건뿐이었고, `ZLinkStateLane` stack은 없었다. 따라서 Framework socket gate나
state-lane을 더 바꾸는 것은 이 contention의 원인 수정이 아니다.

## 변경과 상호 배제

런타임 코드는 변경하지 않았다. 원인 lock은 binding 소유이며 이번 작업은
`bindings/**` 변경 금지다. Framework에서 추가 queue, retry, socket 선택 상태로
이를 숨기는 것은 binding DONT_WAIT ownership과 completion 선형화를 상위 계층에
중복 구현하는 우회가 된다.

현재 `_submitSync`는 send admission과 `Drain`/`DrainRuntime`을 같은 임계구역으로
직렬화한다. 따라서 writable token이 `Arm`되기 전에 drain될 수 없고, close는
`PrepareClose`에서 같은 lock을 획득해 in-flight submit과 선형화한다. 이 상호 배제는
send 완료 의미와 binding DONT_WAIT 소유권을 보장하므로 그대로 유지했다.

수정 전/후 규칙 수: 1/1 — binding-owned submit/drain fence 하나를 그대로 유지했고,
Framework에 보상 규칙을 추가하지 않았다.
