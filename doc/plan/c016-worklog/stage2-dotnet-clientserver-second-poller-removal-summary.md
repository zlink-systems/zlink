# Stage 2 — .NET ClientServer poller·reconnect 제거 결과

승인된 제거안을 runtime과 관련 test에 적용했다. **검증 완료 상태는 아니다.**
`MalformedPushedControl_ReconnectsAndReadmits`의 6 CPU hog 반복에서 두 번째 admission request가
timeout됐다. Framework를 제외한 .NET binding public API 재현에서도 같은 결과를 얻었다.
Core/binding 경계의 BLOCKER로 보고하며, 요청의 중단 조건에 따라 후속 gate와 sample은 실행하지
않았다. Commit은 하지 않았다.

## 소유권 판정

- 소유 계층: Core는 command progress·connect intent·physical pipe 선택과 교체를 소유한다.
  Framework는 endpoint 종료 요청과 종료 관찰, descriptor·generation 검증, service handshake와
  logical liveness를 소유한다.
- Spec 조항: Framework `05-transport-liveness.ko.md` §5·§6, Core socket README §4 RID 중복 정책과
  `zlink_disconnect`, Core `05-polling.ko.md` §3, `06-monitoring.ko.md` §3.1·§3.2.
- 교차언어 대조: C++ `channel_outbound_exchange.cpp:803`은 desired endpoint 변경 때만
  connect/disconnect한다. Node `channel-socket-registry.ts:879`에는 종료 후 수동 재등록이 남아
  있다. .NET의 두 번째 poller와 admission retry는 언어의 구조적 필요가 아니라 중복된 하위 계층
  정책이다. 다른 언어는 수정하지 않았다.
- 변경 분류: **A — 승인된 하위 계층 계약 적응.** 아래 REJECT 재현의 원인 판정과 수정은
  Core/binding 담당 범위이며, 제거안의 검증 통과로 간주하지 않는다.

## 변경 파일과 삭제·유지 목록

Runtime:
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs`.

| 구분 | 내용 |
|---|---|
| 삭제 | `ReconnectAsync`의 두 번째 `ZLinkBackendSocketPoller`, socket command 진행을 위한 poll loop |
| 삭제 | `ScheduleAdmissionRetry`, `RetryAdmissionAsync`, 최초 connect의 100ms fallback |
| 삭제 | `_retryTasks`, `_retryScheduledGeneration`, `_reconnectTask`, disconnect 관찰용 TCS와 reconnect task 관리 |
| 삭제 | request exception을 physical restart로 바꾸는 경로와 Connect의 100ms 재시도 |
| 삭제 | control exception 뒤 재접속·delay로 계속 실행하는 경로; 진단을 남기고 task 실패를 보존한다 |
| 유지 | `RunControlLoopAsync:1267`의 단일 control receive poller |
| 유지 | monitor loop, descriptor RID·security·lifecycle generation 검증, `IsCurrentAttempt:1465`의 늦은 admission 결과 차단 |
| 유지 | physical reconnect 이후 새 service handshake와 logical liveness |
| 유지 | protocol 위반·liveness 만료의 endpoint 종료 요청과 해당 endpoint의 `Disconnected`/`Closed` 관찰 |

`OnMonitorEvent:962`는 이 DEALER가 endpoint 하나를 소유한다는 조건에서 `ConnectionReady`의
count가 0인 snapshot을 제외한다. Count가 양수이면 해당 generation의 handshake를 한 번
시작한다. 완료된 admission request의 실패는 원래 exception type·message를 진단에 남기며
자동 재제출하지 않는다(`:1122`). `Update:784`와 `MergeExpected:802`도 실제 admission이 존재할
때만 descriptor 갱신으로 ready가 될 수 있다.

일반 transport loss에서는 Framework가 `Connect`를 호출하지 않는다. 명시적
`Disconnect(endpoint)`는 connect intent 자체를 제거하므로, 이 경우에만
`_terminationRequested`로 종료 요청을 기록하고 같은 endpoint의 종료 event를 관찰한 뒤
`Connect(endpoint)`를 한 번 호출한다(`:990`, `:1485`). 별도의 reconnect task·timer·poller는
없으며 `connection_id`를 조건으로 사용하지 않는다.

종료 요청 직후 Connect하는 대안과 종료 event에서 한 번 Connect하는 대안을 대조했다.
전자는 Framework §5의 종료 관찰 순서를 충족하지 않으므로 후자를 사용한다. 단순히
Disconnect한 뒤 아무것도 하지 않는 방식도 명시적으로 제거된 intent를 복구하지 못한다.

Tests:
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs`.

- `RemoteDisconnect_ReadmitsTheExistingClientConnection:1715` 추가: 서버가 `DisconnectRid`로
  연결을 끊은 뒤 같은 client RID의 Hello, admission 전 ready 0, admission 후 ready 1과
  추가 Hello 부재를 검증한다. 서버 ROUTER는 운영과 같은 HANDOVER다.
- `AutomaticClient_AdmitsDespiteAdvertisedEndpointNotationDifferingFromExpected:1332`의 raw
  ROUTER에 descriptor와 같은 `notation-server` RID를 설정했다. Endpoint 표기만 다르게 한다는
  기존 fixture 설명과 일치시킨 것이다. 제거한 fallback은 실제 RID 불일치에 대한 monitor 거절도
  우회하고 있었다.
- 기존 malformed-control test의 ROUTER 정책, assertion과 deadline은 유지했다. 다른 기존
  assertion·timeout·budget도 변경하지 않았다.

## 검증 결과

| 검증 | 결과 |
|---|---|
| 최초 malformed-control focused | 1/1 통과 |
| 중간 ClientServer 묶음 | 33/35 통과; count-0 snapshot의 admission 시작과 notation fixture RID 불일치 발견·수정 |
| 수정 후 focused: malformed, remote disconnect, notation | 3/3 통과 |
| 수정 후 malformed + 6 CPU hog, 임시 trace 없음 | 첫 회 실패: 두 번째 Hello 미수신, request timeout |
| 원인 조사용 file trace + 6 CPU hog | 첫 묶음 5/5 통과, 다음 묶음 4/5 통과; 실패 event trace 보존 |
| Public API 재현, 서버 monitor 없음·첫 Received 보유·REJECT + 6 CPU hog | 첫 회 두 번째 request timeout 재현 |
| 같은 public API 재현, HANDOVER + 6 CPU hog | 5/5 통과 |
| 최종 ClientServer 전체 / StatefulServiceRuntimeTests | BLOCKER 확인으로 미실행 |
| 전체 unit gate (`FullyQualifiedName!~CanonicalActorJoinIngressReplyTests`) | 미실행 |
| SupportChat / ShoppingMall 각 1회 | 미실행 |
| `git diff --check`, 임시 trace·삭제 symbol 잔존 검사 | 통과 |

모든 build/test/repro는 지정 환경과 `/tmp/zlink-dotnet-gate.lock` 안에서 실행했다.
Core·binding package를 다시 만들지 않았다. `--artifacts-path`, `ulimit -v`를 사용하지 않았다.

- NuGet SHA-256: `e0d59ad1f17cf9c911db1c6e32170c37b8cba83849743ae0f98f03d859ea1a07`.
- Cache: `/dev/shm/zlink-tmp-dotnet/nuget-e0d59ad1f17cf9c9`.
  Cache의 managed DLL과 Linux native library가 nupkg 내용과 일치함을 확인했다.
- 사용한 `ZLINK_LIBRARY_PATH`: `/home/hep7/project/zlink/core/build-dev/lib`.
  `libzlink.so.0.17.0` SHA-256:
  `a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3`.
- 기존 CS8619 warning (`ZLinkSpotNodeCatalog.cs:768`)이 남았다. 병행 Actor Join 편집 중
  `SubmitNativeActorJoinRequest` 미정의 build 오류가 한 번 있었으며 해당 작업의 후속 편집 뒤
  빌드가 통과했다. 그 파일은 이 작업에서 수정하지 않았다.

## BLOCKERS

**REJECT ROUTER에서 client endpoint 종료를 관찰하고 새 READY를 받은 뒤 제출한 request가
서버에 도착하지 않는다.** Core/binding 담당이 physical admission과 RID 교체 경계를 확인해야 한다.

운영 서버는 `ZLinkChannelBundleFactory.cs:55`에서 `router.Options.Handover = true`를 설정한다.
실패한 기존 malformed-control fixture는 `ClientServerChannelRuntimeTests.cs:1774`의 raw ROUTER로,
기본 REJECT를 사용한다. HANDOVER 비교 통과만으로 기존 REJECT test의 통과를 주장하거나
fixture 정책을 바꾸지 않았다.

Framework 실패 trace(`/tmp/zlink-stage2-clientserver/events-2.log`)의 핵심 순서는 다음과 같다.
시간은 UTC다.

```text
02:18:54.2976309 disconnect-return generation=2 attempt=2 termination=True
02:18:54.3001024 monitor=Disconnected
02:18:54.3004530 connect-return
02:18:54.3009980 monitor=ConnectionReady value=0
02:18:54.3102936 monitor=ConnectionReady value=1
02:18:54.3104223 admission-start generation=2 attempt=3
02:18:55.3127169 request:ZlinkRequestException:zlink error code 101
```

임시 trace 없는 실패는 5초 동안 두 번째 raw record가 도착하지 않았다. 위 trace가 있는 실패는
request timeout 이후 수신한 다음 raw record에 대한 `TryDecodeHello` assertion이 실패했다.
Record subtype은 이 trace에서 수집하지 않았다. Timeout·assertion을 완화하지 않았다.
통과 trace는 `/tmp/zlink-stage2-clientserver/events.log`에 있으며 같은 terminal→READY→admission
순서 뒤 request timeout이 없다. ClientServer admission은 control plane이므로 application
message-flow의 범위 밖이다. 기존 admission diagnostics와 file test log를 먼저 확인한 뒤
임시 event trace를 추가했고, 임시 코드는 최종 source에서 모두 제거했다.

Public API 재현은 Framework를 참조하지 않는다.

1. `Systems.Zlink` 0.17.0으로 tcp ROUTER/DEALER를 만들고 ROUTER는 REJECT로 둔다.
   ROUTER monitor는 열지 않고 DEALER monitor만 연다.
2. 첫 READY edge에서 1초 timeout의 request/reply를 성공시킨다. 서버가 받은 첫 `Received`는
   기존 regression과 같이 재접속 동안 보유한다.
3. DEALER에 control receive용 POLLIN poller 하나를 등록하고 pushed record를 수신한다.
4. DEALER `Disconnect(endpoint)` 후 DEALER monitor만 기다려 해당 endpoint의 terminal을 관찰한다.
   이 대기 중 DEALER poller를 실행하지 않는다.
5. `Connect(endpoint)`를 한 번 호출하고 flags가 `ConnectionReadyEdge`인 event를 기다린다.
   두 번째 request를 한 번 제출한다. 서버는 public `Recv(DontWait)`를 계속 호출한다.
6. 서버에 두 번째 request가 도착하지 않으며 1초 후 `ZlinkRequestException.TimedOut`으로 끝난다.
   같은 재현에서 HANDOVER는 5/5 통과했다.

재현 파일과 증거:

- Source/project: `/tmp/zlink-stage2-clientserver/repro/Program.cs`, `repro/repro.csproj`.
- 실행 환경·부하 script: `/tmp/zlink-stage2-clientserver/run-command.sh`, `repro-stress.sh`.
- 실패 public trace: `/tmp/zlink-stage2-clientserver/repro-reject.log`.
- HANDOVER 비교: `/tmp/zlink-stage2-clientserver/repro-handover.log`.
- 임시 trace 없는 regression 실패: `/tmp/zlink-stage2-clientserver/stress-initial-failure.log`.
- Trace가 있는 regression 실패: `/tmp/zlink-stage2-clientserver/trace-failure-stress-5.log`.

Public trace는 old connection 9의 `Disconnected`를 `02:18:44.9027503`에 관찰하고,
`Connect`를 `02:18:44.9029816`에 호출한 뒤 새 connection 16의 READY edge를
`02:18:44.9129928`에 관찰한다. Request는 `02:18:44.9130072`에 제출되며
`02:18:45.9214819`에 timeout된다. Connection ID는 로그의 진단값으로만 출력한다.

대조해야 할 계약은 Core socket README §4의 RID 중복 처리, monitoring §3의 실제 ready 전이,
polling §3의 application poll과 독립된 command 진행, 그리고 승인된 D-B104의 reconnect READY
진행이다. D-B104에 기록된 gap은 **terminal 이전** disconnect→connect의 overlap인데, 이 재현은
**client terminal 이후**다. 이 관찰만으로 서버 측 old RID 정리 완료까지 보장됐다고 추론하지
않는다. Ready event 이후의 REJECT 처리와 request admission 결과를 Core/binding 소유 계층에서
판정해야 하며, Framework의 두 번째 poller나 admission 재시도로 보상하지 않는다.
