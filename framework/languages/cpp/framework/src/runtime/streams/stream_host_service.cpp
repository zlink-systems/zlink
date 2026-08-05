/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/streams/stream_host_service.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/messaging/async_submit_runtime.hpp"

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"

#include <nlohmann/json.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#endif

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <unordered_set>
#include <vector>

namespace zlink::framework::runtime
{
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using websocket_stream_t = websocket::stream<tcp::socket>;
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
namespace ssl = asio::ssl;
#endif
using detail::stream_header_flags_t;
using detail::stream_header_t;
using detail::stream_message_kind_t;

namespace
{

class session_actor_disconnect_guard_t
{
  public:
    explicit session_actor_disconnect_guard_t (session_actor_manager_t &actors) :
        _actors (&actors)
    {
    }

    ~session_actor_disconnect_guard_t ()
    {
        detail::session_actor_manager_access_t::disconnect (*_actors);
    }

    session_actor_disconnect_guard_t (const session_actor_disconnect_guard_t &) = delete;
    session_actor_disconnect_guard_t &operator= (const session_actor_disconnect_guard_t &) = delete;

  private:
    session_actor_manager_t *_actors;
};

struct parsed_tcp_endpoint_t
{
    std::string host;
    std::string port;
};

class session_transport_failure_t final : public std::runtime_error
{
  public:
    session_transport_failure_t (int native_code, std::string message) :
        std::runtime_error (std::move (message)), _native_code (native_code)
    {
    }

    int native_code () const noexcept { return _native_code; }

  private:
    int _native_code;
};

/* One listener-wide readiness scheduler owns the receive cursor for all
 * externally accepted STREAM connections. Native workers keep incomplete
 * frames in a per-connection buffer and release the lease before waiting for
 * more bytes, so one partial frame cannot block another connection. */
class stream_receive_scheduler_t final
{
    struct slot_t
    {
        slot_t (std::uint64_t sequence_, int fd_) : sequence (sequence_), fd (fd_) {}

        std::uint64_t sequence;
        int fd;
        bool granted = false;
        bool closed = false;
        bool buffered_ready = false;
        std::condition_variable changed;
    };

  public:
    class lease_t
    {
      public:
        lease_t () noexcept = default;

        lease_t (stream_receive_scheduler_t *owner, std::shared_ptr<slot_t> slot) :
            _owner (owner),
            _slot (std::move (slot))
        {
        }

        ~lease_t () { reset (); }

        lease_t (const lease_t &) = delete;
        lease_t &operator= (const lease_t &) = delete;

        lease_t (lease_t &&other) noexcept :
            _owner (std::exchange (other._owner, nullptr)),
            _slot (std::move (other._slot))
        {
        }

        lease_t &operator= (lease_t &&other) noexcept
        {
            if (this != &other) {
                reset ();
                _owner = std::exchange (other._owner, nullptr);
                _slot = std::move (other._slot);
            }
            return *this;
        }

        explicit operator bool () const noexcept
        {
            return _owner != nullptr && _slot != nullptr;
        }

        bool wait_turn (const std::function<bool ()> &stop_requested) const
        {
            return _owner != nullptr && _slot != nullptr
                   && _owner->wait_for_turn (_slot, stop_requested);
        }

        void release_turn () const noexcept
        {
            if (_owner != nullptr && _slot != nullptr) {
                _owner->release_turn (_slot);
            }
        }

        void set_buffered_ready (bool ready) const noexcept
        {
            if (_owner != nullptr && _slot != nullptr) {
                _owner->set_buffered_ready (_slot, ready);
            }
        }

        void reset () noexcept
        {
            if (_owner != nullptr && _slot != nullptr) {
                _owner->unregister_slot (_slot);
            }
            _owner = nullptr;
            _slot.reset ();
        }

      private:
        stream_receive_scheduler_t *_owner = nullptr;
        std::shared_ptr<slot_t> _slot;
    };

    stream_receive_scheduler_t ()
    {
        if (::pipe (_wake_fds) != 0) {
            throw std::runtime_error ("STREAM receive scheduler wake pipe creation failed");
        }
        set_nonblocking (_wake_fds[0]);
        set_nonblocking (_wake_fds[1]);
    }

    ~stream_receive_scheduler_t ()
    {
        stop ();
        if (_wake_fds[0] >= 0) {
            ::close (_wake_fds[0]);
        }
        if (_wake_fds[1] >= 0) {
            ::close (_wake_fds[1]);
        }
    }

    stream_receive_scheduler_t (const stream_receive_scheduler_t &) = delete;
    stream_receive_scheduler_t &operator= (const stream_receive_scheduler_t &) = delete;

    void start ()
    {
        std::lock_guard lock (_mutex);
        if (_thread.joinable ()) {
            return;
        }
        _stopped = false;
        _thread = std::thread ([this] { run (); });
    }

    lease_t register_connection (int fd)
    {
        if (fd < 0) {
            return {};
        }
        std::shared_ptr<slot_t> slot;
        {
            std::lock_guard lock (_mutex);
            if (_stopped) {
                return {};
            }
            slot = std::make_shared<slot_t> (_next_sequence++, fd);
            _slots.push_back (slot);
        }
        wake ();
        return lease_t (this, std::move (slot));
    }

    void request_stop () noexcept
    {
        {
            std::lock_guard lock (_mutex);
            _stopped = true;
            for (const auto &slot : _slots) {
                slot->closed = true;
                slot->changed.notify_all ();
            }
        }
        wake ();
    }

    void stop () noexcept
    {
        request_stop ();
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

  private:
    static void set_nonblocking (int fd) noexcept
    {
        const int flags = ::fcntl (fd, F_GETFL, 0);
        if (flags >= 0) {
            (void) ::fcntl (fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    void wake () noexcept
    {
        if (_wake_fds[1] < 0) {
            return;
        }
        const std::uint8_t marker = 1;
        const auto written = ::write (_wake_fds[1], &marker, sizeof (marker));
        (void) written;
    }

    void drain_wake () noexcept
    {
        std::uint8_t markers[64];
        while (::read (_wake_fds[0], markers, sizeof (markers)) > 0) {
        }
    }

    bool wait_for_turn (const std::shared_ptr<slot_t> &slot,
                        const std::function<bool ()> &stop_requested) const
    {
        std::unique_lock lock (_mutex);
        slot->changed.wait (lock, [&] {
            return slot->granted || slot->closed || _stopped
                   || (stop_requested && stop_requested ());
        });
        return slot->granted && !slot->closed && !_stopped
               && !(stop_requested && stop_requested ());
    }

    void release_turn (const std::shared_ptr<slot_t> &slot) noexcept
    {
        {
            std::lock_guard lock (_mutex);
            if (slot->closed) {
                return;
            }
            slot->granted = false;
        }
        wake ();
    }

    void unregister_slot (const std::shared_ptr<slot_t> &slot) noexcept
    {
        {
            std::lock_guard lock (_mutex);
            slot->closed = true;
            slot->granted = false;
            slot->changed.notify_all ();
            _slots.erase (std::remove (_slots.begin (), _slots.end (), slot), _slots.end ());
        }
        wake ();
    }

    void set_buffered_ready (const std::shared_ptr<slot_t> &slot, bool ready) noexcept
    {
        {
            std::lock_guard lock (_mutex);
            if (slot->closed) {
                return;
            }
            slot->buffered_ready = ready;
        }
        wake ();
    }

    void run () noexcept
    {
        for (;;) {
            std::vector<std::shared_ptr<slot_t>> candidates;
            std::vector<pollfd> poll_fds;
            bool has_buffered_ready = false;
            {
                std::lock_guard lock (_mutex);
                if (_stopped) {
                    break;
                }
                poll_fds.push_back (pollfd{_wake_fds[0], POLLIN, 0});
                for (const auto &slot : _slots) {
                    if (!slot->closed && !slot->granted && slot->fd >= 0) {
                        candidates.push_back (slot);
                        poll_fds.push_back (pollfd{slot->fd, POLLIN, 0});
                        has_buffered_ready = has_buffered_ready || slot->buffered_ready;
                    }
                }
            }

            /* Keep kernel readiness in the same candidate snapshot as
             * user-space buffered frames. A buffered frame must not suppress
             * a newly readable connection from the round-robin set. */
            const int ready = ::poll (
              poll_fds.data (), poll_fds.size (), has_buffered_ready ? 0 : -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (poll_fds[0].revents != 0) {
                drain_wake ();
            }

            if (ready <= 0 && !has_buffered_ready) {
                continue;
            }

            std::shared_ptr<slot_t> selected;
            {
                std::lock_guard lock (_mutex);
                if (_stopped) {
                    break;
                }
                std::size_t start = 0;
                while (start < candidates.size ()
                       && candidates[start]->sequence < _cursor_sequence) {
                    ++start;
                }
                if (start == candidates.size ()) {
                    start = 0;
                }
                for (std::size_t offset = 0; offset < candidates.size (); ++offset) {
                    const auto index = (start + offset) % candidates.size ();
                    const auto &slot = candidates[index];
                    if ((!slot->buffered_ready && poll_fds[index + 1].revents == 0)
                        || slot->closed || slot->granted) {
                        continue;
                    }
                    selected = slot;
                    _cursor_sequence = selected->sequence + 1;
                    break;
                }
                if (selected) {
                    selected->granted = true;
                    selected->changed.notify_one ();
                }
            }
        }

        std::lock_guard lock (_mutex);
        for (const auto &slot : _slots) {
            slot->closed = true;
            slot->granted = false;
            slot->changed.notify_all ();
        }
    }

    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<slot_t>> _slots;
    std::uint64_t _next_sequence = 1;
    std::uint64_t _cursor_sequence = 1;
    int _wake_fds[2]{-1, -1};
    bool _stopped = false;
    std::thread _thread;
};

int stream_native_handle (tcp::socket &socket) noexcept
{
    return socket.native_handle ();
}

template <typename TStream>
int stream_native_handle (TStream &stream) noexcept
{
    return stream.next_layer ().native_handle ();
}

bool is_expected_session_disconnect (const boost::system::error_code &error)
{
    return error == asio::error::eof || error == asio::error::operation_aborted
           || error == websocket::error::closed;
}

parsed_tcp_endpoint_t parse_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM host currently supports tcp endpoints only");
    }
    const auto host_start = prefix.size ();
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= host_start
        || separator + 1 >= endpoint.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM tcp endpoint must be tcp://host:port");
    }
    return {endpoint.substr (host_start, separator - host_start), endpoint.substr (separator + 1)};
}

parsed_tcp_endpoint_t parse_tls_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tls://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM TLS host requires tls://host:port endpoint");
    }
    const auto host_start = prefix.size ();
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= host_start
        || separator + 1 >= endpoint.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM tls endpoint must be tls://host:port");
    }
    return {endpoint.substr (host_start, separator - host_start), endpoint.substr (separator + 1)};
}

parsed_tcp_endpoint_t parse_websocket_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "ws://";
    if (endpoint.rfind (prefix, 0) != 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM WebSocket host requires ws://host:port endpoint");
    }
    const auto authority_start = prefix.size ();
    const auto path = endpoint.find ('/', authority_start);
    const auto authority = endpoint.substr (
      authority_start, path == std::string::npos ? std::string::npos : path - authority_start);
    const auto separator = authority.rfind (':');
    if (separator == std::string::npos || separator == 0
        || separator + 1 >= authority.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM WebSocket endpoint must be ws://host:port[/path]");
    }
    return {authority.substr (0, separator), authority.substr (separator + 1)};
}

bool stream_uses_websocket (const stream_snapshot_t &stream)
{
    return stream.bind_endpoint.rfind ("ws://", 0) == 0;
}

bool stream_uses_tls (const stream_snapshot_t &stream)
{
    return !stream.tls_certificate_file.empty () || !stream.tls_private_key_file.empty ()
           || stream.bind_endpoint.rfind ("tls://", 0) == 0;
}

parsed_tcp_endpoint_t parse_stream_endpoint (const stream_snapshot_t &stream)
{
    if (stream_uses_tls (stream)) {
        return parse_tls_endpoint (stream.bind_endpoint);
    }
    if (stream_uses_websocket (stream)) {
        return parse_websocket_endpoint (stream.bind_endpoint);
    }
    return parse_tcp_endpoint (stream.bind_endpoint);
}

std::optional<std::string> socket_endpoint_text (const tcp::endpoint &endpoint)
{
    boost::system::error_code address_error;
    const auto address = endpoint.address ().to_string (address_error);
    if (address_error) {
        return std::nullopt;
    }
    return address + ":" + std::to_string (endpoint.port ());
}

std::optional<std::string> local_endpoint_text (tcp::socket &socket)
{
    boost::system::error_code error;
    const auto endpoint = socket.local_endpoint (error);
    return error ? std::nullopt : socket_endpoint_text (endpoint);
}

std::optional<std::string> remote_endpoint_text (tcp::socket &socket)
{
    boost::system::error_code error;
    const auto endpoint = socket.remote_endpoint (error);
    return error ? std::nullopt : socket_endpoint_text (endpoint);
}

std::optional<std::string> native_endpoint_text (const sockaddr_storage &address)
{
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const auto *sockaddr = reinterpret_cast<const struct sockaddr *> (&address);
    const auto length = address.ss_family == AF_INET
                          ? sizeof (sockaddr_in)
                          : sizeof (sockaddr_in6);
    if (::getnameinfo (sockaddr, length, host, sizeof (host), service, sizeof (service),
                       NI_NUMERICHOST | NI_NUMERICSERV)
        != 0) {
        return std::nullopt;
    }
    return std::string (host) + ":" + service;
}

std::optional<std::string> native_local_endpoint_text (int fd)
{
    sockaddr_storage address{};
    socklen_t length = sizeof (address);
    if (::getsockname (fd, reinterpret_cast<sockaddr *> (&address), &length) != 0) {
        return std::nullopt;
    }
    return native_endpoint_text (address);
}

std::optional<std::string> native_remote_endpoint_text (int fd)
{
    sockaddr_storage address{};
    socklen_t length = sizeof (address);
    if (::getpeername (fd, reinterpret_cast<sockaddr *> (&address), &length) != 0) {
        return std::nullopt;
    }
    return native_endpoint_text (address);
}

template <typename TStream> tcp::socket &tcp_socket (TStream &stream)
{
    if constexpr (std::is_same_v<TStream, tcp::socket>) {
        return stream;
    } else {
        return stream.next_layer ();
    }
}

bool stream_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_STREAM_TRACE");
    return value != nullptr && std::string_view (value) != "0" && std::string_view (value) != "";
}

const char *stream_kind_name (stream_message_kind_t kind)
{
    switch (kind) {
    case stream_message_kind_t::send:
        return "send";
    case stream_message_kind_t::request:
        return "request";
    case stream_message_kind_t::response:
        return "response";
    case stream_message_kind_t::error:
        return "error";
    case stream_message_kind_t::control:
        return "control";
    }
    return "unknown";
}

/* The stream error payload uses the stable public framework error name. */
const char *stream_error_code (framework_error_kind_t kind)
{
    switch (kind) {
    case framework_error_kind_t::not_found:
        return "not_found";
    case framework_error_kind_t::already_exists:
        return "already_exists";
    case framework_error_kind_t::type_mismatch:
        return "type_mismatch";
    case framework_error_kind_t::not_configured:
        return "not_configured";
    case framework_error_kind_t::rejected:
        return "rejected";
    case framework_error_kind_t::unavailable:
        return "unavailable";
    case framework_error_kind_t::capacity_exceeded:
        return "capacity_exceeded";
    case framework_error_kind_t::deadline_exceeded:
        return "deadline_exceeded";
    case framework_error_kind_t::shutting_down:
        return "shutting_down";
    case framework_error_kind_t::protocol_error:
        return "protocol_error";
    case framework_error_kind_t::invalid_operation:
        return "invalid_operation";
    case framework_error_kind_t::data_lost:
        return "data_lost";
    case framework_error_kind_t::internal_failure:
        return "internal_failure";
    }
    return "internal_failure";
}

zlink::message_t stream_error_payload (const result_t<void> &error)
{
    const auto *failure = error.error ();
    nlohmann::json payload;
    payload["code"] = failure ? stream_error_code (failure->kind ()) : "internal_failure";
    payload["message"] = failure ? failure->what () : "STREAM request failed";
    return zlink::message_t::from (payload.dump ());
}

void trace_stream_host (std::string_view stage,
                        const stream_snapshot_t &stream,
                        std::optional<stream_header_t> header = std::nullopt,
                        std::string_view detail = {})
{
    if (!stream_trace_enabled ()) {
        return;
    }
    std::cerr << "zlink-cpp-stream-trace side=server stage=" << stage
              << " stream=" << stream.name << " endpoint=" << stream.bind_endpoint;
    if (header) {
        std::cerr << " seq="
                  << (header->request_seq () ? std::to_string (*header->request_seq ()) : "-")
                  << " name=" << header->packet_name ()
                  << " kind=" << stream_kind_name (header->kind ());
    }
    if (!detail.empty ()) {
        std::cerr << " " << detail;
    }
    std::cerr << std::endl;
}

template <typename TStream> std::vector<std::uint8_t> read_exact (TStream &socket, std::size_t size)
{
    std::vector<std::uint8_t> bytes (size);
    std::size_t offset = 0;
    while (offset < bytes.size ()) {
        boost::system::error_code error;
        const auto read =
          socket.read_some (asio::buffer (bytes.data () + offset, bytes.size () - offset), error);
        if (error) {
            throw boost::system::system_error (error);
        }
        offset += read;
    }
    return bytes;
}

std::vector<std::uint8_t> message_bytes (const zlink::message_t &message)
{
    return message.to_bytes ();
}

zlink::message_t message_from_bytes (const std::vector<std::uint8_t> &bytes)
{
    return zlink::message_t::from (bytes);
}

bool stream_frame_exceeds_limit (std::size_t header_size,
                                 std::size_t payload_size,
                                 std::int64_t max_message_size) noexcept
{
    if (max_message_size <= 0) {
        return false;
    }
    const auto limit = static_cast<std::uint64_t> (max_message_size);
    if (header_size > limit) {
        return true;
    }
    return payload_size > limit - static_cast<std::uint64_t> (header_size);
}

class stream_message_size_exceeded_t final : public std::runtime_error
{
  public:
    stream_message_size_exceeded_t (std::int64_t max_message_size,
                                    std::size_t header_size,
                                    std::size_t payload_size) :
        std::runtime_error ("STREAM frame exceeds the configured message size"),
        _max_message_size (max_message_size),
        _header_size (header_size),
        _payload_size (payload_size)
    {
    }

    std::int64_t max_message_size () const noexcept { return _max_message_size; }
    std::size_t header_size () const noexcept { return _header_size; }
    std::size_t payload_size () const noexcept { return _payload_size; }

  private:
    std::int64_t _max_message_size;
    std::size_t _header_size;
    std::size_t _payload_size;
};

void validate_stream_frame_size (std::size_t header_size,
                                 std::size_t payload_size,
                                 std::int64_t max_message_size)
{
    if (header_size > std::numeric_limits<std::size_t>::max () - payload_size
        || 6u > std::numeric_limits<std::size_t>::max () - header_size - payload_size) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM frame size overflows the local size type");
    }
    if (stream_frame_exceeds_limit (header_size, payload_size, max_message_size)) {
        throw stream_message_size_exceeded_t (max_message_size, header_size, payload_size);
    }
}

struct raw_core_stream_frame_t
{
    std::vector<std::uint8_t> header;
    zlink::message_t payload;
};

class malformed_core_stream_frame_t final : public std::runtime_error
{
  public:
    explicit malformed_core_stream_frame_t (const char *message) : std::runtime_error (message) {}
};

class core_stream_frame_assembler_t
{
  public:
    explicit core_stream_frame_assembler_t (std::int64_t max_message_size) :
        _max_message_size (max_message_size)
    {
    }

    struct frame_view_t
    {
        std::size_t header_size = 0;
        std::size_t payload_size = 0;
        std::size_t frame_size = 0;
        std::span<const std::uint8_t> header;
    };

    std::optional<frame_view_t> peek () const
    {
        if (_bytes.size () < 6) {
            return std::nullopt;
        }

        const auto header_size = (static_cast<std::size_t> (_bytes[0]) << 8)
                                 | static_cast<std::size_t> (_bytes[1]);
        const auto payload_size = (static_cast<std::size_t> (_bytes[2]) << 24)
                                  | (static_cast<std::size_t> (_bytes[3]) << 16)
                                  | (static_cast<std::size_t> (_bytes[4]) << 8)
                                  | static_cast<std::size_t> (_bytes[5]);
        validate_stream_frame_size (header_size, payload_size, _max_message_size);
        if (header_size > std::numeric_limits<std::size_t>::max () - payload_size
            || 6u > std::numeric_limits<std::size_t>::max () - header_size - payload_size) {
            throw malformed_core_stream_frame_t ("STREAM frame size overflows the local size type");
        }
        const auto frame_size = 6u + header_size + payload_size;
        if (_bytes.size () < frame_size) {
            return std::nullopt;
        }
        return frame_view_t{
          header_size,
          payload_size,
          frame_size,
          std::span<const std::uint8_t> (_bytes.data () + 6, header_size)};
    }

    void append (std::span<const std::byte> part)
    {
        if (part.empty ()) {
            return;
        }
        if (_bytes.size () > std::numeric_limits<std::size_t>::max () - part.size ()) {
            throw malformed_core_stream_frame_t (
              "STREAM receive buffer size overflows the local size type");
        }
        const auto *begin = reinterpret_cast<const std::uint8_t *> (part.data ());
        _bytes.insert (_bytes.end (), begin, begin + part.size ());
    }

    std::optional<raw_core_stream_frame_t> next ()
    {
        const auto view = peek ();
        if (!view) {
            return std::nullopt;
        }

        std::vector<std::uint8_t> header (view->header.begin (), view->header.end ());
        const auto payload_begin = reinterpret_cast<const std::byte *> (
          _bytes.data () + 6 + view->header_size);
        auto payload = zlink::message_t::from (
          std::span<const std::byte> (payload_begin, view->payload_size));
        _bytes.erase (_bytes.begin (), _bytes.begin () + view->frame_size);
        return raw_core_stream_frame_t{std::move (header), std::move (payload)};
    }

    void reset () noexcept { _bytes.clear (); }

  private:
    std::int64_t _max_message_size;
    std::vector<std::uint8_t> _bytes;
};

} // namespace

class stream_host_service_t::listener_t
{
  public:
    listener_t (detail::stream_runtime_t runtime,
                stream_snapshot_t stream,
                detail::stream_session_factory_t session_factory,
                service_provider_t &services,
                std::atomic_bool &stop,
                std::shared_ptr<std::atomic_bool> drain_flag,
                std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring,
                std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
                std::shared_ptr<inbound_dispatch_budget_t> inbound_budget) :
        _runtime (std::move (runtime)),
        _stream (std::move (stream)),
        _session_factory (std::move (session_factory)),
        _services (&services),
        _stop (&stop),
        _drain_flag (std::move (drain_flag)),
        _monitoring (std::move (monitoring)),
        _mesh_node (std::move (mesh_node)),
        _inbound_budget (std::move (inbound_budget)),
        _acceptor (_io)
    {
    }

    /* Server liveness policy (graceful-drain-handoff §7.2): fixed 1s ping /
     * 5s pong / 30s application idle — the owning service drives ONE sweep
     * loop per node across all listeners, no per-session timers. Control
     * packets never refresh the application idle clock; a same-cycle double
     * expiry resolves to heartbeat_timeout. */
    struct session_liveness_t
    {
        std::mutex gate;
        std::chrono::steady_clock::time_point last_application_inbound =
          std::chrono::steady_clock::now ();
        std::chrono::steady_clock::time_point last_ping = std::chrono::steady_clock::now ();
        bool heartbeat_outstanding = false;
        bool terminated = false;
        std::optional<stream_close_reason_t> forced_reason;

        enum class decision_t
        {
            none,
            send_heartbeat,
            idle_timeout,
            heartbeat_timeout
        };

        void record_application_inbound ()
        {
            const std::lock_guard<std::mutex> lock (gate);
            last_application_inbound = std::chrono::steady_clock::now ();
        }

        void record_pong ()
        {
            const std::lock_guard<std::mutex> lock (gate);
            heartbeat_outstanding = false;
        }

        void record_ping ()
        {
            const std::lock_guard<std::mutex> lock (gate);
            last_ping = std::chrono::steady_clock::now ();
            heartbeat_outstanding = true;
        }

        decision_t evaluate ()
        {
            const std::lock_guard<std::mutex> lock (gate);
            if (terminated) {
                return decision_t::none;
            }
            const auto now = std::chrono::steady_clock::now ();
            if (heartbeat_outstanding && now - last_ping >= std::chrono::seconds (5)) {
                return decision_t::heartbeat_timeout;
            }
            if (now - last_application_inbound >= std::chrono::seconds (30)) {
                return decision_t::idle_timeout;
            }
            if (!heartbeat_outstanding && now - last_ping >= std::chrono::seconds (1)) {
                return decision_t::send_heartbeat;
            }
            return decision_t::none;
        }

        // The terminal close runs once even when sweeps race the reader exit.
        bool try_terminate (stream_close_reason_t reason)
        {
            const std::lock_guard<std::mutex> lock (gate);
            if (terminated) {
                return false;
            }
            terminated = true;
            forced_reason = reason;
            return true;
        }

        std::optional<stream_close_reason_t> forced ()
        {
            const std::lock_guard<std::mutex> lock (gate);
            return forced_reason;
        }
    };

    struct active_session_t
    {
        stream_t stream;
        std::shared_ptr<session_liveness_t> liveness;
        std::function<void ()> force_close;
    };

    /* Async packet dispatch may outlive the socket reader. Keep the service
     * scope, session object and Actor manager together until the serial
     * dispatch queue has completed its last callback. */
    struct stream_connection_state_t
    {
        stream_connection_state_t (detail::service_scope_t scope,
                                   packet_stream_session_t *session,
                                   stream_t stream,
                                   session_actor_manager_t *actors) :
            scope (std::move (scope)),
            session (session),
            stream (std::move (stream)),
            actors (actors)
        {
        }

        ~stream_connection_state_t ()
        {
            if (actors != nullptr) {
                detail::session_actor_manager_access_t::disconnect (*actors);
            }
            scope.close ();
        }

        stream_connection_state_t (const stream_connection_state_t &) = delete;
        stream_connection_state_t &operator= (const stream_connection_state_t &) = delete;

        detail::service_scope_t scope;
        packet_stream_session_t *session = nullptr;
        stream_t stream;
        session_actor_manager_t *actors = nullptr;
    };

    /* zlink.stream.connections.* (runtime-metrics §4.1): session accept and
     * close on the server edge; reconnect attempts belong to the connector. */
    void register_active_stream (const stream_t &stream,
                                 std::shared_ptr<session_liveness_t> liveness,
                                 std::function<void ()> force_close)
    {
        const std::lock_guard<std::mutex> lock (_active_streams_mutex);
        _active_streams.push_back (
          active_session_t{stream, std::move (liveness), std::move (force_close)});
    }

    void unregister_active_stream (const stream_t &stream)
    {
        const std::lock_guard<std::mutex> lock (_active_streams_mutex);
        for (auto it = _active_streams.begin (); it != _active_streams.end (); ++it) {
            if (it->stream.session_id () == stream.session_id ()) {
                _active_streams.erase (it);
                break;
            }
        }
    }

    std::size_t active_session_count ()
    {
        std::scoped_lock lock (_active_streams_mutex, _core_sessions_mutex);
        return _active_streams.size () + _core_sessions.size ();
    }

    void begin_drain_sessions ()
    {
        force_close_sessions (stream_close_reason_t::server_drain, "server drain");
        close_core_sessions ("server_drain", stream_close_reason_t::server_drain,
                             "server drain");
    }

    void notify_sessions_closing (stream_close_reason_t reason, std::string_view diagnostic)
    {
        std::vector<active_session_t> sessions;
        {
            const std::lock_guard<std::mutex> lock (_active_streams_mutex);
            sessions = _active_streams;
        }
        for (auto &entry : sessions) {
            _runtime.send_session_closing (entry.stream, reason, diagnostic);
        }
    }

    /* Forced teardown (graceful-drain-handoff §7): the notified sessions are
     * closed so the reason the client keeps is the forced one — a lingering
     * connection would later be re-labeled by the liveness loop. */
    void force_close_sessions (stream_close_reason_t reason, std::string_view diagnostic)
    {
        std::vector<active_session_t> sessions;
        {
            const std::lock_guard<std::mutex> lock (_active_streams_mutex);
            sessions = _active_streams;
        }
        for (auto &entry : sessions) {
            terminate_session (entry, reason, diagnostic);
        }
    }

    void terminate_session (active_session_t &entry,
                            stream_close_reason_t reason,
                            std::string_view diagnostic)
    {
        if (!entry.liveness->try_terminate (reason)) {
            return;
        }
        _runtime.send_session_closing (entry.stream, reason, diagnostic);
        if (entry.force_close) {
            entry.force_close ();
        }
    }

    /* One liveness pass over this listener's sessions; the owning service
     * drives all listeners from a single per-node sweep loop (§7.2: no
     * per-session timers, one loop per node). */
    void sweep_liveness_once ()
    {
        std::vector<active_session_t> sessions;
        {
            const std::lock_guard<std::mutex> lock (_active_streams_mutex);
            sessions = _active_streams;
        }
        for (auto &entry : sessions) {
            switch (entry.liveness->evaluate ()) {
                case session_liveness_t::decision_t::none:
                    break;
                case session_liveness_t::decision_t::send_heartbeat:
                    /* Stamp before writing: a same-instant pong then clears
                     * the outstanding flag instead of racing the stamp and
                     * being treated as stale. A failed write leaves the stamp
                     * in place — the transport is broken and the 5s pong
                     * timeout closes the session. */
                    entry.liveness->record_ping ();
                    _runtime.send_heartbeat_ping (entry.stream);
                    break;
                case session_liveness_t::decision_t::idle_timeout:
                    terminate_session (entry, stream_close_reason_t::idle_timeout,
                                       "application idle timeout");
                    break;
                case session_liveness_t::decision_t::heartbeat_timeout:
                    terminate_session (entry, stream_close_reason_t::heartbeat_timeout,
                                       "heartbeat pong timeout");
                    break;
            }
        }
    }

    static const char *liveness_close_label (stream_close_reason_t reason) noexcept
    {
        switch (reason) {
            case stream_close_reason_t::idle_timeout:
                return "idle_timeout";
            case stream_close_reason_t::heartbeat_timeout:
                return "heartbeat_timeout";
            case stream_close_reason_t::server_drain:
                return "server_drain";
            case stream_close_reason_t::protocol_error:
                return "protocol_error";
            default:
                return "transport_error";
        }
    }

    void record_connection_opened () const
    {
        framework::runtime::runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled ()) {
            metrics.counter ("zlink.stream.connections.opened", "{connection}", 1);
            metrics.updown ("zlink.stream.connections.active", "{connection}", 1);
        }
    }

    void record_connection_closed (const char *close_reason) const
    {
        framework::runtime::runtime_metrics_t metrics (_monitoring);
        if (metrics.enabled ()) {
            metrics.counter ("zlink.stream.connections.closed", "{connection}", 1,
                             {{"close_reason", close_reason}});
            metrics.updown ("zlink.stream.connections.active", "{connection}", -1);
        }
    }

    bool draining () const noexcept
    {
        return _drain_flag && _drain_flag->load (std::memory_order_acquire);
    }

    /* flow-correlation §4.3: session dispatch failures surface as error
     * events carrying the inbound flow pair, so one grep catches the
     * healthy and failing lines of the same flow. */
    void report_packet_dispatch_error (const detail::stream_header_t &header,
                                       const result_t<void> &dispatched) const
    {
        message_dispatch_error_event_t event{
          dispatch_error_surface_t::stream_session,
          header.kind () == stream_message_kind_t::request ? dispatch_message_kind_t::request
                                                           : dispatch_message_kind_t::send,
          detail::dispatch_reason_from_error (dispatched.error_kind ()),
          header.kind () == stream_message_kind_t::request
            ? dispatch_error_action_t::reply_error
            : dispatch_error_action_t::drop,
          std::string (header.packet_name ()),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          header.correlation_id () ? std::make_optional (std::string (*header.correlation_id ()))
                                   : std::nullopt,
          dispatched.error () != nullptr ? std::make_exception_ptr (*dispatched.error ())
                                         : std::exception_ptr{}};
        if (auto flow = header.flow_id ()) {
            event.flow_id = std::string (*flow);
            event.flow_origin = header.flow_origin ();
        }
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report (event);
    }

    void run ()
    {
        _receive_scheduler.start ();
        if (_mesh_node && !stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
            run_core_stream ();
            return;
        }
        if (!stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
            run_tcp_native_accept ();
            return;
        }
        if (stream_uses_tls (_stream)) {
            configure_tls_context ();
        }
        auto endpoint = parse_stream_endpoint (_stream);
        tcp::resolver resolver (_io);
        const auto endpoints = resolver.resolve (endpoint.host, endpoint.port);
        _acceptor.open (endpoints.begin ()->endpoint ().protocol ());
        _acceptor.set_option (tcp::acceptor::reuse_address (true));
        _acceptor.bind (endpoints.begin ()->endpoint ());
        _acceptor.listen ();
        start_boost_accept ();
        mark_started ();
        _io.run ();
    }

    void wait_started ()
    {
        std::unique_lock<std::mutex> lock (_ready_mutex);
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
        if (!_ready_cv.wait_until (lock, deadline, [&] { return _started || _start_failed; })) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "STREAM listener did not become ready: " + _stream.name);
        }
        if (_start_failed) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              _start_error.empty () ? "STREAM listener failed to start: " + _stream.name
                                    : _start_error);
        }
    }

    void request_stop () noexcept
    {
        _receive_scheduler.request_stop ();
        if (_inbound_budget) {
            _inbound_budget->wake_waiters ();
        }
        if (!stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
            wake_native_accept ();
            return;
        }
        asio::post (_io, [this] {
            boost::system::error_code ignored;
            _acceptor.close (ignored);
        });
    }

    void wake_native_accept () noexcept
    {
        int fd = -1;
        {
            const std::lock_guard<std::mutex> lock (_native_accept_mutex);
            fd = _native_accept_fd;
        }
        if (fd < 0) {
            return;
        }
        const auto endpoint = parse_tcp_endpoint (_stream.bind_endpoint);
        const int wake_fd = ::socket (AF_INET, SOCK_STREAM, 0);
        if (wake_fd < 0) {
            return;
        }
        set_nonblocking (wake_fd);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons (static_cast<std::uint16_t> (std::stoi (endpoint.port)));
        if (::inet_pton (AF_INET, endpoint.host.c_str (), &address.sin_addr) != 1) {
            address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        }
        (void) ::connect (wake_fd, reinterpret_cast<sockaddr *> (&address), sizeof (address));
        pollfd wake_poll{};
        wake_poll.fd = wake_fd;
        wake_poll.events = POLLOUT;
        (void) ::poll (&wake_poll, 1, 50);
        ::close (wake_fd);
    }

    void stop_connections () noexcept
    {
        trace_stream_host ("stop-connections-begin", _stream);
        boost::system::error_code ignored;
        if (!stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
            wake_native_accept ();
        }
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            for (auto *socket : _sockets) {
                const std::lock_guard<std::mutex> io_lock (_io_mutex);
                socket->shutdown (tcp::socket::shutdown_both, ignored);
                socket->close (ignored);
            }
            for (auto it = _native_connections.begin (); it != _native_connections.end ();) {
                auto connection = it->lock ();
                if (!connection) {
                    it = _native_connections.erase (it);
                    continue;
                }
                close_connection (*connection);
                ++it;
            }
        }
        _receive_scheduler.stop ();
        {
            const std::lock_guard<std::mutex> lock (_workers_mutex);
            trace_stream_host ("stop-connections-join-workers", _stream);
            for (auto &worker : _workers) {
                if (worker.joinable ()) {
                    worker.join ();
                }
            }
            _workers.clear ();
        }
        trace_stream_host ("stop-connections-end", _stream);
    }

  private:
    struct core_session_t
    {
        detail::service_scope_t scope;
        packet_stream_session_t *session;
        session_actor_manager_t *actors;
        stream_t stream;
        runtime::stateful::stream_connection_t transport_connection;
        std::mutex gate;
        bool connected = false;

        core_session_t (detail::service_scope_t scope_,
                        packet_stream_session_t &session_,
                        session_actor_manager_t &actors_,
                        stream_t stream_,
                        runtime::stateful::stream_connection_t transport_connection_) :
            scope (std::move (scope_)),
            session (&session_),
            actors (&actors_),
            stream (std::move (stream_)),
            transport_connection (std::move (transport_connection_))
        {
        }
    };

    static std::string core_session_key (const zlink::routing_id_t &rid)
    {
        return rid.to_hex ();
    }

    result_t<void> send_core_frame (const zlink::routing_id_t &rid,
                                    const stream_header_t &header,
                                    const zlink::message_t &payload)
    {
        auto encoded = _runtime.encode_header (header);
        if (!encoded) {
            return result_t<void>::failure (
              encoded.error_kind (),
              encoded.error () ? encoded.error ()->what () : "STREAM header encode failed");
        }

        const auto payload_bytes = payload.to_bytes ();
        if (encoded.value ().size () > std::numeric_limits<std::uint16_t>::max ()
            || payload_bytes.size () > std::numeric_limits<std::uint32_t>::max ()) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "Core STREAM frame is too large");
        }

        std::vector<std::uint8_t> bytes;
        bytes.reserve (6 + encoded.value ().size () + payload_bytes.size ());
        const auto header_size = encoded.value ().size ();
        bytes.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        bytes.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        bytes.push_back (
          static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        bytes.push_back (
          static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        bytes.push_back (
          static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        bytes.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        bytes.insert (bytes.end (), encoded.value ().begin (), encoded.value ().end ());
        bytes.insert (bytes.end (), payload_bytes.begin (), payload_bytes.end ());
        auto frame = zlink::message_t::from (bytes);

        std::lock_guard socket_lock (_core_socket_mutex);
        if (!_core_socket) {
            return detail::boundary_failure<void> (
              detail::boundary_error_t::disconnected,
              "Core STREAM socket is stopped");
        }
        runtime::messaging::note_submit_attempt (
          "stream:" + rid.to_hex (), _core_socket.get (), std::chrono::seconds (1),
          _runtime.pending_limit ());
        const bool sent = _core_socket->send (rid)
                            .message (frame)
                            .flags (static_cast<int> (zlink::send_flags_t::dontwait))
                            .submit ();
        trace_stream_host (
          "core-write", _stream, header,
          "rid=" + rid.to_hex () + " bytes=" + std::to_string (frame.size ())
            + " result=" + (sent ? "success" : "rejected"));
        return sent ? result_t<void>::success ()
                    : result_t<void>::failure (
                        framework_error_kind_t::capacity_exceeded,
                        "Core STREAM packet send is backpressured");
    }

    void send_core_error_frame (const zlink::routing_id_t &rid,
                                const stream_header_t &request_header,
                                const result_t<void> &error) noexcept
    {
        if (request_header.kind () != stream_message_kind_t::request
            || !request_header.request_seq ()) {
            return;
        }

        stream_header_t error_header (stream_message_kind_t::error, stream_codec_t::json,
                                      stream_header_flags_t::has_request_seq,
                                      request_header.request_seq (), "", {});
        if (auto correlation = request_header.correlation_id ()) {
            error_header.with_correlation_id (std::string (*correlation));
        }
        auto send = detail::submit_one_way_task (
          [this, rid, error_header = std::move (error_header), payload = stream_error_payload (error)] {
              return send_core_frame (rid, error_header, payload);
          });
        detail::observe_task_completion (
          send, [this, rid] (const result_t<void> &result) {
              if (!result) {
                  disconnect_core_peer (rid, "error_reply_failed");
              }
          });
    }

    void run_core_stream ()
    {
        try {
            _core_socket =
              std::make_unique<zlink::stream_socket_t> (_mesh_node->native_context ());
            _core_socket->options ().max_message_size (zlink::byte_size_t::bytes (
              _stream.max_message_size > 0 ? _stream.max_message_size : -1));
            runtime::messaging::activate_submit_owner (_core_socket.get ());
            _core_socket->set_send_ready_handler ([this] {
                runtime::messaging::notify_submit_ready (_core_socket.get ());
            });
            _core_monitor = std::make_unique<zlink::socket_monitor_t> (
              _core_socket->monitor_open (zlink::monitor_event::disconnected));
            _core_monitor->on_event ([this] (const zlink::monitor_event_t &event) {
                if (event.event == zlink::monitor_event::disconnected && event.routing_id)
                    close_core_session (*event.routing_id, "client_close");
            });
            _core_socket->bind (_stream.bind_endpoint);
            mark_started ();
            zlink::poller_t poller;
            poller.add (*_core_socket, zlink::poll_event_flag_t::pollin, 1);
            zlink::received_t received;
            while (!_stop->load (std::memory_order_acquire)) {
                receive_batch_budget_t batch;
                // Frames retained by an earlier batch are drained before
                // waiting for another Core receive. This prevents a peer
                // whose application HWM is full from retaining work that
                // could otherwise be processed for another peer.
                process_core_frames (batch);
                if (batch.exhausted ()) {
                    continue;
                }

                zlink::poll_event_t event;
                /* Closing the Core socket from request_stop() is not a
                 * cross-thread wake-up guarantee for the poller. Keep the
                 * stop observation bounded so service stop can join this
                 * listener without depending on platform-specific close
                 * behavior. */
                const size_t event_count =
                  poller.wait (&event, 1, std::chrono::milliseconds{100});
                if (_stop->load (std::memory_order_acquire))
                    break;
                if (event_count == 0)
                    continue;

                const short revents = static_cast<short> (event.revents);
                const short pollin =
                  static_cast<short> (zlink::poll_event_flag_t::pollin);
                const short pollerr =
                  static_cast<short> (zlink::poll_event_flag_t::pollerr);
                if ((revents & pollerr) != 0 && (revents & pollin) == 0)
                    break;

                // POLLIN guarantees that a receive is worthwhile. DONTWAIT
                // also makes a stale readiness event harmless: it yields
                // ZLINK_RECV_NO_DATA and the loop re-arms the poller.
                const int receive_result =
                  _core_socket->recv (received, zlink::recv_flags_t::dontwait);
                if (receive_result != 0) {
                    const int receive_errno = errno;
                    received.close ();
                    if (_stop->load (std::memory_order_acquire)
                        || receive_result
                             == static_cast<int> (zlink::recv_result_t::terminated)) {
                        break;
                    }
                    if (receive_result
                        == static_cast<int> (zlink::recv_result_t::no_data)) {
                        continue;
                    }
                    if (receive_result == -1 && receive_errno == EMSGSIZE) {
                        trace_stream_host (
                          "core-recv-size-rejected", _stream, std::nullopt,
                          "errno=EMSGSIZE limit="
                            + std::to_string (_stream.max_message_size));
                        continue;
                    }
                    throw std::runtime_error ("Framework STREAM recv failed");
                }
                process_core_received (received, batch);
                if (received.routing_id ()) {
                    trace_stream_host (
                      "core-recv", _stream, std::nullopt,
                      "rid=" + received.routing_id ()->to_hex ()
                        + " parts="
                        + std::to_string (received.parts ().size ()));
                }
                received.close ();
            }
        }
        catch (const std::exception &error) {
            mark_start_failed (error.what ());
        }
        close_core_sessions ();
        if (_core_monitor) {
            _core_monitor->close ();
            _core_monitor.reset ();
        }
        {
            std::lock_guard lock (_core_socket_mutex);
            if (_core_socket) {
                runtime::messaging::shutdown_submit_owner (_core_socket.get ());
                (void) _core_socket->close ();
                _core_socket.reset ();
            }
        }
        _core_frame_assemblers.clear ();
        _core_frame_routing_ids.clear ();
    }

    std::shared_ptr<core_session_t>
    get_or_create_core_session (const zlink::routing_id_t &rid)
    {
        const auto key = core_session_key (rid);
        std::lock_guard lock (_core_sessions_mutex);
        if (const auto found = _core_sessions.find (key); found != _core_sessions.end ())
            return found->second;
        if (draining ()) {
            throw framework_exception_t (
              framework_error_kind_t::rejected,
              "STREAM node is draining and rejects a new session");
        }

        auto scope = detail::service_scope_t::create (
          *_services, detail::service_scope_kind_t::stream_session);
        auto &session = _session_factory (scope.provider ());
        auto &actors = scope.provider ().get_required<session_actor_manager_t> ();
        auto stream = _runtime.open_session (_stream.name);
        _runtime.set_session_identity (stream, rid);
        auto transport_connection =
          _mesh_node->native_node ().sessions ().open (key);
        detail::session_actor_manager_access_t::attach (actors, stream);
        detail::session_actor_manager_access_t::bind_native (
          actors, [this, transport_connection] (const actor_ref_t &actor) {
              if (!_mesh_node) {
                  return result_t<void>::failure (
                    framework_error_kind_t::not_configured,
                    "STREAM Actor dispatch MeshNode is not started");
              }
              const auto local_node = _mesh_node->routing_id ();
              if (!local_node) {
                  return result_t<void>::failure (
                    framework_error_kind_t::not_configured,
                    "STREAM Actor dispatch MeshNode has no routing id");
              }
              if (local_node->to_hex ()
                  != zlink::routing_id_t::from (
                       std::string (actor.node_rid ().value ())).to_hex ()) {
                  return _mesh_node->bind_application_actor_session (
                    actor, node_rid_t::from_string (local_node->to_string ()),
                    std::chrono::seconds (5));
              }
              const auto native_actor =
                runtime::host::public_host_runtime_t::remote_actor_ref (
                  zlink::routing_id_t::from (
                    std::string (actor.node_rid ().value ())),
                  std::string (actor.actor_id ().value ()), actor.object_generation ());
              const auto resolved =
                _mesh_node->native_node ().resolve_actor (native_actor);
              if (!resolved) {
                  return result_t<void>::failure (
                    framework_error_kind_t::not_configured,
                    "Framework STREAM actor authority is unavailable");
              }
              const auto previous_binding =
                _mesh_node->native_node ().sessions ().current_binding (
                  std::string (actor.actor_id ().value ()));
              const auto [error, binding] =
                _mesh_node->native_node ().sessions ().bind (
                  transport_connection, *resolved);
              if (error != runtime::stateful::stateful_error_t::none) {
                  return result_t<void>::failure (
                    framework_error_kind_t::not_configured,
                    "Framework rejected STREAM actor binding");
              }
              auto recorded = _services->get_required<detail::actor_gateway_runtime_t> ()
                .record_bound_session_route (actor, *local_node, std::nullopt);
              if (!recorded) {
                  const auto rollback = _mesh_node->native_node ().sessions ().unbind (binding);
                  auto restore_error = rollback;
                  if (restore_error == runtime::stateful::stateful_error_t::none
                      && previous_binding) {
                      restore_error = _mesh_node->native_node ().sessions ().restore (
                        *previous_binding);
                  }
                  if (restore_error != runtime::stateful::stateful_error_t::none) {
                      return result_t<void>::failure (
                        framework_error_kind_t::internal_failure,
                        "Framework STREAM actor binding rollback failed");
                  }
              }
              return recorded;
          });
        _runtime.attach_transport_writer (
          stream, [this, rid] (const stream_header_t &header,
                              const zlink::message_t &payload) -> result_t<void> {
              return send_core_frame (rid, header, payload);
          });
        auto created = std::make_shared<core_session_t> (
          std::move (scope), session, actors, std::move (stream),
          std::move (transport_connection));
        auto connected = _runtime.dispatch_connected_async (
          *created->session, created->stream,
          [this, created, rid] (const result_t<void> &result) {
              if (!result) {
                  close_core_session (rid, "connected_dispatch_error");
              }
          });
        if (!connected) {
            throw framework_exception_t (
              connected.error_kind (),
              connected.error () ? connected.error ()->what ()
                                  : "STREAM connected dispatch failed");
        }
        created->connected = true;
        record_connection_opened ();
        _core_sessions.emplace (key, created);
        return created;
    }

    void close_core_session (const zlink::routing_id_t &rid,
                             const char *close_reason) noexcept
    {
        std::shared_ptr<core_session_t> current;
        {
            std::lock_guard lock (_core_sessions_mutex);
            const auto found = _core_sessions.find (core_session_key (rid));
            if (found == _core_sessions.end ())
                return;
            current = std::move (found->second);
            _core_sessions.erase (found);
        }
        _core_frame_assemblers.erase (core_session_key (rid));
        _core_frame_routing_ids.erase (core_session_key (rid));
        if (!current)
            return;

        std::lock_guard session_lock (current->gate);
        (void) _mesh_node->native_node ().sessions ().close (
          current->transport_connection);
        if (current->connected) {
            _runtime.mark_disconnected (current->stream);
            auto finalize = [current] (const result_t<void> &) {
                detail::session_actor_manager_access_t::disconnect (*current->actors);
                current->scope.close ();
            };
            auto submitted = _runtime.dispatch_disconnected_async (
              *current->session, current->stream, finalize);
            if (!submitted) {
                finalize (submitted);
            }
            current->connected = false;
            record_connection_closed (close_reason);
            return;
        }
        detail::session_actor_manager_access_t::disconnect (*current->actors);
        current->scope.close ();
    }

    void disconnect_core_peer (const zlink::routing_id_t &rid,
                               const char *close_reason) noexcept
    {
        try {
            close_core_session (rid, close_reason);
            if (_core_socket) {
                static_cast<zlink::socket_t &> (*_core_socket).disconnect_rid (rid);
            }
        }
        catch (...) {
        }
    }

    bool dispatch_core_frame (const zlink::routing_id_t &rid,
                              raw_core_stream_frame_t frame,
                              stream_header_t header)
    {
        std::shared_ptr<core_session_t> current;
        try {
            current = get_or_create_core_session (rid);
        }
        catch (...) {
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }

        trace_stream_host (
          "core-frame", _stream, header,
          "rid=" + rid.to_hex ()
            + " payload_bytes=" + std::to_string (frame.payload.size ()));
        if (header.kind () == stream_message_kind_t::control) {
            std::lock_guard session_lock (current->gate);
            if (current->connected) {
                detail::session_actor_manager_access_t::set_codec (
                  *current->actors, header.codec ());
                if (header.packet_name () == "$zlink.heartbeat.ping") {
                    _runtime.send_heartbeat_pong (current->stream);
                }
            }
            return true;
        }

        const auto payload_bytes = static_cast<std::uint64_t> (frame.payload.size ());
        if (_inbound_budget) {
            _inbound_budget->received (payload_bytes);
        }
        bool submitted = false;
        bool protocol_error = false;
        try {
            std::lock_guard session_lock (current->gate);
            if (current->connected) {
                trace_stream_host (
                  "core-dispatch-submit", _stream, header,
                  "rid=" + rid.to_hex ());
                detail::session_actor_manager_access_t::set_codec (
                  *current->actors, header.codec ());
                auto dispatched = _runtime.dispatch_packet_async (
                  *current->session, current->stream, header, frame.payload,
                  [this, current, rid, header, inbound_budget = _inbound_budget,
                   payload_bytes] (const result_t<void> &result) {
                      trace_stream_host (
                        "core-dispatch-complete", _stream, header,
                        std::string ("result=")
                          + (result ? "success" : "failure")
                          + (result ? std::string ()
                                    : " kind="
                                        + std::to_string (
                                            static_cast<int> (result.error_kind ()))
                                        + " error="
                                        + (result.error () ? result.error ()->what ()
                                                           : "unknown")));
                      if (inbound_budget) {
                          inbound_budget->completed (payload_bytes, true);
                      }
                      if (!result) {
                          report_packet_dispatch_error (header, result);
                          send_core_error_frame (rid, header, result);
                      }
                  },
                  [inbound_budget = _inbound_budget, payload_bytes] {
                      if (inbound_budget) {
                          inbound_budget->handler_started (payload_bytes);
                      }
                  },
                  [this] { return _stop->load (std::memory_order_acquire); });
                submitted = static_cast<bool> (dispatched);
                if (!dispatched) {
                    trace_stream_host (
                      "core-dispatch-rejected", _stream, header,
                      "rid=" + rid.to_hex () + " kind="
                        + std::to_string (
                            static_cast<int> (dispatched.error_kind ()))
                        + " error="
                        + (dispatched.error () ? dispatched.error ()->what ()
                                                : "unknown"));
                    report_packet_dispatch_error (header, dispatched);
                    send_core_error_frame (rid, header, dispatched);
                    if (dispatched.error_kind ()
                          == framework_error_kind_t::protocol_error
                        || dispatched.error_kind ()
                             == framework_error_kind_t::protocol_error) {
                        protocol_error = true;
                    }
                }
            }
        }
        catch (...) {
            if (_inbound_budget) {
                _inbound_budget->completed (payload_bytes, false);
            }
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }
        if (!submitted && _inbound_budget) {
            _inbound_budget->completed (payload_bytes, false);
        }
        if (protocol_error) {
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }
        return true;
    }

    void process_core_frames (receive_batch_budget_t &budget)
    {
        if (!budget.can_receive () || _core_frame_assemblers.empty ()) {
            return;
        }

        std::vector<std::string> keys;
        keys.reserve (_core_frame_assemblers.size ());
        for (const auto &[key, _] : _core_frame_assemblers) {
            keys.push_back (key);
        }
        std::size_t start = 0;
        if (!_core_frame_cursor.empty ()) {
            const auto next = std::upper_bound (
              keys.begin (), keys.end (), _core_frame_cursor);
            start = static_cast<std::size_t> (next - keys.begin ());
            if (start == keys.size ()) {
                start = 0;
            }
        }

        // Visit each Core peer at most once per batch. The cursor continues
        // from the last visited peer on the next batch.
        for (std::size_t offset = 0;
             offset < keys.size () && budget.can_receive (); ++offset) {
            const auto &key = keys[(start + offset) % keys.size ()];
            _core_frame_cursor = key;
            const auto assembler_found = _core_frame_assemblers.find (key);
            const auto rid_found = _core_frame_routing_ids.find (key);
            if (assembler_found == _core_frame_assemblers.end ()
                || rid_found == _core_frame_routing_ids.end ()) {
                continue;
            }

            auto &assembler = assembler_found->second;
            const auto view = assembler.peek ();
            if (!view) {
                continue;
            }

            const auto rid = rid_found->second;
            const std::vector<std::uint8_t> header_bytes (
              view->header.begin (), view->header.end ());
            auto decoded = _runtime.decode_header (header_bytes);
            if (!decoded) {
                disconnect_core_peer (rid, "protocol_error");
                _core_frame_assemblers.erase (key);
                _core_frame_routing_ids.erase (key);
                continue;
            }

            if (decoded.value ().kind () != stream_message_kind_t::control
                && _inbound_budget
                && !_inbound_budget->can_start_application_receive ()) {
                // Keep the complete frame in the assembler. The listener
                // remains available for control traffic and other peers;
                // the next bounded poll turn retries this peer after the
                // application budget changes.
                continue;
            }

            auto frame = assembler.next ();
            if (!frame) {
                continue;
            }
            const auto frame_bytes = view->frame_size;
            const bool dispatched = dispatch_core_frame (
              rid, std::move (*frame), std::move (decoded.value ()));
            budget.account (frame_bytes);
            if (!dispatched) {
                _core_frame_assemblers.erase (key);
                _core_frame_routing_ids.erase (key);
            }
        }
    }

    void process_core_received (zlink::received_t &received,
                                receive_batch_budget_t &budget)
    {
        if (!received.routing_id ()) {
            return;
        }
        const auto rid = *received.routing_id ();
        const auto key = core_session_key (rid);
        auto [assembler_found, inserted] = _core_frame_assemblers.try_emplace (
          key, _stream.max_message_size);
        (void) inserted;
        auto &assembler = assembler_found->second;
        _core_frame_routing_ids.insert_or_assign (key, rid);
        try {
            for (const auto &part : received.parts ()) {
                assembler.append (part.bytes ());
            }
            process_core_frames (budget);
        }
        catch (const stream_message_size_exceeded_t &error) {
            trace_stream_host (
              "core-recv-size-rejected", _stream, std::nullopt,
              "errno=EMSGSIZE limit=" + std::to_string (error.max_message_size ())
                + " header_bytes=" + std::to_string (error.header_size ())
                + " payload_bytes=" + std::to_string (error.payload_size ())
                + " rid=" + rid.to_hex ());
            disconnect_core_peer (rid, "protocol_error");
            _core_frame_assemblers.erase (key);
            _core_frame_routing_ids.erase (key);
        }
        catch (const malformed_core_stream_frame_t &) {
            disconnect_core_peer (rid, "protocol_error");
            _core_frame_assemblers.erase (key);
            _core_frame_routing_ids.erase (key);
        }
        catch (const std::exception &) {
            disconnect_core_peer (rid, "protocol_error");
            _core_frame_assemblers.erase (key);
            _core_frame_routing_ids.erase (key);
        }
    }

    void close_core_sessions (
      const char *close_reason = "server_stop",
      std::optional<stream_close_reason_t> notify_reason = std::nullopt,
      std::string_view diagnostic = {}) noexcept
    {
        std::map<std::string, std::shared_ptr<core_session_t>> sessions;
        {
            std::lock_guard lock (_core_sessions_mutex);
            sessions.swap (_core_sessions);
        }
        for (auto &[_, current] : sessions) {
            if (!current)
                continue;
            std::lock_guard session_lock (current->gate);
            if (notify_reason)
                _runtime.send_session_closing (current->stream, *notify_reason, diagnostic);
            _runtime.drain_async_dispatch (current->stream);
            if (current->connected) {
                _runtime.mark_disconnected (current->stream);
                (void) _runtime.dispatch_disconnected (*current->session, current->stream);
                current->connected = false;
                record_connection_closed (close_reason);
            }
            detail::session_actor_manager_access_t::disconnect (*current->actors);
            current->scope.close ();
        }
    }

    struct tcp_connection_t
    {
        asio::io_context io;
        tcp::socket socket;

        tcp_connection_t () : socket (io) {}
    };

    struct native_tcp_connection_t
    {
        explicit native_tcp_connection_t (int accepted) : fd (accepted) {}

        std::mutex mutex;
        int fd;
        std::vector<std::uint8_t> receive_buffer;
    };

    struct frame_t
    {
        stream_header_t header;
        zlink::message_t payload;
    };

    static void set_nonblocking (int fd)
    {
        const int flags = ::fcntl (fd, F_GETFL, 0);
        if (flags >= 0) {
            (void) ::fcntl (fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    void run_tcp_native_accept ()
    {
        const auto endpoint = parse_tcp_endpoint (_stream.bind_endpoint);
        const int server_fd = ::socket (AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            mark_start_failed ("STREAM TCP socket create failed: " + _stream.name);
            return;
        }
        {
            const std::lock_guard<std::mutex> lock (_native_accept_mutex);
            _native_accept_fd = server_fd;
        }
        int reuse = 1;
        (void) ::setsockopt (server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));
        set_nonblocking (server_fd);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons (static_cast<std::uint16_t> (std::stoi (endpoint.port)));
        if (::inet_pton (AF_INET, endpoint.host.c_str (), &address.sin_addr) != 1) {
            address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        }
        if (::bind (server_fd, reinterpret_cast<sockaddr *> (&address), sizeof (address)) != 0
            || ::listen (server_fd, SOMAXCONN) != 0) {
            const auto message =
              std::string ("STREAM TCP listener failed: ") + std::strerror (errno);
            const std::lock_guard<std::mutex> lock (_native_accept_mutex);
            ::close (server_fd);
            _native_accept_fd = -1;
            mark_start_failed (message);
            return;
        }
        mark_started ();
        while (!_stop->load (std::memory_order_acquire)) {
            pollfd accept_poll{};
            accept_poll.fd = server_fd;
            accept_poll.events = POLLIN;
            const int ready = ::poll (&accept_poll, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (_stop->load (std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                continue;
            }
            if (ready == 0) {
                continue;
            }
            if ((accept_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                if (_stop->load (std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                continue;
            }
            sockaddr_in peer{};
            socklen_t peer_len = sizeof (peer);
            const int accepted =
              ::accept (server_fd, reinterpret_cast<sockaddr *> (&peer), &peer_len);
            if (accepted < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                if (_stop->load (std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                continue;
            }
            if (_stop->load (std::memory_order_acquire)) {
                ::shutdown (accepted, SHUT_RDWR);
                ::close (accepted);
                break;
            }
            /* Keep accepted sockets blocking for writes. Native receives use
             * MSG_DONTWAIT, so a partial frame still releases the scheduler
             * turn without converting transient send backpressure into a
             * transport failure. */
            trace_stream_host ("accept", _stream);
            auto connection = std::make_shared<native_tcp_connection_t> (accepted);
            {
                const std::lock_guard<std::mutex> lock (_sockets_mutex);
                _native_connections.push_back (connection);
            }
            const std::lock_guard<std::mutex> lock (_workers_mutex);
            _workers.emplace_back ([this, connection] { handle_native_connection (connection); });
        }
        {
            const std::lock_guard<std::mutex> lock (_native_accept_mutex);
            if (_native_accept_fd >= 0) {
                ::close (_native_accept_fd);
                _native_accept_fd = -1;
            }
        }
    }

    void mark_started ()
    {
        {
            const std::lock_guard<std::mutex> lock (_ready_mutex);
            _started = true;
        }
        _ready_cv.notify_all ();
    }

    void mark_start_failed (std::string message)
    {
        {
            const std::lock_guard<std::mutex> lock (_ready_mutex);
            _start_failed = true;
            _start_error = std::move (message);
        }
        _ready_cv.notify_all ();
    }

    void configure_tls_context ()
    {
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
        if (_stream.tls_certificate_file.empty () || _stream.tls_private_key_file.empty ()) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "STREAM TLS endpoint requires certificate and private key");
        }
        _tls_context.emplace (ssl::context::tls_server);
        _tls_context->use_certificate_chain_file (_stream.tls_certificate_file);
        _tls_context->use_private_key_file (_stream.tls_private_key_file, ssl::context::pem);
        if (_stream.tls_require_client_certificate) {
            _tls_context->set_default_verify_paths ();
            _tls_context->set_verify_mode (
              ssl::verify_peer | ssl::verify_fail_if_no_peer_cert | ssl::verify_client_once);
        } else {
            _tls_context->set_verify_mode (ssl::verify_none);
        }
#else
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM TLS support requires OpenSSL");
#endif
    }

    void start_boost_accept ()
    {
        if (_stop->load (std::memory_order_acquire)) {
            return;
        }
        auto connection = std::make_shared<tcp_connection_t> ();
        _acceptor.async_accept (
          connection->socket, [this, connection] (const boost::system::error_code &error) {
              handle_boost_accept (connection, error);
          });
    }

    void handle_boost_accept (const std::shared_ptr<tcp_connection_t> &connection,
                              const boost::system::error_code &error)
    {
        if (error) {
            if (!_stop->load (std::memory_order_acquire)) {
                start_boost_accept ();
            }
            return;
        }
        if (_stop->load (std::memory_order_acquire)) {
            close_connection (connection->socket);
            return;
        }
        trace_stream_host ("accept", _stream);
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            _sockets.insert (&connection->socket);
        }
        if (stream_uses_websocket (_stream)) {
            auto websocket_connection =
              std::make_shared<websocket_stream_t> (std::move (connection->socket));
            {
                const std::lock_guard<std::mutex> lock (_sockets_mutex);
                _sockets.erase (&connection->socket);
                _sockets.insert (&websocket_connection->next_layer ());
            }
            {
                const std::lock_guard<std::mutex> lock (_workers_mutex);
                _workers.emplace_back ([this, connection, websocket_connection] {
                    handle_websocket_connection (connection, websocket_connection);
                });
            }
        } else if (stream_uses_tls (_stream)) {
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
            auto tls_connection =
              std::make_shared<ssl::stream<tcp::socket>> (std::move (connection->socket),
                                                          *_tls_context);
            {
                const std::lock_guard<std::mutex> lock (_sockets_mutex);
                _sockets.erase (&connection->socket);
                _sockets.insert (&tls_connection->next_layer ());
            }
            {
                const std::lock_guard<std::mutex> lock (_workers_mutex);
                _workers.emplace_back ([this, connection, tls_connection] {
                    handle_tls_connection (connection, tls_connection);
                });
            }
#else
            close_connection (connection->socket);
#endif
        } else {
            const std::lock_guard<std::mutex> lock (_workers_mutex);
            _workers.emplace_back ([this, connection] { handle_connection (connection); });
        }
        start_boost_accept ();
    }

    void close_tcp_socket (tcp::socket &socket) noexcept
    {
        boost::system::error_code ignored;
        const std::lock_guard<std::mutex> lock (_io_mutex);
        socket.shutdown (tcp::socket::shutdown_both, ignored);
        socket.close (ignored);
    }

    void close_connection (tcp::socket &socket) noexcept { close_tcp_socket (socket); }

    void close_connection (websocket_stream_t &stream) noexcept
    {
        boost::system::error_code ignored;
        {
            const std::lock_guard<std::mutex> lock (_io_mutex);
            if (stream.is_open ()) {
                stream.close (websocket::close_code::normal, ignored);
            }
        }
        close_tcp_socket (stream.next_layer ());
    }

    void close_connection (native_tcp_connection_t &connection) noexcept
    {
        int fd = -1;
        {
            const std::lock_guard<std::mutex> lock (connection.mutex);
            fd = connection.fd;
            connection.fd = -1;
        }
        if (fd < 0)
            return;
        ::shutdown (fd, SHUT_RDWR);
        ::close (fd);
    }

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    void close_connection (ssl::stream<tcp::socket> &stream) noexcept
    {
        close_tcp_socket (stream.next_layer ());
    }
#endif

    template <typename TStream>
    std::optional<frame_t>
    read_frame (TStream &socket,
                const std::function<bool (const stream_header_t &)> &admit_payload)
    {
        auto prefix = read_exact (socket, 6);
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8)
                                  | static_cast<std::size_t> (prefix[5]);
        validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
        auto header_bytes = read_exact (socket, header_size);
        auto header = _runtime.decode_header (header_bytes);
        if (!header) {
            throw framework_exception_t (header.error_kind (), header.error ()
                                                                 ? header.error ()->what ()
                                                                 : "STREAM header decode failed");
        }
        auto decoded_header = header.value ();
        if (decoded_header.kind () != stream_message_kind_t::control
            && admit_payload && !admit_payload (decoded_header)) {
            return std::nullopt;
        }
        auto payload_bytes = read_exact (socket, payload_size);
        trace_stream_host ("read-frame", _stream, decoded_header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        return frame_t{decoded_header, message_from_bytes (payload_bytes)};
    }

    std::optional<frame_t>
    read_frame (websocket_stream_t &socket,
                const std::function<bool (const stream_header_t &)> &admit_payload)
    {
        beast::flat_buffer buffer;
        socket.read (buffer);
        if (socket.got_text ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM WebSocket messages must be binary");
        }
        const auto size = buffer.size ();
        if (size < 6) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM WebSocket frame prefix is incomplete");
        }
        std::vector<std::uint8_t> bytes (size);
        asio::buffer_copy (asio::buffer (bytes), buffer.data ());
        const auto header_size = static_cast<std::size_t> ((bytes[0] << 8) | bytes[1]);
        const auto payload_size = (static_cast<std::size_t> (bytes[2]) << 24)
                                  | (static_cast<std::size_t> (bytes[3]) << 16)
                                  | (static_cast<std::size_t> (bytes[4]) << 8)
                                  | static_cast<std::size_t> (bytes[5]);
        validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
        if (bytes.size () != 6 + header_size + payload_size) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM WebSocket message size does not match its prefix");
        }
        std::vector<std::uint8_t> header_bytes (bytes.begin () + 6,
                                                bytes.begin () + 6 + header_size);
        std::vector<std::uint8_t> payload_bytes (bytes.begin () + 6 + header_size, bytes.end ());
        auto header = _runtime.decode_header (header_bytes);
        if (!header) {
            throw framework_exception_t (header.error_kind (), header.error ()
                                                                 ? header.error ()->what ()
                                                                 : "STREAM header decode failed");
        }
        auto decoded_header = header.value ();
        if (decoded_header.kind () != stream_message_kind_t::control
            && admit_payload && !admit_payload (decoded_header)) {
            return std::nullopt;
        }
        trace_stream_host ("read-frame", _stream, decoded_header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        return frame_t{decoded_header, message_from_bytes (payload_bytes)};
    }

    template <typename TStream>
    void
    write_frame (TStream &socket, const stream_header_t &header, const zlink::message_t &payload)
    {
        auto encoded_header = _runtime.encode_header (header);
        if (!encoded_header) {
            throw framework_exception_t (encoded_header.error_kind (),
                                         encoded_header.error () ? encoded_header.error ()->what ()
                                                                 : "STREAM header encode failed");
        }
        const auto payload_bytes = message_bytes (payload);
        std::vector<std::uint8_t> frame;
        frame.reserve (6 + encoded_header.value ().size () + payload_bytes.size ());
        const auto header_size = encoded_header.value ().size ();
        frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        frame.insert (frame.end (), encoded_header.value ().begin (),
                      encoded_header.value ().end ());
        frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
        trace_stream_host ("write-frame", _stream, header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        boost::asio::write (socket, boost::asio::buffer (frame));
        trace_stream_host ("write-completion", _stream, header, "result=success");
    }

    void write_frame (websocket_stream_t &socket,
                      const stream_header_t &header,
                      const zlink::message_t &payload)
    {
        auto encoded_header = _runtime.encode_header (header);
        if (!encoded_header) {
            throw framework_exception_t (encoded_header.error_kind (),
                                         encoded_header.error () ? encoded_header.error ()->what ()
                                                                 : "STREAM header encode failed");
        }
        const auto payload_bytes = message_bytes (payload);
        std::vector<std::uint8_t> frame;
        frame.reserve (6 + encoded_header.value ().size () + payload_bytes.size ());
        const auto header_size = encoded_header.value ().size ();
        frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        frame.insert (frame.end (), encoded_header.value ().begin (),
                      encoded_header.value ().end ());
        frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
        trace_stream_host ("write-frame", _stream, header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        socket.binary (true);
        socket.write (asio::buffer (frame));
        trace_stream_host ("write-completion", _stream, header, "result=success");
    }

    bool native_buffer_has_complete_frame (const native_tcp_connection_t &connection) const
    {
        if (connection.receive_buffer.size () < 6) {
            return false;
        }
        const auto &prefix = connection.receive_buffer;
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8)
                                  | static_cast<std::size_t> (prefix[5]);
        validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
        return connection.receive_buffer.size () >= 6u + header_size + payload_size;
    }

    std::optional<frame_t>
    read_frame_native (native_tcp_connection_t &connection,
                       const std::function<bool (const stream_header_t &)> &admit_payload)
    {
        constexpr std::size_t receive_chunk_size = 64u * 1024u;
        for (;;) {
            if (connection.receive_buffer.size () >= 6) {
                const auto &prefix = connection.receive_buffer;
                const auto header_size = static_cast<std::size_t> (
                  (prefix[0] << 8) | prefix[1]);
                const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                          | (static_cast<std::size_t> (prefix[3]) << 16)
                                          | (static_cast<std::size_t> (prefix[4]) << 8)
                                          | static_cast<std::size_t> (prefix[5]);
                validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
                const auto frame_size = 6u + header_size + payload_size;
                if (connection.receive_buffer.size () >= frame_size) {
                    std::vector<std::uint8_t> header_bytes (
                      connection.receive_buffer.begin () + 6,
                      connection.receive_buffer.begin () + 6 + header_size);
                    auto header = _runtime.decode_header (header_bytes);
                    if (!header) {
                        throw framework_exception_t (
                          header.error_kind (),
                          header.error () ? header.error ()->what ()
                                          : "STREAM header decode failed");
                    }
                    auto decoded_header = header.value ();
                    if (decoded_header.kind () != stream_message_kind_t::control
                        && admit_payload && !admit_payload (decoded_header)) {
                        return std::nullopt;
                    }
                    std::vector<std::uint8_t> payload_bytes (
                      connection.receive_buffer.begin () + 6 + header_size,
                      connection.receive_buffer.begin () + frame_size);
                    connection.receive_buffer.erase (
                      connection.receive_buffer.begin (),
                      connection.receive_buffer.begin () + frame_size);
                    trace_stream_host ("read-frame", _stream, decoded_header,
                                       "payload_bytes="
                                         + std::to_string (payload_bytes.size ()));
                    return frame_t{decoded_header, message_from_bytes (payload_bytes)};
                }
            }

            int fd = -1;
            {
                const std::lock_guard<std::mutex> lock (connection.mutex);
                fd = connection.fd;
            }
            if (fd < 0) {
                throw detail::make_boundary_exception (
                  detail::boundary_error_t::disconnected,
                  "stream native socket closed");
            }
            std::array<std::uint8_t, receive_chunk_size> chunk{};
            const auto received = ::recv (
              fd, chunk.data (), chunk.size (), MSG_DONTWAIT);
            const int received_errno = errno;
            if (received < 0) {
                if (received_errno == EINTR)
                    continue;
                if (received_errno == EAGAIN || received_errno == EWOULDBLOCK)
                    return std::nullopt;
                throw session_transport_failure_t (received_errno,
                                                   std::strerror (received_errno));
            }
            if (received == 0) {
                throw detail::make_boundary_exception (
                  detail::boundary_error_t::disconnected,
                  "STREAM TCP peer disconnected");
            }
            connection.receive_buffer.insert (
              connection.receive_buffer.end (), chunk.begin (),
              chunk.begin () + received);
        }
    }

    void write_all_native (native_tcp_connection_t &connection,
                           const std::vector<std::uint8_t> &bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size ()) {
            int fd = -1;
            {
                const std::lock_guard<std::mutex> lock (connection.mutex);
                fd = connection.fd;
            }
            if (fd < 0) {
                throw detail::make_boundary_exception (detail::boundary_error_t::disconnected,
                                             "stream native socket closed");
            }
            const auto sent = ::send (fd, bytes.data () + offset, bytes.size () - offset,
                                      MSG_NOSIGNAL);
            const int sent_errno = errno;
            if (sent < 0) {
                if (sent_errno == EINTR) {
                    continue;
                }
                throw session_transport_failure_t (sent_errno, std::strerror (sent_errno));
            }
            if (sent == 0) {
                throw session_transport_failure_t (0, "STREAM TCP write made no progress");
            }
            offset += static_cast<std::size_t> (sent);
        }
    }

    void write_frame_native (native_tcp_connection_t &connection,
                             const stream_header_t &header,
                             const zlink::message_t &payload)
    {
        auto encoded_header = _runtime.encode_header (header);
        if (!encoded_header) {
            throw framework_exception_t (encoded_header.error_kind (),
                                         encoded_header.error () ? encoded_header.error ()->what ()
                                                                 : "STREAM header encode failed");
        }
        const auto payload_bytes = message_bytes (payload);
        std::vector<std::uint8_t> frame;
        frame.reserve (6 + encoded_header.value ().size () + payload_bytes.size ());
        const auto header_size = encoded_header.value ().size ();
        frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        frame.insert (frame.end (), encoded_header.value ().begin (),
                      encoded_header.value ().end ());
        frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
        trace_stream_host ("write-frame", _stream, header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        write_all_native (connection, frame);
        trace_stream_host ("write-completion", _stream, header, "result=success");
    }

    template <typename TStream>
    void write_error_frame (TStream &socket,
                            const stream_header_t &request_header,
                            const result_t<void> &error)
    {
        if (!request_header.request_seq ()) {
            return;
        }
        stream_header_t error_header (stream_message_kind_t::error, stream_codec_t::json,
                                      stream_header_flags_t::has_request_seq,
                                      request_header.request_seq (), "", {});
        // Echo the request correlation id so a stream request FAILURE is traceable
        // by the same corr as its inbound `received`.
        if (auto correlation = request_header.correlation_id ()) {
            error_header.with_correlation_id (std::string (*correlation));
        }
        write_frame (socket, error_header, stream_error_payload (error));
    }

    void write_error_frame_native (native_tcp_connection_t &connection,
                                   const stream_header_t &request_header,
                                   const result_t<void> &error)
    {
        if (!request_header.request_seq ()) {
            return;
        }
        stream_header_t error_header (stream_message_kind_t::error, stream_codec_t::json,
                                      stream_header_flags_t::has_request_seq,
                                      request_header.request_seq (), "", {});
        if (auto correlation = request_header.correlation_id ()) {
            error_header.with_correlation_id (std::string (*correlation));
        }
        write_frame_native (connection, error_header, stream_error_payload (error));
    }

    template <typename TStream>
    void
    flush_writes (TStream &socket, stream_t &stream, std::size_t &flushed, std::mutex &write_mutex)
    {
        const auto headers = _runtime.written_headers (stream);
        const auto payloads = _runtime.written_payloads (stream);
        const std::lock_guard<std::mutex> lock (write_mutex);
        for (; flushed < headers.size (); ++flushed) {
            write_frame (socket, headers[flushed], payloads[flushed]);
        }
    }

    void flush_writes_native (native_tcp_connection_t &connection,
                              stream_t &stream,
                              std::size_t &flushed,
                              std::mutex &write_mutex)
    {
        const auto headers = _runtime.written_headers (stream);
        const auto payloads = _runtime.written_payloads (stream);
        const std::lock_guard<std::mutex> lock (write_mutex);
        for (; flushed < headers.size (); ++flushed) {
            write_frame_native (connection, headers[flushed], payloads[flushed]);
        }
    }

    template <typename TStream>
    void handle_stream_connection (std::shared_ptr<TStream> connection,
                                   tcp::socket *tracked_socket,
                                   bool attach_immediate_writer)
    {
        auto cleanup = std::unique_ptr<tcp::socket, std::function<void (tcp::socket *)>> (
          tracked_socket, [this] (tcp::socket *socket) {
              const std::lock_guard<std::mutex> lock (_sockets_mutex);
              _sockets.erase (socket);
          });
        if (draining ()) {
            /* graceful-drain-handoff §5: brand-new connections on a draining
             * node receive session-closing(server_drain) and are closed. */
            try {
                const auto payload_bytes = detail::stream_runtime_t::encode_session_closing_payload (
                  stream_close_reason_t::server_drain, "node is draining");
                detail::stream_header_t closing (stream_message_kind_t::control,
                                                 stream_codec_t::raw, stream_header_flags_t::none,
                                                 std::nullopt, "session-closing", {});
                const auto closing_payload = zlink::message_t::from (
                  std::string (payload_bytes.begin (), payload_bytes.end ()));
                if constexpr (std::is_same_v<TStream, native_tcp_connection_t>) {
                    write_frame_native (*connection, closing, closing_payload);
                } else {
                    write_frame (*connection, closing, closing_payload);
                }
            }
            catch (...) {
            }
            return;
        }
        auto scope = detail::service_scope_t::create (
          *_services, detail::service_scope_kind_t::stream_session);
        auto &session = _session_factory (scope.provider ());
        auto stream_instance = _runtime.open_session (_stream.name);
        _runtime.set_session_identity (
          stream_instance, std::nullopt, local_endpoint_text (tcp_socket (*connection)),
          remote_endpoint_text (tcp_socket (*connection)));
        auto &session_actors = scope.provider ().get_required<session_actor_manager_t> ();
        detail::session_actor_manager_access_t::attach (session_actors, stream_instance);
        auto connection_state = std::make_shared<stream_connection_state_t> (
          std::move (scope), &session, std::move (stream_instance), &session_actors);
        auto &stream = connection_state->stream;
        auto write_mutex = std::make_shared<std::mutex> ();
        if (attach_immediate_writer) {
            _runtime.attach_transport_writer (
              stream,
              [this, connection, write_mutex] (const stream_header_t &header,
                                               const zlink::message_t &payload) -> result_t<void> {
                  try {
                      const std::lock_guard<std::mutex> lock (*write_mutex);
                      write_frame (*connection, header, payload);
                      return result_t<void>::success ();
                  }
                  catch (const framework_exception_t &error) {
                      return detail::result_access_t::failure<void> (error);
                  }
                  catch (const std::exception &error) {
                      return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                                      error.what ());
                  }
              });
        }
        std::size_t flushed = 0;
        bool connected_session = false;
        std::optional<stream_error_t> session_transport_error;
        auto liveness = std::make_shared<session_liveness_t> ();
        try {
            if (auto connected = _runtime.dispatch_connected (session, stream); !connected) {
                return;
            }
            connected_session = true;
            record_connection_opened ();
            // close_connection owns the _io_mutex acquisition itself.
            register_active_stream (stream, liveness,
                                    [this, connection] { close_connection (*connection); });
            flush_writes (*connection, stream, flushed, *write_mutex);
            auto receive_lease =
              _receive_scheduler.register_connection (stream_native_handle (*connection));
            while (!_stop->load (std::memory_order_acquire)) {
                receive_batch_budget_t batch;
                while (!_stop->load (std::memory_order_acquire) && batch.can_receive ()) {
                    if (!receive_lease.wait_turn (
                          [this] { return _stop->load (std::memory_order_acquire); })) {
                        break;
                    }
                    /* The synchronous TLS/WebSocket read may wait for the
                     * remainder of a frame. Do not keep the listener-wide
                     * receive lease while waiting; one partial connection
                     * must not block the other connections. A new lease is
                     * acquired for every frame, which also bounds a peer's
                     * turn by the shared batch limits. */
                    if constexpr (!std::is_same_v<TStream, tcp::socket>) {
                        /* TLS and WebSocket implementations may retain
                         * decrypted or frame bytes outside the kernel fd.
                         * Keep this slot eligible for the next frame; the
                         * blocking read still happens after the lease is
                         * released. */
                        receive_lease.set_buffered_ready (true);
                    }
                    receive_lease.release_turn ();
                    auto frame = read_frame (*connection, [this] (const stream_header_t &) {
                        return !_inbound_budget
                               || _inbound_budget->wait_for_application_capacity (
                                 [this] { return _stop->load (std::memory_order_acquire); });
                    });
                    if (!frame) {
                        break;
                    }
                    auto received_frame = std::move (*frame);
                    batch.account (received_frame.payload.size ());
                    if (received_frame.header.kind () == stream_message_kind_t::control) {
                        if (received_frame.header.packet_name () == "$zlink.heartbeat.pong") {
                            liveness->record_pong ();
                        } else if (received_frame.header.packet_name () == "$zlink.heartbeat.ping") {
                            detail::stream_header_t pong (stream_message_kind_t::control,
                                                          stream_codec_t::raw,
                                                          stream_header_flags_t::none,
                                                          std::nullopt, "$zlink.heartbeat.pong", {});
                            const std::lock_guard<std::mutex> lock (*write_mutex);
                            write_frame (*connection, pong, zlink::message_t{});
                          }
                        continue;
                      }
                    const auto payload_bytes = static_cast<std::uint64_t> (
                      received_frame.payload.size ());
                    if (_inbound_budget) {
                        _inbound_budget->received (payload_bytes);
                    }
                    detail::session_actor_manager_access_t::set_codec (
                      session_actors, received_frame.header.codec ());
                    auto handler_started = std::make_shared<std::atomic_bool> (false);
                    const auto header = received_frame.header;
                    trace_stream_host ("dispatch-submit", _stream, header);
                    auto dispatched = _runtime.dispatch_packet_async (
                      session, stream, header, received_frame.payload,
                      [this, connection, connection_state, write_mutex, header, liveness,
                       handler_started, payload_bytes] (const result_t<void> &result) {
                          if (_inbound_budget) {
                              _inbound_budget->completed (
                                payload_bytes,
                                handler_started->load (std::memory_order_acquire));
                          }
                          trace_stream_host (
                            "dispatch-complete", _stream, header,
                            std::string ("result=") + (result ? "success" : "failure"));
                          if (!result) {
                              report_packet_dispatch_error (header, result);
                              if (header.kind () == stream_message_kind_t::request) {
                                  try {
                                      const std::lock_guard<std::mutex> lock (*write_mutex);
                                      write_error_frame (*connection, header, result);
                                  }
                                  catch (...) {
                                  }
                              }
                          }
                          (void) connection_state;
                          (void) liveness;
                      },
                      [this, liveness, handler_started, payload_bytes] {
                          handler_started->store (true, std::memory_order_release);
                          if (_inbound_budget) {
                              _inbound_budget->handler_started (payload_bytes);
                          }
                          liveness->record_application_inbound ();
                      },
                      [this] { return _stop->load (std::memory_order_acquire); });
                    if (!dispatched) {
                        if (_inbound_budget) {
                            _inbound_budget->completed (payload_bytes, false);
                        }
                        report_packet_dispatch_error (header, dispatched);
                        if (header.kind () == stream_message_kind_t::request) {
                            try {
                                const std::lock_guard<std::mutex> lock (*write_mutex);
                                write_error_frame (*connection, header, dispatched);
                            }
                            catch (...) {
                            }
                      }
                    }
                    flush_writes (*connection, stream, flushed, *write_mutex);
                }
            }
        }
        catch (const boost::system::system_error &error) {
            if (connected_session && !_stop->load (std::memory_order_acquire)
                && !liveness->forced () && !is_expected_session_disconnect (error.code ())) {
                session_transport_error.emplace (stream_session_error_t::transport_error,
                                                 error.what ());
            }
            if (stream_trace_enabled ()) {
                std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                          << _stream.name << " endpoint=" << _stream.bind_endpoint
                          << " error=\"" << error.what () << "\"" << std::endl;
            }
        }
        catch (const stream_message_size_exceeded_t &error) {
            trace_stream_host (
              "message-size-rejected", _stream, std::nullopt,
              "errno=EMSGSIZE limit=" + std::to_string (error.max_message_size ())
                + " header_bytes=" + std::to_string (error.header_size ())
                + " payload_bytes=" + std::to_string (error.payload_size ()));
        }
        catch (const framework_exception_t &error) {
            if (stream_trace_enabled ()) {
                std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                          << _stream.name << " endpoint=" << _stream.bind_endpoint
                          << " error=\"" << error.what () << "\"" << std::endl;
            }
        }
        catch (const std::exception &error) {
            if (stream_trace_enabled ()) {
                std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                          << _stream.name << " endpoint=" << _stream.bind_endpoint
                          << " error=\"" << error.what () << "\"" << std::endl;
            }
        }
        catch (...) {
            if (stream_trace_enabled ()) {
                std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                          << _stream.name << " endpoint=" << _stream.bind_endpoint
                          << " error=\"unknown\"" << std::endl;
            }
        }
        if (connected_session) {
            auto dispatch_disconnect = [this, &session, &stream, connection_state] {
                auto completed = [this, connection_state] (const result_t<void> &) {
                    trace_stream_host ("dispatch-disconnected-end", _stream);
                    (void) connection_state;
                };
                auto submitted = _runtime.dispatch_disconnected_async (
                  session, stream, completed);
                if (!submitted) {
                    completed (submitted);
                }
            };
            if (_stop->load (std::memory_order_acquire)) {
                _runtime.mark_disconnected (stream);
                dispatch_disconnect ();
            } else {
                trace_stream_host ("dispatch-disconnected-begin", _stream);
                unregister_active_stream (stream);
                const auto forced = liveness->forced ();
                record_connection_closed (
                  forced ? liveness_close_label (*forced)
                         : session_transport_error ? "transport_error" : "client_close");
                if (session_transport_error && !forced) {
                    (void) _runtime.dispatch_error (session, stream, *session_transport_error);
                }
                dispatch_disconnect ();
            }
            if (!_stop->load (std::memory_order_acquire)) {
                try {
                    flush_writes (*connection, stream, flushed, *write_mutex);
                }
                catch (...) {
                }
            }
        }
        close_connection (*connection);
    }

    void handle_native_connection (std::shared_ptr<native_tcp_connection_t> connection)
    {
        auto cleanup = std::unique_ptr<native_tcp_connection_t, std::function<void (native_tcp_connection_t *)>> (
          connection.get (), [this] (native_tcp_connection_t *native_connection) {
              const std::lock_guard<std::mutex> lock (_sockets_mutex);
              for (auto it = _native_connections.begin (); it != _native_connections.end ();) {
                  auto tracked = it->lock ();
                  if (!tracked || tracked.get () == native_connection)
                      it = _native_connections.erase (it);
                  else
                      ++it;
              }
          });
        if (draining ()) {
            /* graceful-drain-handoff §5: brand-new connections on a draining
             * node receive session-closing(server_drain) and are closed. */
            try {
                const auto payload_bytes = detail::stream_runtime_t::encode_session_closing_payload (
                  stream_close_reason_t::server_drain, "node is draining");
                detail::stream_header_t closing (stream_message_kind_t::control,
                                                 stream_codec_t::raw, stream_header_flags_t::none,
                                                 std::nullopt, "session-closing", {});
                const auto closing_payload = zlink::message_t::from (
                  std::string (payload_bytes.begin (), payload_bytes.end ()));
                write_frame_native (*connection, closing, closing_payload);
            }
            catch (...) {
            }
            return;
        }
        auto scope = detail::service_scope_t::create (
          *_services, detail::service_scope_kind_t::stream_session);
        auto &session = _session_factory (scope.provider ());
        auto stream_instance = _runtime.open_session (_stream.name);
        _runtime.set_session_identity (
          stream_instance, std::nullopt, native_local_endpoint_text (connection->fd),
          native_remote_endpoint_text (connection->fd));
        auto &session_actors = scope.provider ().get_required<session_actor_manager_t> ();
        detail::session_actor_manager_access_t::attach (session_actors, stream_instance);
        auto connection_state = std::make_shared<stream_connection_state_t> (
          std::move (scope), &session, std::move (stream_instance), &session_actors);
        auto &stream = connection_state->stream;
        auto write_mutex = std::make_shared<std::mutex> ();
        _runtime.attach_transport_writer (
          stream,
          [this, connection, write_mutex] (const stream_header_t &header,
                                           const zlink::message_t &payload) -> result_t<void> {
              try {
                  const std::lock_guard<std::mutex> lock (*write_mutex);
                  write_frame_native (*connection, header, payload);
                  return result_t<void>::success ();
              }
              catch (const framework_exception_t &error) {
                  return detail::result_access_t::failure<void> (error);
              }
              catch (const std::exception &error) {
                  return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                                  error.what ());
              }
          });
        std::size_t flushed = 0;
        bool connected_session = false;
        std::optional<stream_error_t> session_transport_error;
        auto liveness = std::make_shared<session_liveness_t> ();
        try {
            if (auto connected = _runtime.dispatch_connected (session, stream); !connected) {
                return;
            }
            connected_session = true;
            record_connection_opened ();
            register_active_stream (stream, liveness,
                                    [this, connection] { close_connection (*connection); });
            flush_writes_native (*connection, stream, flushed, *write_mutex);
            int native_fd = -1;
            {
                const std::lock_guard<std::mutex> lock (connection->mutex);
                native_fd = connection->fd;
            }
            auto receive_lease = _receive_scheduler.register_connection (native_fd);
            while (!_stop->load (std::memory_order_acquire)) {
                receive_batch_budget_t batch;
                while (!_stop->load (std::memory_order_acquire) && batch.can_receive ()) {
                    if (!receive_lease.wait_turn (
                          [this] { return _stop->load (std::memory_order_acquire); })) {
                        break;
                    }
                    bool application_capacity_blocked = false;
                    auto frame = read_frame_native (*connection, [this, &application_capacity_blocked] (
                                                                 const stream_header_t &) {
                        if (!_inbound_budget
                            || _inbound_budget->can_start_application_receive ()) {
                            return true;
                        }
                        application_capacity_blocked = true;
                        return false;
                    });
                    /* The native parser never waits for application HWM
                     * capacity while it owns the listener-wide lease. A
                     * complete frame remains in receive_buffer and the
                     * connection waits below, after releasing the lease. */
                    receive_lease.set_buffered_ready (
                      !application_capacity_blocked
                      && native_buffer_has_complete_frame (*connection));
                    receive_lease.release_turn ();
                    if (!frame) {
                        if (application_capacity_blocked) {
                            if (!_inbound_budget->wait_for_application_capacity (
                                  [this] { return _stop->load (std::memory_order_acquire); })) {
                                break;
                            }
                            receive_lease.set_buffered_ready (
                              native_buffer_has_complete_frame (*connection));
                            continue;
                        }
                        break;
                    }
                    auto received_frame = std::move (*frame);
                    batch.account (received_frame.payload.size ());
                    if (received_frame.header.kind () == stream_message_kind_t::control) {
                        if (received_frame.header.packet_name () == "$zlink.heartbeat.pong") {
                            liveness->record_pong ();
                        } else if (received_frame.header.packet_name () == "$zlink.heartbeat.ping") {
                            detail::stream_header_t pong (stream_message_kind_t::control,
                                                          stream_codec_t::raw,
                                                          stream_header_flags_t::none,
                                                          std::nullopt, "$zlink.heartbeat.pong", {});
                            const std::lock_guard<std::mutex> lock (*write_mutex);
                            write_frame_native (*connection, pong, zlink::message_t{});
                          }
                        continue;
                      }
                    const auto payload_bytes = static_cast<std::uint64_t> (
                      received_frame.payload.size ());
                    if (_inbound_budget) {
                        _inbound_budget->received (payload_bytes);
                    }
                    detail::session_actor_manager_access_t::set_codec (
                      session_actors, received_frame.header.codec ());
                    auto handler_started = std::make_shared<std::atomic_bool> (false);
                    const auto header = received_frame.header;
                    trace_stream_host ("dispatch-submit", _stream, header);
                    auto dispatched = _runtime.dispatch_packet_async (
                      session, stream, header, received_frame.payload,
                      [this, connection, connection_state, write_mutex, header, liveness,
                       handler_started, payload_bytes] (const result_t<void> &result) {
                          if (_inbound_budget) {
                              _inbound_budget->completed (
                                payload_bytes,
                                handler_started->load (std::memory_order_acquire));
                          }
                          trace_stream_host (
                            "dispatch-complete", _stream, header,
                            std::string ("result=") + (result ? "success" : "failure"));
                          if (!result) {
                              report_packet_dispatch_error (header, result);
                              if (header.kind () == stream_message_kind_t::request) {
                                  try {
                                      const std::lock_guard<std::mutex> lock (*write_mutex);
                                      write_error_frame_native (*connection, header, result);
                                  }
                                  catch (...) {
                                  }
                              }
                          }
                          (void) connection_state;
                          (void) liveness;
                      },
                      [this, liveness, handler_started, payload_bytes] {
                          handler_started->store (true, std::memory_order_release);
                          if (_inbound_budget) {
                              _inbound_budget->handler_started (payload_bytes);
                          }
                          liveness->record_application_inbound ();
                      },
                      [this] { return _stop->load (std::memory_order_acquire); });
                    if (!dispatched) {
                        if (_inbound_budget) {
                            _inbound_budget->completed (payload_bytes, false);
                        }
                        report_packet_dispatch_error (header, dispatched);
                        if (header.kind () == stream_message_kind_t::request) {
                            try {
                                const std::lock_guard<std::mutex> lock (*write_mutex);
                                write_error_frame_native (*connection, header, dispatched);
                            }
                            catch (...) {
                            }
                      }
                    }
                    flush_writes_native (*connection, stream, flushed, *write_mutex);
                }
            }
        }
        catch (const session_transport_failure_t &error) {
            if (connected_session && !_stop->load (std::memory_order_acquire)
                && !liveness->forced ()) {
                session_transport_error.emplace (stream_session_error_t::transport_error,
                                                 error.what ());
            }
        }
        catch (const stream_message_size_exceeded_t &error) {
            trace_stream_host (
              "message-size-rejected", _stream, std::nullopt,
              "errno=EMSGSIZE limit=" + std::to_string (error.max_message_size ())
                + " header_bytes=" + std::to_string (error.header_size ())
                + " payload_bytes=" + std::to_string (error.payload_size ()));
        }
        catch (const framework_exception_t &) {
        }
        catch (...) {
        }
        if (connected_session) {
            auto dispatch_disconnect = [this, &session, &stream, connection_state] {
                auto completed = [this, connection_state] (const result_t<void> &) {
                    trace_stream_host ("dispatch-disconnected-end", _stream);
                    (void) connection_state;
                };
                auto submitted = _runtime.dispatch_disconnected_async (
                  session, stream, completed);
                if (!submitted) {
                    completed (submitted);
                }
            };
            if (_stop->load (std::memory_order_acquire)) {
                _runtime.mark_disconnected (stream);
                dispatch_disconnect ();
            } else {
                trace_stream_host ("dispatch-disconnected-begin", _stream);
                unregister_active_stream (stream);
                const auto forced = liveness->forced ();
                record_connection_closed (
                  forced ? liveness_close_label (*forced)
                         : session_transport_error ? "transport_error" : "client_close");
                if (session_transport_error && !forced) {
                    (void) _runtime.dispatch_error (session, stream, *session_transport_error);
                }
                dispatch_disconnect ();
            }
            if (!_stop->load (std::memory_order_acquire)) {
                try {
                    flush_writes_native (*connection, stream, flushed, *write_mutex);
                }
                catch (...) {
                }
            }
        }
        close_connection (*connection);
    }

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    void handle_tls_connection (std::shared_ptr<tcp_connection_t> owner,
                                std::shared_ptr<ssl::stream<tcp::socket>> connection)
    {
        try {
            connection->handshake (ssl::stream_base::server);
        }
        catch (const boost::system::system_error &) {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            _sockets.erase (&connection->next_layer ());
            return;
        }
        (void) owner;
        handle_stream_connection (connection, &connection->next_layer (), true);
    }
#endif

    void handle_websocket_connection (std::shared_ptr<tcp_connection_t> owner,
                                      std::shared_ptr<websocket_stream_t> connection)
    {
        try {
            connection->accept ();
            connection->binary (true);
        }
        catch (const boost::system::system_error &) {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            _sockets.erase (&connection->next_layer ());
            return;
        }
        (void) owner;
        handle_stream_connection (connection, &connection->next_layer (), true);
    }

    void handle_connection (std::shared_ptr<tcp_connection_t> connection)
    {
        handle_stream_connection (std::shared_ptr<tcp::socket> (connection, &connection->socket),
                                  &connection->socket, true);
    }

    detail::stream_runtime_t _runtime;
    stream_snapshot_t _stream;
    detail::stream_session_factory_t _session_factory;
    service_provider_t *_services;
    std::atomic_bool *_stop;
    std::shared_ptr<std::atomic_bool> _drain_flag;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::shared_ptr<detail::mesh_node_runtime_t> _mesh_node;
    std::shared_ptr<inbound_dispatch_budget_t> _inbound_budget;
    std::mutex _active_streams_mutex;
    std::vector<active_session_t> _active_streams;
    asio::io_context _io;
    std::mutex _io_mutex;
    tcp::acceptor _acceptor;
    std::mutex _native_accept_mutex;
    int _native_accept_fd = -1;
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    std::optional<ssl::context> _tls_context;
#endif
    std::mutex _sockets_mutex;
    std::unordered_set<tcp::socket *> _sockets;
    std::vector<std::weak_ptr<native_tcp_connection_t>> _native_connections;
    std::mutex _core_socket_mutex;
    std::unique_ptr<zlink::stream_socket_t> _core_socket;
    std::unique_ptr<zlink::socket_monitor_t> _core_monitor;
    std::mutex _core_sessions_mutex;
    std::map<std::string, std::shared_ptr<core_session_t>> _core_sessions;
    std::map<std::string, core_stream_frame_assembler_t> _core_frame_assemblers;
    std::map<std::string, zlink::routing_id_t> _core_frame_routing_ids;
    std::string _core_frame_cursor;
    stream_receive_scheduler_t _receive_scheduler;
    std::mutex _workers_mutex;
    std::vector<std::thread> _workers;
    std::mutex _ready_mutex;
    std::condition_variable _ready_cv;
    bool _started = false;
    bool _start_failed = false;
    std::string _start_error;
};

stream_host_service_t::stream_host_service_t (
  detail::stream_runtime_t runtime,
  std::vector<stream_snapshot_t> streams,
  std::map<std::string, detail::stream_session_factory_t> session_factories,
  std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
  std::shared_ptr<inbound_dispatch_budget_t> inbound_budget) :
    _runtime (std::move (runtime)),
    _streams (std::move (streams)),
    _session_factories (std::move (session_factories)),
    _mesh_node (std::move (mesh_node)),
    _inbound_budget (std::move (inbound_budget))
{
}

stream_host_service_t::~stream_host_service_t () = default;

void stream_host_service_t::start (service_provider_t &services)
{
    _services = &services;
    _stop.store (false, std::memory_order_release);
    for (const auto &stream : _streams) {
        auto factory = _session_factories.find (stream.packet_session_name);
        if (factory == _session_factories.end ()) {
            continue;
        }
        auto listener =
          std::make_unique<listener_t> (_runtime, stream, factory->second, services, _stop,
                                        _drain_flag, _monitoring, _mesh_node,
                                        _inbound_budget);
        auto *raw = listener.get ();
        _listeners.push_back (std::move (listener));
        _threads.emplace_back ([raw] { raw->run (); });
        raw->wait_started ();
    }
    if (!_listeners.empty ()) {
        /* One liveness sweep loop per node (graceful-drain-handoff §7.2):
         * the service drives every listener's sessions from a single
         * thread; no per-session timers exist. */
        _liveness_thread = std::thread ([this] {
            auto last_sweep = std::chrono::steady_clock::now ();
            while (!_stop.load (std::memory_order_acquire)) {
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
                const auto now = std::chrono::steady_clock::now ();
                if (now - last_sweep < std::chrono::seconds (1)) {
                    continue;
                }
                last_sweep = now;
                for (const auto &listener : _listeners) {
                    if (listener) {
                        listener->sweep_liveness_once ();
                    }
                }
            }
        });
    }
}

void stream_host_service_t::notify_sessions_closing (stream_close_reason_t reason,
                                                     std::string_view diagnostic) noexcept
{
    for (const auto &listener : _listeners) {
        if (listener) {
            try {
                listener->notify_sessions_closing (reason, diagnostic);
            }
            catch (...) {
            }
        }
    }
}

void stream_host_service_t::force_close_sessions (stream_close_reason_t reason,
                                                  std::string_view diagnostic) noexcept
{
    for (const auto &listener : _listeners) {
        if (listener) {
            try {
                listener->force_close_sessions (reason, diagnostic);
            }
            catch (...) {
            }
        }
    }
}

bool stream_host_service_t::drain_sessions_until (
  std::chrono::steady_clock::time_point deadline) noexcept
{
    for (const auto &listener : _listeners) {
        if (!listener)
            continue;
        try {
            listener->begin_drain_sessions ();
        }
        catch (...) {
            return false;
        }
    }
    while (std::chrono::steady_clock::now () < deadline) {
        bool drained = true;
        for (const auto &listener : _listeners) {
            if (listener && listener->active_session_count () != 0) {
                drained = false;
                break;
            }
        }
        if (drained)
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return std::all_of (_listeners.begin (), _listeners.end (), [] (const auto &listener) {
        return !listener || listener->active_session_count () == 0;
    });
}

void stream_host_service_t::request_stop () noexcept
{
    _stop.store (true, std::memory_order_release);
    for (auto &listener : _listeners) {
        listener->request_stop ();
    }
}

void stream_host_service_t::stop () noexcept
{
    request_stop ();
    if (_liveness_thread.joinable ()) {
        _liveness_thread.join ();
    }
    for (auto &listener : _listeners) {
        listener->stop_connections ();
    }
    for (auto &thread : _threads) {
        if (thread.joinable ()) {
            thread.join ();
        }
    }
    _threads.clear ();
    for (auto &listener : _listeners) {
        listener->stop_connections ();
    }
    _listeners.clear ();
    _services = nullptr;
}

} // namespace zlink::framework::runtime
