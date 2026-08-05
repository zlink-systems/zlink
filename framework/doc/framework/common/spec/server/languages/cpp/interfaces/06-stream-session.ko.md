# C++ STREAM session exact interface

[C++ exact interface 목차](README.ko.md) · [Session Actor dispatch](../../../../20-session-actor-dispatch.ko.md)

## 1. Public session surface

Transport control record와 connection identity는 public session interface에 노출하지 않는다.

```cpp
enum class stream_codec_t : std::uint8_t {
    raw = 0,
    json = 1,
    message_pack = 2,
    protobuf = 3
};

enum class stream_session_error_t {
    internal = 0,
    transport_error = 1
};

enum class stream_close_reason_t : std::uint8_t {
    client_close = 1,
    idle_timeout = 2,
    heartbeat_timeout = 3,
    server_drain = 4,
    protocol_error = 5,
    transport_error = 6
};

class stream_compression_codec_t {
public:
    virtual ~stream_compression_codec_t() = default;
    virtual zlink::framework::message_t compress(
      const zlink::framework::message_t &payload) const = 0;
    virtual zlink::framework::message_t decompress(
      const zlink::framework::message_t &payload,
      std::size_t max_decompressed_size) const = 0;
};

std::shared_ptr<const stream_compression_codec_t>
lz4_stream_compression_codec();

class stream_error_t {
public:
    stream_error_t() = default;
    stream_error_t(
      stream_session_error_t error,
      std::string message);

    stream_session_error_t error() const noexcept;
    std::string_view message() const noexcept;
};

struct session_message_context_t {
    std::string packet_name;
    message_metadata_t metadata;
    bool can_reply;
};

class stream_t {
public:
    stream_t();
    ~stream_t();
    stream_t(stream_t &&) noexcept;
    stream_t &operator=(stream_t &&) noexcept;
    stream_t(const stream_t &) = default;
    stream_t &operator=(const stream_t &) = default;

    std::string session_id() const;
    std::optional<zlink::routing_id_t> routing_id() const;
    std::optional<std::string> local_address() const;
    std::optional<std::string> remote_address() const;
    session_actor_manager_t &actors();
    task_t<void> close();
    stream_send_call_t write_packet(
      const zlink::framework::message_t &payload);
    stream_write_call_t reply_packet(
      const zlink::framework::message_t &payload);
};

class packet_stream_session_t {
public:
    virtual ~packet_stream_session_t() = default;
    virtual task_t<void> on_connected(stream_t &stream) = 0;
    virtual task_t<void> on_disconnected(stream_t &stream) = 0;
    virtual task_t<void> on_error(
      stream_t &stream,
      const stream_error_t &error) = 0;
    virtual task_t<void> on_packet(
      stream_t &stream,
      const session_message_context_t &context,
      const zlink::framework::message_t &payload);
};

class stream_builder_t {
public:
    stream_builder_t();
    ~stream_builder_t();
    stream_builder_t(stream_builder_t &&) noexcept;
    stream_builder_t &operator=(stream_builder_t &&) noexcept;
    stream_builder_t(const stream_builder_t &) = default;
    stream_builder_t &operator=(const stream_builder_t &) = default;

    stream_builder_t &bind(std::string endpoint);
    stream_builder_t &bind(std::uint16_t port = 0);
    stream_builder_t &set_bind_host(std::string host);
    stream_builder_t &set_advertise_host(std::string host);
    stream_builder_t &register_session(std::string session_name);
    stream_snapshot_t snapshot() const;
};

struct stream_snapshot_t {
    std::string name;
    std::string bind_endpoint;
    std::string packet_session_name;
    std::string tls_certificate_file;
    std::string tls_private_key_file;
    bool tls_require_client_certificate;
    std::int64_t max_message_size = 64 * 1024;
};

class stream_compression_options_builder_t {
public:
    stream_compression_options_builder_t &use_default();
    stream_compression_options_builder_t &use_lz4();
    stream_compression_options_builder_t &use(
      std::shared_ptr<const stream_compression_codec_t> codec);
    stream_compression_options_builder_t &disable();
};

struct stream_socket_config_t {
    std::int64_t max_message_size = 64 * 1024;
};

class stream_node_options_builder_t {
public:
    stream_node_options_builder_t &bind(std::string endpoint);
    stream_node_options_builder_t &bind(std::uint16_t port = 0);
    stream_node_options_builder_t &set_bind_host(std::string host);
    stream_node_options_builder_t &set_advertise_host(std::string host);
    stream_node_options_builder_t &set_tls_server(
      std::string certificate_file,
      std::string private_key_file,
      bool require_client_certificate = false);
    stream_socket_config_t &configure_socket() noexcept;
    stream_node_options_builder_t &enable_actor_dispatch();
    stream_node_options_builder_t &register_session(std::string session_name);

    template <typename TSession>
      requires std::derived_from<TSession, packet_stream_session_t>
    stream_node_options_builder_t &register_session();
};
```

`configure_socket().max_message_size`는 StreamNode의 Core STREAM inbound 상한이다. 기본값은
`64 KiB`이며 complete message 크기를 6-byte prefix를 제외한 header byte와 payload byte의 합으로
계산한다. 상한은 client에서 server로 수신하는 message에만 적용하고 server에서 client로 보내는
message에는 적용하지 않는다. `0`은 별도 Framework 상한을 사용하지 않는 값이고, 양수는 유한한
상한이다. 음수는 startup configuration error다. 상한을 넘는 client message는 Core가 `EMSGSIZE`로
거부하고 server가 해당 연결을 종료한다. Server callback에는 부분 message를 전달하지 않으며, raw
client는 wire error code를 받지 않고 연결 종료를 관측한다.

`stream_error_t`는 provider-neutral error 종류와 설명만 공개한다. Native transport error code는
runtime 내부 진단 정보이며 public contract에 포함하지 않는다.

Bind 뒤 relay·request relay와 `notify_disconnected()`는 Actor별 저장 route를 사용하며 message마다 Location
Store를 조회하지 않는다. Physical disconnect는 Framework가 current binding 전체에 automatic all-settled
통지를 수행하고 exact binding identity마다 Spot callback을 최대 한 번 실행한다.
`notify_disconnected()`는 connection이 유지된 상태의 logical notification이며 callback terminal까지
기다린다. Relocation route update는 같은 ObjectGeneration에만 허용한다. Target Actor가
복원되어 message 처리를 시작한 뒤 target runtime이 `sessionActorLocationUpdateReqMsg`를
send하여 해당 Actor route와 bound-session의 current `actor_ref_t` location snapshot을 함께
바꾼다. Snapshot은 같은 ActorId·ObjectGeneration과 target MeshName·NodeRid를 반영한다.
응답이 없어도 Target Actor 처리를 멈추지 않으며 정해진 간격으로 같은 요청을 다시 보낸다.
같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와 physical STREAM connection은 유지한다.
Application은 relocation을 알기 위해 `bind()`를 다시 호출하지 않는다.

`bound_session_t`, `session_actor_t`와 `session_actor_manager_t`의 exact Actor 연동 member는
[Actor interface](05-actors.ko.md)가 소유한다. `stream_send_call_t`와 `stream_write_call_t`의
metadata·compression·`submit()` member는 [Channel messaging](03-channel-messaging.ko.md)의 call family와
같은 admission 계약을 유지한다.
STREAM application callback, send·reply와 compression extension은 binding message가 아니라
`zlink::framework::message_t`를 사용한다. Framework codec registry가 typed payload와 이 message 경계를 변환한다.
Framework 내부 recv loop가 raw part를 읽고 queue admission을 완료한 뒤 packet callback을
실행한다. Packet callback은 packet name과 immutable metadata를 가진 `session_message_context_t`를 받는다. Reply 가능 여부는
이 Session specialization의 `can_reply`가 제공한다. Connection 종료와 operation cancellation은 `stream_t` lifecycle과
각 call의 completion으로 처리하며 universal `message_context_t`에 cancellation 상태를 추가하지 않는다.
`stream_t`의 optional routing ID와 local·remote address는 handshake가 확인한 session identity snapshot이며 packet
dispatch까지 보존한다.
Session callback은 받은 `stream_t`의 `actors()`로 해당 session의 Actor binding manager에 접근한다.
Actor dispatch는 global ActorId lookup과 exact `actor_ref_t` bind를 사용하므로 target MeshName 또는 local Actor
overload를 등록하지 않는다. Object role `client`·`server`와 Location Store가 없으면 startup이
`not_configured`로 실패한다.
Handshake failure는 session이 만들어지기 전 runtime monitoring에만 기록되며 `on_error(...)`에
전달하지 않는다.

**wire 값이 계약이다.** `stream_close_reason_t`의 1~6은
[Stream Connector §4.6](../../../../stream-connector/32-stream-connector.ko.md)의 `session-closing` payload와 같은 값이다.
**enum을 정수로 cast해 wire 값으로 쓰지 않는다** — codec이 명시적으로 변환한다.
