# RL-E3 stale reply E2E 검증

이 문서는 .NET `ResilienceLifecycle` E2E가 연결 교체 중 이전 request의 reply를
새 request의 결과로 사용하지 않는지 검증하는 방법을 설명한다. 공개 Framework
계약을 추가하지 않으며, 공통 시나리오
[`RL-E3`](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
[Transport 연결 상태 확인](../../common/spec/server/02-channel-transport/05-transport-liveness.ko.md)의
검증 조건을 .NET fixture에 연결하는 내부 설명이다.

## 검증 범위

Provider handler는 첫 request의 reply를 application gate에서 보류한다. Client는
그 request를 보낸 Framework host를 종료해 이전 physical connection을 닫는다.
그 뒤 별도의 Framework host가 replacement connection을 만들고 자신의 marker를
가진 request를 보낸다. Replacement reply가 도착한 뒤에만 이전 gate를 해제한다.

이 순서는 다음 결과를 구분한다.

- 이전 request는 connection 종료 뒤 reply, timeout 또는 route failure 중 하나로
  terminal이 된다.
- Replacement request는 자신의 correlation과 payload에 해당하는 reply만 받는다.
- 이전 gate를 해제해도 이미 종료된 이전 request의 결과가 replacement request를
  바꾸지 않는다.

## Fixture 책임

`FaultState`는 marker별 `TaskCompletionSource<bool>`를 보관한다. E2E admin endpoint가
gate를 만들고 해제하지만, application message 경로는 이 endpoint를 호출하지 않는다.
`ProfileRequestHandler`는 해당 marker의 gate가 있을 때만 handler 내부에서 대기한다.
Gate 대기는 Framework가 전달한 `CancellationToken`을 사용하므로 connection 종료가
handler의 대기를 끝낼 수 있다.

`EphemeralRouteSession`은 하나의 Framework host와 하나의 RouteMesh client를 함께
소유한다. `StartAsync`는 peer가 Ready이고 channel이 selectable인 뒤에 반환한다.
`DisposeAsync`는 request를 다른 host로 넘기지 않고 현재 host의 connection과 runtime
resource를 정리한다. Replacement session은 별도의 routing ID prefix를 사용해
이전 session의 physical connection 상태를 재사용하지 않는다.

## 시나리오 순서

1. `api-a`의 weight를 0으로 바꾸어 old request가 `api-b`를 선택하도록 한다. Weight
   변경은 topology descriptor row를 삭제하지 않으므로, fixture는 row 삭제를
   selection 제외의 증거로 사용하지 않는다.
2. `api-b`에 old marker의 gate를 만들고 old session에서 `held` request를 제출한다.
3. `profile-start` evidence로 handler 진입을 확인한 뒤 old session을 종료한다.
4. Replacement session을 만들고 new marker의 `fast` request가 자신의 reply를 받는지
   확인한다.
5. old gate를 해제하고 old request가 성공 reply 없이 terminal이 되었는지 확인한다.
6. Provider evidence에서 old marker의 handler completion이 중복되지 않았는지 확인하고
   `api-a` weight를 원래 값으로 복원한다.

## 완료 판정

이 fixture만 통과해도 RL-E3 전체 공통 계약이 자동으로 완료되는 것은 아니다. 실제
판정에는 old request의 terminal 종류가 한 번인지, replacement reply의 marker와
provider identity가 일치하는지, 다른 provider로 자동 재제출되지 않았는지를 함께
확인해야 한다. 테스트가 실패하면 먼저 gate 진입, old session 종료, replacement
channel selectable 상태와 gate 해제 순서를 로그에서 확인한 뒤 Framework 구현 결함과
fixture의 시나리오 gap을 분리한다.
