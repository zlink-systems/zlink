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

세 번의 RL-E2 실행에서 proxy는 연결을 수락했고 `blocked=true` 및 `droppedBytes` 증가를 기록했다. 마지막 실행의 결과는
다음과 같다.

```text
RouteMesh channel: resilience.profile: ReadyTargetCount=2, IsReady=true
Proxy: accepted=2, droppedBytes=4864
Elapsed: 20 seconds or more after the block
```

따라서 proxy가 테스트 경로에 들어가지 않았다는 fixture gap은 아니다. 그러나 public RouteMesh status가 affected target을
not-ready로 바꾸지 않아, 공통 시나리오의 15초 liveness deadline을 만족하지 못했다. 이 결과는 RL-E2 완료 증거가 아니다.

## 남은 구현 조건

RouteMesh liveness owner가 client→server half-open 흐름에서 probe·ack deadline을 만료시키고, 해당 channel의
`ReadyTargetCount`와 public request selection을 갱신해야 한다. 수정 뒤에는 같은 검증을 ClientServer channel에도 수행하고,
반대 target request 성공과 반대 방향 application traffic 지속 조건을 확인해야 한다. 이 조건을 통과하기 전에는 feature-map의
RL-E2를 구현으로 변경하지 않는다.

RouteMesh status의 `NodeRid`는 이 fixture에서 public target routing id가 아니라 transport identity로 표시된다. 따라서 시나리오는
identity 문자열을 추정하지 않고 channel의 ready target 수와 public request 결과를 사용해야 한다.
