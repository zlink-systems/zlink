# RL-B1 Client cancellation과 후속 request E2E

이 문서는 공통 resilience lifecycle E2E의 RL-B1을 .NET
`ResilienceLifecycle` fixture에서 검증하는 방법을 기록한다. 공개 계약은
[공통 RL-B1 시나리오](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
[비동기 실행과 handler turn 명세](../../common/spec/server/01-execution/README.ko.md)에 있다.

## 공통 시나리오와 fixture의 대응

공통 시나리오는 단순한 짧은 timeout endpoint를 요구하지 않는다. Provider
handler가 첫 request를 application gate에서 보류하고, 실제 handler 진입을
확인한 뒤 caller의 await를 취소한다. 그 다음 별도 operation ID로 후속
request를 보내고, 첫 gate를 해제한 뒤 늦은 첫 reply가 후속 completion을
오염시키지 않는지 확인한다.

.NET fixture는 두 provider에 같은 marker의 request gate를 설치한다. 어느
provider가 target으로 선택되더라도 동일한 절차를 적용할 수 있도록 하기 위한
구성이다. Provider evidence의 `profile-start`를 먼저 관찰한 뒤 caller
`CancellationToken`을 취소하고, 후속 request의 typed reply를 확인한다. 마지막
으로 gate를 해제해 첫 handler가 종료되는지 확인한다.

## 검증 결과

2026-08-06 실행 결과:

```text
scenario RL-B1 passed
First request: caller cancellation completed once after handler admission
Follow-up request: profile:fast reply completed normally
Late first reply: completed only for the first correlation after gate release
Log: framework/languages/dotnet/e2e/ResilienceLifecycle/logs/20260806-173546-401329
```

이전 실패는 fixture가 `/profile/request/timeout/100` endpoint와 `slow` delay를
사용해 공통 시나리오의 gate 절차를 실행하지 않았기 때문에 발생했다. 해당
실패를 Framework cancellation bug로 판정하지 않고 시나리오-구현 gap으로
분류한 뒤 fixture를 수정했다.

## 성능 및 경계

gate와 evidence는 E2E fault injection에만 사용한다. Framework message hot path에
새 wrapper, raw frame 처리, message별 codec 또는 중복 lock을 추가하지 않는다.
caller cancellation은 public `CancellationToken`으로 전달하고, late reply의
처리는 Framework correlation 수명 규칙에 맡긴다.
