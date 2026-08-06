# RL-E4 connection loss race E2E 검증

이 문서는 .NET `ResilienceLifecycle` E2E가 connection loss와 request completion이
경쟁하는 상황에서 공통 RL-E4 계약을 어떻게 확인하는지 설명한다. 공개 Framework
계약을 추가하지 않으며, 공통 시나리오
[`RL-E4`](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
[Transport 연결 상태 확인](../../common/spec/29-transport-liveness.ko.md)의
검증 조건을 .NET fixture에 연결한다.

## 검증 범위

세 variant를 각각 새 RouteMesh client session에서 실행한다.

1. request 제출 직후 connection을 닫아 admission과 disconnect가 경쟁한다.
2. handler 진입 evidence를 확인한 뒤 connection을 닫고 reply gate를 해제한다.
3. handler 진입 evidence를 확인한 뒤 reply gate 해제와 connection 종료를 동시에 시작한다.

각 variant는 request 결과가 정상 reply 또는 하나의 오류 terminal 중 하나로 끝나는지
확인한다. Provider evidence에서는 같은 marker의 handler completion이 두 번 기록되지
않는지 확인한다. Provider A의 weight를 0으로 유지해 Framework가 같은 operation을
다른 provider에 자동 재제출하는 경로를 배제한다.

## 수명과 gate 책임

`EphemeralRouteSession`은 하나의 Framework host와 하나의 physical RouteMesh
connection을 소유한다. session을 종료하면 현재 request를 새 host로 넘기지 않고 해당
connection과 runtime resource를 정리한다. Provider의 gate는 E2E admin endpoint가
만들고 해제하지만, message handler는 gate가 존재하는 marker에만 대기한다.

Routing ID prefix에는 짧은 session identity를 사용하고, request marker는 evidence와
gate를 식별하는 별도 값으로 유지한다. 두 값을 합치지 않아 transport identity 길이
제한이 업무 marker의 길이에 영향을 받지 않는다.

## 실패 판정

테스트가 실패하면 먼저 다음 순서를 확인한다.

- 해당 marker의 `profile-start` evidence가 있었는가.
- old session 종료가 replacement나 retry 없이 수행됐는가.
- gate 해제가 connection 종료보다 먼저 또는 동시에 수행됐는가.
- 같은 marker의 `profile-request` evidence가 한 번을 넘었는가.

이 evidence와 public request 결과가 공통 시나리오의 조건을 충족하지 않으면 fixture
gap과 Framework terminal·admission·disconnect 처리 오류를 분리해 기록한다.
