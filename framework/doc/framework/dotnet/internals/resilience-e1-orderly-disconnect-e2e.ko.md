# RL-E1 orderly disconnect 검증

이 문서는 .NET `ResilienceLifecycle`의 RL-E1 E2E fixture가 공통 resilience lifecycle 시나리오를 어떻게 검증하는지 기록한다. 공통 시나리오의 계약은 [`config-5-resilience-lifecycle.ko.md`](../../common/e2e/config-5-resilience-lifecycle.ko.md)의 RL-E1과 Transport Liveness spec이 소유한다.

## 검증 범위

RL-E1은 RouteMesh와 ClientServer에 각각 두 target을 ready 상태로 만든 뒤, provider B를 두 방식으로 종료한다.

- `normal-close`: provider의 public shutdown을 호출한다.
- `rst`: provider B process를 강제 종료한다.

각 variant에서 RouteMesh와 ClientServer의 affected target이 ready selection에서 빠지는 시간을 public status로 관찰하고, provider A가 두 channel에서 계속 request를 처리하는지 확인한다. process 종료 여부 확인을 위해 종료된 provider의 HTTP health endpoint를 측정 경로에 사용하지 않는다. 해당 endpoint probe는 별도의 HTTP client liveness deadline을 기다릴 수 있으므로 peer deadline 측정을 오염시킨다.

## Fixture 수명과 재기동

RL-E1 전용 runner는 각 실행에 독립적인 Redis, HTTP port, RouteMesh port, ClientServer port를 할당한다. provider B를 variant 사이에 재기동할 때는 새 ClientServer endpoint를 사용한다. 이 endpoint 분리는 이전 descriptor와 새 physical connection의 수렴을 혼합하지 않으며, 두 variant가 fresh connection 조건을 만족하도록 한다.

재기동 process configuration에는 ClientServer endpoint뿐 아니라 `ClientServerEnabled`도 함께 전달한다. endpoint만 전달하면 재기동 provider가 RouteMesh만 등록해 ClientServer parity가 깨지므로, `DynamicProviderOptions`가 두 값을 모두 기록한다.

## 판정 기준

public RouteMesh topology와 ClientServer status가 affected target의 not-ready 전환을 보고한 시점부터 peer deadline을 측정한다. provider A에 보낸 RouteMesh와 ClientServer request의 응답에는 `ProviderRid=api-a`가 포함되어야 한다. provider B가 다시 ready selection에 포함되거나 provider A request가 실패하면 해당 variant는 실패한다.

2026-08-06 실행 결과:

```text
RouteMesh and ClientServer: two targets ready before each variant
Result: normal-close affected target excluded, elapsedMs=6284
Result: rst affected target excluded, elapsedMs=14325
Result: provider A served RouteMesh and ClientServer requests
Log: framework/languages/dotnet/e2e/ResilienceLifecycle/logs/20260806-170432-307506
```

이 fixture는 raw frame이나 binding private member를 사용하지 않는다. transport fault의 직접 주입은 process lifecycle로 제한하고, 판정은 Framework public status와 typed request 결과로 수행한다.
