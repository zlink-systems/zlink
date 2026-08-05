# Node.js ResilienceLifecycle E2E 포팅 인벤토리

기준 문서: `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md`

기준 구현: `framework/languages/dotnet/e2e/ResilienceLifecycle`

현재 상태: Node.js `ResilienceLifecycle` config는 공통 문서의 현재 scenario를 모두 구현했다.
이 inventory는 `.NET` 기준 파일과 공통 scenario ID를 빠뜨리지 않고, 각 행을 `done` 또는 public contract
각 행을 `done`으로 고정한다.

## Scenario

| Scenario | .NET 기준 파일 | Node.js 대상 파일 | 상태 | 비고 |
|----------|----------------|-------------------|------|------|
| RL-A1 | `Client/Scenarios/RlA1ProviderRestartScenario.cs` | `Client/Scenarios/rl-a1-provider-restart-scenario.ts` | done | provider same-endpoint restart. PASS: `logs/20260703-211853-78546` |
| RL-A2 | `Client/Scenarios/RlA2ProviderEndpointRemapScenario.cs` | `Client/Scenarios/rl-a2-provider-endpoint-remap-scenario.ts` | done | provider endpoint remap. PASS: `logs/20260703-211853-78546` |
| RL-A3 | `Client/Scenarios/RlA3ReconnectStormScenario.cs` | `Client/Scenarios/rl-a3-reconnect-storm-scenario.ts` | done | client reconnect storm. PASS: `logs/20260703-211853-78546` |
| RL-A4 | `Client/Scenarios/RlA4DrainAndGreenEndpointScenario.cs` | `Client/Scenarios/rl-a4-drain-and-green-endpoint-scenario.ts` | done | runtime drain, green provider 전환, same-endpoint original restore. PASS: `logs/20260703-211853-78546` |
| RL-A5 | `Client/Scenarios/RlA5ProviderFlappingScenario.cs` | `Client/Scenarios/rl-a5-provider-flapping-scenario.ts` | done | provider flapping. PASS: `logs/20260703-211853-78546` |
| RL-B1 | `Client/Scenarios/RlB1CancellationCleanupScenario.cs` | `Client/Scenarios/rl-b1-cancellation-cleanup-scenario.ts` | done | cancellation cleanup. PASS: `logs/20260703-211853-78546` |
| RL-B2 | `Client/Scenarios/RlB2CrashDuringInflightScenario.cs` | `Client/Scenarios/rl-b2-crash-during-inflight-scenario.ts` | done | crash during in-flight request. PASS: `logs/20260703-211853-78546` |
| RL-B3 | `Client/Scenarios/RlB3GracefulShutdownScenario.cs` | `Client/Scenarios/rl-b3-graceful-shutdown-scenario.ts` | done | graceful shutdown stale endpoint avoidance. PASS: `logs/20260703-211853-78546` |
| RL-B4 | `Client/Scenarios/RlB4RuntimeDrainScenario.cs` | `Client/Scenarios/rl-b4-runtime-drain-scenario.ts` | done | 11.0 public RouteMesh channel weight로 descriptor 갱신, 신규 선택 제외, accepted work 완료와 복원을 검증했다. PASS: `log/20260729-163732-3388194` |
| RL-B5 | `Client/Scenarios/RlB5DrainInflightScenario.cs` | `Client/Scenarios/rl-b5-drain-inflight-scenario.ts` | done | runtime drain 중 in-flight slow reply 보존. PASS: `logs/20260703-211853-78546` |
| RL-B6 | `Client/Scenarios/RlB6GrayFaultScenario.cs` | `Client/Scenarios/rl-b6-gray-fault-scenario.ts` | done | gray failure and recovery. PASS: `logs/20260703-211853-78546` |
| RL-C1 | `Client/Scenarios/RlC1ClientHostLifecycleScenario.cs` | `Client/Scenarios/rl-c1-client-host-lifecycle-scenario.ts` | done | short-lived client host lifecycle. PASS: `logs/20260703-211853-78546` |
| RL-C2 | `Client/Scenarios/RlC2TopologyRecoveryScenario.cs` | `Client/Scenarios/rl-c2-topology-recovery-scenario.ts` | done | crash stale topology cleanup and recovery. PASS: `logs/20260703-211853-78546` |
| RL-C3 | `Client/Scenarios/RlC3NodePauseRecoveryScenario.cs` | `Client/Scenarios/rl-c3-node-pause-recovery-scenario.ts` | done | node down/recovery simulation. PASS: `logs/20260703-211853-78546` |
| RL-C4 | `Client/Scenarios/RlC4RegistryOutageScenario.cs` | `Client/Scenarios/rl-c4-store-outage-scenario.ts` | done | Redis location store outage and recovery. PASS: `logs/20260703-211853-78546` |
| RL-D1 | `Client/Scenarios/RlD1HighFanoutScenario.cs` | `Client/Scenarios/rl-d1-high-fanout-scenario.ts` | done | 8 subscriber × 120 event fanout. PASS: `logs/20260715-075646-2279409` |
| RL-D2 | `Client/Scenarios/RlD2ObserverFaultScenario.cs` | `Client/Scenarios/rl-d2-observer-fault-scenario.ts` | done | observer failure isolation과 public runtime error sink의 정확한 1회 event를 검증했다. PASS: `log/20260729-162800-3166911` |
| RL-D3 | `Client/Scenarios/RlD3DispatchErrorEvidenceScenario.cs` | `Client/Scenarios/rl-d3-dispatch-error-evidence-scenario.ts` | done | dispatch-error evidence. PASS: `logs/20260703-211853-78546` |
| RL-D4 | `Client/Scenarios/RlD4MissingRequestHandlerScenario.cs` | `Client/Scenarios/rl-d4-missing-request-handler-scenario.ts` | done | decoded error round-trip + raw Error/Response header gate. PASS: `logs/20260715-080129-2299877` |
| RL-D5 | `Client/Scenarios/RlD5MixedBurstScenario.cs` | `Client/Scenarios/rl-d5-mixed-burst-scenario.ts` | done | 8 client, 120초 mixed soak + latency/cleanup observation. PASS: `logs/20260715-082358-2399471` |

## File Mapping

| .NET 기준 영역 | Node.js 대상 영역 | 분류 | 상태 | 비고 |
|----------------|-------------------|------|------|------|
| `.gitignore` | `.gitignore`, `logs/.gitignore` | ignore | done | dist, node_modules, 실행 로그 제외 |
| `feature-map.ko.md` | `feature-map.ko.md` | feature-map | done | RL-A1 evidence와 남은 scenario 상태 기록 |
| `run_e2e.sh` | `run_e2e.sh` | runner | done | 구현 대상 scenario의 build/start/readiness/cleanup/client 실행 구현 |
| `Shared/Messages.cs`, `Shared/ResilienceLifecycle.Shared.csproj` | `Shared/messages.ts` | shared | done | profile request/reply, command, evidence wait, payload, failure result 계약 포팅 |
| `Client/Program.cs`, `Client/ResilienceLifecycle.Client.csproj` | `Client/main.ts`, `Client/package.json`, `Client/tsconfig.json` | client-entry/project | done | scenario 선택과 실행 앱 구현. default `all`에 RL-B5 포함 |
| `Client/Support/*` | `Client/Support/` | support | done | options, assertion, HTTP/process helper 포팅 |
| `Client/Scenarios/*.cs` | `Client/Scenarios/` | scenario | done | RL-A1~RL-A5/RL-B1~RL-B6/RL-C1~RL-C4/RL-D1~RL-D5 구현 |
| topology 관측 | `Server/Consumer/` | consumer | done | 실제 request consumer가 public location runtime query로 peer row를 함께 제공한다. 별도 probe 역할은 두지 않는다. |
| `Server/Provider/*` | `Server/Provider/` | provider-role | done | provider handler, evidence, fault injection, shutdown, crash, runtime drain/restore/weight endpoint 구현 |
| `Server/Consumer/*` | `Server/Consumer/` | consumer-role | done | location store consumer request/send host, timeout/no-retry endpoint, short-lived client endpoint 구현 |

## Public Contract 확인 결과

- Node framework가 dispatch-error observer, topology recovery evidence를
  `.NET`과 같은 public surface로 노출하는지 확인해야 한다.
- public contract가 없으면 internal helper, raw frame, 테스트 전용 adapter로 우회하지 않고 해당 scenario를
  별도 설계 검토 대상으로 분리한다.
