# RL-A5 Provider lifecycle 반복 수렴 E2E

이 문서는 공통 resilience lifecycle E2E의 RL-A5를 .NET
`ResilienceLifecycle` fixture에서 검증하는 방법을 기록한다. 공개 계약은
[공통 RL-A5 시나리오](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
[Runtime monitoring 명세](../../common/spec/server/06-observability/01-runtime-monitoring.ko.md)에 있다.

## 공통 시나리오와 구현

Provider A와 B를 ready 상태로 시작한 뒤 Provider B를 다섯 번 종료하고
replacement로 다시 시작한다. 각 down 구간에는 consumer request가 Provider A에
도달하는지 확인한다. replacement가 public topology에서 ready가 되면 현재
endpoint와 새로운 routing identity를 확인하고, Provider A의 weight를 0으로
설정한다. Consumer public traffic이 A를 제외한 뒤 directed request가 current
Provider B에서 처리되는지 확인하고, 다음 cycle을 위해 A의 weight를 복원한다.

replacement readiness는 descriptor 발견만으로 판정하지 않는다. 실제 HTTP health,
public topology row, consumer connection-ready evidence를 순서대로 확인한다.
Provider replacement의 startup budget은 반복 lifecycle과 store 초기화 지연을
포함할 수 있도록 30초의 bounded timeout을 사용한다.

## 검증 결과

2026-08-06 실행 결과:

```text
scenario RL-A5 passed
Provider B lifecycle cycles: 5
Down window: requests handled by api-a
Directed window: api-a weight=0, requests handled by current api-b
Topology: previous api-b routing identity was not retained as ready
Log: framework/languages/dotnet/e2e/ResilienceLifecycle/logs/20260806-175549-2881034
```

이전 fixture는 replacement 뒤 두 provider가 모두 request를 처리하는지만
확인했다. 이는 공통 시나리오가 요구하는 A weight 0 directed verification과
달랐으므로 구현 gap으로 분류했다. 같은 실패 실행에서 cleanup 예외가 원래
scenario 예외를 덮지 않도록 client runner도 primary exception을 보존한다.

## 성능 및 경계

이 시나리오는 public topology, public runtime weight와 typed request 결과만
사용한다. binding private member, reflection, raw frame, 메시지별 codec 또는
hot path용 추가 wrapper를 사용하지 않는다. 반복 cycle 사이의 Redis와 process는
runner가 독립적으로 관리하며, scenario 간 상태를 재사용하지 않는다.
