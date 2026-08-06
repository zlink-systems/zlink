# RL-E2 half-open connection 검증과 현재 gap

이 문서는 .NET `ResilienceLifecycle`의 RL-E2 검증에서 확인한 fixture 구성과 구현 gap을 기록한다. 공통 시나리오의 계약은
[`config-5-resilience-lifecycle.ko.md`](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
transport liveness spec에 있으며, 이 문서는 그 계약을 변경하지 않는다.

## 검증 범위

RL-E2는 두 target이 ready인 상태에서 한 target으로 향하는 TCP 흐름의 한 방향만 버린다. 반대 방향의 애플리케이션 응답은
계속 전달할 수 있어야 하며, liveness round trip이 완료되지 않은 target만 공통 deadline 안에 not-ready가 되어야 한다.

.NET fixture는 RouteMesh와 ClientServer를 서로 다른 channel로 등록한다. Provider B는 `127.0.0.2`에 bind하고 `127.0.0.1`을
advertise하며, 각 public endpoint 앞에 방향성 TCP proxy를 둔다. proxy는 `/block/client-to-target` 제어 요청 뒤 client에서
target으로 향하는 bytes만 버리고 target에서 client로 향하는 bytes는 전달한다. proxy의 `/stats` 결과는 실제 연결과 버린
bytes를 확인하는 독립 증거로 사용한다.

## 확인된 결과

초기 fixture에서는 provider endpoint 쪽 physical connection만 차단했다. 그러나 RouteMesh는 provider와 consumer 사이에
양방향 physical connection을 별도로 만들기 때문에 consumer가 provider B에서 직접 받는 liveness probe는 계속 도착했다.
그 결과 proxy의 bytes는 버려졌지만 public RouteMesh status는 `ReadyTargetCount=2`를 유지했다. 이 결과는 fixture가 connection
pair 전체를 차단하지 못한 것으로 판정했다.

수정된 fixture는 provider B endpoint와 consumer endpoint 양쪽에 proxy를 배치한다. consumer endpoint proxy는 초기 ZLink
handshake에 포함된 routing identity를 읽어 `api-b` connection만 분류하고, A connection은 계속 전달한다. 두 physical
connection에서 liveness probe가 향하는 client-to-target 흐름을 차단하면 B의 liveness round trip만 실패한다. 차단 전에
B handler에 request를 진입시키고 gate를 차단 뒤 해제하여, reverse application reply가 계속 전달되는지도 확인한다. 마지막
실행의 결과는 다음과 같다.

```text
RouteMesh and ClientServer: two targets ready before the block
Directional proxy: api-b connection pair only
Result: affected target became not-ready within the liveness budget
Result: reverse application reply completed while the fault was active
Result: surviving target request succeeded
Log: logs/20260806-164652-2539767
```

따라서 RL-E2는 RouteMesh와 ClientServer 양쪽에서 공통 시나리오의 liveness deadline 및 failure isolation 조건을 통과했다.

## 구현 경계

proxy는 초기 handshake를 통과시켜 routing identity를 확인한 뒤 선택된 connection의 liveness 방향 bytes를 버린다.
이 과정은 Framework public API나 binding internal member에 의존하지 않으며, fixture 내부의 TCP fault injection 책임으로
한정된다. proxy의 routing identity 판정은 application payload를 해석하는 codec 경로가 아니라 transport handshake의
초기 식별 정보만 사용한다.

RouteMesh status의 `NodeRid`는 이 fixture에서 public target routing id가 아니라 transport identity로 표시된다. 따라서 시나리오는
identity 문자열을 추정하지 않고 channel의 ready target 수와 public request 결과를 사용해야 한다.
