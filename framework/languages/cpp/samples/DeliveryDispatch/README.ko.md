# DeliveryDispatch C++ 샘플

DeliveryDispatch 샘플은 배달 생성, courier 배정, 픽업, 완료까지의 상태 전이를 C++ framework로 검증한다.
구조와 호출 순서는 공통 [DeliveryDispatch sample](../../../../doc/framework/common/sample/deliverydispatch/README.ko.md)을
따른다. Client는 HTTP API로 배달을 생성하고 stream connector로 고객 세션과 배송원 세션을 연 뒤
상태 알림과 배송 제안을 기다린다.

## 실행

```bash
./run_sample.sh
```

## Topology

- `Client`는 배달 dispatch 흐름을 시나리오처럼 검증한다.
- 각 server role은 Redis location store를 공유해 channel/spot/route 위치를 발견한다.
- `Server/Dispatch`는 `/deliveries`와 `/self-check/assert` HTTP API를 제공하고, 배차 큐를 비우는
  DispatchWorker가 courier 제안과 tracking 상태 갱신을 조율한다. Courier actor와 stream session의
  연결은 Framework가 관리하며 application message에는 session route를 넣지 않는다.
- `Server/CourierSession`은 배송원 stream 연결을 받고 actor session binding을 연결한다.
- `Server/CourierActorNode`는 배송원 actor와 entry spot을 실행하며, runner가 node 2개를 시작한다.
- `Server/CustomerGateway`는 고객 stream 연결, 고객 actor binding, 상태 push를 맡는다.
- `Server/Tracking`은 상태 증거를 기록하고 fanout으로 고객 세션에 상태 알림을 발행한다.
- `Shared`는 배달 상태 계약을 정의한다.

## Public executables

- `sample_cpp_framework_deliverydispatch_dispatch`: HTTP API, 배차 큐, DispatchWorker(courier offer와 timeout 재배차)
- `sample_cpp_framework_deliverydispatch_courier_actor_node`: courier entry spot과 courier actor
- `sample_cpp_framework_deliverydispatch_customer_gateway`: customer stream, customer actor, status fanout
- `sample_cpp_framework_deliverydispatch_courier_session`: courier stream과 actor session binding
- `sample_cpp_framework_deliverydispatch_tracking`: tracking channel, evidence store, status fanout publish
- `sample_cpp_framework_deliverydispatch_client`: client scenario 안에서 수행하는 HTTP 생성, stream subscribe, 배송원 offer/decision, full client/server self-check

테스트 전용 fake 서버는 이 샘플의 공개 실행 파일에 넣지 않는다. runner는 위 실행 파일을 별도
process로 시작하고, client scenario가 실제 HTTP/stream/process 경계를 지나 성공 배차와 timeout
재배정을 확인한다.

## Success Condition

runner가 `deliverydispatch sample result=passed`를 출력하면 location readiness, delivery reassignment,
server evidence, message-flow evidence가 함께 검증된 것이다.

## 회귀 테스트

`run_sample.sh`는 CMake sample target을 먼저 빌드한 뒤 역할별 process를 실행한다.
