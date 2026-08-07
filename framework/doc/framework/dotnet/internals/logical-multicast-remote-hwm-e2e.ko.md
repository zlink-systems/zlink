# .NET Logical Multicast 원격 HWM 검증

이 문서는 .NET RuntimeMonitoring E2E가 Logical Multicast의 원격 전달과
application dispatch HWM 상태를 어떤 순서로 검증하는지 설명한다. 공개 동작의 기준은
[Spot messaging §4](../../common/spec/12-spot-messaging.ko.md)와
[Runtime monitoring §2.2](../../common/spec/24-runtime-monitoring.ko.md)이다.

## 검증 구성

`MON-B1`은 세 개의 독립된 service process를 사용한다. `svc-a`는 source이고,
`svc-b`와 `svc-c`는 같은 channel과 topic을 제공하는 remote target이다.
User Spot은 process별로 다른 stable type을 사용한다. 이 설정은 User Spot 생성 요청이
cluster 전체에서 임의의 eligible node로 이동하지 않고, 각 target process의 factory에
배치되도록 한다. 이는 target process를 직접 지정하는 비공개 우회가 아니다.

`svc-b`의 subject handler는 application gate에서 대기하고 `svc-c`의 handler는
처리를 계속한다. source는 먼저 큰 blocker를 `monitor.blocker` topic으로 제출한다.
`svc-b`가 blocker handler에 진입한 뒤 `/runtime/host/status`의
`InboundDispatch.ApplicationReceivePaused`가 `true`인지 확인한다. 이 확인은 handler가
대기한다는 사실과 HWM이 새 application receive를 막았다는 사실을 구분한다.

그 다음 source는 고유 marker를 `monitor.dynamic` topic으로 한 번 제출한다. 검증 순서는
다음과 같다.

1. `svc-c`가 marker를 한 번 처리하는지 확인한다.
2. `svc-b`의 application gate를 해제한다.
3. `svc-b`가 marker를 한 번 처리하는지 확인한다.
4. source의 peer와 channel readiness가 publish 결과를 나타내는 별도 target count로
   변하지 않았는지 확인한다.

## Socket HWM 설정의 책임

Logical Multicast의 source RouteMesh socket은 control traffic과 application publish가
같은 송신 queue를 사용한다. E2E fixture의 sender HWM을 `4096`으로 둔다. 이 값은
blocked application target의 HWM을 대신하지 않는다. sender queue가 control traffic으로
가득 차서 marker 제출 자체가 `Backpressured`가 되면, accepted remote target의 전달
독립성을 검증할 수 없기 때문이다.

blocked target의 application HWM은 별도로 작은 값으로 설정한다. 따라서 sender queue의
수용 여유와 target application receive pause를 서로 다른 상태로 관찰할 수 있다.

## 확인해야 하는 구현 경계

Framework는 publish 호출부에 target별 결과를 반환하거나 target별 재시도 정책을 노출하지
않는다. source의 RouteMesh status는 peer와 channel의 연결 상태만 나타내며,
application handler의 처리 수를 대신하지 않는다.

Target handler는 message payload를 다시 해석하지 않는다. typed `ProfileReq`를 받아
application evidence를 기록하고, gate와 runtime status는 각각 해당 책임을 가진 public
경로로 확인한다. 이 구조는 raw frame 처리와 message별 codec 등록을 E2E 코드에 추가하지
않는다.
