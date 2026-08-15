<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [이전: Runtime Architecture](../../common/spec/server/README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:end -->

[C++ 묶음](../README.ko.md) | [공개 인터페이스](../../common/spec/server/languages/cpp/interfaces/README.ko.md)

# ZLink Framework C++ Backend Dependency Policy

## 1. 목적

framework public API가 zlink binding의 socket 객체와 native 구현 세부에 결합되지
않도록 backend 경계를 정의한다.

## 2. 원칙

- framework는 binding의 public API만 호출한다.
- backend context, socket, SpotNode, Spot과 monitor handle은 private runtime에 둔다.
- public header에는 Boost.Asio, Boost.Beast, OpenSSL과 binding concrete socket 타입을
  노출하지 않는다.
- `RoutingId`, message와 명시적으로 승인된 전송 값 타입만 공개 계약에 남긴다.
- serializer, location codec과 wire framing 결정은 각각 한 runtime subsystem이
  소유한다.

## 3. adapter 책임

backend adapter는 context 생성, socket bind/connect, frame submit/receive, Spot과
stream operation, monitoring event 변환을 담당한다. registration, handler dispatch,
timeout과 application lifecycle은 framework runtime이 담당한다.

sample이나 application이 backend 부족 기능을 raw frame, private header 또는 test
adapter로 우회하지 않는다. 필요한 binding public API가 없으면 먼저 binding 계약으로
설계하고 구현한다.

## 4. C++ 책임 그래프

C++ 구현은 [공통 layering 원칙](../../common/spec/server/40-internal-layering.ko.md)의 책임 그래프를
따른다. public header와 domain runtime은 binding type을 노출하지 않는다. binding과
framework의 의미가 같은 지점에서는 semantic runtime이 binding public API를 직접 호출한다.
소유권, lifecycle, readiness, error 또는 동시성 규칙을 바꾸는 지점에서는 아래의
semantic adapter가 그 차이를 흡수한다.

| 경로 | Binding operation | 의미 차이와 소유권 | 결정 |
|------|-------------------|--------------------|------|
| `raw_route_port_t`, `raw_dealer_port_t` | `socket_t::recv`, `send`, `request`, `reply`, `poller_t::wait` | binding의 `received_t`와 message를 framework의 routing id, request sequence, raw message 결과로 변환한다. socket과 poller 수명은 port가 관리한다. | semantic adapter 유지 |
| `raw_mesh_node_owner_t`, `raw_client_server_owner_t` | context, routed socket, monitor와 poller | 여러 binding object를 하나의 mesh 또는 client/server 연결 수명으로 결합하고 monitor 결과를 framework 상태로 바꾼다. | semantic adapter 유지 |
| `raw_fanout_owner_t` | pub/sub socket, `topic_message_t`, poller | reserved beacon과 application topic을 구분하고 reconnect, readiness와 close 순서를 관리한다. | semantic adapter 유지 |
| `channel_host_service_t`, `stream_host_service_t` | binding socket과 stream public operation | binding 호출의 결과를 channel dispatch 또는 stream session 의미로 변환한다. 같은 socket 동작을 다시 전달하는 별도 wrapper는 두지 않는다. | semantic runtime에서 binding public API 직접 호출 |
| store·relocation adapter | provider/store operation | 저장소의 record와 framework의 location, ownership, lifecycle 결과가 다르므로 오류와 원자적 상태 변경을 변환한다. | semantic adapter 유지 |
| factory와 transport 선택 | context 또는 transport 생성 | 여러 구성 요소를 조합하고 선택할 뿐, binding method 이름만 바꾸는 facade가 아니다. | composition 경계 유지 |

실제 호출자가 없고 하나의 구현만 감싼 backend나 contract는 이 그래프에 포함하지 않는다.
이런 파일을 CMake에만 남겨 두면 지원되는 경계처럼 보이고, framework가 사용하는 책임을
숨기지 못하므로 제거 대상이다.

## 5. Message 수신 경로와 비용

binding의 `socket_t::receive`는 호출자가 넘긴 `received_t`에 수신 결과를 채운다. binding
runtime은 socket마다 native receive envelope 하나를 재사용하고, 결과를 `received_t`에
반영할 때 기존 message vector의 capacity를 유지한다. 따라서 반복 수신에서 envelope와
결과 vector를 message마다 새로 만들지 않는다.

framework의 `raw_route_port_t`, `raw_dealer_port_t`, channel 수신 loop와 fanout subscriber
loop도 각 실행 owner가 `received_t` 또는 `topic_message_t`를 보관해 다음 수신에 재사용한다.
수신 결과를 비동기 handler로 넘겨야 할 때는 handler 수명에 필요한 framework message만
한 번 복사한 뒤 binding 수신 객체를 즉시 닫는다. 이 복사는 binding object가 loop를 넘어
사용되지 않도록 하는 ownership 경계이며, 호출부에 codec이나 raw-frame 처리를 추가하는
우회가 아니다.

poller는 호출자가 제공한 `poll_event_t` 저장소를 채우고, framework의 poll loop는 해당
저장소를 stack에서 재사용한다. poller event를 wrapper collection으로 다시 만들지 않는다.
수신 경로의 socket mutex는 binding socket의 실행 owner를 보호하기 위한 기존 경계이며,
동일한 operation을 감싸는 일반 목적의 추가 lock을 만들지 않는다.

## 6. 대안 검토 기준

새 binding operation을 연결할 때는 다음 두 대안을 먼저 비교한다.

1. binding public API를 semantic runtime에서 직접 호출한다. 소유권, 완료 시점, 오류와
   동시성 의미가 framework 계약과 같을 때 선택한다.
2. semantic adapter 또는 port를 둔다. 여러 binding object를 하나의 동작으로 결합하거나,
   binding의 raw 결과를 framework domain 결과로 변환하거나, lifecycle·readiness·error의
   책임을 framework 규칙으로 바꿀 때만 선택한다.

두 경우 모두 binding private member, reflection, visibility hack과 raw-frame 우회를
사용하지 않는다. testability만을 이유로 하나의 구현을 감싼 `IBackend`를 추가하지 않는다.

## 7. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `test_cpp_framework_layout_contract` | public header에 private runtime과 backend concrete dependency가 노출되지 않는다. |
| `test_cpp_framework_contract_headers` | public header가 private include path 없이 compile된다. |
| `test_cpp_framework_raw_route_port_contract` | raw route adapter가 routing id, request sequence, 수신 결과와 실패 결과를 framework 의미로 변환한다. |
| `test_cpp_framework_client_server_runtime`, `test_cpp_framework_messaging` | client/server와 channel message 경로가 binding public operation만 사용하고 lifecycle 결과를 유지한다. |
| `test_cpp_framework_stream_framework` | stream adapter가 연결, 수신, close 순서를 framework session 의미로 변환한다. |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../README.ko.md) | [이전: Runtime Architecture](../../common/spec/server/README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:bottom:end -->
