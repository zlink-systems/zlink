# .NET ClientServer Server-only readiness 수정 결과

## 결과와 원인

Perf phase 1 B1의 Server-only topology readiness 결함을 .NET monitoring에서 수정했다.
Serving host에서 시작된 manual Server가 양수 weight로 준비되면 `GetStatus("work")`는
`State=Ready`, `IsReady=true`, `ReadyTargetCount=1`을 반환한다. Public API 변경은 없다.
관련 테스트 51/51, SampleRegression 157/157, 7개 sample이 통과했다. 전체 unit은
2015/2016으로, 별도 RouteMesh admission 경로의 간헐 실패 한 건 때문에 전체 gate의 0 failures 조건은 미충족이다.

소유 계층: Framework의 ClientServer monitoring은 topology readiness를 소유한다.
Outbound 역할 검사는 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeChannels.cs:197`,
Ready connection 선택은 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:497`의 기존 경로가 소유한다.

소유 spec: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:174,194`의
topology 범위와 ready 정의, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359,379`의
public status 및 local·remote Ready Server 집계. Outbound 역할과 선택은
`framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md:17,53,262`가 소유한다.

교차언어 대조 결과: Java는 Server 역할을 readiness로 인정하지만 local target 집계가 빠진다.
Node는 local Server를 target 집계에 넣지 않아 같은 false 증상이 예상된다. C++은 .NET 수정 전과
동일하게 Client 역할이 필요한 selectable을 public readiness로 반환한다. 아래 대조는 소스 분석이며 타 언어 실행 결과가 아니다.

변경 분류: **B — 기존 결함**. 감독이 확인한 B1 원인과 지정한 수정 작업을 구현 승인으로 적용했다.
구현 전에 소유 spec과 타 언어 대조를 보고했다.

수정 전 원인은 `ZLinkClientServerRuntimeService.cs:98`의
`Selectable = state.HasClient && runtime.IsStarted && readyCount > 0`과
`:131`의 `IsReady = hostServing && runtimeStarted && snapshot.Selectable` 연결이다.
Server-only에서는 local Ready Server가 있어도 `HasClient=false`가 공개 readiness를 false로 만든다.
공개 두 process typed echo 재현은 `doc/plan/c016-worklog/perf-dotnet-runner-phase1-summary.md:134`의 B1 증거를 따른다.

## 수정과 규칙 수

`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:39`는
기존 local server identity를 target 목록에 포함한다. Identity는 listener bind 뒤 생성되고
bundle에 등록된다(`ZLinkChannelBundleFactory.cs:58`, `ZLinkChannelRuntimeManager.cs:76`).
Runtime의 Running 전이는 state 생성 완료 뒤다(`ZLinkFrameworkRuntime.cs:944,950`).
따라서 Server-only의 Ready target은 등록·listen이 완료된 local Server다.

`ZLinkClientServerRuntimeService.cs:80,98`에서 Ready server 수와 topology readiness를 각각 계산한다.
Readiness는 host Serving, runtime started, Ready server 수 > 0의 조합이다. Server-only는 local Server,
Client-only는 연결된 Ready Server, Client+Server는 기존에 수집하던 local·remote target 목록을 사용한다.
Local target은 Serving이고 weight > 0일 때만 Ready다. Public count는 기존 snapshot의
`ReadyServerCount`를 그대로 사용한다(`:139`).

대안으로 monitoring의 `Selectable`을 유지하면서 Server 역할 분기를 추가하는 방식을 검토했다.
이 필드는 outbound에서 사용되지 않으므로, 실제 선택 경로와 중복되는 판단을 제거하고 내부 snapshot의
`IsReady`로 교체했다. 새 상태, 타이머, retry, option, public helper를 추가하지 않았다.
Monitoring event도 같은 snapshot의 `IsReady`를 사용한다. 기존 outbound 역할 검사와 선택 코드는 변경하지 않았다.

수정 전/후 규칙 수: **monitoring의 Ready target 집계·readiness 판단식 4 → 2**.
Ready target 집계는 snapshot과 public projection의 2곳에서 snapshot 1곳으로,
readiness는 Selectable과 public projection의 2곳에서 snapshot 1곳으로 줄었다.
Outbound 역할 검사·대상 선택 규칙은 기존 소유자에 유지된다.

변경 파일:

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerMonitoringModels.cs`
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ClientServerChannelRuntimeTests.cs`
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ZLinkObservationQueueTests.cs`
- 이 결과 문서

## 언어별 대조

| 언어 | 소스 근거 | Server-only 결과와 차이 |
|---|---|---|
| Java | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkTopologyRuntimeViews.java:42,61,68` | `hostServing && (server || !client || readyTargetCount > 0)`로 Server-only를 Ready로 인정한다. Outbound Client 역할과 topology readiness를 분리한 참고 구현이다. 다만 Server 역할만으로 ready를 인정하므로 local readiness를 세밀하게 검증하는 기준으로 그대로 복사하지 않았다. |
| Java target 집계 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:573,579,599` | Target 목록이 `clientServerConnections`만 순회한다. Server-only의 local Server가 집계되지 않아 ReadyTargetCount는 0으로 예상된다. B1의 IsReady=false와 다른 monitoring gap이다. |
| Java outbound | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java:1035,1048,1077,1100` | Send/request는 `hasClientRegistration`을 확인하고 Server-only에서 `NOT_CONFIGURED`로 거부한다. |
| Node | `framework/languages/node/packages/framework/src/runtime/diagnostics/topology-runtime-projections.ts:51,68,75,81`; `framework/languages/node/packages/framework/src/runtime/channels/channel-runtime-manager.ts:107,116` | Serving host라도 Ready target 수가 0이면 Degraded/false다. Client 역할 자체를 readiness 조건으로 쓰지는 않는다. |
| Node target 소유자 | `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:524,528,557,736,781`; `framework/languages/node/packages/framework/src/runtime/foundation/service-discovery-registry.ts:99` | Monitoring 목록은 client discovery descriptor만 사용한다. Local Server descriptor는 별도 server descriptor map에 저장되어 목록에 합쳐지지 않는다. Server-only에서는 local target 누락으로 Degraded/false/count 0이 예상된다. .NET의 역할 조건과 다른 원인이다. |
| C++ | `framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.cpp:447,457,471,530`; `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/client_server_runtime.hpp:52,56,57,92` | Local Ready Server는 집계되지만 `selectable = configured->client.enabled && ready_server_count > 0`; `is_ready()`는 selectable을 반환한다. Server-only의 count 1/false는 .NET 수정 전과 같은 결함이다. C++의 public 표면은 snapshot/is_ready이며 .NET의 State enum projection과는 다르다. |

타 언어 runtime은 수정하지 않았다. .NET만 변경한 이유는 이 작업의 승인 범위이며,
Server-only가 ready여야 한다는 계약에 언어별 예외가 있기 때문이 아니다.

## 회귀 테스트와 gate

로그·TRX root: `/dev/shm/zlink-cs-server-ready-gate-20260906/`.

`ClientServerChannelRuntimeTests.cs:632`는 실제 Framework hosted service를 시작하여
Server-only weight 100의 Ready/true/count 1과 weight 0의 Degraded/false/count 0을 검증한다.
`:665`는 연결할 Server가 없는 유효한 manual Client-only의 Degraded/false/count 0을 검증한다.
`:693`의 기존 Client+Server 테스트에는 Ready state와 count 1 검증을 추가했다. 이 테스트는 host
relocation 시 false이면서 target count가 유지되는 동작과 target draining 관찰도 검증한다.

수정 전 유효한 설정의 focused 실행은 **1 fail / 3 pass**였다. 유일한 실패는 Server-only
weight 100의 State가 Ready 기대와 달리 Degraded인 경우다(`focused-before-valid-config.log`,
`tests/focused-before-valid-config.trx`). 초기 test 작성에서 허용 범위 밖의 음수 weight와
peer source 없는 Client 설정이 configuration error로 거부된 기록도 `focused-before.log`에 보존했다.
Spec의 weight 범위 0..10000과 manual peer source 계약에 맞게 test 설정을 바로잡았다.

| 검증 | 결과 | 증거 |
|---|---|---|
| ClientServerChannelRuntimeTests + ZLinkObservationQueueTests | PASS 51/51, failed 0, skipped 0 | `focused-after.log`, `tests/focused-after.trx` |
| Unit — CanonicalActorJoinIngressReplyTests 제외 | FAIL: passed 1999 / failed 1 / total 2000, skipped 0; exit 1 | `unit-main.log`, `tests/unit-main.trx` |
| Unit — CanonicalActorJoinIngressReplyTests | PASS 16/16, failed 0, skipped 0; exit 0 | `unit-join.log`, `tests/unit-join.trx` |
| SampleRegression | PASS 157/157, failed 0, skipped 0; exit 0 | `sample-regression.log`, `tests/sample-regression.trx` |
| 실패한 RouteMesh admit test 단독 진단 | PASS 1/1; 수정 없이 진단 환경만 활성화. 전체 gate 실패를 대체하지 않음 | `unrelated-admit-diagnostic.log`, `tests/unrelated-admit-diagnostic.trx` |
| samples/run_samples.sh — 7 samples | PASS 7/7, aggregate exit 0 | `samples.log`, `samples/<SampleName>/logs/` |

최종 unit 결과는 **2015 pass / 1 fail / 2016 total**이다.
Sample 실행 순서는 TicTacToe, Bingo, SupportChat, ShoppingMall, DeliveryDispatch,
GameQuest, ZoneWorld이며 모두 완료 marker와 정상 runner 종료를 확인했다.
ZoneWorld는 선택 시나리오가 아닌 기본 `all` 실행으로, `zoneworld=completed`까지 통과했다.
Unit은 지정대로 `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests`와
`FullyQualifiedName~CanonicalActorJoinIngressReplyTests`로 나누어 실행했다.
모든 unit 및 SampleRegression 호출에 `--blame-hang --blame-hang-timeout 10m`을 적용했다.
전체 solution은 빌드하지 않았다.

저장소 root에서 사용한 gate 명령은 다음과 같다. 각 명령의 stdout/stderr는 표의 log에,
`--logger 'trx;LogFileName=...' --results-directory /dev/shm/zlink-cs-server-ready-gate-20260906/tests`로
TRX를 별도 저장했다.

```bash
source framework/languages/dotnet/perf/scripts/dotnet-env.sh
flock --exclusive --close /tmp/zlink-dotnet-gate.lock \
  dotnet test framework/languages/dotnet/tests/Zlink.Framework.UnitTests \
  --filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests' \
  --blame-hang --blame-hang-timeout 10m
flock --exclusive --close /tmp/zlink-dotnet-gate.lock \
  dotnet test framework/languages/dotnet/tests/Zlink.Framework.UnitTests \
  --filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests' \
  --blame-hang --blame-hang-timeout 10m
flock --exclusive --close /tmp/zlink-dotnet-gate.lock \
  dotnet test framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests \
  --blame-hang --blame-hang-timeout 10m
export ZLINK_SAMPLE_EVIDENCE_DIR=/dev/shm/zlink-cs-server-ready-gate-20260906/samples
flock --exclusive --close /tmp/zlink-samples-gate.lock \
  flock --exclusive --close /tmp/zlink-dotnet-gate.lock \
  bash framework/languages/dotnet/samples/run_samples.sh
```

Samples는 공용 sample lock을 먼저 얻은 뒤 .NET lock을 얻는다. Sample의 기존 Normal diagnostics와
file logger를 사용하고 `ZLINK_SAMPLE_EVIDENCE_DIR`로 첫 실행부터 로그를 보존한다.

환경은 `source framework/languages/dotnet/perf/scripts/dotnet-env.sh`를 사용했다.
`TMPDIR=/dev/shm/zlink-tmp-dotnet`, `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
`NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-350b8b789a1b3132`, `UseSharedCompilation=false`,
`MSBUILDDISABLENODEREUSE=1`, telemetry off다.

- Local NuGet SHA256: `350b8b789a1b31328bd477d895283efc6b986a1b242c310eadda914cf79c98c3`.
- `core/build-dev/lib/libzlink.so.0.17.0` SHA256: `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`.

## BLOCKERS

**Unit gate 0 failures 조건은 충족하지 못했다.**
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:94`의
`IdempotentAdmit_CompletesWithoutReadmittingOrResettingTheEpoch`가 `:122`에서 첫 Hello의 Admit reply를
기다리다가 `:414`의 기존 2초 deadline으로 실패했다. `:121`의 admitted count 1 확인은 통과한 뒤다.
실패는 `unit-main.log:37`과 TRX에 보존했다.

이 test는 `:96,97,107`에서 binding context, `ZLinkManagedMeshNode`, Dealer를 직접 만들며,
ClientServer monitoring service나 이번 수정의 host/status projection을 호출하지 않는다.
따라서 B1과 다른 RouteMesh admission 경로의 실패다. Native 송수신 또는 test scheduling 중
어디서 reply 관측이 지연됐는지 root cause는 확정하지 못했다.

기존 control-plane 진단 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`,
`ZLINK_DEBUG_FRAMEWORK_TASKS=1`을 활성화하고 해당 test만 1회 실행했다.
단독 실행은 200 ms에 통과했으며 control-plane trace line은 출력되지 않았다.
Application message-flow에 잡히지 않는 admission 경로이므로 이 결과를 flow 증거로 해석하지 않는다.
Timeout·retry·assertion 변경과 전체 gate 재실행은 하지 않았다. 단독 진단 성공으로 최초 gate 실패를 지우지 않는다.

Sample gate에는 남은 실패가 없다. 타 언어 monitoring gap은 위 file:line에 남겼으며 이 작업 범위 밖이다.
Perf matrix 재측정은 병행 중인 perf 작업의 범위다. 이 수정으로 perf 처리량이나 다른 RouteMesh deadline 문제가 해결됐다고 판정하지 않는다.

작업 branch는 `main`이며 commit은 하지 않았다. Core, binding, protected spec/doc, 다른 언어 runtime,
`framework/languages/dotnet/perf/`는 수정하지 않았다. 작업 시작 전에 존재하던 Node 변경과 untracked 파일은 보존했다.
