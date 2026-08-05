# C++ DeliveryDispatch Sample feature map

기준 문서: `framework/doc/framework/common/sample/deliverydispatch/README.ko.md`

이 파일은 공통 sample 문서에 대응하는 C++ 역할, 메시지 흐름과 검증 근거를 기록한다. 이 sample에는
공통 E2E scenario ID가 없으므로 아래 `DD-*` 식별자는 C++ feature map 안에서만 사용한다.

| 항목 | 상태 | 근거 |
|------|------|------|
| `DD-A1` location readiness | 구현 | runner가 Tracking을 포함한 역할 endpoint의 readiness를 기다리고, client가 실제 배달 흐름을 시작한 뒤 Tracking evidence를 확인한다. `run_sample.sh`의 성공 출력은 `deliverydispatch sample result=passed`다. |
| `DD-A2` successful delivery | 구현 | client가 `delivery-success`를 생성하고 `Assigned`, `Accepted`, `PickedUp`, `Delivered` push를 customer stream connector로 기다린다. CustomerGateway는 customer actor를 stream에 bind하고 bound session으로 status를 push한다. courier-a stream session은 `OfferDeliveryNotify`를 받은 뒤 `CourierDecisionMsg`로 수락한다. 최신 통과: `timeout 420s framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`, 출력: `deliverydispatch sample result=passed`. |
| `DD-A3` delivery reassignment | 구현 | courier-a stream session이 첫 `OfferDeliveryNotify`를 받은 뒤 응답하지 않고, dispatch timeout 뒤 courier-b stream session이 두 번째 offer를 받아 `CourierDecisionMsg`로 수락한다. client는 `deliverydispatch-reassignment=completed` marker를 확인한다. 최신 통과: `timeout 420s framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`, 출력: `deliverydispatch sample result=passed`. |
| `DD-A4` server evidence self-check | 구현 | `/self-check/assert`가 두 delivery의 상태 순서를 evidence log에서 확인하고 `deliverydispatch-server-evidence=completed` marker를 출력한다. 최신 통과: `timeout 420s framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh`. |
| `DD-A5` message-flow evidence | 구현 | runner가 `flow-dispatch.log`, `flow-customer-gateway.log`, `flow-courier-session.log`, `flow-delivery-courier-node-1.log`, `flow-delivery-courier-node-2.log`의 `message flow` 기록을 확인한다. |
| role split | 구현 | Dispatch, Tracking, CustomerGateway, CourierSession, CourierActorNode, Client를 별도 executable로 실행한다. CourierActorNode 실행 파일은 서로 다른 설정으로 두 번 시작한다. Customer stream과 courier stream endpoint도 분리한다. |
| shared contract | 구현 | C++ `Shared/Contracts/messages.hpp`가 배송 생성, tracking, customer subscription, courier session bind, offer notify와 courier decision DTO를 제공한다. Client wire에는 `ActorRef`, owner `NodeRid`와 session route를 넣지 않으며 public JSON stream connector codec 경로를 사용한다. |
| tracking file split | 구현 | CustomerActor, DeliverySpotDirectory, DeliveryTrackingSpot, CustomerEntrySpot, Tracking handler를 별도 header로 나누고 role wiring만 `Tracking/main.cpp`에 둔다. |

## C++ 구현 배치

- CMake target 하나가 실행 역할 하나를 만든다. 같은 CourierActorNode target을 서로 다른 node 설정으로
  두 번 시작한다.
- `sample_log_dir.hpp`와 framework trace option이 역할별 message-flow 파일을 구성한다.
- Tracking role은 상태 history와 classic fanout publish를 맡고, CustomerGateway role은 customer actor를
  소유하는 MeshNode와 bound session push를 맡는다.
- CustomerGateway는 public `session_actor_manager_t`로 customer actor ref를 현재 session에 바인드하고,
framework가 stream 연결과 disconnect 정리를 관리한다. status fanout handler는 bound session으로
`DeliveryStatusNotify`를 push한다.
- CourierSession도 public `session_actor_manager_t`로 actor ref를 현재 session에 바인드하고,
  CourierActorNode Entry Spot handler는 actor context의 bound session으로 `OfferDeliveryNotify`를 push한다.
