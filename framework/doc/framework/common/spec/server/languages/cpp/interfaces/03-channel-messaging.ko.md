# C++ channel messaging exact interface

[C++ exact interface 목차](README.ko.md) · [MeshNode](../../../../13-mesh-node.ko.md) ·
[Framework API](../../../../06-framework-api.ko.md)

## 1. RouteMesh 등록

RouteMesh builder는 물리 mesh 하나와 그 MeshNode를 등록한다. 논리 channel은 같은
builder에 membership으로 추가하며 별도 socket을 만들지 않는다.
Application이 조회하는 RouteMesh 상태 interface는
[C++ monitoring exact interface](08-monitoring.ko.md)가 정의한다.

```cpp
namespace zlink::framework {

struct mesh_peer_connection_t {
    std::uint64_t intent_id = 0;
    std::optional<zlink::routing_id_t> expected_routing_id;
    std::string endpoint;
};

class mesh_peer_connections_t {
public:
    void connect(std::string endpoint);
    void connect(zlink::routing_id_t expected_routing_id, std::string endpoint);
    void disconnect(std::string endpoint);
    std::vector<mesh_peer_connection_t> list_connections() const;
};

class mesh_channel_server_builder_t;
class mesh_channel_client_builder_t {};

class mesh_channel_builder_t {
public:
    mesh_channel_client_builder_t client();
    mesh_channel_server_builder_t server();
};

class mesh_channel_server_builder_t {
public:
    mesh_channel_server_builder_t &set_weight(int weight);
    mesh_channel_server_builder_t &add_handler_group(std::string group_name);

    template <typename THandler, typename TMessage>
    mesh_channel_server_builder_t &add_send_handler(std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    mesh_channel_server_builder_t &add_request_handler(std::string packet_name = {});
};

class network_options_t {
public:
    std::string bind_host() const;
    network_options_t &set_bind_host(std::string host);
    std::optional<std::string> advertise_host() const;
    network_options_t &set_advertise_host(std::optional<std::string> host);
};

class client_server_channel_client_builder_t {
public:
    client_server_channel_client_builder_t &connect(std::string endpoint);
};

class client_server_channel_server_builder_t {
public:
    client_server_channel_server_builder_t &listen(std::uint16_t port = 0);
    client_server_channel_server_builder_t &set_bind_host(std::string host);
    client_server_channel_server_builder_t &set_advertise_host(std::string host);
    client_server_channel_server_builder_t &set_weight(int weight);
    client_server_channel_server_builder_t &add_handler_group(std::string group_name);

    template <typename THandler, typename TMessage>
    client_server_channel_server_builder_t &add_send_handler(
      std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    client_server_channel_server_builder_t &add_request_handler(
      std::string packet_name = {});
};

struct mesh_node_socket_config_t {
    std::int64_t max_message_size = 16777216;
    zlink::byte_count_t send_high_water_mark =
        zlink::byte_count_t::bytes(4'096'000);
    zlink::byte_count_t receive_high_water_mark =
        zlink::byte_count_t::bytes(4'096'000);
    std::uint64_t mailbox_message_budget = 1024;
    std::uint64_t mailbox_byte_budget = 64 * 1024 * 1024;
    std::optional<std::chrono::milliseconds> receive_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
};

enum class object_role_t : std::uint8_t {
    none = 0,
    client = 1,
    server = 2
};

enum class user_spot_execution_mode_t {
    spot_wide = 0,
    per_actor = 1
};

enum class spot_relocation_readiness_mode_t {
    any_turn_boundary = 0,
    application_signaled = 1
};

class mesh_node_builder_t {
public:
    mesh_channel_builder_t channel(std::string channel_name);
    mesh_node_builder_t &listen(std::string endpoint);
    mesh_node_builder_t &listen(std::uint16_t port = 0);
    mesh_node_builder_t &set_bind_host(std::string host);
    mesh_node_builder_t &set_advertise_host(std::string host);
    mesh_node_builder_t &set_routing_id(zlink::routing_id_t routing_id);
    mesh_node_builder_t &set_automatic_routing_id_prefix(std::string prefix);
    mesh_node_builder_t &set_object_role(object_role_t role);
    mesh_node_builder_t &set_placement_weight(int weight);
    mesh_node_builder_t &set_actor_limit(std::int32_t limit);
    mesh_node_builder_t &set_spot_limit(std::int32_t limit);
    mesh_node_builder_t &set_activation_concurrency(std::int32_t limit);
    mesh_node_builder_t &set_instance_spot_idle_timeout(
      std::chrono::milliseconds timeout);
    mesh_node_socket_config_t &configure_router_socket();
    mesh_peer_connections_t &peer_connections();
    mesh_node_builder_t &set_default_request_timeout(std::chrono::milliseconds timeout);

    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_route_send_handler(std::string packet_name = {});

    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_route_request_handler(std::string packet_name = {});

    template <typename TEntrySpot>
      requires std::derived_from<
        TEntrySpot, entry_spot_t<typename TEntrySpot::actor_type>>
    mesh_node_builder_t &add_entry_spot();

    template <typename TEntrySpot>
      requires std::derived_from<
        TEntrySpot, entry_spot_t<typename TEntrySpot::actor_type>>
    mesh_node_builder_t &add_entry_spot(
      std::function<std::shared_ptr<TEntrySpot>(
        class entry_spot_context_t)> factory);

    template <typename TSpot>
      requires std::derived_from<
        TSpot, spot_t<typename TSpot::actor_type>>
    mesh_node_builder_t &add_spot_factory(
      std::string stable_type,
      std::function<std::shared_ptr<TSpot>(
        class spot_context_t)> factory,
      std::function<void(user_spot_factory_builder_t<TSpot> &)> configure);

    template <typename TSpot>
      requires std::derived_from<TSpot, instance_spot_t>
    mesh_node_builder_t &add_instance_spot_factory(
      std::string stable_type,
      std::function<std::shared_ptr<TSpot>(
        class instance_spot_context_t)> factory,
      std::function<void(instance_spot_factory_builder_t<TSpot> &)> configure);

    template <typename TActor, typename TActorFactory>
      requires std::derived_from<TActor, actor_t> &&
        std::derived_from<TActorFactory, actor_factory_t<TActor>>
    mesh_node_builder_t &add_actor_factory(
      std::string stable_type,
      std::shared_ptr<TActorFactory> factory,
      std::function<void(actor_factory_builder_t<TActor> &)> configure);

};

enum class client_server_role_t { client, server, client_and_server };

enum class client_server_server_state_t {
    configured,
    connecting,
    ready,
    draining,
    disconnected,
    rejected
};

struct client_server_server_snapshot_t {
    zlink::routing_id_t server_rid;
    std::uint64_t lifecycle_generation;
    int weight;
    bool ready;
    client_server_server_state_t state;
    std::string descriptor_source;
    std::optional<std::string> last_failure;
};

struct client_server_channel_snapshot_t {
    std::string channel_name;
    client_server_role_t local_role;
    bool selectable;
    int ready_server_count;
    int connection_intent_count;
    int pending_request_count;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
    std::vector<client_server_server_snapshot_t> servers;
    location_runtime_snapshot_t location;
};

struct client_server_runtime_event_t {
    std::string identifier;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    std::optional<zlink::routing_id_t> server_rid;
    std::optional<std::uint64_t> lifecycle_generation;
    std::optional<int> weight;
    std::optional<bool> ready;
    std::optional<client_server_server_state_t> state;
    std::optional<std::string> reason;
};

class client_server_runtime_t {
public:
    virtual client_server_channel_snapshot_t snapshot(
      std::string channel_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t> observe(
      std::string channel_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<client_server_runtime_event_t> &)> observer) = 0;
    virtual bool is_ready(std::string channel_name) const = 0;
};

enum class fanout_publisher_connection_state_t {
    connecting,
    ready,
    disconnected,
    reconnecting,
    excluded_draining,
    excluded_stale
};

struct fanout_publisher_connection_snapshot_t {
    zlink::routing_id_t publisher_rid;
    std::uint64_t lifecycle_generation;
    bool connection_intent;
    bool ready;
    fanout_publisher_connection_state_t state;
    std::optional<std::string> last_failure;
};

struct fanout_channel_snapshot_t {
    std::string channel_name;
    std::size_t connection_intent_count;
    std::size_t ready_connection_count;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
    std::vector<fanout_publisher_connection_snapshot_t> publishers;
    location_runtime_snapshot_t location;
};

struct fanout_publisher_changed_event_t {
    static constexpr std::string_view event_identifier =
      "zlink.runtime.fanout.publisher_changed";
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    fanout_publisher_connection_snapshot_t entry;

    constexpr std::string_view identifier() const noexcept {
        return event_identifier;
    }
};

struct fanout_location_changed_event_t {
    static constexpr std::string_view event_identifier =
      "zlink.runtime.location.store_changed";
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::string channel_name;
    location_runtime_snapshot_t location;

    constexpr std::string_view identifier() const noexcept {
        return event_identifier;
    }
};

using fanout_runtime_event_t = std::variant<
  fanout_publisher_changed_event_t,
  fanout_location_changed_event_t>;

class fanout_runtime_observation_t {
public:
    virtual ~fanout_runtime_observation_t() = default;
    virtual void close() = 0;
};

class fanout_runtime_t {
public:
    virtual fanout_channel_snapshot_t snapshot(std::string channel_name) const = 0;
    virtual std::unique_ptr<fanout_runtime_observation_t> observe(
      std::string channel_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<fanout_runtime_event_t> &)> observer) = 0;
};

class mesh_channel_runtime_options_t {
public:
    virtual ~mesh_channel_runtime_options_t() = default;
    virtual int weight() const = 0;
    virtual void weight(int value) = 0;
};

class route_mesh_runtime_options_t {
public:
    virtual ~route_mesh_runtime_options_t() = default;
    virtual int placement_weight() const = 0;
    virtual void placement_weight(int value) = 0;
    virtual mesh_channel_runtime_options_t &channel(std::string channel_name) = 0;
};

} // namespace zlink::framework
```

`mailbox_message_budget`와 `mailbox_byte_budget`은 owner별 application mailbox가 보관할 수 있는 메시지 수와
byte 합계를 제한한다. Byte 회계는 payload 크기만 세지 않는다 —
`payload 크기 + metadata 크기 + 작업당 고정 비용`을 더한다. Payload가 비어 있어도 작업 하나는 `0` byte가
아니며, 큰 payload에서도 고정 비용은 그대로 더한다. 합이 `std::uint64_t` 표현 범위를 넘으면 최댓값으로
고정하고 그 제출을 거절한다. 회계 규칙은
[Framework API §8.2](../../../../06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)가 소유한다.
두 값은 startup 전에만 설정한다. `0`은 unlimited가 아니라 Framework profile이
정한 유한 기본값을 선택한다. Logical Multicast의 local target도 이 용량 제한으로 admission을 판단한다.

`channel(channel_name)` 뒤에는 `client()` 또는 `server()`를 정확히 한 번 호출한다.
`server()`가 반환한 builder만 weight와 handler를 설정한다. Server [membership](../../../../01-glossary.ko.md#membership)이 없는
[MeshNode](../../../../01-glossary.ko.md#meshnode)도 시작할 수 있다. `add_client_server_channel(channel_name)`은 단방향
request 시작 권한을 client에만 두며 server는 수신한 send/request 처리와 reply만 수행한다. ClientServer
builder에서는 `client()`와 `server()` 중 하나 또는 둘 다 호출할 수 있지만 각 역할은 최대 한 번만
등록한다. Registration key는 `(ChannelName, Role)`이고 같은 역할의 중복 등록은 startup 오류다. 서로 다른
역할은 별도 registration으로 같은 ChannelName의 topology를 공유한다. [RouteMesh](../../../../01-glossary.ko.md#routemesh)의 역할 단일 선택과
[ChannelName](../../../../01-glossary.ko.md#channelname) 충돌 규칙은 바꾸지 않는다.

두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 peer
connection이 필요하지 않다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도 weight
`0`을 포함한 Channel Server membership이 있으면 연결을 만들고 liveness를 유지한다. ClientServer와
classic fanout은 별도 물리 topology이므로 이 판정에 포함하지 않는다. Object Client에도 RouteMesh
Channel Server를 등록할 수 있지만 application Node direct handler는 등록할 수 없다. Object Client RID를
Node direct target으로 지정하면 다른 RID로 바꾸지 않고 `not_found`로 끝낸다.

RouteMesh Channel Server, ClientServer Server와 node-wide placement weight는 모두 `int`이며 범위는
`0..10000`, 기본값은 `100`이다. 범위 밖 값은 startup 설정과 runtime 변경에서 configuration error다.
Weighted selection은 후보 weight 합계를 최소 64-bit 정수로 계산한다.

Root BindHost 기본값은 `127.0.0.1`이다. AdvertiseHost를 생략하면 wildcard가 아닌 [BindHost](../../../../01-glossary.ko.md#bindhost)를
사용하고, wildcard BindHost에서는 [AdvertiseHost](../../../../01-glossary.ko.md#advertisehost)를 반드시 명시한다. Automatic discovery
listener의 port를 생략하거나 listener 호출을 생략하면 port `0`을 사용한다.
Listener별 host 설정은 root 기본값보다 우선한다.

Automatic RID prefix는 `[A-Za-z0-9._-]` 1..64자다. Runtime은
`prefix-<lowercase-canonical-uuid-v4>` 형식으로 RID를 만들고 전체 RID를 255 byte 이하로 제한한다.
UUID v4는 `8-4-4-4-12` 자리의 lowercase canonical 문자열로 표현한다. Active descriptor
[owner](../../../../01-glossary.ko.md#owner) CAS가 충돌하면 새 UUID로 다시 시도하지 않고 즉시
`routing_id_conflict`로 startup을 실패한다. Fixed RID는
Object role `none`인 explicit manual topology에서만 허용한다.

Object role `server`는 `client` 기능을 포함한다. `client`와 `server`는 Location Store가 필수이며 `none`은
manager, factory와 hidden local object runtime을 만들지 않는다. Placement [weight](../../../../01-glossary.ko.md#weight)는 `0..10000`, 기본값은 100이고
0은 새 create·relocation target에서만 제외한다. 범위 밖 값은 startup 설정과 runtime 변경에서
configuration error다. Node Actor limit과 Node Spot limit의 기본값은 `0`이며 제한 없음을 뜻한다.
`set_actor_limit(...)`은 Entry Spot과 User Spot에 존재하는 모든 Actor를 계산하고,
`set_spot_limit(...)`은 User·Instance Spot을 계산하지만 Entry Spot은 제외한다. 두 node limit은
`0..2147483647`을 허용하며 `0`은 제한 없음을 뜻한다. User·Instance Spot factory의 stable type limit을
명시하면 범위는 `1..2147483647`이다. 생략하면 해당 stable type에 별도 제한을 두지 않는다.
Actor stable type별 limit은 제공하지 않는다.

`set_activation_concurrency(...)`는 factory와 initialization의 process-local 동시 실행 gate를 설정하며
기본값은 `128`이고 양수만 허용한다. `0`이나 음수는 socket bind 전에 configuration error다. Population
capacity와 activation concurrency를 같은 counter나 option으로 합치지 않는다. 모든 값은 MeshNode lifecycle
시작 전에 고정한다.

`set_instance_spot_idle_timeout(...)`은 유휴 [Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) 정리 기준 시간이다. 기본값은
`std::chrono::milliseconds::zero()`이고 `0`은 정리하지 않음을 뜻한다. 허용 범위는 `0`과 양수이며 음수는
socket bind 전에 configuration error다. 값은 MeshNode lifecycle 시작 전에 고정하고 runtime setter를
제공하지 않는다. Worker의 `idle_timeout(...)`과는 별개의 설정이며 서로 값을 상속하지 않는다.
정리 대상은 Instance Spot뿐이고 Entry Spot과 User Spot은 이 설정의 영향을 받지 않는다. 유휴
판정 조건, `spot_close_reason_t::idle_evicted` 전달과 정리 뒤 cold activation 규칙은
[Spot 모델 §6.2](../../../../11-spot-model.ko.md#62-유휴-instance-spot-정리)가 소유한다.

Descriptor capacity는 candidate filter에만 사용한다. Framework는 선택한 node에서 Location Store의 typed
bundle reservation을 원자적으로 얻은 뒤에만 factory를 실행한다. Actor는 Actor slot 하나, Spot은 Spot 전체
slot 하나와 해당 stable type slot 하나를 예약한다. `SpotWide` User Spot과 member Actor `N`개의 aggregate relocation은
Spot total 1개, 해당 Spot stable type 1개와 Actor total `N`개를 all-or-none으로 예약한다. 모든 후보의
reservation이 capacity 때문에 실패하면 `capacity_exceeded`로 완료하고 application factory나
handler를 호출하지 않는다.
Actor·User Spot·Instance Spot [factory](../../../../01-glossary.ko.md#factory)는 relocation policy를 항상 명시하며 이를 생략하는
overload는 없다. State 보존 Actor factory에는 `actor_relocation_adapter_t<TActor>`, state 보존 User·[Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)
factory에는 `spot_relocation_adapter_t<TSpot>`가 필요하다. Factory 종류와 adapter 종류 또는 instance type이
일치하지 않으면 socket bind 전에 configuration error로 실패한다.

`user_spot_execution_mode_t::per_actor`는 recreate Spot policy만 허용한다. Disabled나
state 보존 Spot policy를 함께 등록하면 socket bind 전에 configuration error다.
PerActor Spot은 stateless execution shell이며 Actor policy와
`actor_relocation_adapter_t<TActor>`가 Actor state를 각각 처리한다.

`relocation_readiness`의 기본값은 `any_turn_boundary`다.
`application_signaled`는 `spot_wide`에서만 허용하며 `per_actor`와 함께 등록하면
socket bind 전에 configuration error다. Spot callback은 기본 no-op virtual
member이므로 application override는 필수가 아니다.

Factory와 Entry Spot member는 Object role `server`에서만 유효하다. 단일 C++ builder가 이 member를 함께
노출하더라도 `none` 또는 `client` role에 factory를 등록한 조합은 socket bind 전에 configuration error로 실패한다.

Entry Spot ID는 Framework가 startup에서 발급한다. Caller가 Spot ID를 전달하거나 Entry Spot별 option을 구성하는
public member는 제공하지 않는다. Entry Spot factory 등록과 초기화가 완료된 뒤에만 Framework가 [descriptor](../../../../01-glossary.ko.md#descriptor)와
resolver에 RID를 게시한다.

RID 형식은 `<prefix>-entry-<lowercase-canonical-uuid-v4>`이며 MeshNode와 별도로 생성한 UUID v4를
사용한다. Framework 내부 MeshNode descriptor의 `entry_spot_id`가 lifecycle의 exact mapping을 제공한다. Global Spot
ID가 active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 configuration exception으로 startup을
실패시킨다. Caller가 지정한 User·Instance Spot ID가 이 예약 형식과 일치하면
Store와 factory를 시작하기 전에 `invalid_operation`으로 거부한다.

Framework가 모든 registration에서 만든 fully encoded MeshNode descriptor는 1 MiB 이하여야 한다.
Spot type과 stateful object capability collection은 각각 최대 1024개다. Runtime은 완성된 descriptor를 socket
bind 전에 한 번에 검증한다. Bound를 넘으면
startup을 실패시키며 collection을 truncate·split하거나 descriptor 일부를 게시하지 않는다.

Topology 등록은 `zlink_framework_options_t`의 `add_route_mesh(...)`,
`add_client_server_channel(...)`, `add_fanout_channel(...)` 세 진입점만 사용한다. RouteMesh는
`add_route_mesh(...).channel(...).client()` 또는 `.server()`로 역할을 고른다. 같은 topology를 다시 만드는
generic channel builder나 node builder를 두지 않는다. Framework 내부는 아래
binding 타입을 조합한다.

- `zlink::context_t`
- `zlink::router_socket_t`
- `zlink::dealer_socket_t`
- `zlink::pub_socket_t`
- `zlink::sub_socket_t`
- MeshNode runtime handler
- `zlink::stream_socket_t`

## 2. ClientServer·Fanout builder

ClientServer와 Fanout은 ChannelName을 사용하지만 서로 다른 물리 topology를 구성한다.
ClientServer client는 send/request를 시작하고 server는 handler/reply를 수행한다. Fanout은
publisher/subscriber 역할을 구성한다.

```cpp
namespace zlink::framework {

class endpoint_connections_t;

class fanout_channel_builder_t {
public:
    fanout_channel_builder_t &enable_publisher(std::string endpoint);
    fanout_channel_builder_t &enable_publisher(std::uint16_t port = 0);
    fanout_channel_builder_t &set_bind_host(std::string host);
    fanout_channel_builder_t &set_advertise_host(std::string host);
    fanout_channel_builder_t &set_routing_id(
      zlink::routing_id_t publisher_routing_id);
    fanout_channel_builder_t &set_automatic_routing_id_prefix(
      std::string prefix);
    fanout_channel_builder_t &enable_subscriber();
    fanout_channel_builder_t &connect(std::string endpoint);
    endpoint_connections_t subscriber_connections();
    fanout_channel_builder_t &add_handler_group(std::string group_name);
};

class client_server_channel_builder_t {
public:
    client_server_channel_client_builder_t client();
    client_server_channel_server_builder_t server();
};

} // namespace zlink::framework
```

요청 timeout은 call object의 `.timeout(...)`과 route request fluent 표면에서 설정한다. pending
queue 상한은 `zlink_framework_options_t::set_max_pending(...)`이 runtime 단위로 소유한다. C++ 공개 계약은
`.NET` 역할 builder에 없는 per-역할 timeout/pending option을 만들지 않는다.

내부 매핑은 아래와 같다.

| Capability | Binding 구현 기준 |
|------------|------------------|
| server | `zlink::router_socket_t` |
| client | `zlink::dealer_socket_t` |
| publisher | `zlink::pub_socket_t` |
| subscriber | `zlink::sub_socket_t` |

ClientServer와 Fanout은 서로 다른 물리 topology이므로 같은 process에서 ChannelName을 공유할 수 없다.
각 topology의 연결 집합과 descriptor는 서로 분리한다.

Automatic RouteMesh는 RID를 canonical byte order로 비교하고 더 작은 RID의 MeshNode만 상대 endpoint로
connect한다. Manual topology는 application endpoint 구성에 따라 한쪽 또는 양쪽에서 connect할 수 있다.
양쪽 연결이나 [automatic discovery](../../../../01-glossary.ko.md#automatic-discovery) 경합·오래된 snapshot으로 중복 후보가 생기면 handshake와 admission이
같은 RID와 lifecycle generation을 확인해 하나만 ready 상태로 유지한다.

ClientServer client는 manual endpoint와 [location store](../../../../01-glossary.ko.md#location-store) automatic discovery를 함께 사용할 수 있다. 두 source가
같은 Server RID와 [lifecycle generation](../../../../01-glossary.ko.md#lifecycle-generation)을 가리키면 connection intent와 ready target을 하나로 합친다.
Automatic과 manual 모두 client만 server로 connect하며 server는 client endpoint를 찾거나 outbound connect를
시작하지 않는다.

같은 process에 Client와 Server를 모두 등록하면 listener와 service admission을 마친 local Server도 remote
Server와 같은 candidate 집합에 포함한다. [Ready](../../../../01-glossary.ko.md#ready), positive weight, non-draining 조건을 동일하게 적용하고
local 우선순위나 remote 제외 규칙을 두지 않는다. Local Server가 선택되어도 client DEALER에서 server
ROUTER로 실제 transport message를 전달한다. Handler 직접 호출로 codec, HWM, timeout, cancellation,
correlation 또는 terminal completion을 우회하지 않는다. `client_and_server`는 channel snapshot의 aggregate
projection에만 사용하며 builder에서 선택하거나 registration key로 사용하지 않는다.

Location store를 등록한 fanout publisher는 fixed Publisher RID와 automatic RID prefix 중 하나를 startup 전에
선택하고 전용 descriptor를 게시한다. Store가 없는 publisher는 listener endpoint를 수동으로 전달하는
대상으로 사용할 수 있지만 automatic discovery 등록은 수행하지 않는다. 인자 없는
`enable_subscriber()`는 같은 ChannelName의 유효한 publisher descriptor를 location store에서 조회해 모두
연결한다. `connect(endpoint)`는 명시한 endpoint만 사용하는 manual subscriber를 구성한다. 두 subscriber
mode를 한 channel에 함께 설정하면 startup이 실패한다. Automatic subscriber는 location store가 필요하며,
manual publisher와 manual subscriber만 사용하는 host는 다른 location 기능이 없으면 store가 필요하지 않다.
Publisher는 descriptor만 게시하고 subscriber endpoint로 outbound connect를 시작하지 않는다. Subscriber만
publisher endpoint로 connect하며 automatic subscriber는 Publisher RID와 lifecycle generation마다 connection
intent 하나를 만든다.
`subscriber_connections()`는 builder의 `connect(endpoint)`와 같은 [manual endpoint](../../../../01-glossary.ko.md#manual-endpoint) 집합을 가리키는
runtime handle이다. 이 handle은 endpoint 연결, 해제와 현재 목록 조회를 제공하며 automatic discovery
결과를 변경하지 않는다.

Endpoint 없이 등록한 automatic subscriber는 `fanout_runtime_t`에서 ChannelName별
`fanout_channel_snapshot_t`를 읽고 `fanout_runtime_event_t`를 관찰한다.
`fanout_publisher_changed_event_t::entry`는 Publisher RID와 공개 연결 상태만 제공한다.
Descriptor revision과 endpoint는 Framework 내부에서 identity와 stale 상태를 판정하는 데만
사용한다. `fanout_location_changed_event_t::location`은 publisher가
0개여도 store degraded·recovered 상태를 전달한다. `std::variant`의 두 대안은 서로의 payload를 optional
field로 섞지 않는다. 각 variant의 `identifier()`는 `static constexpr event_identifier`를 반환하므로 호출자가
identifier를 바꿀 수 없다. `state`와 event identifier는
[Runtime monitoring](../../../../24-runtime-monitoring.ko.md)의 lowercase identifier를 그대로 사용한다. 이 runtime은
읽기 전용이며 `subscriber_connections()`의 manual endpoint 집합을 변경하지 않는다. Manual subscriber로만
등록한 ChannelName을 조회하면 configuration error다.

`client_server_runtime_t::observe(...)`와 `fanout_runtime_t::observe(...)`가 전달하는 단위는
[Monitoring interface](08-monitoring.ko.md)가 선언한 `observed_status_t<TStatus>`다. ClientServer와 fanout은
[Runtime monitoring §3](../../../../24-runtime-monitoring.ko.md#3-현재-상태-조회와-변화-관찰)의 source 표에서
ChannelName을 source 키로 갖는 topology source이므로, 두 stream도 같은 envelope로 event variant를 감싸
observer별 유실 누계를 함께 전달한다. `status` field에 들어가는 값이 snapshot이 아니라 event variant라는
점만 다르고 `loss`의 의미와 reset·saturation 규칙은 같다.

`fanout_runtime_observation_t`는 fanout observer 등록의 수명과 `close()`만 소유한다.
`fanout_runtime_t::observe(...)` callback은 `fanout_runtime_event_t`만 받으므로 RouteMesh·ClientServer event나
raw socket event와 섞이지 않는다.
`close()`는 해당 observation 하나의 queue에 새 event를 넣는 작업을 멈추고 아직 소비하지 않은
event를 폐기한다. 이미 실행 중인 callback은 반환할 수 있지만 `close()`가 반환된 뒤에 새
callback을 시작하지 않는다. Callback 안에서 자신의 observation을 close해도 deadlock을 만들지
않는다. Close는 다른 observer, automatic connection과 `subscriber_connections()`의 manual endpoint
집합을 바꾸지 않는다. `close()`는 여러 번 호출해도 같은 결과를 보장한다. 파생 observation handle의
destructor는 `close()`와 같은 등록 해제를 수행하므로 명시적 `close()` 없이
`std::unique_ptr<fanout_runtime_observation_t>`를 파괴해도 observer 등록이 남지 않는다.

`connection_intent=true`는 automatic planner가 endpoint 연결을 요청했다는 뜻이고 transport readiness가
아니다. `ready=true`, `ready_connection_count`와 publisher changed event의 `ready` state는 publisher 전용
SUB socket의 native-ready와 같은 socket의 첫 valid application record 또는 liveness beacon 수신을 모두
반영한다. `disconnected`는 native disconnect 또는 15초 inbound timeout을 반영한다. `connect` 반환,
native-ready 하나와 내부 active target 수로 이 값을 먼저 바꾸지 않는다.

따라서 `listen`, `connect`, `enable_subscriber` 같은 연결 설정은 generic channel builder가 아니라
ClientServer server, ClientServer client와 Fanout builder에 둔다.

## 3. Handler Registry

handler registry는 typed payload를 함수 수준에서 처리하게 하는 표면이다.

```cpp
namespace zlink::framework {

enum class handler_execution_t {
    inline_on_runtime = 0,
    offload = 1
};

struct handler_options_t {
    std::optional<std::string> packet_name;
    handler_execution_t execution = handler_execution_t::inline_on_runtime;
};

class endpoint_connections_t {
public:
    void connect(std::string endpoint);
    void disconnect(std::string endpoint);
    std::vector<std::string> list_connections() const;
};

// 숫자 값은 관측·진단 데이터의 안정 키이므로 고정한다(framework API §13).
enum class framework_error_kind_t {
    not_found = 0,
    already_exists = 1,
    type_mismatch = 2,
    not_configured = 3,
    rejected = 4,
    unavailable = 5,
    capacity_exceeded = 6,
    deadline_exceeded = 7,
    shutting_down = 8,
    protocol_error = 9,
    invalid_operation = 10,
    data_lost = 11,
    internal_failure = 12
};

class framework_exception_t : public std::exception {
public:
    framework_error_kind_t kind() const noexcept;
    // Platform 원인이 있으면 진단용 error_code를 함께 제공한다.
    // Application의 오류 분기는 kind()를 사용한다.
    std::error_code code() const noexcept;
    const char *what() const noexcept override;
};

template <typename TReply>
class request_call_t {
public:
    request_call_t &timeout(std::chrono::milliseconds timeout);
    request_call_t &metadata(std::string key, std::string value);
    task_t<TReply> submit();
    task_t<TReply> yield();
};

class channel_request_call_t {
public:
    channel_request_call_t &timeout(std::chrono::milliseconds timeout);
    channel_request_call_t &metadata(std::string key, std::string value);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();
};

class send_call_t {
public:
    send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class bound_session_send_call_t {
public:
    bound_session_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class stream_send_call_t {
public:
    ~stream_send_call_t();
    stream_send_call_t(stream_send_call_t &&) noexcept;
    stream_send_call_t &operator=(stream_send_call_t &&) noexcept;
    stream_send_call_t(const stream_send_call_t &) = delete;
    stream_send_call_t &operator=(const stream_send_call_t &) = delete;

    stream_send_call_t &metadata(std::string key, std::string value);
    stream_send_call_t &packet_name(std::string packet_name);
    stream_send_call_t &compress();
    task_t<void> submit();
};

class stream_write_call_t {
public:
    using metadata_map_t = std::map<std::string, std::string>;

    ~stream_write_call_t();
    stream_write_call_t(stream_write_call_t &&) noexcept;
    stream_write_call_t &operator=(stream_write_call_t &&) noexcept;
    stream_write_call_t(const stream_write_call_t &) = delete;
    stream_write_call_t &operator=(const stream_write_call_t &) = delete;

    stream_write_call_t &metadata(std::string key, std::string value);
    stream_write_call_t &compress();
    task_t<void> submit();
};

template <typename TActor>
class bind_actor_call_t {
public:
    bind_actor_call_t &timeout(std::chrono::milliseconds timeout);
    task_t<TActor> async();
};

class message_metadata_t {
public:
    std::optional<std::string_view> find(std::string_view key) const;
    bool contains(std::string_view key) const;
    bool empty() const noexcept;
    const std::map<std::string, std::string> &values() const noexcept;
};

struct message_context_t {
    std::optional<std::string> mesh_name;
    std::optional<std::string> channel_name;
    std::string packet_name;
    std::optional<std::string> content_type;
    message_metadata_t metadata;
    std::optional<std::string> correlation_id;
};

struct publish_message_context_t : message_context_t {
    std::string topic;
    std::optional<std::string> source;
};

enum class handler_dispatch_kind_t {
    node_direct_send = 0,
    node_direct_request = 1,
    channel_send = 2,
    channel_request = 3,
    classic_fanout = 4
};

struct handler_filter_context_t : message_context_t {
    handler_dispatch_kind_t dispatch_kind;
};

using handler_next_t = std::function<task_t<void>()>;

} // namespace zlink::framework
```

`message_context_t`는 inbound message 정보만 제공하며 cancellation 상태를 포함하지 않는다. Request reply 대기
cancellation은 call object가 처리하고 STREAM connection 종료와 cancellation은 session lifecycle이 처리한다.

handler owner 타입은 service collection에서 resolve한다. 일반 application은
`add_zlink_framework(...)` 안에서 handler와 service를 함께 등록한다.

```cpp
options.services().add_transient<order_handler_t>();

options.handlers()
  .group ("orders-api")
  .add<order_created_handler_t> ();
```

STREAM application 업무 경로는 header 객체를 직접 받지 않는다. C++ stream session과 actor relay는
`zlink::message_t` payload 하나를 사용하고, reply와 relay에 필요한 header 값은 runtime 내부
dispatch state가 보존한다. 별도 `_raw` 이름의 public API는 두지 않는다.

handler dispatch는 binding의 `zlink::message_t`와 `zlink::multipart_t`를 받은 뒤,
serializer를 통해 typed payload로 변환하고, DI에서 owner를 resolve한 다음 method를
호출한다.

handler method는 payload만 받을 수도 있고, payload 뒤에 typed context를 함께 받을 수도
있다. request와 send handler는 `message_context_t`, event/publish handler는
`publish_message_context_t`를 받는다. Channel context는 nullable ChannelName, packet name, nullable content
type, immutable metadata와 correlation ID를 제공한다. Node direct context는 이 공통 정보에 source RID만
추가한다. Raw multipart header나 dispatch table은 public context로 노출하지 않는다.

handler filter는 `.NET`의 handler filter처럼 handler 호출 앞뒤의 공통 처리를 맡는다.
일반 application 설정에서는 `options.use_filter<TFilter>()`로 등록한다. filter 타입은
`task_t<void> invoke(const handler_filter_context_t &, handler_next_t)`를 제공한다. 계속 처리하려면
`co_await next()`를 호출한다. filter는 request reply를 만들거나 교체하지 않는다.

적용 범위는 Node direct send/request, RouteMesh·ClientServer Channel send/request와 Classic Fanout
subscription handler다. Spot·Actor handler, Logical Multicast subscription과 STREAM session에는
적용하지 않는다.

`next()`를 호출하지 않은 send는 정상 완료하며 현재 handler를 실행하지 않는다. Classic Fanout은 현재
handler만 종료하고 다른 matching handler를 계속 처리한다. request는 정상 reply 대신
`rejected`로 완료한다. `next()`는 한 번만 호출할 수 있으며 두 번째 호출은
`invalid_operation` 오류다.

handler와 filter는 dispatch마다 만든 같은 handler invocation scope에서 resolve한다. Classic Fanout의
matching handler마다 scope를 따로 만들며 한 handler의 filter 중단이나 실패가 다른 handler를 취소하지
않는다. descriptor lookup, serializer 선택, DI resolve 순서와 filter chain 저장 방식은 public API로
노출하지 않는다.

STREAM handler는 일반 request/send/event handler와 분리한다. Framework runtime은 typed packet
방식만 지원하며 사용자 정의 header framing은 Framework public 표면에 넣지 않는다.

Framework 내부 `recv loop`는 STREAM raw part를 수신하고 header framing과 queue admission을
완료한 뒤 stream callback을 실행한다. Core packet callback이나 raw receive callback으로
queue admission을 우회하지 않는다. 같은 stream session의 packet/lifecycle callback은
managed queue에서 직렬로 처리한다. CPU-bound 또는 blocking 가능성이 있는 stream handler는
offload 실행 정책을 명시한다.

request handler 반환값은 `TReply` 또는 `task_t<TReply>`를 허용한다. `task_t<TReply>`를
반환하는 handler는 `.NET`의 `async Task<TReply>` handler와 같은 의미이며, 내부
request처럼 결과를 기다려야 하는 호출은 `co_await call.submit()` 형태로 사용한다.
one-way send/push는 `co_await call.submit()`으로 send timeout까지 bounded admission 결과를 받는다.
즉시 수락되면 준비된 task가 바로 완료될 수 있으며 remote handler 완료는 기다리지 않는다.

Handler coroutine은 blocking wait 없이 `task_t<T>`로 완료된다. 같은 task의 terminal 결과는 한 번만
확정되며 중복 완료는 먼저 확정된 결과를 바꾸지 않는다. Handler 실행 scheduler와 continuation 배치는 public
API에 노출하지 않는다.

## 4. Messaging API

사용자 코드에서 raw socket 대신 주입받아 쓰는 messaging 표면은 아래와 같다.

```cpp
namespace zlink::framework {

struct send_options_t {
    std::optional<std::string> packet_name;
};

struct request_options_t {
    std::optional<std::string> packet_name;
    std::optional<std::chrono::milliseconds> timeout;
};

class publisher_t {
public:
    template <typename TEvent>
    fanout_publish_call_t publish(std::string channel_name,
      TEvent event);

    template <typename TEvent>
    fanout_publish_call_t publish(std::string channel_name,
      std::string topic, TEvent event);
};

class spot_publisher_client_t {
public:
    template <typename TEvent>
    publish_call_t publish(std::string channel_name, std::string topic,
                           const TEvent &event) const;
};

class request_client_t {
public:
    template <typename TCommand>
    send_call_t send(std::string_view channel_name, const TCommand &command,
      send_options_t options = {});

    template <typename TRequest>
    channel_request_call_t request(std::string_view channel_name,
      const TRequest &request,
      request_options_t options = {});
};

class message_bus_t {
public:
    ~message_bus_t();
    message_bus_t(message_bus_t &&) noexcept;
    message_bus_t &operator=(message_bus_t &&) noexcept;
    message_bus_t(const message_bus_t &) = default;
    message_bus_t &operator=(const message_bus_t &) = default;

    template <typename TRequest>
    channel_request_call_t request(
      std::string channel_name,
      TRequest request);

    template <typename TMessage>
    send_call_t send(std::string channel_name, TMessage message);

    template <typename TEvent>
    send_call_t publish(
      std::string channel_name,
      std::string topic,
      TEvent event);

    std::chrono::milliseconds default_request_timeout(
      const std::string &channel_name) const;
};

class spot_send_call_t;
class spot_request_call_t;

class route_client_t {
public:
    ~route_client_t();
    route_client_t(route_client_t &&) noexcept;
    route_client_t &operator=(route_client_t &&) noexcept;
    route_client_t(const route_client_t &) = default;
    route_client_t &operator=(const route_client_t &) = default;

    // node 대상 — infra 계층과 owner 일관 라우팅용
    template <typename TMessage>
    route_send_call_t send_to_node(std::string mesh_name,
      zlink::routing_id_t target_node_rid,
      TMessage message);

    template <typename TRequest>
    channel_request_call_t request_to_node(std::string mesh_name,
      zlink::routing_id_t target_node_rid,
      TRequest request);

    template <typename TMessage>
    route_send_call_t send_to_channel(std::string channel_name,
      TMessage message);

    template <typename TRequest>
    channel_request_call_t request_to_channel(std::string channel_name,
      TRequest request);

    template <typename TMessage>
    spot_send_call_t send_to_spot(spot_id_t target, TMessage message);

    template <typename TRequest>
    spot_request_call_t request_to_spot(spot_id_t target, TRequest request);
};

class route_send_call_t {
public:
    route_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class spot_send_call_t {
public:
    spot_send_call_t &metadata(std::string key, std::string value);
    spot_send_call_t &instance_spot();
    spot_send_call_t &instance_spot(std::string stable_type);
    spot_send_call_t &in_mesh(std::string mesh_name);
    task_t<void> submit();
};

class spot_request_call_t {
public:
    spot_request_call_t &timeout(std::chrono::milliseconds timeout);
    spot_request_call_t &metadata(std::string key, std::string value);
    spot_request_call_t &instance_spot();
    spot_request_call_t &instance_spot(std::string stable_type);
    spot_request_call_t &in_mesh(std::string mesh_name);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();
};

class fanout_publish_call_t {
public:
    task_t<void> submit();
};

class publish_call_t {
public:
    publish_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

} // namespace zlink::framework
```

`send_to_spot(...)`과 `request_to_spot(...)`의 target은 항상 global `spot_id_t` 하나다. Fluent option은
Missing Instance Spot의 cold activation intent를 표현하며 address에 [MeshName](../../../../01-glossary.ko.md#meshname), stable type, owner RID 또는
generation을 추가하지 않는다. Instance marker를 설정하지 않은 call은 existing-only이고 Missing RID에서
`not_found`로 끝난다.

`instance_spot()`은 [stable type](../../../../01-glossary.ko.md#stable-type)을 생략하고 `instance_spot(stable_type)`은 stable type을 명시한다.
`in_mesh(...)`는 Instance marker와 함께 Missing RID의 최초 placement에만 적용한다. Marker와 이 option은
한 call에서 한 번만 설정할 수 있고 중복 설정은
`invalid_operation`이다. `submit()` 또는 `yield<TReply>()` 가운데 terminal operation도
한 번만 시작할 수 있으며 두 번째 호출은 `invalid_operation`이다.

Public API는 transport 종류와 무관하게 channel name과 typed payload를 기준으로 유지한다.
`publisher_t::publish(...)`는 typed event의 [packet name](../../../../01-glossary.ko.md#packet-name)을 topic으로 사용하는 편의 호출과 [topic](../../../../01-glossary.ko.md#topic)을
명시하는 호출을 함께 제공한다. 두 호출 모두 classic fanout에 사용하며 Framework가 codec을 결정한다.
명시한 topic이 내부 liveness용 exact byte `01 5A 4C 46 31`이면 transport를 시작하지 않고
`framework_exception_t`를 발생시킨다.
`fanout_publish_call_t::submit()`은 local publisher transport가 event를 수락하면 정상 완료한다.
Subscriber 수와 수신 완료는 반환하지 않는다. `publish_call_t`는
[Logical Multicast](../../../../01-glossary.ko.md#logical-multicast) 전용이다. Subscriber가 0개여도
publisher local queue가 event를 수락하면 정상 완료한다.

모든 server one-way call의 `submit()`과 session Actor `relay(...)`는 정상 완료 값을 만들지 않는다. 정상
완료는 operation family가 정의한 source-local queue가 message를 수락했다는 뜻이다. Remote handler 실행,
subscriber 수신, remote Spot queue 수락과 application callback 완료는 기다리지 않는다. Queue capacity가
부족하면 해당 family의 send timeout까지 capacity signal을 기다리고, deadline 안에 공간이 생기면 message를
정확히 한 번 제출한다. `backpressured`는 public terminal result나 즉시 발생하는 application exception이
아니다. Timeout은 `deadline_exceeded`, route 단절은 `unavailable`, runtime 종료는
`shutting_down` kind의 `framework_exception_t`로 완료한다. Actor·Spot·Mesh·session target 부재는 operation
family가 정의한 기존 error kind를 사용한다. C++ server call에는 별도 cancellation 인자가 없다.
반환된 task를 보관하지 않거나 파괴해도 operation이 취소된다고 보장하지 않으며 timeout이나 shutdown 뒤에
같은 operation을 자동으로 다시 제출하지 않는다. 잘못된 argument·state와 중복 submit은
`framework_exception_t`로 완료한다. STREAM reply의 유효한 첫 terminator는
transport를 시작하기 전에 one-shot reply token을 원자적으로
claim하고 소비한다. 같은 token에서 만든 두 call이 경쟁하면 claim에 실패한 call은 transport를 시도하지 않고
`framework_exception_t`로 완료한다. Token을 소비한 call이 `deadline_exceeded`로 끝나도 token을
다시 사용할 수 없으며 이미 사용한 token도 exceptional completion으로 처리한다. STREAM reply는 client
request timeout을 전달받지 않으며 해당 STREAM socket의 send timeout만 사용한다.

RouteMesh node·Channel·[Spot](../../../../01-glossary.ko.md#spot)·Actor는 선택한 MeshNode ROUTER, ClientServer는 client DEALER, [classic fanout](../../../../01-glossary.ko.md#classic-fanout)은
publisher socket, STREAM send·reply는 해당 STREAM socket의 send timeout을 사용한다. Bound session은
local·remote Actor route가 바뀌어도 framework socket send timeout 하나를 사용한다. One-way call에는
per-call `timeout(...)`을 두지 않는다. Socket 또는 MeshNode 설정이 없으면 무한 대기 대신 1초 기본값을
사용한다. One-way admission에 사용하는 socket·MeshNode `std::chrono::milliseconds` 값은 `1..INT_MAX`
범위만 허용한다. `0`, 음수와 상한 초과는 설정 시점 또는 늦어도 startup에서 configuration error로
거부하며 기본값으로 바꾸지 않는다.

Logical Multicast의 `publish_call_t::submit()`은 bounded I/O executor에 direct handoff한다. 즉시 worker slot을
얻지 못하면 send timeout까지 capacity를 기다린다. Slot을 얻으면 raw binding publish를 정확히 한 번 호출한다.
이 call이 시작된 시점이 operation commit barrier다. Transaction이 시작된 뒤 개별 target 실패는 이미 수락한
target을 rollback하거나 전체 publish를 자동 재시도하지 않는다. Remote transport와 local Spot queue의
target별 수락·실패 결과는 반환하거나 monitoring에 집계하지 않는다. Target snapshot이 0개여도 정상
완료한다. Remote Spot queue 제출과 remote·local handler 실행 또는 완료는 `task_t<void>` 완료 조건이
아니다.

framework는 아래 서비스를 기본 등록한다. 사용자는 직접 생성하지 않고 DI에서
주입받아 사용할 수 있다.

- `message_bus_t`
- `publisher_t` (classic fanout client)
- `spot_publisher_client_t` (MeshNode Logical Multicast client)
- `request_client_t`
- `route_client_t`

## 5. Channel 표면

```cpp
struct route_message_context_t : message_context_t {
    zlink::routing_id_t source_node_rid;
};
class channel_client_t {
public:
    template <typename TRequest>
    channel_request_call_t request_to_channel(
      std::string channel_name,
      TRequest request);

    template <typename TMessage>
    send_call_t send_to_channel(
      std::string channel_name,
      TMessage message);
};
```

인자 없는 `add_entry_spot<TEntrySpot>()`은 `TEntrySpot(entry_spot_context_t)` constructor를 사용한다.
명시 factory도 Framework가 먼저 만든 Context를 값으로 받아 application object에 move한다. User·Instance
Spot factory도 각각 정확한 Context를 받아야 하며 생성 뒤 Context를 주입하거나 교체하는 overload는
제공하지 않는다.

## 6. Handler

```cpp
enum class handler_kind_t;   // request / send / publish
```

이 문서의 request·Spot request builder에 선언된 `yield()`는 호출자가 `SpotWide` User Spot 또는 Instance
Spot의 shared turn을 소유할 때만 유효하다. 다른 실행 문맥에서는 message나 operation을 제출하거나 turn을
반환하지 않고 `invalid_operation`으로 완료한다. Actor Join은 현재 handler 안에서
`defer()`로만 등록하며 `async()`와 `yield()`를 제공하지 않는다.
