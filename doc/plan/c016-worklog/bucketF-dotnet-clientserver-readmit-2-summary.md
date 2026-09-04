# Bucket F — .NET ClientServer 재승인 round 2

## 결론

`MalformedPushedControl_ReconnectsAndReadmits`의 full-suite 실패 원인은 이전 physical
generation에 예약한 admission fallback이 reconnect 중인 새 generation에서 실행된 것이다.
이 fallback의 `Request()`가 우연히 socket command를 진행시켜 `Disconnected`를 발생시킨 뒤,
admission attempt를 2에서 3으로 바꿔 round-1 reconnect의 attempt fence가 `Connect()`를
실행하지 못하게 했다.

Fallback을 physical generation과 reconnect 상태로 fence하고, reconnect가 terminal monitor
edge를 기다리는 동안 별도 receive를 하지 않는 socket poll로 Core command progress를 유지했다.
Timeout과 assertion은 바꾸지 않았다.

## 실패 trace와 메커니즘

6개 `yes` 부하에서 round-1 상태를 재현한 임시 file trace
`/tmp/zlink-clientserver-round2-events-2.log`의 핵심 순서는 다음과 같다.

```text
20:29:27.5346543 restart ... generation=2 attempt=2
20:29:27.5350806 disconnect-return ... generation=2
20:29:27.5620751 admission-start ... generation=2 attempt=3
20:29:27.5657122 monitor ... event=Disconnected ... generation=2 attempt=3 reconnect=True
20:29:27.5665223 disconnect-observed ... generation=2
20:29:27.5668528 monitor ... event=ConnectionReady value=0 flags=None ... reconnect=False
```

`ReconnectAsync`가 캡처한 값은 attempt 2였지만, 최초 `Start()`가 예약한 100 ms fallback이
attempt 3을 먼저 시작했다. 따라서 `ReconnectAsync`의 current-attempt 검사에서 반환했고 trace에
`connect-return`이 없다. Attempt 3의 request는 endpoint가 제거된 동안 제출되어 admission을
기다렸고, test의 diagnostics와 동일하게 `admissionStarted=True`, `current=False`, `ready=0`으로
남았다.

Fallback만 fence한 중간 trace
`/tmp/zlink-clientserver-round2-events-4.log`는 round-1 wait의 두 번째 잘못된 가정을 확인했다.

```text
20:35:22.5524106 retry-run generation=1
20:35:22.5526716 try-admission expected=1 generation=2 ... reconnect=True
20:35:27.5132627 monitor event=Disconnected ... generation=2 ... reconnect=True
```

Stale request가 socket을 건드리지 않자 `Disconnect()` 뒤 약 5초 동안 terminal edge가 진행되지
않았다. ClientServer control loop는 admission이 fenced된 동안 poll하지 않고 100 ms delay만
수행한다(`ZLinkClientServerClientRuntime.cs:1349-1365`). Test timeout 뒤 teardown이 socket을
다시 진행시킨 시점에 `Disconnected`가 도착했다. 즉 monitor queue를 기다리는 것만으로 대상
socket의 command progress가 계속된다는 round-1 가정이 틀렸다.

최종 수정 trace `/tmp/zlink-clientserver-round2-events-5.log`에서는 다음 순서가 됐다.

```text
20:36:43.2908069 monitor event=Disconnected ... generation=2 attempt=2 reconnect=True
20:36:43.2918050 monitor event=ConnectionReady value=0 ... reconnect=True
20:36:43.3231232 try-admission expected=1 generation=2 ... reconnect=True
20:36:43.3877547 reconnect-connect generation=2 attempt=2
20:36:43.3937659 monitor event=ConnectionReady value=1 ... reconnect=False
20:36:43.3938278 try-admission ... generation=2 attempt=2 reconnect=False
```

이전 generation fallback과 disconnect에 동반된 edge 없는 ready-count snapshot은 reconnect 중에
admission을 시작하지 못했고, terminal 관찰 뒤 `Connect()`와 새 `Hello`가 순서대로 실행됐다.
임시 trace 코드와 환경 변수는 최종 source에서 제거했다.

## 원인 위치와 계약

- `ZLinkClientServerClientRuntime.cs:1191-1228`: round-1 이전 retry 예약은 generation을
  보관하지 않는 단일 boolean이었고, delay 뒤 현재 generation에서 무조건
  `TryStartAdmission()`을 호출했다.
- `ZLinkClientServerClientRuntime.cs:1638-1657`: round-1 reconnect는 terminal monitor task만
  기다렸다. `RunControlLoopAsync`가 not-admitted 상태에서 poll하지 않으므로 대상 socket command를
  진행할 owner가 없었다.
- Core socket 계약은 connect/disconnect와 poll을 runtime control path에서 호출할 수 있고 서로
  다른 poller의 동시 사용을 허용한다(`core/doc/spec/core/socket/README.ko.md:44-53`,
  `core/doc/spec/core/05-polling.ko.md:322`).
- Framework transport 계약은 endpoint disconnect 반환만으로 close 완료를 판정하지 않고
  terminal snapshot/event 관찰 뒤 같은 endpoint의 새 connection을 만들도록 요구한다
  (`framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:239-246`).
  Reconnect 뒤에는 service handshake를 다시 수행하고 이전 ready 상태를 재사용하지 않아야 한다
  (같은 문서 `:271-276`).

## 수정과 회귀

변경 파일은
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs`
하나다.

- `:634`, `:1191-1228`: retry 예약을 `ulong?` physical generation으로 추적한다. 이전 generation
  task의 `finally`가 새 generation 예약을 지우지 않으며, retry 실행 시 예약 generation을
  `TryStartAdmission`에 전달한다.
- `:1067-1100`: expected generation이 다르거나 reconnect가 진행 중이면 admission을 시작하지
  않는다. 실제 `Connect()`가 반환된 뒤 `ReconnectAsync`가 새 generation retry를 예약한다.
- `:1640-1657`: terminal monitor edge를 기다리는 동안 socket poller를 실행한다. Poll은 record를
  dequeue하지 않으므로 control receive ownership은 유지되고, stop token도 기존처럼 대기를
  취소한다.

기존 회귀 test `ClientServerChannelRuntimeTests.cs:1713-1779`가 malformed pushed control 뒤
두 번째 `Hello`, 새 admission과 `ReadyCount == 1`을 검증한다. 수정 전 6개 CPU hog 부하에서
연속 2회 같은 failure를 재현했고, 최종 수정 후 같은 부하에서 5/5 통과했다. 별도 test-only
hook이나 timeout 변경은 추가하지 않았다.

## 검증 결과

모든 명령은 지정된 local Core, package hash 기반 `NUGET_PACKAGES`, `TMPDIR`, shared compilation과
node reuse 비활성화, `/tmp/zlink-dotnet-gate.lock`을 사용했다. `--artifacts-path`와 `ulimit -v`는
사용하지 않았다.

| 검증 | 결과 |
|---|---:|
| 수정 전 focused + 6 CPU hog | 2/2 실패, 동일 attempt 3 상태 재현 |
| 최종 focused | 1/1 통과 |
| 최종 focused + 6 CPU hog | 5/5 통과 |
| `ClientServerChannelRuntimeTests` | 35/35 통과 |
| `StatefulServiceRuntimeTests` | 50/50 통과 |
| `git diff --check` | 통과 |

Build에는 기존 `ZLinkSpotNodeCatalog.cs:768` CS8619 warning 하나가 남았다. Core, binding, 다른
언어, 보호된 문서와 test assertion은 수정하지 않았다.

## BLOCKERS

없음.
