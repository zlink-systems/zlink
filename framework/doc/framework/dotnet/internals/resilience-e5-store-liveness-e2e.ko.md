# RL-E5 Store 장애와 transport liveness 검증

이 문서는 .NET `ResilienceLifecycle`의 RL-E5 fixture가 Store 장애와 transport liveness를 분리해 검증하는 방법을 기록한다. 공통 시나리오의 계약은 [`config-5-resilience-lifecycle.ko.md`](../../common/e2e/config-5-resilience-lifecycle.ko.md)의 RL-E5와 Transport Liveness spec이 소유한다.

## 실행 순서

1. RouteMesh provider 두 target과 consumer connection을 public status에서 ready로 확인한다.
2. consumer와 provider가 사용하는 Redis container를 pause한다. 이 상태에서는 Location Store가 새 응답을 처리하지 않지만 established connection은 Store 상태와 독립적으로 실행되어야 한다.
3. provider B의 RouteMesh connection pair를 directional proxy 두 개로 차단한다. provider endpoint proxy는 client-to-target 흐름을 차단하고, consumer endpoint proxy는 routing identity가 `api-b`인 connection의 target-to-client 흐름을 차단한다.
4. consumer의 public RouteMesh status에서 ready target 수가 감소하는지 관찰한다. 이 판정은 Store 응답이 아니라 transport liveness 결과를 확인한다.
5. Store가 pause된 상태에서 consumer Host의 public shutdown을 호출하고 process terminal을 기다린다.

shutdown 이후 provider A의 public evidence를 다시 읽어 shutdown 뒤 새 handler 실행이 발생하지 않았는지 확인한다. consumer process가 종료된 뒤에는 status endpoint를 반복 호출하지 않는다. 종료된 HTTP client가 별도의 transport timeout을 기다리면서 shutdown 판정을 오염시킬 수 있기 때문이다.

## 판정 결과

2026-08-06 실행 결과:

```text
Store: paused before packet blackhole
Transport: affected RouteMesh target became not-ready
Shutdown: consumer process reached terminal while Store remained paused
Post-shutdown: provider handler evidence did not increase
Log: framework/languages/dotnet/e2e/ResilienceLifecycle/logs/20260806-171802-1911880
```

이 fixture는 binding private member, raw protocol frame, Store 내부 자료구조를 사용하지 않는다. fault injection은 E2E process와 TCP proxy의 책임이고, liveness·shutdown·handler 실행 판정은 Framework public status, public shutdown endpoint, process terminal 및 typed evidence로 수행한다.
