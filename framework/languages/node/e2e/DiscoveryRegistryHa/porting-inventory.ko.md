# Node.js StoreFailure E2E 포팅 인벤토리

기준 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

이 디렉토리는 과거 registry HA 시나리오 이름을 가진 위치에 남아 있지만, 현재 구현 기준은
Redis location store 장애·복구 시나리오다. legacy registry cluster 시나리오와 embedded/probe
role은 제거했다.

## Scenario inventory

| Scenario | Client | Runner | 상태 |
|----------|--------|--------|------|
| SF-A1 | `Client/Scenarios/SfA1BaselineScenario.ts` | `run_e2e.sh SF-A1` | done |
| SF-A2 | `Client/Scenarios/SfA2PollingFallbackScenario.ts` | `run_e2e.sh SF-A2` | done |
| SF-B1 | `Client/Scenarios/SfB1StoreOutageScenario.ts` | `run_e2e.sh SF-B1` | done |
| SF-B2 | `Client/Scenarios/SfB2StoreFailureGraceScenario.ts` | `run_e2e.sh SF-B2` | done |
| SF-C1 | `Client/Scenarios/SfC1ProviderCrashScenario.ts` | `run_e2e.sh SF-C1` | done |
| SF-C2 | `Client/Scenarios/SfC2GracefulShutdownScenario.ts` | `run_e2e.sh SF-C2` | done |
| SF-D1 | `Client/Scenarios/SfD1ShortOutageRecoveryScenario.ts` | `run_e2e.sh SF-D1` | done |
| SF-D2 | `Client/Scenarios/SfD2LongOutageRecoveryScenario.ts` | `run_e2e.sh SF-D2` | done |
| SF-D3 | `Client/Scenarios/SfD3RuntimeStatusTransitionScenario.ts` | `run_e2e.sh SF-D3` | done |
| SF-E1 | `Client/Scenarios/SfE1StoreResponseDelayScenario.ts` | `run_e2e.sh SF-E1` | done |

## Role inventory

| Role | Node path | 상태 | 메모 |
|------|-----------|------|------|
| Shared | `Shared/messages.ts`, `Shared/location-store.ts` | done | profile packet DTO와 Redis location store helper |
| Client | `Client/` | done | SF scenario dispatch만 유지 |
| Provider | `Server/Provider/` | done | Redis location store로 peer row와 owner lease 등록 |
| Consumer | `Server/Consumer/` | done | Redis location store + public runtime query endpoint + profile request endpoint |
| Store probe | `Server/LocationProbe/` | done | legacy registry module 없이 location store row/topology/status probe 역할만 수행 |

## Removed legacy inventory

- old registry-cluster client scenario files
- old registry-cluster support helpers
- old in-process combined role
- old remote query role
- old pub/router endpoint cluster harness
