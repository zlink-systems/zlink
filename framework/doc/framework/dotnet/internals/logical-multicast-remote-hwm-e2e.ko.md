# .NET Logical Multicast 원격 job-queue backpressure 검증

이 문서는 .NET RuntimeMonitoring E2E가 Logical Multicast의 원격 전달과
Application Job Queue 포화 상태를 어떤 순서로 검증하는지 설명한다. 공개 동작의 기준은
[Spot messaging §4](../../common/spec/server/12-spot-messaging.ko.md)와
[Core/Framework API §2.1](../../common/spec/server/06-framework-api.ko.md),
[Runtime monitoring §2.2](../../common/spec/server/24-runtime-monitoring.ko.md)이다.

## 검증 구성

`MON-B1`은 세 개의 독립된 service process를 사용한다. `svc-a`는 source이고,
`svc-b`와 `svc-c`는 같은 channel과 topic을 제공하는 remote target이다.
User Spot은 process별로 다른 stable type을 사용한다. 이 설정은 User Spot 생성 요청이
cluster 전체에서 임의의 eligible node로 이동하지 않고, 각 target process의 factory에
배치되도록 한다. 이는 target process를 직접 지정하는 비공개 우회가 아니다.

`svc-b`는 `MaxQueuedApplicationJobs = 1`로 시작하고 subject handler 하나는 application gate에서
대기한다. `svc-c`의 handler는 처리를 계속한다. Source가 먼저 `monitor.blocker`를 제출해
`svc-b` handler의 실제 첫 instruction 진입을 확인한다. 이 순간 reserved permit은 이미 반환됐으므로
handler가 기다린다는 사실만으로 queue가 포화됐다고 판정하지 않는다. 같은 serial execution queue에
두 번째 filler를 넣어 queued permit 하나를 점유한 뒤 `/runtime/host/status`에서
`Capacity.ApplicationJobQueue.PermitsInUse == 1`과 `CapacityWaiters > 0`을 확인한다. 이 상태가
다음 일반 receive가 취소 가능한 permit wait에 들어갔다는 증거다.

그 다음 source는 고유 marker를 `monitor.dynamic` topic으로 한 번 제출한다. 검증 순서는
다음과 같다.

1. `svc-c`가 marker를 한 번 처리하는지 확인한다.
2. `svc-b`의 application gate를 해제한다.
3. filler가 시작하며 permit을 반환한 뒤 `svc-b`가 marker를 한 번 처리하는지 확인한다.
4. source의 peer와 channel readiness가 publish 결과를 나타내는 별도 target count로
   변하지 않았는지 확인한다.

## Socket HWM 설정의 책임

Logical Multicast의 source RouteMesh socket은 control traffic과 application publish가
같은 송신 queue를 사용한다. E2E fixture의 sender HWM을 `4096`으로 둔다. 이 값은
target의 Application Job Queue permit limit을 대신하지 않는다. Sender queue가 control traffic으로
가득 차서 marker 제출 자체가 `Backpressured`가 되면, accepted remote target의 전달
독립성을 검증할 수 없기 때문이다.

따라서 sender의 물리 queue 수용 여유와 target의 job permit wait를 서로 다른 상태로 관찰한다.
Target 포화는 marker를 reject·drop하거나 별도 target 결과로 바꾸지 않는다. 일반 receive 전에
취소 가능하게 기다렸다가 permit이 반환되면 진행한다. Receive 전에 식별할 수 있는 terminal reply·
error completion은 이 permit을 우회해 liveness를 유지한다.

## 확인해야 하는 구현 경계

Framework는 publish 호출부에 target별 결과를 반환하거나 target별 재시도 정책을 노출하지
않는다. source의 RouteMesh status는 peer와 channel의 연결 상태만 나타내며,
application handler의 처리 수를 대신하지 않는다.

Target handler는 message payload를 다시 해석하지 않는다. typed `ProfileReq`를 받아
application evidence를 기록하고, gate와 runtime status는 각각 해당 책임을 가진 public
경로로 확인한다. 이 구조는 raw frame 처리와 message별 codec 등록을 E2E 코드에 추가하지
않는다.
