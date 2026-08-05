/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/messaging/message.hpp>
#include <zlink/framework/contracts/messaging/message_context.hpp>

#include <concepts>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework
{

class zlink_builder_t;
class stream_node_options_builder_t;
class session_actor_manager_t;

namespace detail
{
class stream_builder_state_t;
class stream_state_t;
class stream_runtime_t;
class actor_gateway_runtime_t;
class session_actor_manager_access_t;
enum class stream_message_kind_t : std::uint8_t
{
    send = 1,
    request = 2,
    response = 3,
    error = 4,
    control = 5
};

enum class stream_header_flags_t : std::uint8_t
{
    none = 0,
    has_request_seq = 0x01,
    has_metadata = 0x02,
    payload_compressed = 0x04,
    has_correlation_id = 0x08,
    has_flow_id = 0x10
};

constexpr stream_header_flags_t operator| (stream_header_flags_t lhs,
                                           stream_header_flags_t rhs) noexcept
{
    return static_cast<stream_header_flags_t> (static_cast<std::uint8_t> (lhs)
                                               | static_cast<std::uint8_t> (rhs));
}

constexpr stream_header_flags_t operator& (stream_header_flags_t lhs,
                                           stream_header_flags_t rhs) noexcept
{
    return static_cast<stream_header_flags_t> (static_cast<std::uint8_t> (lhs)
                                               & static_cast<std::uint8_t> (rhs));
}
class stream_header_t;
} // namespace detail

enum class stream_codec_t : std::uint8_t
{
    raw = 0,
    json = 1,
    message_pack = 2,
    protobuf = 3
};

enum class stream_session_error_t
{
    internal = 0,
    transport_error = 1
};

/* Closed set of session close reasons (graceful-drain-handoff §7.1). The
 * server sends a versioned `session-closing` control with this reason before
 * intentionally closing a STREAM connection. */
enum class stream_close_reason_t : std::uint8_t
{
    client_close = 1,
    idle_timeout = 2,
    heartbeat_timeout = 3,
    server_drain = 4,
    protocol_error = 5,
    transport_error = 6
};

class stream_compression_codec_t
{
  public:
    virtual ~stream_compression_codec_t () = default;
    virtual zlink::message_t compress (const zlink::message_t &payload) const = 0;
    virtual zlink::message_t decompress (const zlink::message_t &payload,
                                         std::size_t max_decompressed_size) const = 0;
};

std::shared_ptr<const stream_compression_codec_t> lz4_stream_compression_codec ();

class stream_error_t
{
  public:
    stream_error_t () = default;
    stream_error_t (stream_session_error_t error, std::string message);

    stream_session_error_t error () const noexcept;
    std::string_view message () const noexcept;

  private:
    stream_session_error_t _error = stream_session_error_t::internal;
    std::string _message;
};

namespace detail
{

/* Transport headers retain a mutable map internally. The public session
 * callback exposes message_metadata_t instead, so the header storage does not
 * become a public transport type. */
class stream_metadata_t
{
  public:
    stream_metadata_t () = default;
    explicit stream_metadata_t (std::map<std::string, std::string> values);

    stream_metadata_t &with (std::string key, std::string value);
    std::optional<std::string_view> find (std::string_view key) const;
    bool empty () const noexcept;
    const std::map<std::string, std::string> &values () const noexcept;

  private:
    std::map<std::string, std::string> _values;
};

class stream_header_t
{
  public:
    stream_header_t ();
    stream_header_t (stream_message_kind_t kind,
                     stream_codec_t codec,
                     stream_header_flags_t flags,
                     std::optional<std::uint64_t> request_seq,
                     std::string packet_name,
                     stream_metadata_t metadata = {});

    stream_message_kind_t kind () const noexcept;
    stream_codec_t codec () const noexcept;
    stream_header_flags_t flags () const noexcept;
    std::optional<std::uint64_t> request_seq () const noexcept;
    std::string_view packet_name () const noexcept;
    std::optional<std::string_view> metadata (std::string_view key) const;
    const stream_metadata_t &metadata () const noexcept;
    std::optional<std::string_view> correlation_id () const;
    std::optional<std::string_view> content_type () const;

    // correlation_id is a first-class stream-header field (parity with the
    // channel envelope). The sending client generates it; the server echoes it
    // onto replies. Empty means "not set".
    stream_header_t &with_correlation_id (std::string correlation_id);

    // flow_id/flow_origin are an optional pair (flow-correlation §3.2):
    // both present or both absent, preserved as-is by every relay.
    std::optional<std::string_view> flow_id () const;
    std::optional<flow_origin_t> flow_origin () const noexcept;
    stream_header_t &with_flow (std::string flow_id, flow_origin_t origin);

  private:
    stream_message_kind_t _kind = stream_message_kind_t::send;
    stream_codec_t _codec = stream_codec_t::raw;
    stream_header_flags_t _flags = stream_header_flags_t::none;
    std::optional<std::uint64_t> _request_seq;
    std::string _packet_name;
    stream_metadata_t _metadata;
    std::string _correlation_id;
    std::string _flow_id;
    std::optional<flow_origin_t> _flow_origin;
};

} // namespace detail

struct session_message_context_t
{
    std::string packet_name;
    message_metadata_t metadata;
    bool can_reply = false;
};

class stream_t
{
  public:
    stream_t ();
    ~stream_t ();

    stream_t (stream_t &&) noexcept;
    stream_t &operator= (stream_t &&) noexcept;
    stream_t (const stream_t &) = default;
    stream_t &operator= (const stream_t &) = default;

    std::string session_id () const;
    std::optional<zlink::routing_id_t> routing_id () const;
    std::optional<std::string> local_address () const;
    std::optional<std::string> remote_address () const;
    session_actor_manager_t &actors ();
    task_t<void> close ();
    stream_send_call_t write_packet (const zlink::message_t &payload);
    stream_write_call_t reply_packet (const zlink::message_t &payload);

  private:
    friend class detail::actor_gateway_runtime_t;
    friend class detail::session_actor_manager_access_t;
    friend class detail::stream_runtime_t;
    explicit stream_t (std::shared_ptr<detail::stream_state_t> state);
    stream_write_call_t write_packet_with_header (detail::stream_header_t header,
                                                  zlink::message_t payload);

    std::shared_ptr<detail::stream_state_t> _state;
    std::optional<detail::stream_header_t> _reply_header;
    std::shared_ptr<detail::submit_once_t> _reply_submission;
};

/* Typed session packet handler contract: the serializer registry decodes the
 * payload first, then the handler completes with task_t<void>. The raw
 * message_t on_packet callback stays at the session runtime boundary. */
template <typename THandler, typename TSessionContext, typename TPayload>
concept typed_session_packet_handler_for =
  requires (THandler &handler, TSessionContext &context, const TPayload &payload) {
      { handler.handle (context, payload) } -> std::same_as<task_t<void>>;
  };

template <typename TPayload, typename THandler>
  requires typed_session_packet_handler_for<THandler, stream_t, TPayload>
task_t<void> dispatch_typed_session_packet (THandler &handler,
                                            stream_t &stream,
                                            serializer_registry_t &serializers,
                                            const zlink::message_t &payload)
{
    const auto decoded =
      serializers.get<TPayload> ().deserialize (detail::encoded_payload_from_raw (payload));
    co_await handler.handle (stream, decoded);
}

class packet_stream_session_t
{
  public:
    virtual ~packet_stream_session_t () = default;
    virtual task_t<void> on_connected (stream_t &stream) = 0;
    virtual task_t<void> on_disconnected (stream_t &stream) = 0;
    virtual task_t<void> on_error (stream_t &stream, const stream_error_t &error) = 0;
    virtual task_t<void> on_packet (stream_t &stream,
                                    const session_message_context_t &context,
                                    const zlink::message_t &payload)
    {
        (void) stream;
        (void) context;
        (void) payload;
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                      "stream packet handler is not implemented"));
    }
};

struct stream_snapshot_t
{
    std::string name;
    std::string bind_endpoint;
    std::string packet_session_name;
    std::string tls_certificate_file;
    std::string tls_private_key_file;
    bool tls_require_client_certificate = false;
    // Complete header plus payload bytes accepted from a client. Zero means
    // that Framework adds no separate limit.
    std::int64_t max_message_size = 64 * 1024;
};

class stream_builder_t
{
  public:
    stream_builder_t ();
    ~stream_builder_t ();

    stream_builder_t (stream_builder_t &&) noexcept;
    stream_builder_t &operator= (stream_builder_t &&) noexcept;
    stream_builder_t (const stream_builder_t &) = default;
    stream_builder_t &operator= (const stream_builder_t &) = default;

    stream_builder_t &bind (std::string endpoint);
    stream_builder_t &bind (std::uint16_t port = 0);
    stream_builder_t &set_bind_host (std::string host);
    stream_builder_t &set_advertise_host (std::string host);
    stream_builder_t &register_session (std::string session_name);
    stream_snapshot_t snapshot () const;

  private:
    friend class zlink_builder_t;
    friend class stream_node_options_builder_t;
    friend class detail::stream_runtime_t;
    explicit stream_builder_t (std::shared_ptr<detail::stream_builder_state_t> state);
    stream_builder_t &set_max_message_size (std::int64_t value);
    stream_builder_t &configure_tls_server (std::string certificate_file,
                                            std::string private_key_file,
                                            bool require_client_certificate);

    std::shared_ptr<detail::stream_builder_state_t> _state;
};

} // namespace zlink::framework
