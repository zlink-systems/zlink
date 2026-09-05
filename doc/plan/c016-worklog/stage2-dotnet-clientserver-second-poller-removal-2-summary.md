# Stage 2 round 2 — .NET ClientServer 운영 ROUTER 정책 검증

ClientServer raw ROUTER fixture에 운영 서버와 같은 HANDOVER 정책을 적용했다.
ClientServer 전체 클래스, CPU 부하 반복과 두 샘플은 통과했다. 전체 unit gate에는
ClientServer 밖의 StatefulService 실패 6건이 남았다. Round-1의 승인된 runtime 제거·유지 목록은 그대로이며,
이 round에서는 runtime을 수정하지 않았다. Commit은 하지 않았다.

## Fixture 변경

변경 파일: `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs`.

`CreateClientServerRouter:1835`에서 public binding API인 `router.Options.Handover = true`를
설정한다. 운영 `ZLinkChannelBundleFactory.cs:55`와 같은 옵션이다. 다음 ClientServer
fixture가 이 helper를 공유한다.

- `AutomaticClient_AdmitsDespiteAdvertisedEndpointNotationDifferingFromExpected`
- `RequestedLivenessProbe_RepliesAndProcessesFollowingUpdate`
- `RemoteDisconnect_ReadmitsTheExistingClientConnection`
- `MalformedPushedControl_ReconnectsAndReadmits`

Binding socket만 검증하는 `BindingSockets_DeliverUnsolicitedLivenessProbe`는 변경하지 않았다.
기존 assertion·timeout·deadline은 그대로다. 두 번째 poller, admission 재시도와 수동 reconnect
loop를 추가하지 않았다. Runtime source는 round-2 시작 시 보존한 사본과 byte 단위로 일치한다.

## 검증 결과

| 검증 | 결과 | 증거 (`/tmp/zlink-stage2-clientserver-round2/` 기준) |
|---|---|---|
| ClientServerChannelRuntimeTests 전체 1회 | **36/36 통과** | `clientserver.log`, `results/clientserver.trx` |
| MalformedPushedControl + CPU hog 6개, 5회 | **5/5 통과**, 부하 프로세스 6개 종료 확인 | `stress-1.log` ~ `stress-5.log`, `results/stress-*.trx` |
| StatefulServiceRuntimeTests 1회 | **58/63 통과, 5건 실패** (ClientServer 밖) | `stateful.log`, `results/stateful.trx` |
| 전체 unit gate 1회 | **1,933/1,939 통과, 6건 실패** (모두 StatefulService); ClientServer 36/36 통과 | `unit.log`, `results/unit.trx` |
| SupportChat 1회 | **통과**, `supportchat-placement=completed` | `supportchat.log`, `sample-evidence/SupportChat/` |
| ShoppingMall 1회 | **통과**, `shoppingmall-placement=completed` | `shoppingmall.log`, `sample-evidence/ShoppingMall/` |
| 변경 범위·assertion·runtime 사본 비교, `git diff --check` | **통과** | round-2 시작 시 사본과 최종 diff |

전체 unit gate는 요청된 `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` filter와
`--blame-hang --blame-hang-timeout 10m`을 사용한다. 부하 반복은 최초 class build 이후
`--no-build`로 실행하며, 같은 gate lock 안에서 시작한 `yes > /dev/null` 프로세스 6개를
EXIT trap으로 종료·회수한다. 샘플은 `samples/run_samples.sh SupportChat`과
`samples/run_samples.sh ShoppingMall`을 각각 한 번 실행한다.

모든 실행은 지정된 `/tmp/zlink-dotnet-gate.lock`의 `flock -w7200` 안에서 수행한다.
다른 .NET 작업의 기존 변경은 그대로 검증 대상 build에 포함된다. 각 단계의 시작 시점
`git status`와 소유 source hash는 `<stage>.status`, `<stage>.sources`에 기록한다.

- `TMPDIR=/dev/shm/zlink-tmp-dotnet`
- `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`
- `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`
- NuGet SHA-256: `e0d59ad1f17cf9c911db1c6e32170c37b8cba83849743ae0f98f03d859ea1a07`
- `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-e0d59ad1f17cf9c9`
- Core `libzlink.so.0.17.0` SHA-256: `a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3`

Cache의 `Systems.Zlink.dll`과 Linux native library가 nupkg 내용과 일치한다.
Core·binding을 다시 만들거나 수정하지 않았으며 `--artifacts-path`, `ulimit -v`를 사용하지 않았다.
실행 wrapper, 부하 script와 exit code는 각각 `run.sh`, `stress.sh`, `results.tsv`에 보존한다.
샘플은 기존 `ZLINK_SAMPLE_EVIDENCE_DIR` 기능으로 로그를 보존했다.
두 샘플 build에는 기존 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning이 남았다.

## ClientServer 밖의 실패

StatefulService 단독 실행의 실패 위치는 모두
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs`다.

| 테스트 | 위치 | 기대값 → 실제값 |
|---|---|---|
| `BilateralCallerFirstAdmissionCompletesImmediateActorRequestOnSurvivor` | `:946` | terminal `0` → `101` |
| `RemoteActorStaleAuthorityReturnsOneTerminalForTheOriginalOperation` | `:1041` | terminal `107` → `101` |
| `RemoteActorStaleAuthorityUsesTheActiveFollowerBeforeAStaleTerminal` | `:1138` | terminal `0` → `101` |
| `RemoteActorStaleAuthorityDisposesRejectedFollowerPartsAndReturnsOneStaleTerminal` | `:1229` | terminal `107` → `101` |
| `RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution` | `:4007` | `operationTarget.CreateCount` `1` → `0` |

이 테스트들은 `NewNode:4124`로 `ZLinkManagedMeshNode`를 생성한다. ClientServer runtime과
이번 raw ROUTER helper를 호출하지 않는다. Actor reply/terminal과 User Spot durable 실행은
병행 작업이 소유한 managed RouteMesh 범위다. 해당 runtime과 StatefulService test에는 시작
시점부터 미커밋 변경이 있었다. 실패를 그 담당 범위에 귀속해 보고하며, 병행 변경이 직접 원인인지는
이 검증만으로 판정하지 않는다. 이 작업에서는 해당 source, assertion과 시간 예산을 수정하지 않았다.

전체 unit gate에서는 위 다섯 건과
`FrameworkHostAutomaticallyExecutesRemoteUserSpotCreateAndCloseAgainstAuthorityStore`가 실패했다.
추가 실패는 `:2705`의 양쪽 RouteMesh `AdmittedPeerCount == 1` 대기 timeout이며,
User Spot create 호출 전 admission 단계다. 이 테스트는 StatefulService 단독 실행에서는
통과했다. 전체 gate의 StatefulService 결과는 57/63, ClientServer는 36/36이다.
TRX의 실패 목록 여섯 건이 모두 StatefulService임을 확인했다. Hang 없이 모든 테스트가
종료됐으며 Blame collector는 sequence file을 생성하지 않았다.

## 소유권 판정

- 소유 계층: Core는 connect intent·physical reconnect·RID 중복 pipe 교체·command progress를, Framework는 종료 요청·종료 관찰·service handshake·descriptor 검증·logical liveness를 소유한다.
- Spec 조항: Framework `02-channel-transport/05-transport-liveness.ko.md` §5·§6; Core socket README §4의 RID 중복 정책, `05-polling.ko.md` §3, `06-monitoring.ko.md` §3.1·§3.2.
- 교차언어 대조: Round-1의 C++ 대조 결과를 유지한다. Node `channel-socket-registry.ts:879`에는 liveness 종료 뒤 수동 disconnect/connect가 남아 있다(`:884`·`:885`). .NET poller·retry 제거는 언어의 구조적 예외가 아니라 하위 계층과 중복된 정책의 정리이며, 이번 fixture 설정은 .NET test double의 운영 설정 불일치를 바로잡는다. 다른 언어는 수정하지 않았다.
- 변경 분류: **A — 승인된 계약 적응.** Round-1의 제거·유지 목록과 이번 test double의 운영 HANDOVER 설정 일치를 검증한다.

## REJECT 후속 과제

감독 판정에 따라 round-1 BLOCKER는 이 작업의 선행 조건에서 해제됐다. 운영 ClientServer
서버는 HANDOVER를 사용한다. 기존 fixture의 REJECT 정책에서는 old pipe 종료 전에 도착한
같은 RID의 새 pipe를 등록하지 않는 것이 Core 계약이다. 제거한 manual reconnect loop는
반복 재등록으로 이 차이를 가리고 있었다. Fixture 변경은 감독이 승인한 운영 설정 일치이며,
REJECT reconnect 의미를 Framework에서 보상하는 변경이 아니다.

D-B104 spec gap #1은 사용자에게 전달할 Core/spec 후속 과제로 남는다. REJECT ROUTER가
중복 연결을 닫아 DEALER의 자동 reconnect를 유도해야 하는지, DEALER의 READY가 ROUTER의
admission 완료를 뜻하는지는 별도 판정 대상이다. 이번 HANDOVER 검증으로 REJECT 동작이
해결됐다고 주장하지 않는다. Round-1의 public API 재현과 로그 위치는
[round-1 보고서](./stage2-dotnet-clientserver-second-poller-removal-summary.md)에 있다.

## BLOCKERS

ClientServer 범위의 BLOCKER는 없다. 운영 HANDOVER 설정에서 요청한 검증을 모두 실행했고,
ClientServer 검증과 두 샘플이 통과했다.

전체 unit gate의 성공 판정은 위 StatefulService 6건 때문에 차단된다. 병행 managed RouteMesh /
durable sender 담당이 해당 실패를 확인해야 한다. 이번 작업에서는 범위 밖의 실패를 수정하거나
전체 gate를 재실행하지 않았다. REJECT의 D-B104 spec gap #1은 별도 Core/spec 후속 과제이며,
이 HANDOVER fixture 변경의 BLOCKER로 되돌리지 않는다.
