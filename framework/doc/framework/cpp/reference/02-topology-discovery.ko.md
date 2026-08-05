# 02. Topology discovery

[레퍼런스 목차](README.ko.md)

이 category는 `zlink_framework_options_t`가 제공하는 topology 등록 진입점과, RouteMesh·ClientServer·Fanout
운영 상태를 조회하는 진입점을 다룬다. 정확한 signature는
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)와
[Configuration과 host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)가
소유한다. 등록 진입점은 모두 host 구성 시점의 호출이다.

---

## `add_route_mesh` (구성 시점)

물리 MeshNode 하나를 등록한다. RouteMesh 기반 topology의 시작점이다.

```cpp
auto play = options.add_route_mesh("play")
  .listen(5501)
  .set_automatic_routing_id_prefix("play")
  .set_placement_weight(100);
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.listen(endpoint)` / `.listen(port)` | 없으면 bind하지 않음 | 이 MeshNode의 수신 endpoint |
| `.set_bind_host(string)` / `.set_advertise_host(string)` | `configure_network()`의 root 기본값을 따름 | 이 MeshNode에만 적용하는 bind·advertise host |
| `.set_routing_id(routing_id)` / `.set_automatic_routing_id_prefix(prefix)` | Framework가 발급 | 고정 RID 또는 발급 RID의 prefix(`[A-Za-z0-9._-]` 1..64자) |
| `.set_object_role(object_role_t)` | `none` | `client`·`server`·`none` 중 하나. `server`는 `client` 기능을 포함하며 `client`·`server`는 Location Store가 필수다 |
| `.set_placement_weight(int)` | 100(범위 `0..10000`) | 새 Actor·Spot을 이 node에 배치할 상대 가중치 |
| `.set_actor_limit(int32_t)` / `.set_spot_limit(int32_t)` | `0`(무제한) | 이 node가 수용하는 Actor·Spot 상한 |
| `.set_activation_concurrency(int32_t)` | 128(양수) | activation admission 동시 실행 상한 |
| `.set_default_request_timeout(milliseconds)` | 이 MeshNode의 request 기본 timeout | `request_to_node`/`request_to_channel`(messaging-execution category)이 `.timeout(...)`을 생략했을 때 쓰는 값 |
| `.set_instance_spot_idle_timeout(milliseconds)` | `0`(정리하지 않음) | Instance Spot idle 회수 시간 |
| `.configure_router_socket()` | `mesh_node_socket_config_t` 기본값 | 이 MeshNode ROUTER 소켓의 HWM·buffer·timeout(`max_message_size`, `send_high_water_mark` 등) |
| `.channel(channel_name)` | — | 이 MeshNode의 RouteMesh Channel role 등록으로 진입. RouteMesh Channel 등록 항목을 참고 |
| `.peer_connections()` | — | Manual peer 연결 항목을 참고 |
| `.add_route_send_handler<THandler, TMessage>(packet_name = {})` | packet name은 메시지 타입에서 결정 | Node direct one-way handler 등록. `send_to_node`(messaging-execution category)가 호출하는 대상 |
| `.add_route_request_handler<THandler, TRequest, TReply>(packet_name = {})` | packet name은 메시지 타입에서 결정 | Node direct request handler 등록. `request_to_node`가 호출하는 대상 |

**완료 결과.** 반환값 없이 동기로 등록된다. 잘못된 조합(중복 MeshName, listener 설정 누락 등)은
`app.run(...)`이 socket bind 전 검증에서 configuration error로 드러낸다.

**선택 기준.** RouteMesh를 쓰는 모든 host가 최소 하나의 MeshNode를 등록할 때 쓴다. Manual peer만
쓰고 분산 discovery가 필요 없는 node는 Location Store 없이 시작할 수 있다.

---

## Object role 등록 (구성 시점)

MeshNode가 Actor·Spot을 어떻게 다루는지(Client만 하는지, Server로 호스팅하는지) 등록한다.

```cpp
play.add_entry_spot<game_entry_spot_t>();

play.add_spot_factory<room_spot_t>(
  "room",
  [](zlink::framework::spot_context_t context) {
      return std::make_shared<room_spot_t>(std::move(context));
  },
  [](auto &factory) {
      factory.set_execution_mode(
        zlink::framework::user_spot_execution_mode_t::spot_wide);
      factory.template preserve_state_with<room_relocation_adapter_t>();
  });

play.add_actor_factory<player_actor_t, player_actor_factory_t>(
  "player",
  std::make_shared<player_actor_factory_t>(),
  [](auto &factory) {
      factory.template preserve_state_with<player_relocation_adapter_t>();
  });
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.add_entry_spot<TEntrySpot>()` / `.add_entry_spot<TEntrySpot>(factory)` | 없음 | 외부 진입 전용 Entry Spot 타입 등록. Factory를 생략하면 `TEntrySpot(entry_spot_context_t)` 생성자를 사용한다 |
| `.add_spot_factory<TSpot>(stable_type, factory, configure)` | 없음 | stable User Spot 타입 등록. `configure`는 `set_stable_type_limit`·`set_execution_mode`·`set_relocation_readiness`에 더해 `preserve_state_with<TAdapter>`/`recreate_on_relocation`/`disable_relocation` 중 정확히 하나를 받는다 |
| `.add_instance_spot_factory<TSpot>(stable_type, factory, configure)` | 없음 | cold-activation Instance Spot 타입 등록. `configure`는 `set_stable_type_limit`에 더해 relocation policy 중 정확히 하나를 받는다 |
| `.add_actor_factory<TActor, TFactory>(stable_type, factory, configure)` | 없음 | stable Actor 타입 등록. `configure`는 relocation policy 중 정확히 하나를 받는다(Actor factory에는 `stable_type_limit`이 없다) |

**완료 결과.** 반환값 없이 동기로 등록된다. Relocation을 쓰려는 stable type의 adapter·factory
불일치, type 중복은 `app.run(...)`의 startup 검증에서 configuration error로 드러난다.

**선택 기준.** 이 node가 Actor·Spot을 실제로 호스팅(`server`)하거나, 다른 node가 호스팅하는
Actor·Spot을 메시징 대상으로만 참조(`client`)할 때 각각의 role을 등록한다. Relocation 정책 선택
기준은 actor-relocation category를 참고한다.

---

## RouteMesh Channel 등록 (구성 시점)

같은 MeshNode 안에서 논리 ChannelName membership을 등록한다.

```cpp
play.channel("play.api").server()
  .set_weight(100)
  .add_handler_group("api");

play.channel("play.events").client();
```

**옵션.** `channel(channel_name)` 뒤에는 `.client()` 또는 `.server()`를 정확히 한 번 호출한다.
`.client()`는 송신 경로만 만들고 modifier가 없다. `.server()`에 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.set_weight(int)` | 100(범위 `0..10000`) | 이 Server가 request/send 대상으로 선택될 상대 가중치. `0`이면 선택 대상에서 제외 |
| `.add_handler_group(group_name)` | 없음 | `options.handlers().group(group_name)`에 등록한 handler group을 연결 |
| `.add_send_handler<THandler, TMessage>(packet_name = {})` | packet name은 메시지 타입에서 결정 | one-way handler를 이 channel에 직접 등록 |
| `.add_request_handler<THandler, TRequest, TReply>(packet_name = {})` | packet name은 메시지 타입에서 결정 | request/reply handler를 이 channel에 직접 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. 같은 channel에 같은 packet 이름의 handler가 두 번
노출되면 `app.run(...)`이 message를 받기 전 startup 설정 오류로 실패한다.

**선택 기준.** `send_to_channel`/`request_to_channel`(messaging-execution category)로 받을
handler를 등록할 때 `.server()`를 쓴다. 이 MeshNode가 다른 node의 Server만 호출하고 자신은
handler를 두지 않으면 `.client()`만 등록한다. 서로 다른 프로세스 사이 통신이 필요하면
`add_client_server_channel`을 대신 쓴다.

---

## `add_client_server_channel` (구성 시점)

RouteMesh와 무관하게 독립된 ClientServer Channel을 등록한다.

```cpp
options.add_client_server_channel("payments.api").server()
  .listen(6001)
  .set_weight(100)
  .add_handler_group("payments");

options.add_client_server_channel("payments.api").client()
  .connect("payments-1:6001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.server().listen(port = 0)` | `0`(automatic bind) | 이 Server의 수신 port |
| `.server().set_bind_host(string)` / `.set_advertise_host(string)` | root `configure_network()` 기본값 | 이 Server에만 적용하는 bind·advertise host |
| `.server().set_weight(int)` / `.add_send_handler`/`.add_request_handler` | RouteMesh Channel Server와 동일 | 가중치와 handler 등록 |
| `.client().connect(endpoint)` | manual | 특정 Server에 수동 연결. 생략하면 automatic discovery로 target을 찾는다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic discovery를 쓰는 Client·Server는 Location
Store 등록이 없으면 startup 검증에서 configuration error로 드러난다.

**선택 기준.** RouteMesh 멤버가 아닌 독립 서비스 사이의 request/reply나 one-way 메시징에 쓴다.
같은 RouteMesh 안 node끼리는 RouteMesh Channel 등록을 대신 쓴다.

---

## `add_fanout_channel` (구성 시점)

Classic fanout 전용 채널을 등록한다. `publisher_t::publish`(messaging-execution category)로
발행할 대상이다.

```cpp
options.add_fanout_channel("lobby.events")
  .enable_publisher(7001)
  .add_handler_group("events");

// automatic subscriber — 같은 ChannelName의 publisher를 location store에서 자동으로 발견한다.
options.add_fanout_channel("lobby.events")
  .enable_subscriber();

// manual subscriber — 명시한 endpoint만 쓴다. enable_subscriber()와 함께 쓰면 startup이 실패한다.
options.add_fanout_channel("lobby.events")
  .connect("lobby-1:7001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.enable_publisher(endpoint)` / `.enable_publisher(port = 0)` | 없음 | 이 채널의 발행자 역할과 수신 endpoint 등록 |
| `.set_bind_host(string)` / `.set_advertise_host(string)` / `.set_routing_id(routing_id)` / `.set_automatic_routing_id_prefix(prefix)` | root 기본값 또는 Framework 발급 | publisher에만 적용하는 bind·advertise host와 RID |
| `.enable_subscriber()` | — | automatic subscriber. Location Store에서 같은 ChannelName의 유효한 publisher를 전부 찾는다 |
| `.connect(endpoint)` | — | manual subscriber. 명시한 endpoint만 사용 |
| `.subscriber_connections()` | — | manual subscriber endpoint 집합의 runtime handle(`connect`/`disconnect`/`list_connections`) 반환 |
| `.add_handler_group(group_name)` | 없음 | typed event handler group 연결 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic subscriber와 manual subscriber를 같은
fanout channel에 함께 설정하면 startup 실패로 드러난다.

**선택 기준.** 발행자가 구독자를 알 필요가 없는 관찰·통지 채널을 새로 만들 때 쓴다. Reply가
필요한 메시징에는 RouteMesh Channel이나 ClientServer Channel 등록을 대신 쓴다.

---

## `add_stream_node` (구성 시점)

외부 STREAM 연결을 받는 listener를 등록한다.

```cpp
options.add_stream_node("public-gateway")
  .bind(9001)
  .enable_actor_dispatch()
  .register_session<game_session_t>();
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.bind(endpoint)` / `.bind(port = 0)` | `0`(automatic bind) | 이 STREAM listener의 수신 port |
| `.set_bind_host(string)` / `.set_advertise_host(string)` | root `configure_network()` 기본값 | 이 listener에만 적용하는 bind·advertise host |
| `.set_tls_server(cert_path, key_path, require_client_certificate = false)` | TLS 없음 | TLS 서버 인증서·키, 상호 인증 여부 |
| `.enable_actor_dispatch()` | 비활성 | 수신 message를 global ActorId lookup으로 bound Actor에 dispatch. 같은 builder에서 두 번 호출하면 startup이 실패한다 |
| `.register_session<TSession>()` | 없음 | `packet_stream_session_t`를 상속한 Session 타입 등록. 하나의 stream node에는 packet session을 하나만 선언한다 |
| `.register_session(name)` | — | Session 이름을 직접 지정해야 하는 low-level 구성 전용 overload |

**완료 결과.** 반환값 없이 동기로 등록된다. TLS 설정 오류, `register_session` 중복 호출은
startup 검증에서 configuration error로 드러난다.

**선택 기준.** 외부 client가 STREAM 프로토콜로 직접 연결하는 gateway를 열 때 쓴다. 정확한
Session·Actor 연결 규칙은 stream-session category를 참고한다.

---

## Manual peer 연결 (구성 시점·런타임)

Automatic discovery 없이 특정 endpoint에 수동으로 연결한다. `mesh_node_builder_t::peer_connections()`로
호출한다.

```cpp
play.peer_connections().connect("play-node-2:5501");
std::vector<zlink::framework::mesh_peer_connection_t> connections =
  play.peer_connections().list_connections();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.connect(endpoint)` | expected RID 없음 | Admission handshake가 remote identity를 결정 |
| `.connect(expected_routing_id, endpoint)` | — | Handshake identity가 다르면 admission하지 않음 |
| `.disconnect(endpoint)` | — | 등록한 연결을 해제 |
| `.list_connections()` | — | 현재 등록된 연결 목록 조회 |

**완료 결과.** 반환값 없이 동기로 등록·해제된다. 양쪽 MeshNode가 Object Client이고 둘 다
RouteMesh Channel Server membership이 없으면 이 연결 intent는 목록에 남아도 ready peer가 되지
않는다. 어느 한쪽에라도 weight `0`을 포함한 Channel Server membership이 있으면 일반 peer
admission·liveness 규칙을 적용한다.

**선택 기준.** Automatic discovery(Location Store)를 쓰지 않고 고정된 peer 목록으로 RouteMesh를
구성할 때 쓴다.

---

## `use_filter<TFilter>` (구성 시점)

모든 handler dispatch 앞에 공통 로직(인증, 로깅 등)을 끼워 넣는다.

```cpp
options.use_filter<authentication_filter_t>();

class authentication_filter_t {
public:
    zlink::framework::task_t<void> invoke(
      const zlink::framework::handler_filter_context_t &context,
      zlink::framework::handler_next_t next) {
        if (!is_authenticated(context)) {
            co_return; // next()를 호출하지 않으면 request는 rejected로 끝난다
        }
        co_await next();
    }
};
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.use_filter<TFilter>()` | 없음(등록한 순서대로 실행) | filter 타입을 dispatch 체인에 추가 |

**완료 결과.** 반환값 없이 동기로 등록된다. `next()`를 호출하면 남은 filter와 handler가
실행된다. Request에서 `next()`를 호출하지 않으면 `rejected`로 끝나고, `next()`를 두 번 호출하면
`invalid_operation`으로 실패한다. `context.dispatch_kind`로
`node_direct_send`/`node_direct_request`/`channel_send`/`channel_request`/`classic_fanout`을
구분한다 — `channel_send`/`channel_request`는 RouteMesh와 ClientServer를 모두 포함한다.

**선택 기준.** 개별 handler마다 반복할 공통 전처리·검증이 필요할 때 쓴다. Filter는 업무 reply를
직접 만들지 않는다 — 거부만 표현하고 나머지는 handler가 처리한다. Spot·Actor handler와 STREAM
session에는 적용하지 않는다.

---

## 기타 host-wide 옵션 (구성 시점)

`zlink_framework_options_t`가 제공하는 단순 값 하나로 끝나는 구성이다.

```cpp
options.services().add_singleton<order_repository_t>();
options.configure_network().set_bind_host("0.0.0.0");
options.configure_inbound_dispatch()
  .set_application_hwm_profile(
    zlink::framework::application_hwm_profile_t::low_latency);
options.metadata()
  .allow_session_to_actor("trace-id")
  .allow_actor_to_session("server-region");
options.configure_stream_compression().use_lz4();
options.set_application_version(2);
options.handler_coroutine_workers(8);
```

**옵션.** 자주 쓰는 항목은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.services()` | — | `service_collection_t`. Handler와 hosted component의 service lifetime(`add_singleton`/`add_scoped`/`add_transient`/`add_factory`) 등록 |
| `.metadata().allow_session_to_actor(key)` / `.allow_actor_to_session(key)` | 지정하지 않은 key는 forward 안 함 | STREAM session↔Actor relay로 넘길 metadata key를 방향별 allowlist에 추가 |
| `.configure_network()` | `bind_host()`는 `127.0.0.1` | 개별 listen 호출이 override하지 않는 한 쓰는 기본 bind·advertise host |
| `.worker()` | `worker_options_t` 기본값 | bounded worker pool의 최소·최대 thread 수, idle timeout, queue 상한(`RunCpuWorker`/`RunIoWorker`가 쓰는 pool) |
| `.configure_inbound_dispatch()` | `application_hwm_profile_t::balanced` | Inbound application HWM 크기·profile, process 메모리 상한 |
| `.configure_dispatch()` | Framework 기본 정책 | Dispatch·diagnostics 옵션. observability-diagnostics category 참고 |
| `.configure_stream_compression()` | 압축 없음 | STREAM 기본 압축 codec(`use_default()`/`use_lz4()`/`use(codec)`/`disable()`) |
| `.set_max_pending(count)` | Framework 기본값 | runtime 전체 pending queue 상한 |
| `.set_application_version(version)` / `.set_maintenance_wave(wave)` | `0` / 없음(exclusion 없음) | 모든 local MeshNode가 게시하는 배포 버전과 maintenance wave |
| `.set_default_request_timeout(timeout)` | Framework 기본값 | host 전체 request 기본 timeout |
| `.handler_coroutine_workers(count)` | Framework 기본값 | handler coroutine 실행 worker 수 |
| `.codecs()` | JSON만 등록 | `options.codecs().use(extension)`. messaging-execution category의 Codec 등록 항목을 참고 |

**완료 결과.** 대부분 반환값 없이 동기로 실행되며, `.services()`/`.configure_network()`/
`.worker()`/`.configure_inbound_dispatch()`/`.configure_dispatch()`는 해당 builder나 options
객체를 반환해 그 위에서 추가 설정을 이어간다. 값 범위를 벗어나면 startup 검증에서
configuration error로 드러난다.

**선택 기준.** 위 전용 항목(host lifecycle·topology 등록·diagnostics)에 속하지 않는, 단순 값
하나로 끝나는 host-wide 설정을 조정할 때 쓴다.

---

## 런타임 weight 조회·변경

배포를 다시 하지 않고 placement weight나 channel weight를 바꾼다.

```cpp
zlink::framework::route_mesh_runtime_options_t &placement =
  route_mesh_runtime_options; // DI에서 주입받은 인스턴스
placement.placement_weight(50); // 이 node로 가는 새 Actor·Spot 배치 비중을 낮춘다
placement.channel("play.api").weight(0); // 이 Channel Server를 선택 대상에서 제외한다
```

**옵션.** 이 진입점에는 두 개의 독립된 property가 있다.

| Property | 기본값 | 의미 |
| --- | --- | --- |
| `route_mesh_runtime_options_t::placement_weight()`/`(value)` | 등록 시점 값 | node 단위 Actor·Spot 배치 가중치 |
| `route_mesh_runtime_options_t::channel(name).weight()`/`(value)` | 등록 시점 값 | ChannelName 단위 Server 선택 가중치 |

**완료 결과.** 동기 get/set이다. 즉시 적용되며 별도 완료 신호가 없다. 등록되지 않은 ChannelName을
조회하면 configuration error다.

**선택 기준.** 운영 중 배치나 트래픽 비중을 조정할 때 쓴다. `max_message_size`를 포함한 transport
option은 이 경로로 바꿀 수 없다 — startup 전에만 설정한다.

---

## Topology 상태 조회·관찰

RouteMesh·ClientServer·Fanout 각각의 운영 상태를 확인한다. 세 runtime이 같은 모양(`snapshot`
한 번 조회, `observe`로 스트리밍 관찰)을 제공한다.

```cpp
zlink::framework::mesh_node_snapshot_t status = route_mesh_runtime.snapshot("play");
bool can_place_new_objects = status.is_ready && status.placement.is_available;

auto observation = route_mesh_runtime.observe(
  "play",
  /*capacity=*/64,
  [](const auto &observed) {
      // observed.status.channels, observed.status.peers를 확인한다
  });
```

**옵션.** 세 runtime의 대응 관계는 다음과 같다.

| Runtime | 대상 | 반환 snapshot |
| --- | --- | --- |
| `route_mesh_runtime_t` | MeshName | `mesh_node_snapshot_t`(channels, peers, placement 포함) |
| `client_server_runtime_t` | ChannelName | `client_server_channel_snapshot_t`(servers 포함) |
| `fanout_runtime_t` | ChannelName | `fanout_channel_snapshot_t`(publishers 포함) |

**완료 결과.** `snapshot(...)`은 즉시 값을 반환하는 동기 호출이다. `observe(...)`는
`observed_status_t<TStatus>`를 콜백으로 전달하며, `loss` field(`coalesced_count`/
`discarded_terminal_count`)로 관찰 유실 여부를 판단한다. 반환한 observation handle의 `close()`
(또는 `unique_ptr` 파괴)로 관찰을 끝낸다. Manual ChannelName만 등록한 fanout을
`fanout_runtime_t`로 조회하면 configuration error다.

**선택 기준.** 특정 MeshName·ChannelName의 가용성을 판단하거나 장애 범위를 좁힐 때 쓴다. Host
전체 상태가 필요하면 host-lifecycle category의 `is_ready`/observability-diagnostics category의
`status()`를 쓴다.

---

전체 근거는
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)와
[Configuration과 host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)를
참고한다.
