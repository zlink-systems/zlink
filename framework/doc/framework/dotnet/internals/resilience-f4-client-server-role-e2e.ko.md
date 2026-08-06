# RL-F4 ClientServer role 분리 E2E 구현

이 문서는 공통 resilience lifecycle E2E의 RL-F4 시나리오를 .NET
`ResilienceLifecycle` fixture에서 검증하는 방법과 구현상 제약을 기록한다.
시나리오의 공개 계약은 [공통 RL-F4 시나리오](../../common/e2e/config-5-resilience-lifecycle.ko.md)와
[ClientServer Channel 명세](../../common/spec/09-client-server-channel.ko.md)에 있다.

## 검증 대상

ClientServer에서 Server role만 등록한 process는 outbound Client egress를
갖지 않는다. 따라서 같은 `ChannelName`으로 request를 시작해도 server-only
process의 호출은 `NotFound`로 끝나며 server handler는 실행되지 않아야 한다.
별도의 Client role을 등록한 process의 호출은 준비된 server에 전달되고
handler가 한 번 실행되어 정상 response를 반환해야 한다.

## Fixture 구성

Provider A와 Provider B는 ClientServer Server role을 등록한다. Consumer는
Client role을 등록하고 두 provider의 endpoint를 사용한다. Provider A의
weight를 100, Provider B의 weight를 0으로 설정해 정상 Client 호출의 target을
결정한다.

Provider process에는 server-only 호출을 시도하는 public HTTP endpoint가 있다.
이 endpoint는 process 내부에서 ClientServer channel의 outbound 호출을
시도하고, Framework의 `NotFound` 결과를 typed response로 반환한다. 호출
전후의 handler evidence가 같아야 server-only 호출이 handler를 실행하지
않았다고 판정한다.

Consumer process는 같은 HTTP endpoint를 사용하지만 Client role을 통해
정상 호출을 수행한다. 테스트는 provider-local 상태만 기다리지 않고
consumer의 public ClientServer status에서 두 target이 ready이며 weight가
각각 100과 0으로 관찰될 때까지 기다린다. 이 순서를 생략하면 topology
descriptor가 consumer에 전파되기 전에 request가 실행되어 시나리오 자체가
간헐적으로 실패할 수 있다.

## 검증 순서

1. Provider A와 B의 ClientServer server endpoint를 준비한다.
2. Consumer의 public ClientServer status에서 target readiness와 weight 전파를
   확인한다.
3. Provider A의 server-only endpoint에 request를 보낸다.
4. response가 `Succeeded=false`, `ErrorKind=NotFound`인지 확인하고 handler
   evidence가 변하지 않았는지 확인한다.
5. Consumer의 정상 Client endpoint에 같은 channel request를 보낸다.
6. Provider A가 marker를 포함한 request를 한 번 처리하고 response를 반환하는지
   확인한다.

## 구현 및 성능 경계

이 시나리오는 binding private member, reflection, raw frame 또는 별도
wrapper를 사용하지 않는다. ClientServer readiness는 Framework public status를
통해 관찰하고, 호출 결과는 Framework public error와 typed response로
확인한다. 검증 경로에 message별 codec 등록이나 추가 task를 넣지 않으며,
정상 message hot path의 allocation·copy·lock 비용을 변경하지 않는다.

## 검증 결과

2026-08-06 실행 결과:

```text
scenario RL-F4 passed
Server-only call: NotFound, handler evidence unchanged
Normal ClientServer call: Provider A handled the marker once
Log: framework/languages/dotnet/e2e/ResilienceLifecycle/logs/20260806-172727-3192643
```

실패했던 첫 실행은 server-only 계약 위반이 아니었다. Provider A의
server-only 호출은 이미 `NotFound`와 evidence 불변을 만족했지만, Consumer에
Provider B의 weight 0 descriptor가 전파되기 전에 정상 호출을 시작했다. 이후
구현은 Consumer public status의 readiness와 weight를 확인한 다음 정상 호출을
시작하도록 수정되었고, 재실행에서 공통 시나리오와 같은 순서로 통과했다.
