# C++ STREAM Session Exact Interface

[C++ exact interface table of contents](README.en.md) · [Session Actor Dispatch](../../../../20-session-actor-dispatch.en.md)

## 1. Public Session Surface

The public session interface doesn't expose transport control record
and connection identity.

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

`configure_socket().max_message_size` is the StreamNode's Core STREAM
inbound ceiling. It defaults to `64 KiB`; a complete message is measured
as header bytes plus payload bytes, excluding the 6-byte prefix. The
ceiling applies only to messages received from client to server, not to
messages sent from server to client. `0` means that Framework adds no
separate ceiling, while a positive value is finite. A negative value is a
startup configuration error. When a client message exceeds the ceiling,
Core rejects it with `EMSGSIZE` and the server closes that connection. The
server callback receives no partial message. A raw client observes the
connection close and doesn't receive a wire error code.

`stream_error_t` only exposes a provider-neutral error kind and
description. The native transport error code is runtime-internal
diagnostic information and isn't included in the public contract.

After bind, relay/request relay and `notify_disconnected()` use a
per-Actor stored route and don't look up Location Store for every
message. On physical disconnect, the Framework performs an automatic
all-settled notification across every current binding and runs the
Spot callback at most once per exact binding identity.
`notify_disconnected()` is a logical notification while the connection
is kept, and waits until the callback terminal. Relocation route update
is only allowed within the same ObjectGeneration. After the target
Actor is restored and starts processing messages, the target runtime
sends `sessionActorLocationUpdateReqMsg` to change that Actor route and
the bound session's current `actor_ref_t` location snapshot together.
The snapshot reflects the same ActorId/ObjectGeneration and the target
MeshName/NodeRid. Even without a response, target Actor processing
doesn't stop, and the same request is resent at a fixed interval. In
the same Session, the route and physical STREAM connection of a
different Actor not included in the relocation target is kept. The
application doesn't call `bind()` again to learn about relocation.

The exact Actor-interworking member of `bound_session_t`,
`session_actor_t`, and `session_actor_manager_t` is owned by the
[Actor interface](05-actors.en.md). The metadata/compression/
`submit()` member of `stream_send_call_t` and `stream_write_call_t`
keeps the same admission contract as the call family in
[Channel messaging](03-channel-messaging.en.md).
STREAM application callback, send/reply, and compression extension use
`zlink::framework::message_t`, not the binding message. Framework's
codec registry converts a typed payload and this message boundary.
After the Framework-internal recv loop reads a raw part and completes
queue admission, it runs the packet callback. The packet callback
receives `session_message_context_t`, which carries the packet name
and immutable metadata. Whether reply is possible is provided by that
Session specialization's `can_reply`. Connection close and operation
cancellation are handled through `stream_t` lifecycle and each call's
completion, and cancellation state isn't added to the universal
`message_context_t`. `stream_t`'s optional routing ID and local/remote
address are a session identity snapshot the handshake confirmed, kept
through packet dispatch.
The Session callback accesses that session's Actor binding manager
through the received `stream_t`'s `actors()`. Since Actor dispatch uses
global ActorId lookup and an exact `actor_ref_t` bind, a target
MeshName or local Actor overload isn't registered. Without Object role
`client`/`server` and Location Store, startup fails with
`not_configured`.
A handshake failure is only recorded to runtime monitoring before the
session is created, and isn't delivered to `on_error(...)`.

**The wire value is a contract.** `stream_close_reason_t`'s 1-6 are the
same values as the `session-closing` payload in
[Stream Connector §4.6](../../../../stream-connector/32-stream-connector.en.md).
**Don't cast the enum to an integer and use it as the wire value** —
the codec converts it explicitly.
