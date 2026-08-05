/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <netinet/in.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#endif

namespace
{

class sample_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        events.push_back ("connected:" + stream.session_id ());
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &stream) override
    {
        events.push_back ("disconnected:" + stream.session_id ());
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        events.push_back ("error:" + std::string (error.message ()));
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &stream,
                                              const zlink::framework::session_message_context_t &dispatch,
                                              const zlink::message_t &payload) override
    {
        events.push_back ("packet:" + std::string (dispatch.packet_name) + ":"
                          + payload.to_string ());
        last_can_reply = dispatch.can_reply;
        last_metadata = dispatch.metadata;
        stream.reply_packet (payload).submit ().result ().value ();
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    std::vector<std::string> events;
    bool last_can_reply = false;
    zlink::framework::message_metadata_t last_metadata;
};

class duplicate_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_error (
      zlink::framework::stream_t &,
      const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &,
      const zlink::message_t &payload) override
    {
        auto winner = stream.reply_packet (payload);
        auto loser = stream.reply_packet (payload);
        co_await winner.submit ();
        winner_completed = true;
        try {
            (void) co_await loser.submit ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            loser_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::protocol_error;
        }
        co_return;
    }

    bool winner_completed = false;
    bool loser_rejected = false;
};

class failed_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_error (
      zlink::framework::stream_t &,
      const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &,
      const zlink::message_t &payload) override
    {
        const auto first = stream.reply_packet (payload).submit ().result ();
        first_failed = !first
                       && first.error_kind ()
                            == zlink::framework::framework_error_kind_t::internal_failure;
        const auto second = stream.reply_packet (payload).submit ().result ();
        second_rejected = !second
                          && second.error_kind ()
                               == zlink::framework::framework_error_kind_t::protocol_error;
        co_return;
    }

    bool first_failed = false;
    bool second_rejected = false;
};

class throwing_packet_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        on_error_called = true;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::message_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
          zlink::framework::framework_error_kind_t::internal_failure, "application packet failure"));
    }

    bool on_error_called = false;
};

class delayed_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    delayed_reply_session_t () :
        _entered_future (_entered.get_future ()),
        _resume ([this] (std::function<void ()> continuation) {
            {
                std::lock_guard lock (_mutex);
                _continuations.push_back (std::move (continuation));
            }
            _continuation_ready.notify_one ();
        })
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_error (
      zlink::framework::stream_t &,
      const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &,
      const zlink::message_t &payload) override
    {
        _entered.set_value ();
        co_await _resume.task ();
        try {
            (void) co_await stream.reply_packet (payload).submit ();
            reply_result = zlink::framework::result_t<void>::success ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            reply_result = zlink::framework::detail::result_access_t::failure<void> (error);
        }
    }

    void wait_until_suspended () { _entered_future.wait (); }

    void resume ()
    {
        _resume.complete (zlink::framework::result_t<void>::success ());
        std::function<void ()> continuation;
        {
            std::unique_lock lock (_mutex);
            _continuation_ready.wait (lock, [this] { return !_continuations.empty (); });
            continuation = std::move (_continuations.front ());
            _continuations.pop_front ();
        }
        continuation ();
    }

    std::optional<zlink::framework::result_t<void>> reply_result;

  private:
    std::promise<void> _entered;
    std::future<void> _entered_future;
    std::mutex _mutex;
    std::condition_variable _continuation_ready;
    std::deque<std::function<void ()>> _continuations;
    zlink::framework::detail::task_completion_source_t<void> _resume;
};

class prefix_stream_compression_codec_t final
  : public zlink::framework::stream_compression_codec_t
{
  public:
    explicit prefix_stream_compression_codec_t (std::string prefix) :
        _prefix (std::move (prefix))
    {
    }

    zlink::message_t compress (const zlink::message_t &payload) const override
    {
        return zlink::message_t::from (_prefix + payload.to_string ());
    }

    zlink::message_t decompress (const zlink::message_t &payload,
                                 std::size_t max_decompressed_size) const override
    {
        const auto text = payload.to_string ();
        if (text.rfind (_prefix, 0) != 0) {
            throw std::runtime_error ("custom stream compression marker is invalid");
        }
        auto decoded = text.substr (_prefix.size ());
        if (decoded.size () > max_decompressed_size) {
            throw std::runtime_error ("custom stream payload exceeds receive limit");
        }
        return zlink::message_t::from (decoded);
    }

  private:
    std::string _prefix;
};

class oversized_stream_compression_codec_t final
  : public zlink::framework::stream_compression_codec_t
{
  public:
    zlink::message_t compress (const zlink::message_t &payload) const override { return payload; }

    zlink::message_t decompress (const zlink::message_t &, std::size_t) const override
    {
        return zlink::message_t::from (std::string (64 * 1024 + 1, 'x'));
    }
};

class transport_error_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        {
            const std::lock_guard lock (_mutex);
            _identity_available = stream.local_address ().has_value ()
                                  && stream.remote_address ().has_value ();
            if (_identity_available) {
                _last_local = *stream.local_address ();
                _last_remote = *stream.remote_address ();
            }
        }
        record (_connected);
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        record (_disconnected);
        co_return;
    }

    zlink::framework::task_t<void> on_error (
      zlink::framework::stream_t &,
      const zlink::framework::stream_error_t &error) override
    {
        {
            const std::lock_guard lock (_mutex);
            ++_errors;
            _last_error = error.error ();
        }
        _changed.notify_all ();
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &dispatch,
      const zlink::message_t &payload) override
    {
        record (_packets);
        if (dispatch.can_reply) {
            (void) co_await stream.reply_packet (payload).submit ();
        }
        co_return;
    }

    bool wait_connected (int count) { return wait_for (_connected, count); }
    bool wait_disconnected (int count) { return wait_for (_disconnected, count); }
    bool wait_errors (int count) { return wait_for (_errors, count); }
    bool wait_packets (int count) { return wait_for (_packets, count); }

    bool identity_available () const
    {
        const std::lock_guard lock (_mutex);
        return _identity_available;
    }

    std::string last_local () const
    {
        const std::lock_guard lock (_mutex);
        return _last_local;
    }

    std::string last_remote () const
    {
        const std::lock_guard lock (_mutex);
        return _last_remote;
    }

    int errors () const
    {
        const std::lock_guard lock (_mutex);
        return _errors;
    }

    int packets () const
    {
        const std::lock_guard lock (_mutex);
        return _packets;
    }

    zlink::framework::stream_session_error_t last_error () const
    {
        const std::lock_guard lock (_mutex);
        return _last_error;
    }

  private:
    void record (int &counter)
    {
        {
            const std::lock_guard lock (_mutex);
            ++counter;
        }
        _changed.notify_all ();
    }

    bool wait_for (int &counter, int count)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (2),
                                  [&] { return counter >= count; });
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    int _connected = 0;
    int _disconnected = 0;
    int _errors = 0;
    int _packets = 0;
    bool _identity_available = false;
    std::string _last_local;
    std::string _last_remote;
    zlink::framework::stream_session_error_t _last_error =
      zlink::framework::stream_session_error_t::internal;
};

class rejected_connected_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        {
            const std::lock_guard lock (_mutex);
            _stream = stream;
            _manager_was_attached = &stream.actors () != nullptr;
        }
        _changed.notify_all ();
        return zlink::framework::task_t<void> (
          zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "connection callback rejected"));
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_error (
      zlink::framework::stream_t &,
      const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    bool wait_until_actor_manager_is_detached ()
    {
        {
            std::unique_lock lock (_mutex);
            if (!_changed.wait_for (lock, std::chrono::seconds (2),
                                    [&] { return _stream.has_value (); })) {
                return false;
            }
            if (!_manager_was_attached) {
                return false;
            }
        }
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        while (std::chrono::steady_clock::now () < deadline) {
            zlink::framework::stream_t observed;
            {
                const std::lock_guard lock (_mutex);
                observed = *_stream;
            }
            try {
                (void) observed.actors ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                return error.kind ()
                       == zlink::framework::framework_error_kind_t::not_configured;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        return false;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::optional<zlink::framework::stream_t> _stream;
    bool _manager_was_attached = false;
};

std::uint16_t reserve_loopback_port ()
{
    const int socket_fd = ::socket (AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error ("failed to reserve STREAM test port");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind (socket_fd, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0) {
        ::close (socket_fd);
        throw std::runtime_error ("failed to bind STREAM test port");
    }
    socklen_t size = sizeof (address);
    if (::getsockname (socket_fd, reinterpret_cast<sockaddr *> (&address), &size) != 0) {
        ::close (socket_fd);
        throw std::runtime_error ("failed to inspect STREAM test port");
    }
    const auto port = ntohs (address.sin_port);
    ::close (socket_fd);
    return port;
}

int connect_loopback (std::uint16_t port)
{
    const int socket_fd = ::socket (AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    address.sin_port = htons (port);
    if (::connect (socket_fd, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0) {
        ::close (socket_fd);
        return -1;
    }
    return socket_fd;
}

std::vector<std::uint8_t> make_native_stream_frame (
  const zlink::framework::detail::stream_runtime_t &runtime,
  const zlink::framework::detail::stream_header_t &header,
  const zlink::message_t &payload)
{
    const auto encoded_header = runtime.encode_header (header);
    if (!encoded_header) {
        throw std::runtime_error ("failed to encode STREAM fairness frame header");
    }
    const auto payload_bytes = payload.to_bytes ();
    const auto header_size = encoded_header.value ().size ();
    std::vector<std::uint8_t> frame;
    frame.reserve (6 + header_size + payload_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
    frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
    frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
    frame.insert (frame.end (), encoded_header.value ().begin (), encoded_header.value ().end ());
    frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
    return frame;
}

void send_native_bytes (int socket_fd,
                        const std::vector<std::uint8_t> &bytes,
                        std::size_t offset,
                        std::size_t length)
{
    if (offset > bytes.size () || length > bytes.size () - offset) {
        throw std::runtime_error ("invalid STREAM fairness frame range");
    }
    const auto end = offset + length;
    while (offset < end) {
        const auto sent = ::send (socket_fd, bytes.data () + offset, end - offset,
                                  MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error ("failed to send STREAM fairness frame");
        }
        if (sent == 0) {
            throw std::runtime_error ("STREAM fairness frame send made no progress");
        }
        offset += static_cast<std::size_t> (sent);
    }
}

void send_native_bytes (int socket_fd, const std::vector<std::uint8_t> &bytes)
{
    send_native_bytes (socket_fd, bytes, 0, bytes.size ());
}

} // namespace

int main ()
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::stream_codec_t;
    using zlink::framework::detail::stream_header_flags_t;
    using zlink::framework::detail::stream_message_kind_t;

    struct stream_dispatch_executor_guard_t
    {
        stream_dispatch_executor_guard_t ()
        {
            zlink::framework::detail::configure_stream_dispatch_executor ();
        }

        ~stream_dispatch_executor_guard_t ()
        {
            zlink::framework::detail::shutdown_stream_dispatch_executor ();
        }
    } stream_dispatch_executor_guard;

    zlink::framework::zlink_builder_t zlink;
    zlink.stream ("client-stream")
      .bind ("tcp://0.0.0.0:9200")
      .register_session ("client");
    zlink::framework::serializer_registry_t serializers;
    serializers.add<std::string> (
      [] (const std::string &value) {
          return zlink::framework::encoded_payload_t::from_string (value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) { return payload.to_string (); });
    zlink::framework::detail::bind_stream_serializers (zlink, serializers);

    const auto snapshots = zlink::framework::detail::stream_runtime_t::from (zlink).snapshots ();
    if (snapshots.size () != 1 || snapshots[0].name != "client-stream"
        || snapshots[0].bind_endpoint != "tcp://0.0.0.0:9200"
        || snapshots[0].packet_session_name != "client")
        return 1;

    auto runtime = zlink::framework::detail::stream_runtime_t::from (zlink);
    zlink::framework::detail::stream_metadata_t metadata;
    metadata.with ("trace", "42").with ("content_type", "application/json");
    zlink::framework::detail::stream_header_t request_header (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      77, "move", metadata);
    // correlation_id is now a first-class stream-header field (not a metadata key).
    request_header.with_correlation_id ("abc");

    const auto encoded = runtime.encode_header (request_header);
    if (!encoded) {
        return 2;
    }
    const auto decoded = runtime.decode_header (encoded.value ());
    if (!decoded || decoded.value ().kind () != stream_message_kind_t::request
        || decoded.value ().codec () != stream_codec_t::json
        || decoded.value ().request_seq () != 77 || decoded.value ().packet_name () != "move"
        || decoded.value ().correlation_id () != "abc"
        || decoded.value ().content_type () != "application/json"
        || decoded.value ().metadata ("trace") != "42") {
        return 3;
    }

    zlink::framework::detail::stream_header_t reply_header (
      stream_message_kind_t::response, stream_codec_t::json,
      stream_header_flags_t::has_request_seq, 77, "");
    const auto encoded_reply = runtime.encode_header (reply_header);
    if (!encoded_reply || encoded_reply.value ().size () < 13 || encoded_reply.value ()[12] != 0
        || !runtime.decode_header (encoded_reply.value ())) {
        return 33;
    }
    auto legacy_reply = encoded_reply.value ();
    legacy_reply[12] = 6;
    legacy_reply.insert (legacy_reply.begin () + 13, {'l', 'e', 'g', 'a', 'c', 'y'});
    const auto decoded_legacy_reply = runtime.decode_header (legacy_reply);
    if (!decoded_legacy_reply || decoded_legacy_reply.value ().packet_name () != "legacy") {
        return 35;
    }
    zlink::framework::detail::stream_header_t named_reply_header (
      stream_message_kind_t::response, stream_codec_t::json,
      stream_header_flags_t::has_request_seq, 77, "legacy.reply");
    if (runtime.encode_header (named_reply_header)) {
        return 34;
    }

    zlink::framework::detail::stream_header_t invalid_send (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::has_request_seq, 1,
      "bad");
    if (runtime.validate_header (invalid_send)
        || runtime.validate_header (invalid_send).error_kind ()
             != framework_error_kind_t::protocol_error) {
        return 4;
    }

    zlink::framework::detail::stream_header_t reserved (stream_message_kind_t::send, stream_codec_t::raw,
                                                stream_header_flags_t::none, std::nullopt,
                                                "__zlink.internal");
    if (runtime.validate_header (reserved)) {
        return 5;
    }

    zlink::framework::detail::stream_header_t valid_control (
      stream_message_kind_t::control, stream_codec_t::raw, stream_header_flags_t::none,
      std::nullopt, "__zlink.ping");
    if (!runtime.validate_header (valid_control)) {
        return 6;
    }
    zlink::framework::detail::stream_header_t missing_request_seq (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "missing-seq");
    zlink::framework::detail::stream_header_t zero_request_seq (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      0, "zero-seq");
    zlink::framework::detail::stream_header_t invalid_error (
      stream_message_kind_t::error, stream_codec_t::raw, stream_header_flags_t::has_request_seq, 1,
      "error");
    zlink::framework::detail::stream_header_t invalid_control (
      stream_message_kind_t::control, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "__zlink.bad");
    if (runtime.validate_header (missing_request_seq) || runtime.validate_header (zero_request_seq)
        || runtime.validate_header (invalid_error) || runtime.validate_header (invalid_control)) {
        return 20;
    }
    zlink::framework::detail::stream_metadata_t large_metadata;
    large_metadata.with ("trace", std::string (65536, 'x'));
    zlink::framework::detail::stream_header_t too_large_metadata (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "large", large_metadata);
    if (runtime.encode_header (too_large_metadata)
        || runtime.encode_header (too_large_metadata).error_kind ()
             != framework_error_kind_t::protocol_error) {
        return 21;
    }
    const std::vector<std::vector<std::uint8_t>> invalid_headers{
      {},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::request),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_request_seq), 1},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 0},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 4, 'n'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 3,
       'k'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 1,
       'k'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 1,
       'k', 3, 'v'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 4, 'n', 'a', 'm', 'e', 0}};
    for (const auto &invalid_header : invalid_headers) {
        if (runtime.decode_header (invalid_header)
            || runtime.decode_header (invalid_header).error_kind ()
                 != framework_error_kind_t::protocol_error) {
            return 22;
        }
    }

    /* flow-correlation §3.2/§3.4: missing/wrong marker and malformed flow
     * fields fail as protocol errors; a valid flow pair round-trips. */
    const std::vector<std::uint8_t> no_marker_header{
      static_cast<std::uint8_t> (stream_message_kind_t::send),
      static_cast<std::uint8_t> (stream_codec_t::json),
      static_cast<std::uint8_t> (stream_header_flags_t::none), 4, 'n', 'a', 'm', 'e'};
    auto no_marker = runtime.decode_header (no_marker_header);
    if (no_marker
        || no_marker.error_kind () != framework_error_kind_t::protocol_error) {
        return 220;
    }
    const std::vector<std::uint8_t> truncated_flow_header{
      0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
      static_cast<std::uint8_t> (stream_codec_t::json),
      static_cast<std::uint8_t> (stream_header_flags_t::has_flow_id), 4, 'n', 'a', 'm', 'e', 'x'};
    if (runtime.decode_header (truncated_flow_header)) {
        return 221;
    }
    const std::string sample_flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
    zlink::framework::detail::stream_header_t flow_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "flowed", {});
    flow_header.with_flow (sample_flow_id, zlink::framework::flow_origin_t::inbound);
    auto flow_encoded = runtime.encode_header (flow_header);
    if (!flow_encoded || flow_encoded.value ()[0] != 0xF2) {
        return 222;
    }
    auto flow_decoded = runtime.decode_header (flow_encoded.value ());
    if (!flow_decoded || !flow_decoded.value ().flow_id ()
        || *flow_decoded.value ().flow_id () != sample_flow_id
        || flow_decoded.value ().flow_origin () != zlink::framework::flow_origin_t::inbound) {
        return 223;
    }
    if (runtime.encode_header (flow_decoded.value ())
          .value ()
          != flow_encoded.value ()) {
        return 224;
    }
    zlink::framework::detail::stream_header_t bad_flow_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "flowed", {});
    bad_flow_header.with_flow ("UPPERCASE-not-a-uuid7-value-000000000", // 37 bytes, invalid
                               zlink::framework::flow_origin_t::inbound);
    if (runtime.encode_header (bad_flow_header)) {
        return 225;
    }

    auto stream = runtime.open_session ("client-stream");
    if (stream.routing_id () || stream.local_address () || stream.remote_address ()) {
        return 226;
    }
    runtime.set_session_identity (
      stream, zlink::routing_id_t::from ("stream-rid"),
      std::string ("127.0.0.1:7101"), std::string ("127.0.0.1:48210"));
    if (!stream.routing_id () || stream.routing_id ()->to_string () != "stream-rid"
        || !stream.local_address () || *stream.local_address () != "127.0.0.1:7101"
        || !stream.remote_address () || *stream.remote_address () != "127.0.0.1:48210") {
        return 227;
    }
    sample_session_t session;
    if (!runtime.dispatch_connected (session, stream)) {
        return 7;
    }
    if (!runtime.dispatch_packet (session, stream, request_header,
                                  zlink::message_t::from (std::string ("payload")))) {
        return 8;
    }
    if (!session.last_can_reply || session.last_metadata.find ("trace") != "42"
        || session.last_metadata.find ("content_type") != "application/json") {
        return 228;
    }
    if (!runtime.dispatch_disconnected (session, stream)) {
        return 9;
    }
    const auto log = runtime.serial_log (stream);
    if (log.size () != 3 || log[0] != "connected" || log[1] != "packet:move"
        || log[2] != "disconnected") {
        return 10;
    }
    if (session.events.size () != 3 || session.events[1] != "packet:move:payload"
        || runtime.written_headers (stream).size () != 1) {
        return 11;
    }
    /* stream connector §5.2: Response는 request의 packet name을 그대로 되돌린다. */
    if (runtime.written_headers (stream)[0].kind () != stream_message_kind_t::response
        || runtime.written_headers (stream)[0].request_seq () != 77
        || !runtime.written_headers (stream)[0].packet_name ().empty ()) {
        return 15;
    }

    auto duplicate_reply_stream = runtime.open_session ("client-stream");
    duplicate_reply_session_t duplicate_reply_session;
    if (!runtime.dispatch_packet (
          duplicate_reply_session, duplicate_reply_stream, request_header,
          zlink::message_t::from (std::string ("duplicate-reply")))
        || !duplicate_reply_session.winner_completed
        || !duplicate_reply_session.loser_rejected
        || runtime.written_headers (duplicate_reply_stream).size () != 1) {
        return 236;
    }

    auto failed_reply_stream = runtime.open_session ("client-stream");
    std::size_t failed_reply_attempts = 0;
    runtime.attach_transport_writer (
      failed_reply_stream,
      [&failed_reply_attempts] (const auto &, const auto &) {
          ++failed_reply_attempts;
          return zlink::framework::result_t<void>::failure (
            framework_error_kind_t::internal_failure, "stream transport rejected reply");
      });
    failed_reply_session_t failed_reply_session;
    if (!runtime.dispatch_packet (
          failed_reply_session, failed_reply_stream, request_header,
          zlink::message_t::from (std::string ("failed-reply")))
        || !failed_reply_session.first_failed
        || !failed_reply_session.second_rejected
        || failed_reply_attempts != 1
        || !runtime.written_headers (failed_reply_stream).empty ()) {
        return 237;
    }

    auto heartbeat_stream = runtime.open_session ("client-stream");
    runtime.send_heartbeat_pong (heartbeat_stream);
    const auto heartbeat_headers = runtime.written_headers (heartbeat_stream);
    if (heartbeat_headers.size () != 1
        || heartbeat_headers[0].kind () != stream_message_kind_t::control
        || heartbeat_headers[0].codec () != stream_codec_t::raw
        || heartbeat_headers[0].request_seq ()
        || heartbeat_headers[0].packet_name () != "$zlink.heartbeat.pong") {
        return 235;
    }

    auto delayed_stream = runtime.open_session ("client-stream");
    delayed_reply_session_t delayed_session;
    std::optional<zlink::framework::result_t<void>> delayed_dispatch;
    std::thread delayed_dispatch_thread ([&] {
        delayed_dispatch = runtime.dispatch_packet (
          delayed_session, delayed_stream, request_header,
          zlink::message_t::from (std::string ("delayed-payload")));
    });
    delayed_session.wait_until_suspended ();
    delayed_session.resume ();
    delayed_dispatch_thread.join ();
    if (!delayed_dispatch || !*delayed_dispatch || !delayed_session.reply_result
        || !*delayed_session.reply_result || runtime.written_headers (delayed_stream).size () != 1
        || runtime.written_headers (delayed_stream)[0].request_seq () != 77
        || !runtime.written_headers (delayed_stream)[0].packet_name ().empty ()
        || runtime.written_payloads (delayed_stream)[0].to_string () != "delayed-payload") {
        return 29;
    }

    /* Native Core callbacks must return after enqueueing. The suspended packet
     * keeps the per-session turn, so disconnect cannot overtake it and close
     * the stream before its reply is written. */
    auto async_stream = runtime.open_session ("client-stream");
    delayed_reply_session_t async_session;
    std::promise<zlink::framework::result_t<void>> packet_completion_source;
    auto packet_completion = packet_completion_source.get_future ();
    const auto async_submitted = runtime.dispatch_packet_async (
      async_session, async_stream, request_header,
      zlink::message_t::from (std::string ("async-payload")),
      [&packet_completion_source] (const zlink::framework::result_t<void> &result) {
          packet_completion_source.set_value (result);
      });
    if (!async_submitted) {
        return 230;
    }
    async_session.wait_until_suspended ();
    if (packet_completion.wait_for (std::chrono::milliseconds::zero ())
        == std::future_status::ready) {
        return 231;
    }
    std::promise<zlink::framework::result_t<void>> disconnect_completion_source;
    auto disconnect_completion = disconnect_completion_source.get_future ();
    const auto disconnect_submitted = runtime.dispatch_disconnected_async (
      async_session, async_stream,
      [&disconnect_completion_source] (const zlink::framework::result_t<void> &result) {
          disconnect_completion_source.set_value (result);
      });
    if (!disconnect_submitted
        || disconnect_completion.wait_for (std::chrono::milliseconds::zero ())
             == std::future_status::ready) {
        return 232;
    }
    async_session.resume ();
    if (packet_completion.wait_for (std::chrono::seconds (2))
          != std::future_status::ready
        || !packet_completion.get ()
        || disconnect_completion.wait_for (std::chrono::seconds (2))
             != std::future_status::ready
        || !disconnect_completion.get ()) {
        return 233;
    }
    runtime.drain_async_dispatch (async_stream);
    if (!async_session.reply_result || !*async_session.reply_result
        || runtime.written_payloads (async_stream).size () != 1
        || runtime.written_payloads (async_stream)[0].to_string () != "async-payload") {
        return 234;
    }

    auto fluent_stream = runtime.open_session ("client-stream");
    auto send_call =
      fluent_stream.write_packet (zlink::message_t::from (std::string ("send-payload")));
    send_call.packet_name ("original");
    if (!runtime.written_headers (fluent_stream).empty ()) {
        return 17;
    }
    send_call.metadata ("trace", "send-trace")
      .packet_name ("renamed")
      .compress ()
      .submit ()
      .result ()
      .value ();
    bool duplicate_send_rejected = false;
    try {
        (void) send_call.submit ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        duplicate_send_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (runtime.written_headers (fluent_stream).size () != 1
        || !duplicate_send_rejected
        || runtime.written_headers (fluent_stream)[0].packet_name () != "renamed"
        || runtime.written_headers (fluent_stream)[0].metadata ("trace") != "send-trace"
        || (runtime.written_headers (fluent_stream)[0].flags ()
            & stream_header_flags_t::payload_compressed)
             != stream_header_flags_t::payload_compressed) {
        return 18;
    }
    if (runtime.written_payloads (fluent_stream).empty ()
        || runtime.written_payloads (fluent_stream)[0].to_string () == "send-payload") {
        return 23;
    }
    const auto close_result = fluent_stream.close ().result ();
    const auto write_rejected_disconnected = [] (auto &&write_fn) {
        try {
            write_fn ();
            return false;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            return error.kind ()
                   == zlink::framework::framework_error_kind_t::unavailable;
        }
    };
    if (!write_rejected_disconnected ([&] {
            return fluent_stream
              .write_packet (zlink::message_t::from (std::string ("after-close")))
              .submit ().result ().value ();
        })) {
        return 24;
    }
    if (!close_result || runtime.written_headers (fluent_stream).size () != 1) {
        return 19;
    }
    if (!write_rejected_disconnected ([&] {
            return stream
              .write_packet (zlink::message_t::from (std::string ("after-disconnect")))
              .submit ().result ().value ();
        })) {
        return 25;
    }
    if (runtime.written_headers (stream).size () != 1) {
        return 16;
    }

    sample_session_t validation_session;
    auto validation_stream = runtime.open_session ("client-stream");
    const auto rejected =
      runtime.dispatch_packet (validation_session, validation_stream, invalid_send,
                               zlink::message_t::from (std::string ("bad")));
    if (rejected || rejected.error_kind () != framework_error_kind_t::protocol_error
        || !validation_session.events.empty ()) {
        return 12;
    }

    throwing_packet_session_t throwing_session;
    auto throwing_stream = runtime.open_session ("client-stream");
    const auto handler_failure =
      runtime.dispatch_packet (throwing_session, throwing_stream, request_header,
                               zlink::message_t::from (std::string ("payload")));
    if (handler_failure || handler_failure.error_kind () != framework_error_kind_t::internal_failure
        || throwing_session.on_error_called) {
        return 13;
    }

    sample_session_t error_session;
    auto error_stream = runtime.open_session ("client-stream");
    const auto transport_error = runtime.dispatch_error (
      error_session, error_stream,
      zlink::framework::stream_error_t (zlink::framework::stream_session_error_t::transport_error,
                                        "transport"));
    if (!transport_error || error_session.events.size () != 1
        || error_session.events[0] != "error:transport") {
        return 14;
    }

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
    const auto mutual_tls_port = reserve_loopback_port ();
    zlink::framework::service_collection_t mutual_tls_services;
    zlink::framework::handler_registry_t mutual_tls_handlers;
    zlink::framework::serializer_registry_t mutual_tls_serializers;
    zlink::framework::zlink_builder_t mutual_tls_zlink;
    zlink::framework::zlink_framework_options_t mutual_tls_options (
      mutual_tls_services, mutual_tls_handlers, mutual_tls_serializers,
      mutual_tls_zlink);
    mutual_tls_options.add_stream_node ("mutual-tls-listener")
      .bind ("tls://127.0.0.1:" + std::to_string (mutual_tls_port))
      .set_tls_server (ZLINK_FRAMEWORK_STREAM_TEST_CERT,
                       ZLINK_FRAMEWORK_STREAM_TEST_KEY, true)
      .register_session ("mutual-tls-listener-session");
    mutual_tls_options.apply ();
    auto mutual_tls_provider = mutual_tls_services.build_provider ();
    sample_session_t mutual_tls_session;
    zlink::framework::runtime::stream_host_service_t mutual_tls_host (
      zlink::framework::detail::stream_runtime_t::from (mutual_tls_zlink),
      zlink::framework::detail::stream_runtime_t::from (mutual_tls_zlink).snapshots (),
      {{"mutual-tls-listener-session",
        [&mutual_tls_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return mutual_tls_session; }}});
    mutual_tls_host.start (mutual_tls_provider);
    boost::asio::io_context mutual_tls_io;
    boost::asio::ssl::context mutual_tls_client_context (
      boost::asio::ssl::context::tls_client);
    mutual_tls_client_context.set_verify_mode (boost::asio::ssl::verify_none);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> mutual_tls_client (
      mutual_tls_io, mutual_tls_client_context);
    boost::system::error_code mutual_tls_error;
    mutual_tls_client.next_layer ().connect (
      {boost::asio::ip::make_address ("127.0.0.1"), mutual_tls_port}, mutual_tls_error);
    if (mutual_tls_error) {
        mutual_tls_host.stop ();
        return 26;
    }
    mutual_tls_client.handshake (
      boost::asio::ssl::stream_base::client, mutual_tls_error);
    mutual_tls_host.stop ();
    // TLS 1.3 clients may observe the server's certificate-required alert only
    // on their next I/O; the server-side contract is that no Session exists.
    if (!mutual_tls_session.events.empty ()) {
        return 26;
    }
#endif

    auto custom_codec =
      std::make_shared<prefix_stream_compression_codec_t> ("custom-stream:");
    zlink::framework::service_collection_t custom_services;
    zlink::framework::handler_registry_t custom_handlers;
    zlink::framework::serializer_registry_t custom_serializers;
    zlink::framework::zlink_builder_t custom_zlink;
    zlink::framework::zlink_framework_options_t custom_options (
      custom_services, custom_handlers, custom_serializers, custom_zlink);
    custom_options.configure_stream_compression ().use (custom_codec);
    custom_options.add_stream_node ("custom-stream")
      .bind ("tcp://0.0.0.0:9201")
      .register_session ("custom-session");
    custom_options.add_stream_node ("configured-mutual-tls")
      .bind ("tls://127.0.0.1:9204")
      .set_tls_server ("server.crt", "server.key", true)
      .register_session ("configured-mutual-tls-session");
    custom_options.apply ();
    auto custom_runtime = zlink::framework::detail::stream_runtime_t::from (custom_zlink);
    const auto configured_tls = custom_runtime.snapshots ();
    const auto configured_tls_snapshot = std::find_if (
      configured_tls.begin (), configured_tls.end (), [] (const auto &candidate) {
          return candidate.name == "configured-mutual-tls";
      });
    if (configured_tls_snapshot == configured_tls.end ()
        || !configured_tls_snapshot->tls_require_client_certificate) {
        return 26;
    }
    auto custom_stream = custom_runtime.open_session ("custom-stream");
    custom_stream.write_packet (zlink::message_t::from (std::string ("custom-outbound")))
      .compress ()
      .submit ().result ().value ();
    if (custom_runtime.written_payloads (custom_stream).size () != 1
        || custom_runtime.written_payloads (custom_stream)[0].to_string ()
             != "custom-stream:custom-outbound") {
        return 24;
    }
    zlink::framework::detail::stream_header_t custom_inbound_header (
      stream_message_kind_t::request, stream_codec_t::raw,
      stream_header_flags_t::has_request_seq | stream_header_flags_t::payload_compressed, 88,
      "custom-inbound");
    sample_session_t custom_session;
    const auto custom_dispatch = custom_runtime.dispatch_packet (
      custom_session, custom_stream, custom_inbound_header,
      custom_codec->compress (zlink::message_t::from (std::string ("custom-inbound-payload"))));
    if (!custom_dispatch || custom_session.events.size () != 1
        || custom_session.events[0] != "packet:custom-inbound:custom-inbound-payload") {
        return 25;
    }

    zlink::framework::zlink_builder_t disabled_zlink;
    zlink::framework::zlink_framework_options_t disabled_options (
      custom_services, custom_handlers, custom_serializers, disabled_zlink);
    disabled_options.configure_stream_compression ().disable ();
    disabled_options.add_stream_node ("disabled-stream")
      .bind ("tcp://0.0.0.0:9202")
      .register_session ("disabled-session");
    disabled_options.apply ();
    auto disabled_runtime = zlink::framework::detail::stream_runtime_t::from (disabled_zlink);
    auto disabled_stream = disabled_runtime.open_session ("disabled-stream");
    bool disabled_compress_rejected = false;
    try {
        disabled_stream.write_packet (zlink::message_t::from (std::string ("disabled")))
          .compress ()
          .submit ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        disabled_compress_rejected = true;
    }
    if (!disabled_compress_rejected) {
        return 27;
    }
    sample_session_t disabled_session;
    const auto disabled_receive = disabled_runtime.dispatch_packet (
      disabled_session, disabled_stream, custom_inbound_header,
      custom_codec->compress (zlink::message_t::from (std::string ("disabled-inbound"))));
    if (disabled_receive
        || disabled_receive.error_kind () != framework_error_kind_t::protocol_error
        || std::string (disabled_receive.error ()->what ()).find (
             "compression codec is not configured") == std::string::npos
        || !disabled_session.events.empty ()) {
        return 27;
    }

    zlink::framework::zlink_builder_t oversized_zlink;
    zlink::framework::zlink_framework_options_t oversized_options (
      custom_services, custom_handlers, custom_serializers, oversized_zlink);
    oversized_options.configure_stream_compression ().use (
      std::make_shared<oversized_stream_compression_codec_t> ());
    oversized_options.add_stream_node ("oversized-stream")
      .bind ("tcp://0.0.0.0:9203")
      .register_session ("oversized-session");
    oversized_options.apply ();
    auto oversized_runtime = zlink::framework::detail::stream_runtime_t::from (oversized_zlink);
    auto oversized_stream = oversized_runtime.open_session ("oversized-stream");
    sample_session_t oversized_session;
    const auto oversized_receive = oversized_runtime.dispatch_packet (
      oversized_session, oversized_stream, custom_inbound_header,
      zlink::message_t::from (std::string ("compressed")));
    if (oversized_receive
        || oversized_receive.error_kind () != framework_error_kind_t::protocol_error
        || !oversized_session.events.empty ()) {
        return 28;
    }

    const auto transport_port = reserve_loopback_port ();
    const auto transport_endpoint =
      "tcp://127.0.0.1:" + std::to_string (transport_port);
    zlink::framework::service_collection_t transport_services;
    zlink::framework::handler_registry_t transport_handlers;
    zlink::framework::serializer_registry_t transport_serializers;
    zlink::framework::zlink_builder_t transport_zlink;
    transport_services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    transport_services.add_factory<zlink::framework::session_actor_manager_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::session_actor_manager_t> (
            provider.get_required<zlink::framework::detail::actor_gateway_runtime_t> ().manager ());
      },
      zlink::framework::service_lifetime_t::scoped);
    zlink::framework::zlink_framework_options_t transport_options (
      transport_services, transport_handlers, transport_serializers, transport_zlink);
    auto transport_stream_options = transport_options.add_stream_node ("transport-stream");
    transport_stream_options.configure_socket ().max_message_size = 0;
    transport_stream_options
      .bind (transport_endpoint)
      .register_session ("transport-session");
    transport_options.apply ();
    auto transport_provider = transport_services.build_provider ();
    transport_error_session_t transport_session;
    zlink::framework::runtime::stream_host_service_t transport_host (
      zlink::framework::detail::stream_runtime_t::from (transport_zlink),
      zlink::framework::detail::stream_runtime_t::from (transport_zlink).snapshots (),
      {{"transport-session",
        [&transport_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return transport_session; }}});
    transport_host.start (transport_provider);

    const int graceful_client = connect_loopback (transport_port);
    if (graceful_client < 0 || !transport_session.wait_connected (1)
        || !transport_session.identity_available ()
        || transport_session.last_local ().find (":") == std::string::npos
        || transport_session.last_remote ().find (":") == std::string::npos) {
        transport_host.stop ();
        return 29;
    }
    ::shutdown (graceful_client, SHUT_RDWR);
    ::close (graceful_client);
    if (!transport_session.wait_disconnected (1) || transport_session.errors () != 0) {
        transport_host.stop ();
        return 30;
    }

    const int failed_client = connect_loopback (transport_port);
    if (failed_client < 0 || !transport_session.wait_connected (2)) {
        transport_host.stop ();
        return 31;
    }
    linger reset_on_close{1, 0};
    (void) ::setsockopt (failed_client, SOL_SOCKET, SO_LINGER, &reset_on_close,
                         sizeof (reset_on_close));
    ::close (failed_client);
    const bool transport_failure_reported = transport_session.wait_errors (1);
    const bool transport_disconnect_reported = transport_session.wait_disconnected (2);
    if (!transport_failure_reported || !transport_disconnect_reported
        || transport_session.last_error ()
             != zlink::framework::stream_session_error_t::transport_error) {
        transport_host.stop ();
        return 32;
    }

    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = transport_endpoint;
    connector_options.connect_timeout = std::chrono::seconds (2);
    auto connector =
      zlink::stream_connector::connector_factory_t::create (connector_options);
    if (!connector.connect () || !transport_session.wait_connected (3)) {
        transport_host.stop ();
        return 36;
    }
    auto connector_reply =
      connector
        .request (zlink::stream_connector::packet_t{
          .name = "connector-probe",
          .payload = zlink::message_t::from ("connector-payload")})
        .timeout (std::chrono::seconds (2))
        .submit<zlink::message_t> ();
    if (!connector_reply
        || connector_reply.value ().to_string () != "connector-payload"
        || !transport_session.wait_packets (1) || !connector.close ()
        || !transport_session.wait_disconnected (3)) {
        transport_host.stop ();
        return 36;
    }

    /* common internals §7.6 / transport liveness §4: a connection that has
     * only part of a frame must release the listener turn while it waits for
     * more bytes. A second connection must therefore deliver its complete
     * frame before the first connection completes. */
    auto transport_runtime = zlink::framework::detail::stream_runtime_t::from (
      transport_zlink);
    const auto transport_snapshots = transport_runtime.snapshots ();
    if (transport_snapshots.size () != 1
        || transport_snapshots[0].max_message_size != 0) {
        transport_host.stop ();
        return 45;
    }
    const zlink::framework::detail::stream_header_t fairness_header (
      zlink::framework::detail::stream_message_kind_t::send,
      zlink::framework::stream_codec_t::raw,
      zlink::framework::detail::stream_header_flags_t::none,
      std::nullopt,
      "fairness-probe");
    const auto fairness_frame = make_native_stream_frame (
      transport_runtime, fairness_header, zlink::message_t::from (std::string ("fairness")));
    const int partial_client = connect_loopback (transport_port);
    if (partial_client < 0 || !transport_session.wait_connected (4)) {
        if (partial_client >= 0) {
            ::close (partial_client);
        }
        transport_host.stop ();
        return 39;
    }
    send_native_bytes (partial_client, fairness_frame, 0, 1);
    /* Leave only the first byte at the listener. The second client must not
     * wait for the rest of this frame before it can be dispatched. */
    const int ready_client = connect_loopback (transport_port);
    if (ready_client < 0 || !transport_session.wait_connected (5)) {
        ::close (partial_client);
        if (ready_client >= 0) {
            ::close (ready_client);
        }
        transport_host.stop ();
        return 40;
    }
    send_native_bytes (ready_client, fairness_frame);
    if (!transport_session.wait_packets (2)) {
        ::close (partial_client);
        ::close (ready_client);
        transport_host.stop ();
        return 41;
    }
    send_native_bytes (partial_client, fairness_frame, 1, fairness_frame.size () - 1);
    if (!transport_session.wait_packets (3)) {
        ::close (partial_client);
        ::close (ready_client);
        transport_host.stop ();
        return 42;
    }
    ::shutdown (partial_client, SHUT_RDWR);
    ::close (partial_client);
    ::shutdown (ready_client, SHUT_RDWR);
    ::close (ready_client);

    /* The receive batch is limited to 64 frames. Keep 65 complete frames in
     * one user-space read so the final frame has no new kernel readability
     * event to wake the scheduler; the buffered-ready handoff must schedule
     * it again without another client write. */
    const int buffered_client = connect_loopback (transport_port);
    if (buffered_client < 0 || !transport_session.wait_connected (6)) {
        if (buffered_client >= 0) {
            ::close (buffered_client);
        }
        transport_host.stop ();
        return 43;
    }
    const auto buffered_payload = zlink::message_t::from (std::string (512, 'x'));
    std::vector<std::uint8_t> buffered_frames;
    for (int index = 0; index < 65; ++index) {
        const auto frame = make_native_stream_frame (
          transport_runtime, fairness_header, buffered_payload);
        buffered_frames.insert (buffered_frames.end (), frame.begin (), frame.end ());
    }
    send_native_bytes (buffered_client, buffered_frames);
    if (!transport_session.wait_packets (68)) {
        ::close (buffered_client);
        transport_host.stop ();
        return 44;
    }
    ::shutdown (buffered_client, SHUT_RDWR);
    ::close (buffered_client);

    transport_host.stop ();

    const auto limited_port = reserve_loopback_port ();
    const auto limited_endpoint =
      "tcp://127.0.0.1:" + std::to_string (limited_port);
    zlink::framework::zlink_builder_t limited_zlink;
    zlink::framework::zlink_framework_options_t limited_options (
      transport_services, transport_handlers, transport_serializers, limited_zlink);
    auto limited_stream_options = limited_options.add_stream_node ("limited-stream");
    limited_stream_options.configure_socket ().max_message_size = 128;
    limited_stream_options.bind (limited_endpoint).register_session ("limited-session");
    limited_options.apply ();
    auto limited_runtime = zlink::framework::detail::stream_runtime_t::from (limited_zlink);
    const auto limited_snapshots = limited_runtime.snapshots ();
    if (limited_snapshots.size () != 1
        || limited_snapshots[0].max_message_size != 128) {
        return 46;
    }
    transport_error_session_t limited_session;
    zlink::framework::runtime::stream_host_service_t limited_host (
      limited_runtime, limited_snapshots,
      {{"limited-session",
        [&limited_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return limited_session; }}});
    limited_host.start (transport_provider);
    const int limited_client = connect_loopback (limited_port);
    if (limited_client < 0 || !limited_session.wait_connected (1)) {
        if (limited_client >= 0) {
            ::close (limited_client);
        }
        limited_host.stop ();
        return 47;
    }
    const zlink::framework::detail::stream_header_t limited_header (
      zlink::framework::detail::stream_message_kind_t::send,
      zlink::framework::stream_codec_t::raw,
      zlink::framework::detail::stream_header_flags_t::none,
      std::nullopt,
      "limited-probe");
    const auto limited_frame = make_native_stream_frame (
      limited_runtime, limited_header, zlink::message_t::from (std::string (512, 'x')));
    send_native_bytes (limited_client, limited_frame);
    const bool limited_disconnected = limited_session.wait_disconnected (1);
    ::shutdown (limited_client, SHUT_RDWR);
    ::close (limited_client);
    limited_host.stop ();
    if (!limited_disconnected || limited_session.packets () != 0) {
        return 48;
    }

    const auto rejected_port = reserve_loopback_port ();
    const auto rejected_endpoint =
      "tcp://127.0.0.1:" + std::to_string (rejected_port);
    zlink::framework::zlink_builder_t rejected_zlink;
    zlink::framework::zlink_framework_options_t rejected_options (
      transport_services, transport_handlers, transport_serializers, rejected_zlink);
    rejected_options.add_stream_node ("rejected-stream")
      .bind (rejected_endpoint)
      .register_session ("rejected-session");
    rejected_options.apply ();
    rejected_connected_session_t rejected_session;
    zlink::framework::runtime::stream_host_service_t rejected_host (
      zlink::framework::detail::stream_runtime_t::from (rejected_zlink),
      zlink::framework::detail::stream_runtime_t::from (rejected_zlink).snapshots (),
      {{"rejected-session",
        [&rejected_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return rejected_session; }}});
    rejected_host.start (transport_provider);
    const int rejected_client = connect_loopback (rejected_port);
    if (rejected_client < 0 || !rejected_session.wait_until_actor_manager_is_detached ()) {
        if (rejected_client >= 0) {
            ::close (rejected_client);
        }
        rejected_host.stop ();
        return 37;
    }
    ::close (rejected_client);
    rejected_host.stop ();

    /* A Core STREAM listener owns a poller that is stopped from the host
     * lifecycle thread. Closing the Core socket is not itself a portable
     * cross-thread poller wake-up, so stop must still join the listener within
     * the bounded poll interval. */
    const auto core_mesh_port = reserve_loopback_port ();
    const auto core_stream_port = reserve_loopback_port ();
    zlink::framework::service_collection_t core_services;
    zlink::framework::handler_registry_t core_handlers;
    zlink::framework::serializer_registry_t core_serializers;
    zlink::framework::zlink_builder_t core_zlink;
    zlink::framework::zlink_framework_options_t core_options (
      core_services, core_handlers, core_serializers, core_zlink);
    core_options.add_route_mesh ("core-stream-mesh")
      .set_routing_id (zlink::routing_id_t::from ("core-stream-node"))
      .listen ("tcp://127.0.0.1:" + std::to_string (core_mesh_port));
    core_options.add_stream_node ("core-stream")
      .bind ("tcp://127.0.0.1:" + std::to_string (core_stream_port))
      .register_session ("core-session");
    core_options.apply ();
    auto core_provider = core_services.build_provider ();
    auto core_mesh = zlink::framework::detail::mesh_node_runtime_t::from (
      core_zlink, "core-stream-mesh");
    if (!core_mesh) {
        return 38;
    }
    core_mesh->bind_serializers (core_serializers);
    core_mesh->start ();
    auto core_stream_runtime =
      zlink::framework::detail::stream_runtime_t::from (core_zlink);
    sample_session_t core_session;
    zlink::framework::runtime::stream_host_service_t core_host (
      core_stream_runtime, core_stream_runtime.snapshots (),
      {{"core-session",
        [&core_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return core_session; }}},
      core_mesh);
    core_host.start (core_provider);
    const auto core_stop_started = std::chrono::steady_clock::now ();
    core_host.stop ();
    const auto core_stop_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now () - core_stop_started);
    core_mesh->stop ();
    if (core_stop_elapsed > std::chrono::seconds (2)) {
        return 38;
    }
    return 0;
}
