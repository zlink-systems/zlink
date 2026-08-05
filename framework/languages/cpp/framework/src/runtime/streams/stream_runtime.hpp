/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::runtime
{
class serial_execution_queue_t;
}

namespace zlink::framework::detail
{

class stream_builder_state_t
{
  public:
    explicit stream_builder_state_t (std::string name) : snapshot{.name = std::move (name)} {}

    stream_snapshot_t snapshot;
    std::optional<std::string> advertise_host;
};

class stream_state_t
{
  public:
    std::string session_id;
    std::optional<zlink::routing_id_t> routing_id;
    std::optional<std::string> local_address;
    std::optional<std::string> remote_address;
    std::atomic_bool closed{false};
    std::deque<std::string> serial_log;
    std::vector<stream_header_t> written_headers;
    std::vector<zlink::message_t> written_payloads;
    mutable std::mutex state_mutex;
    mutable std::mutex dispatch_mutex;
    std::shared_ptr<zlink::framework::runtime::serial_execution_queue_t> dispatch_queue;
    std::mutex transport_writer_mutex;
    std::function<result_t<void> (const stream_header_t &, const zlink::message_t &)>
      transport_writer;
    serializer_registry_t *serializers = nullptr;
    std::atomic<session_actor_manager_t *> actors{nullptr};
    std::shared_ptr<const stream_compression_codec_t> compression_codec;
};

class stream_runtime_state_t
{
  public:
    std::map<std::string, std::shared_ptr<stream_builder_state_t>> streams;
    std::uint64_t next_session_id = 1;
    std::size_t max_pending = 1024;
    dispatch_options_t dispatch;
    serializer_registry_t *serializers = nullptr;
    std::shared_ptr<const stream_compression_codec_t> compression_codec =
      lz4_stream_compression_codec ();
};

class stream_runtime_t
{
  public:
    explicit stream_runtime_t (std::shared_ptr<stream_runtime_state_t> state);

    static stream_runtime_t from (const zlink_builder_t &builder);
    std::vector<stream_snapshot_t> snapshots () const;

    result_t<std::vector<std::uint8_t>> encode_header (const stream_header_t &header) const;
    result_t<stream_header_t> decode_header (const std::vector<std::uint8_t> &bytes) const;
    /* Encodes the versioned session-closing control payload
     * (graceful-drain-handoff §7.1): u8 version=1, u8 reason, u16 diagnostic
     * length (network order, <=512), UTF-8 diagnostic bytes. */
    static std::vector<std::uint8_t>
    encode_session_closing_payload (stream_close_reason_t reason, std::string_view diagnostic);

    result_t<void> validate_header (const stream_header_t &header) const;

    stream_t open_session (std::string stream_name) const;
    void set_session_identity (stream_t &stream,
                               std::optional<zlink::routing_id_t> routing_id,
                               std::optional<std::string> local_address = std::nullopt,
                               std::optional<std::string> remote_address = std::nullopt) const;
    std::size_t pending_limit () const noexcept { return _state->max_pending; }
    result_t<void> dispatch_connected (packet_stream_session_t &session, stream_t &stream) const;
    result_t<void> dispatch_packet (packet_stream_session_t &session,
                                    stream_t &stream,
                                    const stream_header_t &header,
                                    const zlink::message_t &payload) const;
    result_t<void> dispatch_disconnected (packet_stream_session_t &session, stream_t &stream) const;
    void mark_disconnected (stream_t &stream) const;
    result_t<void> dispatch_error (packet_stream_session_t &session,
                                   stream_t &stream,
                                   const stream_error_t &error) const;
    using async_dispatch_completion_t =
      std::function<void (const result_t<void> &)>;
    using async_dispatch_started_t = std::function<void ()>;
    using async_dispatch_cancel_t = std::function<bool ()>;
    result_t<void> dispatch_connected_async (
      packet_stream_session_t &session,
      stream_t &stream,
      async_dispatch_completion_t completion = {}) const;
    result_t<void> dispatch_packet_async (
      packet_stream_session_t &session,
      stream_t &stream,
      const stream_header_t &header,
      const zlink::message_t &payload,
      async_dispatch_completion_t completion = {},
      async_dispatch_started_t started = {},
      async_dispatch_cancel_t cancelled = {}) const;
    result_t<void> dispatch_disconnected_async (
      packet_stream_session_t &session,
      stream_t &stream,
      async_dispatch_completion_t completion = {}) const;
    void drain_async_dispatch (stream_t &stream) const;
    void attach_transport_writer (
      stream_t &stream,
      std::function<result_t<void> (const stream_header_t &, const zlink::message_t &)> writer)
      const;

    std::vector<std::string> serial_log (const stream_t &stream) const;
    std::vector<stream_header_t> written_headers (const stream_t &stream) const;
    std::vector<zlink::message_t> written_payloads (const stream_t &stream) const;

  private:
    result_t<void> dispatch_serial (stream_t &stream,
                                    std::string operation,
                                    std::function<task_t<void> ()> callback) const;
    result_t<void> dispatch_serial_async (
      stream_t &stream,
      std::string operation,
      std::function<task_t<void> ()> callback,
      async_dispatch_completion_t completion,
      async_dispatch_started_t started = {},
      async_dispatch_cancel_t cancelled = {}) const;

  public:
    const dispatch_options_t &dispatch_options_ref () const noexcept { return _state->dispatch; }

    /* Sends the versioned session-closing control on an active session
     * (graceful-drain-handoff §7): best effort, bounded by the transport
     * writer; the caller closes the connection afterwards. */
    void send_session_closing (stream_t &stream,
                               stream_close_reason_t reason,
                               std::string_view diagnostic) const noexcept;

    /* Server liveness ping (graceful-drain-handoff §7.2): empty control frame
     * on the session's transport writer; the sweep loop owns the cadence. */
    void send_heartbeat_ping (stream_t &stream) const noexcept;
    void send_heartbeat_pong (stream_t &stream) const noexcept;

  private:
    std::shared_ptr<stream_runtime_state_t> _state;
};

void configure_stream_dispatch_executor ();
void shutdown_stream_dispatch_executor () noexcept;

} // namespace zlink::framework::detail
