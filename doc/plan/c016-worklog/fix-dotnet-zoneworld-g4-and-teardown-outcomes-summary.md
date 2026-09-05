# .NET ZoneWorld G4와 종료 outcome 조사·수정 결과

2026-09-05. 감독 검토용 결과다. **작업 전체는 미완료**다. G4에서 Core 공개 API의
연결 제거 후 request completion 결함을 확인해 요청된 STOP 조건을 적용했다.
Framework 종료 경로의 잘못된 actor relocation, discovery보다 먼저 client runtime을
파괴하는 순서, authority 삭제 시 공유 capacity 경합을 잘못 보고하는 세 결함은 수정했다.
처음 보고된 `TeardownFailed` 7개 역할은 개별 sample 재검증에서 `Stopped/None`이다.
ShoppingMall의 진행 중 relocation은 여전히 `ForceStopped/DeadlineExceeded`다.

전체 증거는 [작업 증거 디렉터리](../../../scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/)에
보존했다. Core·binding·sample·다른 언어·보호 문서는 수정하지 않았고 commit하지 않았다.
동시에 진행된 다른 작업의 변경은 유지했다.

## Item A — ZW-G4: Core 공개 API에서 확인된 blocker

### 관찰한 호출 경로

기존 aggregate의 G4 trace는 `g4-crash-fcb068`, flow
`01a06fe7-cbe8-72ca-8ee9-fc43de74d437`이다. Source가 probe를 받고 target이 join을
admit한 뒤 crash boundary에 도달한다. Target 제거 뒤 source의 deferred join이
15초 request deadline까지 기다리고 `DeadlineExceeded/TaskCanceledException`으로 끝난다.
`CrashRelocationProbeRes` 자체는 전송하지만 `Unavailable` 응답을 기다리는 client 조건을
만족하지 못한다. 응답을 전혀 보내지 않는 teardown 문제와 구분된다.

- 대기 경계: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:1046`.
- 실제 wire request 경계: `Runtime/Service/ZLinkManagedMeshNode.cs:9197`.
- 이번 focused 재현: `g4-focused/evidence/ZoneWorld/logs/zone-node-1.log:1780,1793`.
  Actor `g4-crash-4b85e0`, flow `01a06ff7-7fce-7103-aac8-57d60173f637`에서
  같은 `DeadlineExceeded`와 probe 응답 전송을 확인했다.

기존 mesh peer 정보로 target lifecycle을 검사하는 sender 수정안을 시험했지만,
native request가 pair 제거 시 완료되지 않아 그 검사가 timeout 뒤에야 실행됐다.
이 수정안은 최종 diff에서 모두 제거했다. `unapplied-durable-lifecycle.patch`는 조사 기록이며
적용할 수정안이 아니다. 그 상태에서 통과한 sender 테스트 24개도 최종 검증 수치에 넣지 않았다.
Core completion을 Framework의 별도 monitor 상태·poller·timer로 대체하지 않았다.

### Framework 없는 공개 API 재현

[.NET 재현](../../../scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/Program.cs)과
[C 공개 API 재현](../../../scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/repro.cpp)은
다음 순서를 사용한다.

1. TCP ROUTER socket 두 개에 source/target RID를 지정하고 연결한다.
2. Source가 5초 timeout의 request를 제출한다. Target이 실제 request와 reply token을 받았음을 확인한다.
3. 첫 경우는 target socket을 닫고, 둘째 경우는 source에서 endpoint를 명시적으로 disconnect한다.
4. .NET은 typed request task, C는 `zlink_completion_recv(..., DONTWAIT)`로 completion을 관찰한다.
   C의 이 API는 socket command 처리도 수행한다(`core/src/api/socket/socket_message_handler_api.cpp:126`).

| 공개 API | Target close | Source endpoint 제거 |
|---|---|---|
| .NET | `TimedOut`, 5008 ms | `TimedOut`, 5001 ms |
| C | `request_result=101` (`TIMED_OUT`), 5004 ms | `request_result=101`, 5000 ms |
| 계약 | Pair 종료 즉시 `NOT_CONNECTED` | 명시적 제거 시 `NOT_FOUND` |

실행 결과는 `public-api-repro.log`, `public-api-native-repro.log`다. 최초 .NET 재현의
두 번째 경우에는 연결 준비 전 submit이 실패했으며, 위 표는 target의 request 수신까지
확인한 두 번째 실행이다. 100 ms startup 대기는 request deadline을 늘리지 않는다.

```bash
source scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/env.sh
flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet run \
  --project scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/Repro.csproj
g++ -std=c++17 -I core/include \
  scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/repro.cpp \
  -L core/build-dev/lib -Wl,-rpath,/home/hep7/project/zlink/core/build-dev/lib \
  -lzlink -pthread \
  -o scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/native-repro
scratchpad/fix-dotnet-zoneworld-g4-and-teardown-outcomes/public-api-repro/native-repro
```

Core의 구체적인 조사 지점은
`core/src/api/socket/socket_request_reply_pending_api.cpp:161-185`와
`core/src/runtime/sockets/common/socket_base_endpoint.cpp:1009-1034`다.
Endpoint pending matcher는 `logical_rid.empty()`인 request만 correlation pipe로 찾고,
endpoint 제거의 RID별 후속 처리는 blocking send wait를 종료한다. ROUTER request의
logical RID pending completion이 빠지는 지점이다. Target close의 pair 종료 경로는
`socket_request_reply_dispatch.cpp:423-469`의 pair failure 처리도 확인해야 한다.
이 작업은 제공된 binary의 공개 동작 위반까지 입증했으며 Core 내부 수정·재빌드는 하지 않았다.

- 소유 계층: **Core socket request/reply completion**. Target lifecycle에 따른 operation terminal은
  Framework 라우팅 계약이 소유하지만, 먼저 하위 pair 종료 completion이 전달되어야 한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:1143-1152` completion 표;
  `03-spot-actor/08-routing.ko.md:322-331`; `00-foundation/07-framework-error-model.ko.md:75-85`.
- 교차언어 대조: Java `ZLinkJavaDurableRequest.java:87-134`도 `NOT_CONNECTED/TIMED_OUT`을
  typed sender에서 처리한다. C 공개 API에서 동일 실패를 확인했으므로 .NET 전용 결함으로
  분류하지 않는다. 다른 언어의 G4 통과 여부는 검증하지 않았다.
- 변경 분류: **B — Core 공개 completion 계약 위반 확인, Item A의 최종 .NET 변경 없음**.
- 수정 전/후 규칙 수: **변경 없음**. Framework에 pair 상태·재시도 규칙을 추가하지 않았다.

## Item B — 수정한 종료 결함과 남은 ShoppingMall 실패

### B1. Shutdown이 새 actor relocation을 시작함

수정 전 `ZLinkFrameworkDrainExecutor.cs:218-242`는 Shutdown에서도 `DrainActors`를
반복 호출했다. Relocate 분기는 이미 `:215`에서 반환하므로 이 반복문은 종료를 위해
새 actor handoff를 시작하는 별도 경로였다. 모든 역할이 동시에 Draining으로 바뀌면
target이 없고, `ZLinkActorDrainCoordinator.cs:159-162`의 `TargetUnavailable`이
executor에서 `RelocationFailed`로 바뀐다.

`diagnosis-support/evidence/SupportChat/logs/support.log:866-868`에서
`peers=3 accepting=0 → force_stop_begin reason=RelocationFailed`를 확인했다.
이 반복문과 operations delegate 배선을 제거했다. Shutdown은 accepted work를 기다린 뒤
membership을 유지한 Spot closing과 local scope 정리로 진행한다. Actor relocation 시작은
기존 relocation workload coordinator 하나만 소유한다.

### B2. 강제 종료에서 discovery가 이미 파괴된 client state lane에 접근함

수정 전 executor `ForceStopAsync`는 `ForceStopRuntime → StopAutoConnect` 순서였다.
ClientServer client runtime이 먼저 dispose된 뒤 discovery가 `ReplaceAutomatic([])`를
호출해 `ObjectDisposedException: ZLinkStateLane`이 발생했다.

`diagnosis-support/.../support.log:872-873`에 예외와 전체 stack이 있다.
호출 경계는 `ZLinkClientServerDiscovery.cs:367 → ZLinkClientServerClientRuntime.cs:119 →
ZLinkStateLane.cs:70`이다. 정상 종료와 같이 **discovery를 먼저 중지하고 그 discovery가
사용하는 runtime을 닫는 순서**로 강제 종료도 맞췄다. Catch로 예외를 숨기지 않았다.

### B3. Bingo의 공유 capacity 경합이 authority 변경으로 보고됨

B1/B2 수정 뒤에도 Bingo `play-a`는 자기 dispatch에서 player를 destroy한 후
`ZLinkActorOwnershipCoordinator.cs:1438`의 `authority changed before release`를 보고했다.
Host unwind가 이미 시작된 terminal completion을 기다리면서 이 실패를 관찰했다.

임시 exception 상세를 추가한 `diagnosis-Bingo-authority/.../play-a.log:673`에서
expected/current authority의 version, owner와 generation이 모두 같은 것을 확인했다.
Provider repository의 Delete는 authority와 **공유 capacity counter**를 같은 조건부 batch로
수정한다. 서로 다른 actor를 동시에 삭제하면 counter condition이 충돌할 수 있다.
수정 전 `ZLinkProviderLocationRepository.Authority.cs:55-104`는 이 경합을 `NewOwner`
mutation에만 흡수하고, Delete에서는 authority 충돌로 호출자에게 전달했다(`:175-215`).

새 동시 삭제 회귀 테스트에서 수정 전 2개 중 하나가 같은 authority version의 `Conflict`로
실패함을 재현했다(`capacity-before.log`). 기존 CAS 경합 처리에서 `NewOwner` 전용 조건을
제거했다. 같은 authority version에만 기존 경합 처리를 적용하고, 변경된 version은 그대로
terminal conflict다. 기존 재시도 상한 8과 backoff 값은 유지했다. Actor lifecycle에 재시도를
추가하는 대안 대신 Store transaction을 소유한 repository의 기존 규칙을 통합했다.
임시 exception 상세는 제거했다. 수정 후 Bingo 7개 역할 모두 `Stopped/None`이다.

### B4. ShoppingMall: 원인 경계를 찾았으나 미수정

`diagnosis-ShoppingMall-cutover/evidence/ShoppingMall/logs/workflow-b.log:153`에서 target이
command 34를 받은 것을 확인했다. 같은 로그 `:206-210`의 기존 task failure trace는
`ZLinkRelocationDataLostException: Published SPOT relocation root does not extend its staged root`
및 다음 stack을 남긴다.

```text
ZLinkSpotRetireTargetRuntime.ReconcilePublishedStageAsync  ZLinkSpotRetireTransport.cs:708
ZLinkSpotRetireTargetRuntime.ReconcileStageAuthorityAsync ZLinkSpotRetireTransport.cs:2288
ZLinkSpotRetireTargetRuntime.RunReconciliationLoopAsync   ZLinkSpotRetireTransport.cs:2213
```

Source `workflow-a.log:330`은 `ForceStopped/DeadlineExceeded`다. Source는
`ZLinkSpotRetireTransport.ReconcilePublishedAuthorityAsync:349-423`에서 publication을
기다리고, host의 `ZLinkFrameworkMaintenanceRuntime.cs:479`는 진행 중 relocation을 기다린다.
이 경우는 context/socket dispose 정지가 아니다. 첫 개별 검증에서는 역할이 반대였지만 같은
종료 실패가 발생했다. Root prefix 검사의 어느 필드가 불일치하는지까지는 확정하지 않았다.
`ZLinkFrameworkRuntimeSpotRetire.cs:61-65`의 사전 target generation 계산과 source의
root/reference 대조를 후속 조사해야 한다. 검증 조건·deadline·outcome을 완화하지 않았다.

- 소유 계층: **Framework host drain executor**(B1/B2), **Framework provider authority repository**(B3).
  미수정 B4는 Framework Spot relocation publication/reconciliation 경계다.
- Spec 조항: **05-host-relocation-flow §14, :759-828**의 새 relocation 금지와 ordered teardown;
  **01-location-runtime :613-629**의 Delete와 capacity 원자 갱신;
  **02-location-store-redis :100-109**의 opaque condition batch 계약.
- 교차언어 대조: Java `runtime/host/ZLinkFrameworkRuntime.java:1987-2021`은 LIFO shutdown
  등록으로 auto-connect를 channels보다 먼저 중지한다. Java provider authority repository도
  Delete에서 capacity를 같은 batch로 갱신한다(`:108-156`). Java의 동시 삭제 재현은 실행하지
  않았으며 동일 경합 가능성은 남는다. B4의 Java staging owner도 root prefix를 검증한다
  (`ZLinkUserSpotAggregateStagingOwner.java:474-494`).
- 변경 분류: **B — 기존 Framework 결함 수정(B1/B2/B3); B4는 미수정**.
- 수정 전/후 규칙 수: **6 → 3**. Actor relocation 시작 경로 2→1, discovery/runtime 종료 순서
  정책 2→1, authority mutation별 공유 경합 정책 2→1. 새 상태·timer·helper 계층은 없다.

Baseline의 sample이 모두 통과했다는 전제와 별개로, drain executor의 원본 blob은
`4bad5ac979`와 조사 시점 HEAD에서 모두 `f80b08a1d2209e658268c17811d35399d83b9ff2`다.
따라서 B1/B2 코드가 지정된 unwind commit에서 처음 생겼다고 주장하지 않는다.
이번 조사는 현재 실패의 원인과 수정 효과를 확인했으며, 최초 회귀 유발 commit의 bisect는
완료하지 않았다.

## 최종 diff

| 변경 파일 | 내용 |
|---|---|
| `src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:218,478` | Shutdown의 actor relocation 반복문·delegate 제거, 강제 종료에서 auto-connect를 runtime보다 먼저 정리 |
| `src/Zlink.Framework/Runtime/Locations/ZLinkProviderLocationRepository.Authority.cs:61` | NewOwner 전용 CAS 경합 처리를 authority mutation 공통 규칙으로 통합 |
| `tests/Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs:31,1181` | §14 순서 회귀 검증, RouteMesh actor·ClientServer request·Fanout publish·Stream session이 활성화된 실제 host의 Stopped/None 검증 |
| `tests/Zlink.Framework.UnitTests/Runtime/ProviderLocationRepositoryAuthorityTests.cs:1150` | 서로 다른 actor 동시 삭제가 공유 capacity 경합으로 실패하지 않는지 검증 |
| `tests/Zlink.Framework.UnitTests/Runtime/RelocationBehaviorConformanceTests.cs:27,69` | Planned maintenance 테스트가 명시적 RelocateAsync 후 ShutdownAsync를 호출. 기존 actor 이전 결과 assertion 유지 |
| 이 문서 | 원인, 수정, 공개 API repro와 미완료 검증 기록 |

위 코드 경로의 기준은 `framework/languages/dotnet/`이다. Planned maintenance 테스트는
원래 `ShutdownAsync`만 호출하고 actor 이전을 기대해 §14와 충돌했다. 결과 assertion을
낮추지 않고 `RelocateAsync(PlannedMaintenance)`라는 계약상의 trigger를 추가했다.

## 검증 결과

| 검증 | 결과 | 증거 |
|---|---|---|
| 전체 채널 활성 host stop 회귀 | 1/1 PASS, Stopped/None | `host-stop.log` |
| StatefulServiceRuntimeTests | focused 62/63; remote UserSpot 생성에서 TaskCanceledException 1회. 동일 테스트는 full gate에서 PASS | `tests/focused-final.trx`, `tests/unit-full.trx` |
| ServiceRuntimeFoundationTests | 59/59 PASS | `tests/focused-final.trx` |
| DrainCoordinatorTests | 48/48 PASS | `tests/focused-final.trx` |
| 전체 unit gate 1회, CanonicalActorJoinIngressReplyTests 제외 | 1939/1940. 유일 실패는 위의 Shutdown에 이전을 기대하던 maintenance 테스트 | `unit-full.log`, `tests/unit-full.trx` |
| Maintenance trigger 수정 후 해당 테스트 | 1/1 PASS | `maintenance-trigger.log`, `tests/maintenance-trigger.trx` |
| Capacity 동시 삭제 회귀, 수정 전 | FAIL: 2개 중 1개 동일 authority version Conflict | `capacity-before.log` |
| Provider authority + LocationLifecycle + DrainCoordinator, capacity 수정 후 | 122/122 PASS | `authority-final.log`, `tests/authority-final.trx` |
| ZoneWorld focused G4 | FAIL, Unavailable 미관찰. 전체 ZoneWorld 2회·aggregate는 Core blocker로 미실행 | `g4-focused.log`, `g4-focused/evidence/` |
| TicTacToe | sample PASS, 4/4 역할 Stopped/None | `final-TicTacToe/evidence/` |
| Bingo, capacity 수정 후 | sample PASS, 7/7 역할 Stopped/None | `final-Bingo-capacity/evidence/` |
| SupportChat | sample PASS, 3/3 역할 Stopped/None | `final-SupportChat/evidence/` |
| ShoppingMall | sample exit 0, 종료 검증 FAIL: workflow-a DeadlineExceeded, 나머지 3개 Stopped/None | `diagnosis-ShoppingMall-cutover/evidence/` |
| DeliveryDispatch | sample PASS, 6/6 역할 Stopped/None | `final-DeliveryDispatch/evidence/` |
| GameQuest | sample PASS, cleanup 3/3 역할 Stopped/None; mission-b는 시나리오가 의도적으로 kill | `final-GameQuest/evidence/` |

최종 개별 로그의 정상 terminal 표기는 logger의
`termination_changed state=Stopped outcome=Stopped reason=None`다. Process exit 0만으로
종료 검증 PASS로 계산하지 않았다. Bingo 최초 시도는 session-a startup의 `EADDRINUSE`
(errno 98)로 실패했으며 `diagnosis-Bingo-port-collision/`에 보존하고 한 번 재실행했다.

전체 gate는 host 수정 후, provider 경합 수정 전에 한 번 실행했다. 최종 provider 변경은
122개 관련 테스트와 Bingo로 검증했으며 전체 gate를 다시 실행하지 않았다. G4 focused는
제거한 sender 수정안을 포함한 진단 실행이므로 최종 tree의 G4 PASS 근거가 아니다.

모든 .NET build/test/sample은 요청된 `TMPDIR`, package SHA별 `NUGET_PACKAGES`,
`ZLINK_LIBRARY_PATH`, compilation 환경과 `flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용했다.
`--artifacts-path`, `ulimit -v`와 deadline 증가는 없다. Package와 실제 test/sample 출력의
native library hash를 대조했으며, 종료 시에도 원본 package/library hash는 같다.

- NuGet SHA256: `be4ab2bbff665e04886c139dbab712da71b3c7fdcef412ab6b795fa816ad5f3a`.
- libzlink.so SHA256: `98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb`.

## BLOCKERS

1. **Core 공개 API blocker**: admitted ROUTER request가 target close·명시적 endpoint 제거 후에도
   `NOT_CONNECTED/NOT_FOUND` 대신 timeout까지 남는다. 위 C/.NET repro와 library hash를
   Core 담당자에게 전달해야 한다. 이 작업의 명시적 Core 수정 금지 및 STOP 조건을 적용했다.
2. **ShoppingMall 미해결**: target의 published/staged root 대조 실패와 source의 publication
   대기가 남아 `ForceStopped/DeadlineExceeded`다. 이 항목을 Core 결함으로 분류하지 않는다.
   B1/B2/B3 수정만으로 Item B 전체가 완료됐다고 판단하면 안 된다.
3. **요청된 최종 gate 미완료**: ZoneWorld 2회, 최종 tree의 7개 개별 sample 전체 및 aggregate의
   모든 역할 STOPPED 조건은 달성하지 못했다. Core 수정·local package 갱신 후 G4의 lifecycle
   terminal과 durable sender 동작을 다시 검증해야 한다. 이후 ShoppingMall 수정과 최종 gate가 필요하다.
