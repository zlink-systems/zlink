# D-108 — .NET ClientServer liveness와 부재 STREAM close 수정

F-R5-11과 F-R5-12를 독립된 두 diff로 수정했다. D-108 및 감독자의 이번 작업 지시가
두 B(기존 결함) 수정의 승인 근거다. 새 회귀 테스트는 9개이며 5회 연속 통과했다.
전체 .NET unit gate는 **2,013 passed, 0 failed, 0 skipped**로 통과했다.

## F-R5-11 — ACK와 독립적인 probe 주기

- 원인: 수정 전 `ZLinkClientServerClientRuntime.cs:1381`의 `RunLivenessLoopAsync`가
  5초 delay 뒤 probe request의 reply를 최대 15초 기다려 다음 probe와 만료 판정을 지연했다.
- 소유 계층: Framework ClientServer connection이 liveness ID·주기·deadline을 소유한다.
  Core/binding은 request/reply correlation과 request completion을 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md`
  §2·§3·§10, `06-wire-protocol.ko.md` §4·§5.
- 교차언어 대조: C++ `raw_client_server_owner.cpp:1149`와 Node
  `channel-socket-registry.ts:864`는 request completion을 tick에서 기다리지 않는다.
  Java `ZLinkChannelSocketRegistry.java:760,952`는 tick에서 send하고 receive 경로에서 ACK를 처리한다.
  .NET의 직렬 await가 원인이며, 언어의 구조적 제약은 아니다.
- 변경 분류: **B — 기존 결함**.

기존 5초 delay를 유지하고 `RequestLivenessProbeAsync`의 완료 대기를 tick에서 분리했다.
각 tick은 deadline을 먼저 확인하고, 미응답 ID가 있으면 같은 ID로 요청한다. ACK가 없으면
admission 이후 약 15초에 not-ready로 바꾸고 기존 endpoint 종료 경로를 실행한다.
Delay와 경과 시간 판정은 기존 `TimeProvider`를 사용하며 테스트는 `Stopwatch`로 측정한다.

두 ACK 처리부는 `AcceptLivenessAck` 하나로 합쳤다. 요청 완료의 physical generation 검사와
현재 outstanding ID 검사, 첫 ACK만 deadline을 갱신하는 동작을 유지한다. 진행 중인 probe
request는 기존 admission task 목록을 일반화한 `_requestTasks`에 함께 보관한다.
종료 시 기존 cancellation·drain에서 함께 완료하고, 예상 밖 실패는 목록에서 버리지 않아
기존 failure collector로 전파한다. 요청 실패의 catch-all은 제거했다.

대안은 plain send 후 기존 control receive에서만 ACK를 받는 방식이었다. Node 서버의
`tryHandleClientServerControl`은 client probe에 reply token을 요구하므로 기존 Core
request/reply envelope를 유지하는 방식을 택했다. 새 timer·poller·retry 정책은 추가하지 않았다.

수정 전/후 규칙 수: ACK 판정 정의 **2 → 1**, 다음 liveness 검사 진행 조건
**2(delay·reply 완료) → 1(delay)**. Task 보관 목록은 **1 → 1**이다.

## F-R5-12 — 이미 종료된 물리 세션의 close

- 원인: `ZLinkManagedStream.CloseAsync:41`에서 backend의 `DisconnectPeer:169`를 호출하면
  이미 없는 RID의 native disconnect 오류가 그대로 전파됐다.
- 소유 계층: Framework backend가 physical close의 부재 결과를 성공으로 해석한다.
  기존 session lifecycle이 local binding·callback·scope 정리를 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md`
  §4.1·§10 및 D-108의 F-R5-12 판정.
- 교차언어 대조: Node `node-socket-backend-adapter.ts:292`의 `disconnectStreamPeer`와
  동일하게 native disconnect의 정확한 NotFound만 흡수한다. 예외 투영은 언어마다 다르다.
  Node는 `ConfigError.NotFound`, .NET은 **`ZlinkConnectException.NotFound`(605)**다.
- 변경 분류: **B — 기존 결함**.

`.NET SocketKernel.Connection.cs:45`의 공개 `DisconnectRid` 호출 경로와 실제 TCP peer
종료 테스트로 오류 605를 확인했다. Backend의 해당 호출만 정확한 예외 필터로 감쌌다.
다른 Connect 오류와 Config·Submit 타입의 NotFound는 원래 예외 인스턴스를 그대로 전파한다.
Lifecycle에 별도 cleanup이나 닫힘 상태를 추가하지 않았다.

대안인 `ZLinkManagedStream.CloseAsync`의 catch는 binding 예외 해석을 stream 호출부로
노출하므로 채택하지 않았다. Node와 같은 backend 경계에서 처리한다.

수정 전/후 규칙 수: 물리 close의 존재/부재별 완료 의미 **2 → 1**.
Session 정리 소유자는 기존 lifecycle **1 → 1**이다.

## 분리 가능한 변경 파일

아래 경로는 `framework/languages/dotnet/` 기준이다. 두 diff는 파일을 공유하지 않는다.

| Diff | 변경 파일 |
|---|---|
| F-R5-11 | `src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs`, `tests/Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs` |
| F-R5-12 | `src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs`, `tests/Zlink.Framework.UnitTests/Runtime/BackendStreamSocketConcurrencyTests.cs`, `tests/Zlink.Framework.UnitTests/Runtime/StreamSessionForcedCleanupTests.cs` |

별도 patch: `/tmp/zlink-dotnet-r5/F-R5-11.patch`, `/tmp/zlink-dotnet-r5/F-R5-12.patch`.
각 patch의 `git apply --reverse --check`가 통과했다. 공통 산출물은 이 요약 파일 하나다.
Commit·branch 변경과 sample runner 실행은 하지 않았다. 보호 경로 및 다른 언어는 수정하지 않았다.

## 검증

| 회귀 테스트 | 관찰 |
|---|---|
| `LivenessWithoutAcks_ProbesEveryFiveSecondsAndExpiresAtFifteenSeconds` | ACK 없이 5초·10초에 같은 non-zero ID의 request probe를 수신하고, 14~17초 안에 not-ready를 관찰한다. |
| `LivenessDelayedAcks_DoNotShiftTheFiveSecondProbeCadence` | 각 ACK를 2초 늦춰도 연속 세 probe의 간격은 4~6초이고, 새 ID·ACK 수·ready 상태가 일치한다. |
| `CloseAfterPhysicalPeerDisconnect_CompletesAndCleansBindingOnce` | 실제 TCP peer 종료와 native NotFound를 확인한 뒤 반복 close/dispose가 성공한다. Binding 제거와 disconnected·scope disposal·session 제거 callback은 한 번 완료된다. |
| `CloseAbsentPhysicalSession_AbsorbsOnlyConnectNotFound` | 정확한 Connect.NotFound에서 반복 close가 성공한다. |
| `ClosePhysicalSession_PropagatesOtherConnectErrors` (3개) | InvalidHandle·InvalidArgument·InternalError가 그대로 전파된다. |
| `ClosePhysicalSession_PropagatesNotFoundFromOtherOperationTypes` (2개) | Config.NotFound·Submit.NotFound를 흡수하지 않는다. |

수정 전 liveness 테스트는 두 번째 probe 미수신과 RTT 2초에서 약 7.008초 간격으로 실패했다.
최종 수정본의 새 테스트 9개는 **5회 연속 45/45 통과**했다.
로그: `/tmp/zlink-dotnet-r5/regressions-{1,2,3,4,5}.log`.

전체 unit gate는 지정된 환경과 lock에서 한 번 실행해 **2,013 passed, 0 failed,
0 skipped**, 3분 29초로 통과했다. 로그: `/tmp/zlink-dotnet-r5/unit-gate.log`.
`git diff --check`도 통과했다. 남은 실패는 없다.

- `TMPDIR=/dev/shm/zlink-tmp-dotnet`
- `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`
- `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`
- `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-350b8b789a1b3132`
- NuGet SHA-256: `350b8b789a1b31328bd477d895283efc6b986a1b242c310eadda914cf79c98c3`
- Core library SHA-256: `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`

```bash
cd framework/languages/dotnet
flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test tests/Zlink.Framework.UnitTests \
  --blame-hang --blame-hang-timeout 10m
```
