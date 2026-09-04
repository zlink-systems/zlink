/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/streams/stream_host_service.hpp"
#include "runtime/transport/listener_identity.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/execution/state_lane.hpp"

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/eventing/runtime_wake_timer.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/timers/async_delay.hpp"

#include <nlohmann/json.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/stream_packet.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <unordered_map>
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

stream_host_core_test_faults_t &stream_host_core_test_faults () noexcept
{
    static stream_host_core_test_faults_t faults;
    return faults;
}

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

/* One listener-wide turn scheduler owns the receive cursor for all externally
 * accepted STREAM connections. Workers release the lease before a synchronous
 * Asio read may wait for more bytes, so one partial frame cannot block another
 * connection. The read itself remains the authority for EOF and transport
 * errors; an availability-byte probe cannot represent an orderly close. */
class stream_receive_scheduler_t final
{
    struct slot_t
    {
        explicit slot_t (std::uint64_t sequence_) : sequence (sequence_) {}

        std::uint64_t sequence;
        bool granted = false;
        bool closed = false;
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

    stream_receive_scheduler_t () = default;

    ~stream_receive_scheduler_t () { stop (); }

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

    lease_t register_connection ()
    {
        std::shared_ptr<slot_t> slot;
        {
            std::lock_guard lock (_mutex);
            if (_stopped) {
                return {};
            }
            slot = std::make_shared<slot_t> (_next_sequence++);
            _slots.push_back (slot);
        }
        _changed.notify_one ();
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
        _changed.notify_all ();
    }

    void stop () noexcept
    {
        request_stop ();
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

  private:
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
        _changed.notify_one ();
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
        _changed.notify_one ();
    }

    void run () noexcept
    {
        for (;;) {
            std::vector<std::shared_ptr<slot_t>> candidates;
            {
                std::lock_guard lock (_mutex);
                if (_stopped) {
                    break;
                }
                for (const auto &slot : _slots) {
                    if (!slot->closed && !slot->granted) {
                        candidates.push_back (slot);
                    }
                }
            }
            if (candidates.empty ()) {
                std::unique_lock lock (_mutex);
                _changed.wait (lock, [this] {
                    return _stopped || std::any_of (
                             _slots.begin (), _slots.end (), [] (const auto &slot) {
                                 return !slot->closed && !slot->granted;
                             });
                });
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
                    if (slot->closed || slot->granted) {
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
    bool _stopped = false;
    std::thread _thread;
    std::condition_variable _changed;
};

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
    std::size_t separator = endpoint.rfind (':');
    std::size_t host_end = separator;
    if (endpoint.size () > host_start && endpoint[host_start] == '[') {
        const auto closing = endpoint.find (']', host_start + 1);
        if (closing == std::string::npos || closing + 1 >= endpoint.size ()
            || endpoint[closing + 1] != ':') {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM tcp endpoint must be tcp://host:port");
        }
        separator = closing + 1;
        host_end = closing;
    }
    if (separator == std::string::npos || separator <= host_start
        || separator + 1 >= endpoint.size () || host_end <= host_start) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM tcp endpoint must be tcp://host:port");
    }
    const auto bracketed = endpoint[host_start] == '[';
    return {endpoint.substr (host_start + (bracketed ? 1 : 0),
                             host_end - host_start - (bracketed ? 1 : 0)),
            endpoint.substr (separator + 1)};
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

void validate_stream_listener_identity (
  const stream_snapshot_t &stream,
  const std::optional<std::string> &advertise_host)
{
    const auto endpoint = parse_stream_endpoint (stream);
    if (advertise_host && transport::is_wildcard_host (*advertise_host)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "STREAM advertise host must be a remotely reachable, non-wildcard host: "
            + *advertise_host);
    }
    if (transport::is_wildcard_host (endpoint.host) && !advertise_host) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "STREAM wildcard bind host requires an advertise host: " + stream.name);
    }
}

std::string stream_listener_advertised_endpoint (
  std::string bound_endpoint,
  const std::optional<std::string> &advertise_host)
{
    std::string scheme = "tcp://";
    if (const auto separator = bound_endpoint.find ("://");
        separator != std::string::npos) {
        scheme = bound_endpoint.substr (0, separator + 3);
        bound_endpoint.erase (0, separator + 3);
    }
    const auto port_separator = bound_endpoint.rfind (':');
    if (port_separator == std::string::npos
        || port_separator + 1 >= bound_endpoint.size ())
        return {};
    const auto port = bound_endpoint.substr (port_separator + 1);
    if (port == "0")
        return {};
    auto bound_host = bound_endpoint.substr (0, port_separator);
    if (bound_host.size () >= 2 && bound_host.front () == '['
        && bound_host.back () == ']')
        bound_host = bound_host.substr (1, bound_host.size () - 2);
    const auto host = advertise_host ? *advertise_host : bound_host;
    const auto formatted_host = transport::bracket_ipv6_host (host);
    return transport::normalize_endpoint (scheme + formatted_host + ":" + port);
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

void trace_stream_host_enabled (
  std::string_view stage,
  const stream_snapshot_t &stream,
  std::optional<stream_header_t> header = std::nullopt,
  std::string_view detail = {})
{
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

// Avoid building detail strings or copying an optional header unless the
// explicitly requested low-level diagnostic trace is enabled.
#define trace_stream_host(...)                                                  \
    do {                                                                        \
        if (stream_trace_enabled ())                                            \
            trace_stream_host_enabled (__VA_ARGS__);                            \
    } while (false)

struct stream_async_operation_state_t : std::enable_shared_from_this<stream_async_operation_state_t>
{
    explicit stream_async_operation_state_t (asio::io_context &io) : timer (io) {}

    asio::steady_timer timer;
    std::function<void ()> cancel;
    std::mutex error_mutex;
    boost::system::error_code error;
    std::atomic_bool completed{false};
    std::atomic_bool cancel_requested{false};

    void arm_timer (const std::atomic_bool &stop,
                    const std::atomic_bool *connection_stop)
    {
        timer.expires_after (std::chrono::milliseconds (100));
        const auto weak_state = weak_from_this ();
        timer.async_wait ([weak_state, &stop, connection_stop] (
                            const boost::system::error_code &timer_error) {
            const auto state = weak_state.lock ();
            if (!state || timer_error || state->completed.load (std::memory_order_acquire)) {
                return;
            }
            const auto stopped = stop.load (std::memory_order_acquire)
                                 || (connection_stop
                                     && connection_stop->load (std::memory_order_acquire));
            if (stopped) {
                if (!state->cancel_requested.exchange (true, std::memory_order_acq_rel)) {
                    state->cancel ();
                }
                return;
            }
            state->arm_timer (stop, connection_stop);
        });
    }

    void complete (const boost::system::error_code &completion_error)
    {
        {
            const std::lock_guard<std::mutex> lock (error_mutex);
            error = completion_error;
        }
        completed.store (true, std::memory_order_release);
        boost::system::error_code ignored;
        timer.cancel (ignored);
    }

    boost::system::error_code result ()
    {
        const std::lock_guard<std::mutex> lock (error_mutex);
        return error;
    }
};

bool stream_stop_requested (const std::atomic_bool &stop,
                            const std::atomic_bool *connection_stop) noexcept
{
    return stop.load (std::memory_order_acquire)
           || (connection_stop && connection_stop->load (std::memory_order_acquire));
}

template <typename Start, typename Cancel>
boost::system::error_code run_stream_async_operation (asio::io_context &io,
                                                        const std::atomic_bool &stop,
                                                        Start &&start,
                                                        Cancel &&cancel,
                                                        const std::atomic_bool *connection_stop = nullptr)
{
    if (stream_stop_requested (stop, connection_stop)) {
        return asio::error::operation_aborted;
    }
    auto state = std::make_shared<stream_async_operation_state_t> (io);
    state->cancel = std::forward<Cancel> (cancel);
    state->arm_timer (stop, connection_stop);
    std::forward<Start> (start) ([state] (const boost::system::error_code &error, std::size_t) {
        state->complete (error);
    });
    io.restart ();
    while (!state->completed.load (std::memory_order_acquire)) {
        if (io.run_one () == 0) {
            io.restart ();
        }
    }
    return state->result ();
}

void cancel_stream (tcp::socket &socket) noexcept
{
    boost::system::error_code ignored;
    socket.cancel (ignored);
}

void cancel_stream (websocket_stream_t &stream) noexcept
{
    cancel_stream (stream.next_layer ());
}

template <typename TStream> void cancel_stream (TStream &stream) noexcept
{
    boost::system::error_code ignored;
    stream.lowest_layer ().cancel (ignored);
}

void close_stream (tcp::socket &socket) noexcept
{
    boost::system::error_code ignored;
    socket.cancel (ignored);
    socket.close (ignored);
}

void close_stream (websocket_stream_t &stream) noexcept
{
    close_stream (stream.next_layer ());
}

template <typename TStream> void close_stream (TStream &stream) noexcept
{
    close_stream (stream.lowest_layer ());
}

template <typename TStream>
std::vector<std::uint8_t> read_exact (TStream &socket,
                                      std::size_t size,
                                      asio::io_context &io,
                                      const std::atomic_bool &stop,
                                      const std::atomic_bool *connection_stop)
{
    std::vector<std::uint8_t> bytes (size);
    if (bytes.empty ()) {
        return bytes;
    }
    const auto error = run_stream_async_operation (
      io, stop,
      [&] (auto completion) {
          asio::async_read (socket, asio::buffer (bytes), asio::transfer_exactly (size),
                            std::move (completion));
      },
      [&socket] { cancel_stream (socket); },
      connection_stop);
    if (error) {
        throw boost::system::system_error (error);
    }
    return bytes;
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

} // namespace

class stream_host_service_t::listener_t
{
  public:
    listener_t (detail::stream_runtime_t runtime,
                stream_snapshot_t stream,
                std::optional<std::string> advertise_host,
                detail::stream_session_factory_t session_factory,
                service_provider_t &services,
                std::atomic_bool &stop,
                std::shared_ptr<std::atomic_bool> drain_flag,
                std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring,
                std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
                std::shared_ptr<application_job_queue_t> application_jobs,
                std::chrono::milliseconds session_replacement_callback_timeout) :
        _runtime (std::move (runtime)),
        _stream (std::move (stream)),
        _advertise_host (std::move (advertise_host)),
        _session_factory (std::move (session_factory)),
        _services (&services),
        _stop (&stop),
        _drain_flag (std::move (drain_flag)),
        _monitoring (std::move (monitoring)),
        _mesh_node (std::move (mesh_node)),
        _application_jobs (std::move (application_jobs)),
        _session_replacement_callback_timeout (session_replacement_callback_timeout),
        _acceptor (_io),
        _accept_retry_timer (_io)
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

    struct replacement_session_state_t
    {
        struct actor_binding_t
        {
            std::uint64_t object_generation = 0;
            std::uint64_t binding_generation = 0;
        };

        std::mutex gate;
        zlink::routing_id_t session_rid;
        stream_t stream;
        std::map<std::string, actor_binding_t> actor_bindings;
        std::atomic_bool closing{false};
        bool accepting_replacements = true;
        std::function<void ()> close_connection;
        detail::stream_runtime_t::actor_binding_replaced_dispatch_t
          dispatch_replacement;
        detail::actor_gateway_runtime_t actor_gateway;
        std::shared_ptr<detail::bound_session_replacement_handler_t>
          handler;

        replacement_session_state_t (
          zlink::routing_id_t session_rid_,
          stream_t stream_,
          std::function<void ()> close_connection_,
          detail::stream_runtime_t::actor_binding_replaced_dispatch_t
            dispatch_replacement_,
          detail::actor_gateway_runtime_t actor_gateway_) :
            session_rid (std::move (session_rid_)),
            stream (std::move (stream_)),
            close_connection (std::move (close_connection_)),
            dispatch_replacement (std::move (dispatch_replacement_)),
            actor_gateway (std::move (actor_gateway_))
        {
        }

        ~replacement_session_state_t ()
        {
            deactivate ();
        }

        void deactivate () noexcept
        {
            std::shared_ptr<detail::bound_session_replacement_handler_t>
              registered;
            {
                const std::lock_guard lock (gate);
                accepting_replacements = false;
                registered = std::move (handler);
            }
            if (!registered)
                return;
            try {
                actor_gateway.unregister_bound_session_replacement_handler (
                  session_rid, registered);
            }
            catch (...) {
            }
        }

        task_t<void> dispatch_if_active (stream_t &dispatch_stream,
                                         std::string actor_id)
        {
            detail::stream_runtime_t::actor_binding_replaced_dispatch_t
              dispatch;
            {
                const std::lock_guard lock (gate);
                if (!accepting_replacements || !dispatch_replacement) {
                    return task_t<void> (result_t<void>::success ());
                }
                dispatch = dispatch_replacement;
            }
            return dispatch (dispatch_stream, std::move (actor_id));
        }

        bool exact (
          const runtime::protocol::bound_session_replaced_t &replacement)
        {
            const auto &retired = replacement.retired_session;
            const auto found = actor_bindings.find (
              replacement.actor_authority.actor_id);
            return found != actor_bindings.end ()
                   && found->second.object_generation
                        == replacement.actor_authority.object_generation
                   && found->second.binding_generation
                        == retired.retired_binding_generation;
        }
    };

    /* Async packet dispatch may outlive the socket reader. Keep the service
     * scope, session object and Actor manager together until the serial
     * dispatch queue has completed its last callback. */
    struct stream_connection_state_t
    {
        stream_connection_state_t (detail::service_scope_t scope,
                                   packet_stream_session_t *session,
                                   stream_t stream,
                                   session_actor_manager_t *actors,
                                   runtime::stateful::stream_session_registry_t *registry,
                                   std::optional<runtime::stateful::stream_connection_t>
                                     transport_connection) :
            scope (std::move (scope)), session (session),
            stream (std::move (stream)),
            actors (actors),
            registry (registry),
            transport_connection (std::move (transport_connection))
        {
        }

        ~stream_connection_state_t ()
        {
            if (replacement) {
                replacement->deactivate ();
            }
            if (actors != nullptr) {
                detail::session_actor_manager_access_t::disconnect (*actors);
            }
            if (registry != nullptr && transport_connection) {
                (void) registry->close (*transport_connection);
            }
            scope.close ();
        }

        stream_connection_state_t (const stream_connection_state_t &) = delete;
        stream_connection_state_t &operator= (const stream_connection_state_t &) = delete;

        detail::service_scope_t scope;
        packet_stream_session_t *session = nullptr;
        stream_t stream;
        session_actor_manager_t *actors = nullptr;
        runtime::stateful::stream_session_registry_t *registry = nullptr;
        std::optional<runtime::stateful::stream_connection_t>
          transport_connection;
        std::shared_ptr<replacement_session_state_t> replacement;
    };

    /* zlink.stream.connections.* (runtime-metrics §4.1): session accept and
     * close on the server edge; reconnect attempts belong to the connector. */
    void register_active_stream (const stream_t &stream,
                                 std::shared_ptr<session_liveness_t> liveness,
                                 std::function<void ()> force_close)
    {
        _active_streams_lane
          .run ([this, stream, liveness = std::move (liveness),
                 force_close = std::move (force_close)] () mutable {
              _active_streams.push_back (
                active_session_t{stream, std::move (liveness), std::move (force_close)});
          })
          .get ();
    }

    void unregister_active_stream (const stream_t &stream)
    {
        _active_streams_lane
          .run ([this, &stream] {
              for (auto it = _active_streams.begin (); it != _active_streams.end (); ++it) {
                  if (it->stream.session_id () == stream.session_id ()) {
                      _active_streams.erase (it);
                      break;
                  }
              }
          })
          .get ();
    }

    std::size_t active_session_count ()
    {
        const auto active =
          _active_streams_lane.run ([this] { return _active_streams.size (); }).get ();
        const std::lock_guard lock (_core_sessions_mutex);
        return active + _core_sessions.size () + _retired_core_sessions.size ();
    }

    void begin_drain_sessions ()
    {
        force_close_sessions (stream_close_reason_t::server_drain, "server drain");
        close_core_sessions ("server_drain", stream_close_reason_t::server_drain,
                             "server drain");
    }

    void notify_sessions_closing (stream_close_reason_t reason, std::string_view diagnostic)
    {
        auto sessions = _active_streams_lane.run ([this] { return _active_streams; }).get ();
        for (auto &entry : sessions) {
            _runtime.send_session_closing (entry.stream, reason, diagnostic);
        }
    }

    /* Forced teardown (graceful-drain-handoff §7): the notified sessions are
     * closed so the reason the client keeps is the forced one — a lingering
     * connection would later be re-labeled by the liveness loop. */
    void force_close_sessions (stream_close_reason_t reason, std::string_view diagnostic)
    {
        auto sessions = _active_streams_lane.run ([this] { return _active_streams; }).get ();
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
        auto sessions = _active_streams_lane.run ([this] { return _active_streams; }).get ();
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
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ())
          .report_lazy ([&] {
              message_dispatch_error_event_t event{
                dispatch_error_surface_t::stream_session,
                header.kind () == stream_message_kind_t::request
                  ? dispatch_message_kind_t::request
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
                header.correlation_id ()
                  ? std::make_optional (std::string (*header.correlation_id ()))
                  : std::nullopt,
                dispatched.error () != nullptr
                  ? std::make_exception_ptr (*dispatched.error ())
                  : std::exception_ptr{}};
              if (auto flow = header.flow_id ()) {
                  event.flow_id = std::string (*flow);
                  event.flow_origin = header.flow_origin ();
              }
              return event;
          });
    }

    void run ()
    {
        validate_stream_listener_identity (_stream, _advertise_host);
        _receive_scheduler.start ();
        if (_mesh_node && !stream_uses_tls (_stream) && !stream_uses_websocket (_stream)) {
            run_core_stream ();
            return;
        }
        if (stream_uses_tls (_stream)) {
            configure_tls_context ();
        }
        auto endpoint = parse_stream_endpoint (_stream);
        tcp::resolver resolver (_io);
        boost::system::error_code error;
        const auto wildcard = endpoint.host == "*";
        const auto resolve_host = wildcard ? std::string () : endpoint.host;
        const auto resolve_flags = wildcard ? tcp::resolver::flags::passive
                                            : tcp::resolver::flags ();
        const auto endpoints = resolver.resolve (resolve_host, endpoint.port, resolve_flags, error);
        if (error || endpoints.begin () == endpoints.end ()) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "STREAM endpoint address resolution failed: "
                + (error ? error.message () : "no addresses"));
        }
        for (const auto &candidate : endpoints) {
            _acceptor.open (candidate.endpoint ().protocol (), error);
            if (!error) {
                _acceptor.set_option (tcp::acceptor::reuse_address (true), error);
            }
            if (!error) {
                _acceptor.bind (candidate.endpoint (), error);
            }
            if (!error) {
                _acceptor.listen (asio::socket_base::max_listen_connections, error);
            }
            if (!error) {
                break;
            }
            boost::system::error_code ignored;
            _acceptor.close (ignored);
        }
        if (error) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "STREAM endpoint bind/listen failed: " + error.message ());
        }
        if (const auto endpoint = socket_endpoint_text (_acceptor.local_endpoint ()))
            _bound_endpoint = *endpoint;
        start_boost_accept ();
        mark_started ();
        _io.run ();
    }

    void run_guarded () noexcept
    {
        try {
            run ();
        }
        catch (const std::exception &error) {
            mark_start_failed (error.what ());
        }
        catch (...) {
            mark_start_failed ("STREAM listener failed with an unknown exception: "
                               + _stream.name);
        }
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

    std::string listener_endpoint () const
    {
        std::lock_guard lock (_ready_mutex);
        return stream_listener_advertised_endpoint (
          _bound_endpoint, _advertise_host);
    }

    const std::string &name () const noexcept
    {
        return _stream.name;
    }

    void request_stop () noexcept
    {
        _core_wake_timer.signal ();
        _receive_scheduler.request_stop ();
        asio::post (_io, [this] {
            boost::system::error_code ignored;
            _acceptor.close (ignored);
            _accept_retry_timer.cancel (ignored);
        });
    }

    void stop_connections () noexcept
    {
        trace_stream_host ("stop-connections-begin", _stream);
        std::vector<std::function<void ()>> cancellers;
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            cancellers.reserve (_sockets.size ());
            for (const auto &entry : _sockets) {
                cancellers.push_back (entry.second);
            }
        }
        for (auto &cancel : cancellers) {
            cancel ();
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
        std::shared_ptr<replacement_session_state_t> replacement;
        std::mutex gate;
        bool connected = false;
        bool closing = false;
        bool finalized = false;

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

    /* Coroutine invariant (session-reconnect-and-coroutine-lifetime doc):
     * parameters are taken by value. Callers pass stack locals and
     * temporaries (send_core_error_frame's error_header/payload) that die
     * while this coroutine may still be suspended at the tail await; the
     * message copy is a cheap handle copy, not a buffer copy. */
    task_t<void> send_core_frame (zlink::routing_id_t rid,
                                  stream_header_t header,
                                  zlink::message_t payload,
                                  std::optional<std::chrono::milliseconds> timeout = std::nullopt)
    {
        if (header.kind () == stream_message_kind_t::error
            && stream_host_core_test_faults ().fail_core_error_frame_send.load (
                 std::memory_order_acquire)) {
            /* Test-only: forces the pre-suspension throw shape (same as an
             * encode failure or a stopped Core socket below) so tests can pin
             * the error-observation path without a wire-level race. */
            throw framework_exception_t (
              framework_error_kind_t::unavailable,
              "test fault: Core STREAM error frame send");
        }
        auto encoded = _runtime.encode_frame (header, payload);
        if (!encoded) {
            throw framework_exception_t (
              encoded.error_kind (),
              encoded.error () ? encoded.error ()->what () : "STREAM frame encode failed");
        }
        auto frame = zlink::message_t::from (std::move (encoded.value ()));
        const auto frame_bytes = frame.size ();

        std::optional<zlink::async_result_t<void>> pending;
        {
            std::lock_guard socket_lock (_core_socket_mutex);
            if (!_core_socket) {
                throw framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Core STREAM socket is stopped");
            }
            const auto configured_timeout =
              _core_socket->options ().send_timeout ();
            if (timeout)
                _core_socket->options ().send_timeout (*timeout);
            try {
                pending.emplace (
                  _core_socket->send (rid).message (std::move (frame)).async ());
            }
            catch (...) {
                if (timeout)
                    _core_socket->options ().send_timeout (configured_timeout);
                throw;
            }
            if (timeout)
                _core_socket->options ().send_timeout (configured_timeout);
        }
        co_await std::move (*pending);
        trace_stream_host (
          "core-write", _stream, header,
          "rid=" + rid.to_hex () + " bytes=" + std::to_string (frame_bytes)
            + " result=admitted");
        co_return;
    }

    /* Writes the error reply for a rejected request. The optional
     * `completed` continuation runs at the error-frame task terminal (or
     * immediately when no error frame applies); callers use it to sequence
     * a follow-up close behind the error write so the peer observes
     * error -> close order. The continuation may run inline in the
     * submitting context (pre-suspension throw), so it must only record
     * intent (request_core_peer_disconnect), never enter a close. */
    void send_core_error_frame (const zlink::routing_id_t &rid,
                                const stream_header_t &request_header,
                                const result_t<void> &error,
                                std::function<void ()> completed = {}) noexcept
    {
        if (request_header.kind () != stream_message_kind_t::request
            || !request_header.request_seq ()) {
            if (completed) {
                completed ();
            }
            return;
        }

        stream_header_t error_header (stream_message_kind_t::error, stream_codec_t::json,
                                      stream_header_flags_t::has_request_seq,
                                      request_header.request_seq (), "", {});
        if (auto correlation = request_header.correlation_id ()) {
            error_header.with_correlation_id (std::string (*correlation));
        }
        auto send = send_core_frame (
          rid, error_header, stream_error_payload (error));
        detail::observe_task_completion (
          send, [this, rid, completed = std::move (completed)] (
                  const result_t<void> &result) {
              if (!result) {
                  /* This observer runs inline in the submitting context when
                   * send_core_frame throws before its first suspension (the
                   * task completes eagerly). That context may hold a session
                   * gate, and cpp-internals forbids completing work under a
                   * session mutex, so only the close intent is recorded here.
                   * The Core loop drains it with no session lock held;
                   * begin_core_session_close is idempotent, so a duplicate
                   * request is a no-op. */
                  request_core_peer_disconnect (rid, "error_reply_failed");
              }
              if (completed) {
                  completed ();
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
            _core_socket->options ().recv_mode (zlink::stream_recv_mode_t::packet);
            _core_monitor = std::make_unique<zlink::socket_monitor_t> (
              _core_socket->monitor_open (zlink::monitor_event::disconnected));
            _core_socket->bind (_stream.bind_endpoint);
            _bound_endpoint = _core_socket->options ().last_endpoint ();
            mark_started ();
            zlink::poller_t poller;
            poller.add (
              *_core_socket,
              zlink::poll_event_flag_t::pollin
                | zlink::poll_event_flag_t::pollout
                | zlink::poll_event_flag_t::pollcompletion,
              1);
            poller.add (*_core_monitor, zlink::poll_event_flag_t::pollin, 2);
            _core_wake_timer.attach (poller);
            _core_application_supply =
              std::make_unique<application_supply_slot_t> (
                _application_jobs,
                [this] { _core_wake_timer.signal (); });
            try {
                while (!_stop->load (std::memory_order_acquire)) {
                    /* Deferred close intents recorded by completion
                     * observers (request_core_peer_disconnect) are executed
                     * here, on the Core loop with no session lock held. */
                    drain_pending_core_disconnects ();
                    receive_batch_budget_t batch;

                    zlink::poll_event_t event;
                    /* The Core poller has no cross-thread close wake contract.
                     * A Core-backed timer is registered in the same poller so
                     * stop is delivered as an event instead of relying on a
                     * platform-specific socket close side effect. */
                    const size_t event_count =
                      poller.wait (&event, 1, std::chrono::milliseconds{-1});
                    if (_stop->load (std::memory_order_acquire))
                        break;
                    if (event_count == 0)
                        continue;
                    if (_core_wake_timer.is_event (event)) {
                        _core_wake_timer.consume ();
                        continue;
                    }

                    const short revents = static_cast<short> (event.revents);
                    const short pollin = static_cast<short> (zlink::poll_event_flag_t::pollin);
                    const short pollerr = static_cast<short> (zlink::poll_event_flag_t::pollerr);
                    if ((revents & pollerr) != 0 && (revents & pollin) == 0)
                        break;

                    if (event.slot == 2) {
                        for (;;) {
                            auto monitor_event =
                              _core_monitor->recv (zlink::recv_flags_t::dontwait);
                            if (!monitor_event)
                                break;
                            if (monitor_event->event
                                  == zlink::monitor_event::disconnected
                                && monitor_event->routing_id) {
                                close_core_session (
                                  *monitor_event->routing_id, "client_close");
                            }
                        }
                        continue;
                    }
                    if (event.slot != 1 || (revents & pollin) == 0)
                        continue;

                    // Reserve Framework capacity before pulling exactly one
                    // Core packet. Leaving the packet in Core while capacity
                    // is unavailable preserves Core RCVHWM backpressure.
                    _core_application_supply->ensure_waiter ();
                    auto reserved = _core_application_supply->take ();
                    if (!reserved)
                        continue;

                    // POLLIN guarantees that a receive is worthwhile. DONTWAIT
                    // also makes a stale readiness event harmless: it yields
                    // ZLINK_RECV_NO_DATA and the loop re-arms the poller.
                    zlink::stream_packet_t packet;
                    try {
                        if (!_core_socket->recv_packet (
                              packet, zlink::recv_flags_t::dontwait)) {
                            continue;
                        }
                    }
                    catch (const zlink::recv_error_t &error) {
                        if (_stop->load (std::memory_order_acquire)
                            || error.result () == zlink::recv_result_t::terminated) {
                            break;
                        }
                        if (error.internal_errno () == EMSGSIZE) {
                            trace_stream_host ("core-recv-size-rejected", _stream,
                                               std::nullopt,
                                               "errno=EMSGSIZE limit="
                                                 + std::to_string (_stream.max_message_size));
                            continue;
                        }
                        throw;
                    }
                    const auto routing_id = packet.routing_id ();
                    const auto header_bytes = packet.header ().size ();
                    const auto payload_bytes = packet.body ().size ();
                    process_core_packet (packet, std::move (*reserved), batch);
                    if (routing_id) {
                        trace_stream_host ("core-recv", _stream, std::nullopt,
                                           "rid=" + routing_id->to_hex ()
                                             + " header_bytes="
                                             + std::to_string (header_bytes)
                                             + " payload_bytes="
                                             + std::to_string (payload_bytes));
                    }
                }
            }
            catch (...) {
                if (_core_application_supply) {
                    _core_application_supply->close ();
                    _core_application_supply.reset ();
                }
                _core_wake_timer.detach ();
                throw;
            }
            if (_core_application_supply) {
                _core_application_supply->close ();
                _core_application_supply.reset ();
            }
            _core_wake_timer.detach ();
        }
        catch (const std::exception &error) {
            mark_start_failed (error.what ());
        }
        drain_pending_core_disconnects ();
        close_core_sessions ();
        if (_core_monitor) {
            _core_monitor->close ();
            _core_monitor.reset ();
        }
        {
            std::lock_guard lock (_core_socket_mutex);
            if (_core_socket) {
                (void) _core_socket->close ();
                _core_socket.reset ();
            }
        }
    }

    bool begin_actor_binding_replacement (
      const std::shared_ptr<replacement_session_state_t> &state,
      const runtime::protocol::bound_session_replaced_t &replacement)
    {
        std::unique_lock admission_lock (state->gate);
        if (!state->accepting_replacements
            || state->closing.load (std::memory_order_acquire)) {
            return false;
        }
        const auto local = _mesh_node->native_node ().status ();
        const auto &retired = replacement.retired_session;
        if (retired.session_owner_node_routing_id
              != local.routing_id ().to_bytes ()
            || retired.session_owner_node_generation
                 != local.lifecycle_generation ()
            || retired.session_owner_id
                 != local.routing_id ().to_string ()
            || retired.session_owner_lease_generation
                 != local.lifecycle_generation ()
            || retired.session_routing_id
                 != state->session_rid.to_bytes ()) {
            trace_stream_host (
              "actor-binding-replacement-rejected", _stream,
              std::nullopt, "reason=session-fence-mismatch");
            return false;
        }
        if (!state->exact (replacement)) {
            trace_stream_host (
              "actor-binding-replacement-rejected", _stream,
              std::nullopt, "reason=binding-fence-mismatch");
            return false;
        }
        state->closing.store (true, std::memory_order_release);
        trace_stream_host (
          "actor-binding-replacement-admitted", _stream,
          std::nullopt, "actor="
            + replacement.actor_authority.actor_id);

        const auto replacement_copy = replacement;
        const auto weak =
          std::weak_ptr<replacement_session_state_t> (state);
        auto deadline = std::make_shared<asio::steady_timer> (
          asio::system_executor {});
        deadline->expires_after (_session_replacement_callback_timeout);
        deadline->async_wait (
          [weak, replacement_copy, deadline] (
            const boost::system::error_code &error) {
              if (error)
                  return;
              const auto current = weak.lock ();
              if (!current)
                  return;
              std::function<void ()> close;
              {
                  const std::lock_guard lock (current->gate);
                  if (!current->closing.load (
                        std::memory_order_acquire)
                      || !current->exact (replacement_copy))
                      return;
                  close = current->close_connection;
              }
              if (close)
                  close ();
          });

        const auto completed =
          [weak, replacement_copy, deadline] (
            const result_t<void> &) {
              boost::system::error_code ignored;
              deadline->cancel (ignored);
              auto delay = std::make_shared<asio::steady_timer> (
                asio::system_executor {});
              delay->expires_after (std::chrono::milliseconds (100));
              delay->async_wait (
                [weak, replacement_copy, delay] (
                  const boost::system::error_code &error) {
                    if (error)
                        return;
                    const auto current = weak.lock ();
                    if (!current)
                        return;
                    std::function<void ()> close;
                    {
                        const std::lock_guard lock (current->gate);
                        if (!current->closing.load (
                              std::memory_order_acquire)
                            || !current->exact (replacement_copy))
                            return;
                        close = current->close_connection;
                    }
                    if (close)
                        close ();
                });
          };
        const auto submitted =
          _runtime.dispatch_actor_binding_replaced_async (
            state->stream,
            replacement_copy.actor_authority.actor_id,
            [weak] (stream_t &dispatch_stream,
                    std::string actor_id) {
                const auto current = weak.lock ();
                if (!current) {
                    return task_t<void> (result_t<void>::success ());
                }
                return current->dispatch_if_active (
                  dispatch_stream, std::move (actor_id));
            },
            completed);
        admission_lock.unlock ();
        if (!submitted)
            completed (submitted);
        return true;
    }

    std::shared_ptr<replacement_session_state_t>
    register_replacement_session (
      const zlink::routing_id_t &session_rid,
      const stream_t &stream,
      std::function<void ()> close_connection,
      detail::stream_runtime_t::actor_binding_replaced_dispatch_t
        dispatch_replacement)
    {
        auto actor_gateway =
          _services->get_required<detail::actor_gateway_runtime_t> ();
        auto state = std::make_shared<replacement_session_state_t> (
          session_rid, stream, std::move (close_connection),
          std::move (dispatch_replacement), actor_gateway);
        auto registered =
          actor_gateway.register_bound_session_replacement_handler (
            session_rid,
            [this, weak = std::weak_ptr<replacement_session_state_t> (
                     state)] (
              const runtime::protocol::bound_session_replaced_t &replacement) {
                if (const auto current = weak.lock ())
                    return begin_actor_binding_replacement (
                      current, replacement);
                return false;
            });
        {
            const std::lock_guard lock (state->gate);
            state->handler = std::move (registered);
        }
        return state;
    }

    task_t<void> bind_actor_session (
      runtime::stateful::stream_connection_t transport_connection,
      zlink::routing_id_t session_rid,
      actor_ref_t actor,
      std::shared_ptr<replacement_session_state_t> replacement,
      std::uint64_t binding_generation,
      detail::application_actor_session_bind_attempt_t bind_attempt =
        detail::application_actor_session_bind_attempt_t::initial,
      std::optional<std::chrono::steady_clock::time_point>
        retry_deadline = std::nullopt)
    {
        const auto _mesh_node = this->_mesh_node;
        auto *const services = _services;
        auto *const io = &_io;
        const auto binding_deadline = retry_deadline.value_or (
          std::chrono::steady_clock::now () + std::chrono::seconds (5));
        if (bind_attempt
              == detail::application_actor_session_bind_attempt_t::retry
            && std::chrono::steady_clock::now () >= binding_deadline) {
            throw framework_exception_t (
              framework_error_kind_t::deadline_exceeded,
              "Actor session binding deadline elapsed");
        }
        if (!_mesh_node) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "STREAM Actor dispatch MeshNode is not started");
        }
        const auto local_node = _mesh_node->routing_id ();
        if (!local_node) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "STREAM Actor dispatch MeshNode has no routing id");
        }
        auto actor_route =
          _mesh_node->resolve_application_actor_route (actor);
        if (!actor_route) {
            throw framework_exception_t (
              framework_error_kind_t::not_found,
              "Actor session binding route is unavailable");
        }
        const auto previous_binding =
          _mesh_node->native_node ().sessions ().current_binding (
            std::string (actor.actor_id ().value ()));
        /* A re-bind (a previous binding exists -- the reconnect window) may
         * be riding a stale cached route after its one-way command 44 was
         * lost. The former owner can still accept the route during its
         * retransmission window, so actor_not_ready is not a reliable
         * refresh trigger. Re-resolve every re-bind from Store before
         * selecting either the local or remote owner. A first bind keeps its
         * freshly resolved route. */
        if (previous_binding) {
            const auto cached_route = *actor_route;
            actor_route = _mesh_node->refresh_application_actor_route (
              actor, cached_route);
            if (!actor_route) {
                throw framework_exception_t (
                  framework_error_kind_t::not_found,
                  "Actor session binding route refresh failed");
            }
        }

        runtime::stateful::stream_binding_t binding;
        runtime::stateful::stateful_error_t error;
        if (local_node->to_hex () == actor_route->node_rid.to_hex ()) {
            const auto native_actor =
              runtime::host::public_host_runtime_t::remote_actor_ref (
                actor_route->node_rid,
                std::string (actor.actor_id ().value ()),
                actor.object_generation ());
            const auto resolved =
              _mesh_node->native_node ().resolve_actor (native_actor);
            if (!resolved) {
                /* actor_not_ready on the local node: short-backoff retry
                 * bounded by the binding deadline (bind can race the
                 * actor's own admission). */
                const auto remaining =
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    binding_deadline - std::chrono::steady_clock::now ());
                if (detail::can_retry_application_actor_session_bind (
                      detail::application_actor_session_bind_outcome_t::
                        actor_not_ready)
                    && remaining > std::chrono::milliseconds::zero ()) {
                    co_await detail::delay (
                      std::min (remaining, std::chrono::milliseconds (10)));
                    co_await bind_actor_session (
                      transport_connection, session_rid, actor, replacement,
                      binding_generation,
                      detail::application_actor_session_bind_attempt_t::retry,
                      binding_deadline);
                    co_return;
                }
                throw framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Framework STREAM Actor is not ready for session binding");
            }
            const auto verified_actor =
              detail::make_local_application_actor_session_ref (
                *resolved, actor, *actor_route);
            if (!verified_actor) {
                throw verified_actor.error ()
                        ? *verified_actor.error ()
                        : framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Local Actor session binding failed");
            }
            std::tie (error, binding) =
              _mesh_node->native_node ().sessions ().bind_remote (
                transport_connection, verified_actor.value (),
                actor_route->node_generation,
                static_cast<std::uint64_t> (
                  actor_route->owner.lease_generation), true,
                binding_generation);
        } else {
            const runtime::stateful::object_ref_t verified_actor{
              runtime::stateful::object_kind_t::actor,
              std::string (actor.actor_id ().value ()),
              actor.object_generation (),
              actor_route->authority_owner_generation,
              actor_route->mesh_name,
              actor_route->node_rid.to_string ()};
            std::tie (error, binding) =
              _mesh_node->native_node ().sessions ().bind_remote (
                transport_connection, verified_actor,
                actor_route->node_generation,
                static_cast<std::uint64_t> (
                  actor_route->owner.lease_generation), true,
                binding_generation);
        }
        if (error != runtime::stateful::stateful_error_t::none) {
            if (error == runtime::stateful::stateful_error_t::conflict
                && binding_generation != 0 && previous_binding
                && previous_binding->binding_generation
                     > binding_generation) {
                auto actor_gateway =
                  services->get_required<detail::actor_gateway_runtime_t> ();
                actor_gateway.trace_bound_session_send_stage (
                  std::string (actor.actor_id ().value ()),
                  "session_registry_bind_stale_ignored",
                  "session_rid=" + session_rid.to_hex ()
                    + " binding_generation="
                    + std::to_string (binding_generation)
                    + " current_binding_generation="
                    + std::to_string (
                      previous_binding->binding_generation));
            }
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Framework rejected STREAM actor binding");
        }

        result_t<void> recorded = result_t<void>::success ();
        std::optional<detail::application_actor_session_bind_outcome_t>
          retryable_outcome;
        const auto publish_session_route = [&] {
            const auto local_status = _mesh_node->native_node ().status ();
            auto actor_gateway =
              services->get_required<detail::actor_gateway_runtime_t> ();
            auto transition =
              actor_gateway.record_bound_session_route_transition (
                actor,
                detail::actor_bound_session_route_t{
                  *local_node, session_rid,
                  actor.object_generation (),
                  local_status.lifecycle_generation (),
                  actor_route->authority_owner_generation,
                  static_cast<std::uint64_t> (
                    actor_route->owner.lease_generation),
                  binding.binding_generation, 0, 0});
            if (!transition) {
                return result_t<void>::failure (
                  transition.error_kind (),
                  transition.error () ? transition.error ()->what ()
                                      : "bound Session route registration failed");
            }
            actor_gateway.trace_bound_session_send_stage (
              std::string (actor.actor_id ().value ()),
              "session_owner_route_publish",
              "session_rid=" + session_rid.to_hex ()
                + " binding_generation="
                + std::to_string (binding.binding_generation)
                + " replaced="
                + (transition.value ().changed ? "true" : "false"));
            if (transition.value ().current
                && transition.value ().current->binding_generation
                     != binding.binding_generation) {
                actor_gateway.trace_bound_session_send_stage (
                  std::string (actor.actor_id ().value ()),
                  "session_owner_route_publish_stale_ignored",
                  "session_rid=" + session_rid.to_hex ()
                    + " binding_generation="
                    + std::to_string (binding.binding_generation)
                    + " current_binding_generation="
                    + std::to_string (
                      transition.value ().current->binding_generation));
            }
            if (transition.value ().changed
                && transition.value ().previous
                && transition.value ().previous->session_rid
                && transition.value ().previous->node_generation != 0
                && transition.value ().previous->binding_generation != 0) {
                const auto &previous = *transition.value ().previous;
                const runtime::protocol::bound_session_replaced_t notice{
                  runtime::protocol::actor_route_fence_t{
                    std::string (actor.actor_id ().value ()),
                    actor.object_generation (),
                    actor_route->node_rid.to_bytes (),
                    actor_route->node_generation,
                    actor_route->authority_owner_generation,
                    static_cast<std::uint64_t> (
                      actor_route->owner.lease_generation)},
                  runtime::protocol::retired_bound_session_route_fence_t{
                    previous.node_rid.to_bytes (),
                    previous.node_generation,
                    previous.node_rid.to_string (),
                    previous.node_generation,
                    previous.session_rid->to_bytes (),
                    previous.binding_generation}};
                asio::post (
                  *io, [_mesh_node, actor_gateway, notice] () mutable {
                      const auto local_status =
                        _mesh_node->native_node ().status ();
                      if (notice.retired_session
                            .session_owner_node_routing_id
                          == local_status.routing_id ().to_bytes ()) {
                          (void) actor_gateway
                            .dispatch_bound_session_replaced (notice);
                      } else {
                          (void) _mesh_node->native_node ().transport ()
                            .send_bound_session_replaced (
                              notice.retired_session
                                .session_owner_node_routing_id,
                              notice);
                      }
                  });
            }
            return result_t<void>::success ();
        };
        if (local_node->to_hex () == actor_route->node_rid.to_hex ()) {
            recorded = publish_session_route ();
        } else {
            auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds> (
                binding_deadline - std::chrono::steady_clock::now ());
            if (remaining <= std::chrono::milliseconds::zero ()) {
                recorded = result_t<void>::failure (
                  framework_error_kind_t::deadline_exceeded,
                  "Remote Actor session binding deadline elapsed");
            } else {
                const auto remote_binding = co_await
                  _mesh_node->bind_application_actor_session (
                    actor, session_rid, binding.binding_generation,
                    *actor_route, remaining);
                if (remote_binding
                  == detail::application_actor_session_bind_outcome_t::
                    stale_route) {
                    retryable_outcome = remote_binding;
                    recorded = result_t<void>::failure (
                      framework_error_kind_t::invalid_operation,
                      "Remote Actor session binding route is stale");
                } else if (
                  remote_binding
                  == detail::application_actor_session_bind_outcome_t::
                    actor_not_ready) {
                    retryable_outcome = remote_binding;
                    recorded = result_t<void>::failure (
                      framework_error_kind_t::unavailable,
                      "Remote Actor session binding owner is not ready");
                } else {
                    recorded = publish_session_route ();
                }
            }
        }
        if (recorded) {
            auto actor_gateway =
              services->get_required<detail::actor_gateway_runtime_t> ();
            recorded = actor_gateway.record_session_relay_source (
              actor, session_rid, binding.binding_generation);
        }
        if (recorded) {
            if (replacement) {
                const std::lock_guard lock (replacement->gate);
                replacement->actor_bindings.insert_or_assign (
                  std::string (actor.actor_id ().value ()),
                  replacement_session_state_t::actor_binding_t{
                    actor.object_generation (),
                    binding.binding_generation});
            }
            auto retained = _mesh_node->native_node ().sessions ()
                              .complete_route_publish (binding);
            if (!retained) {
                recorded = result_t<void>::failure (
                  framework_error_kind_t::internal_failure,
                  "STREAM Actor route publication lost its binding fence");
            }
            else {
                auto actor_gateway =
                  services->get_required<detail::actor_gateway_runtime_t> ();
                actor_gateway.trace_bound_session_send_stage (
                  std::string (actor.actor_id ().value ()),
                  "session_owner_route_publish_complete",
                  "session_rid=" + session_rid.to_hex ()
                    + " binding_generation="
                    + std::to_string (binding.binding_generation)
                    + " held_pushes="
                    + std::to_string (retained->size ()));
                for (auto &settle : *retained) {
                    asio::post (
                      *io, [settle = std::move (settle)] () mutable {
                          if (settle)
                              settle (true);
                      });
                }
            }
        }
        if (recorded) {
            co_return;
        }

        const auto rollback =
          _mesh_node->native_node ().sessions ().unbind (binding);
        auto restore_error = rollback;
        if (restore_error == runtime::stateful::stateful_error_t::none
            && previous_binding) {
            restore_error = _mesh_node->native_node ().sessions ().restore (
              *previous_binding);
        }
        if (restore_error != runtime::stateful::stateful_error_t::none) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "Framework STREAM actor binding rollback failed");
        }
        if (retryable_outcome
            && detail::can_retry_application_actor_session_bind (
              *retryable_outcome)) {
            const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds> (
                binding_deadline - std::chrono::steady_clock::now ());
            if (remaining <= std::chrono::milliseconds::zero ()) {
                throw framework_exception_t (
                  framework_error_kind_t::deadline_exceeded,
                  "Remote Actor session binding deadline elapsed");
            }
            if (*retryable_outcome
                == detail::application_actor_session_bind_outcome_t::
                  stale_route) {
                /* Invalidate the stale cached route, then wait (capped
                 * ~1s) for the authority to publish a changed route
                 * before re-entering the bind. */
                (void) _mesh_node->refresh_application_actor_route (
                  actor, *actor_route);
                (void) co_await _mesh_node->wait_for_application_actor_route_change (
                  actor, *actor_route,
                  std::min (remaining, std::chrono::milliseconds (1000)));
            } else {
                /* actor_not_ready can be a short target-materialization lag,
                 * but it is also how the former owner reports a route whose
                 * Store watcher has not observed the committed relocation
                 * yet. Refresh before retrying so a reconnect cannot pin the
                 * removed source Actor for the entire binding deadline. */
                (void) _mesh_node->refresh_application_actor_route (
                  actor, *actor_route);
                co_await detail::delay (
                  std::min (remaining, std::chrono::milliseconds (10)));
            }
            co_await bind_actor_session (
              transport_connection, session_rid, actor, replacement,
              binding_generation,
              detail::application_actor_session_bind_attempt_t::retry,
              binding_deadline);
            co_return;
        }
        throw recorded.error ()
                ? *recorded.error ()
                : framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Framework STREAM actor binding failed");
    }

    /* Coroutine: parameters are taken by value because the detached async
     * wrapper's frame unwinds before the first resume. */
    task_t<void> retire_actor_session_bindings (
      runtime::stateful::stream_connection_t connection,
      zlink::routing_id_t session_rid)
    {
        /* Detached coroutine (retire_actor_session_bindings_async): copy the
         * mesh-node handle into the frame so iteration 2+ after a co_await
         * never reads `this` (session-reconnect-and-coroutine-lifetime doc). */
        const auto _mesh_node = this->_mesh_node;
        if (!_mesh_node)
            co_return;
        const auto local = _mesh_node->native_node ().status ();
        const auto bindings = _mesh_node->native_node ().sessions ().bindings (
          connection);
        auto actor_gateway =
          _services->get_required<detail::actor_gateway_runtime_t> ();
        const auto retire_connection =
          "connection_id=" + connection.connection_id
          + " connection_generation="
          + std::to_string (connection.connection_generation);
        for (const auto &binding : bindings) {
            try {
                if (!_mesh_node->native_node ().sessions ()
                       .is_current_for_connection (connection, binding)) {
                    actor_gateway.trace_bound_session_send_stage (
                      binding.actor.key,
                      "session_owner_route_tombstone_stale_ignored",
                      retire_connection
                        + " session_rid=" + session_rid.to_hex ()
                        + " binding_generation="
                        + std::to_string (binding.binding_generation)
                        + " binding_connection_id="
                        + binding.connection.connection_id
                        + " binding_connection_generation="
                        + std::to_string (
                          binding.connection.connection_generation));
                    continue;
                }
                if (binding.actor.node_id
                    == local.routing_id ().to_string ()) {
                    const auto actor = detail::actor_ref_access_t::make (
                      node_rid_t::from_string (binding.actor.node_id), {},
                      binding.actor.key, binding.actor.object_generation);
                    const auto retired = actor_gateway.retire_bound_session_route (
                      actor, local.routing_id (), session_rid,
                      binding.binding_generation);
                    if (!retired)
                        continue;
                }
                else {
                    co_await _mesh_node->retire_application_actor_session (
                      binding, session_rid, std::chrono::seconds (5));
                }
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone",
                  retire_connection
                    + " session_rid=" + session_rid.to_hex ()
                    + " binding_generation="
                    + std::to_string (binding.binding_generation));
            }
            catch (const std::exception &error) {
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone_failed",
                  retire_connection
                    + " session_rid=" + session_rid.to_hex ()
                    + " binding_generation="
                    + std::to_string (binding.binding_generation)
                    + " error=" + error.what ());
            }
        }
        co_return;
    }

    void retire_actor_session_bindings_async (
      runtime::stateful::stream_connection_t connection,
      zlink::routing_id_t session_rid,
      std::function<void ()> completed)
    {
        auto pending = std::make_shared<task_t<void>> (
          retire_actor_session_bindings (connection, session_rid));
        detail::observe_task_completion (
          *pending,
          [pending, completed = std::move (completed)] (
            const result_t<void> &) mutable {
              if (completed)
                  completed ();
          });
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
          _mesh_node->native_node ().sessions ().open (
            key,
            [this, rid] {
                request_core_peer_disconnect (
                  rid, "session_relocation_seal_timeout");
            });
        detail::session_actor_manager_access_t::attach (actors, stream);
        auto created = std::make_shared<core_session_t> (
          std::move (scope), session, actors, std::move (stream),
          std::move (transport_connection));
        auto replacement = register_replacement_session (
          rid, created->stream,
          [this, rid] { disconnect_core_peer (
            rid, "actor_binding_replaced"); },
          [weak = std::weak_ptr<core_session_t> (created)] (
            stream_t &dispatch_stream,
            std::string actor_id) {
              const auto owner = weak.lock ();
              if (!owner || owner->session == nullptr) {
                  return task_t<void> (result_t<void>::success ());
              }
              return owner->session->on_actor_binding_replaced (
                dispatch_stream, std::move (actor_id));
          });
        created->replacement = replacement;
        detail::session_actor_manager_access_t::bind_native (
          actors, [this, transport_connection = created->transport_connection, rid,
                   replacement] (actor_ref_t actor,
                                  std::uint64_t binding_generation) -> task_t<void> {
              return bind_actor_session (
                transport_connection, rid, actor, replacement,
                binding_generation);
          });
        _runtime.attach_transport_writer (
          created->stream,
          [this, rid] (const stream_header_t &header,
                       const zlink::message_t &payload,
                       std::optional<std::chrono::milliseconds> timeout) -> task_t<void> {
              co_await send_core_frame (rid, header, payload, timeout);
          });
        auto connected = _runtime.dispatch_connected_async (
          *created->session, created->stream,
          [this, created, rid] (const result_t<void> &result) {
              if (!result) {
                  //  The client is still connected here: only the server's connect dispatch
                  //  failed. `close_core_session` retires the server-side session and tombstones
                  //  the bound-session routes but neither notifies nor drops the physical
                  //  connection, so the client sees nothing and waits out its own deadline.
                  //  Spec 2.2 requires an observable disconnect, which is what
                  //  `disconnect_core_peer` produces.
                  disconnect_core_peer (rid, "connected_dispatch_error");
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
        const auto key = core_session_key (rid);
        std::shared_ptr<core_session_t> current;
        std::string retirement_id;
        {
            std::lock_guard lock (_core_sessions_mutex);
            const auto found = _core_sessions.find (key);
            if (found == _core_sessions.end ())
                return;
            current = std::move (found->second);
            if (current) {
                retirement_id = current->stream.session_id ();
                _retired_core_sessions.emplace (retirement_id, current);
            }
            _core_sessions.erase (found);
            ++_core_sessions_revision;
        }
        _core_sessions_changed.notify_all ();
        if (!current)
            return;

        begin_core_session_close (
          std::move (retirement_id), std::move (current), close_reason);
    }

    void finalize_core_session (
      const std::string &retirement_id,
      const std::shared_ptr<core_session_t> &current) noexcept
    {
        if (current) {
            const std::lock_guard session_lock (current->gate);
            if (!current->finalized) {
                current->finalized = true;
                try {
                    (void) _mesh_node->native_node ().sessions ().close (
                      current->transport_connection);
                }
                catch (...) {
                }
                try {
                    detail::session_actor_manager_access_t::disconnect (
                      *current->actors);
                }
                catch (...) {
                }
                try {
                    current->scope.close ();
                }
                catch (...) {
                }
            }
        }
        {
            const std::lock_guard lock (_core_sessions_mutex);
            const auto found = _retired_core_sessions.find (retirement_id);
            if (found != _retired_core_sessions.end ()
                && found->second == current) {
                _retired_core_sessions.erase (found);
                ++_core_sessions_revision;
            }
        }
        _core_sessions_changed.notify_all ();
    }

    void begin_core_session_close (
      std::string retirement_id,
      std::shared_ptr<core_session_t> current,
      const char *close_reason,
      std::optional<stream_close_reason_t> notify_reason = std::nullopt,
      std::string_view diagnostic = {}) noexcept
    {
        bool dispatch_disconnected = false;
        {
            const std::lock_guard session_lock (current->gate);
            if (current->closing) {
                return;
            }
            current->closing = true;
            if (current->replacement) {
                current->replacement->deactivate ();
            }
            if (notify_reason) {
                _runtime.send_session_closing (
                  current->stream, *notify_reason, diagnostic);
            }
            if (current->connected) {
                _runtime.mark_disconnected (current->stream);
                current->connected = false;
                dispatch_disconnected = true;
            }
        }

        auto finalize = [this, retirement_id, current] (const result_t<void> &) {
            const auto session_rid = current->stream.routing_id ().value_or (
              zlink::routing_id_t::from (current->stream.session_id ()));
            retire_actor_session_bindings_async (
              current->transport_connection,
              session_rid,
              [this, retirement_id, current] {
                  finalize_core_session (retirement_id, current);
              });
        };
        if (dispatch_disconnected) {
            auto submitted = _runtime.dispatch_disconnected_async (
              *current->session, current->stream, finalize);
            if (!submitted) {
                finalize (submitted);
            } else {
                record_connection_closed (close_reason);
            }
        } else {
            finalize (result_t<void>::success ());
        }
        {
            const std::lock_guard lock (_core_sessions_mutex);
            ++_core_sessions_revision;
        }
        _core_sessions_changed.notify_all ();
    }

    void wait_for_retired_core_sessions () noexcept
    {
        for (;;) {
            std::vector<std::shared_ptr<core_session_t>> retired;
            std::uint64_t observed_revision = 0;
            {
                std::lock_guard lock (_core_sessions_mutex);
                if (_retired_core_sessions.empty ()) {
                    return;
                }
                observed_revision = _core_sessions_revision;
                retired.reserve (_retired_core_sessions.size ());
                for (const auto &[_, session] : _retired_core_sessions) {
                    if (session) {
                        retired.push_back (session);
                    }
                }
            }
            for (auto &session : retired) {
                _runtime.drain_async_dispatch (session->stream);
            }
            std::unique_lock lock (_core_sessions_mutex);
            if (_retired_core_sessions.empty ()) {
                return;
            }
            _core_sessions_changed.wait (
              lock, [this, observed_revision] {
                  return _retired_core_sessions.empty ()
                         || _core_sessions_revision != observed_revision;
              });
        }
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

    /* Close-intent recording for completion observers that may run in a
     * session-gate-holding context: only the (rid, reason) pair is stored
     * here; the actual close runs when the Core loop drains the queue with
     * no session lock held. This removes the shape where an eagerly
     * completed task could re-enter begin_core_session_close while
     * current->gate (a non-recursive mutex) is still held. */
    void request_core_peer_disconnect (const zlink::routing_id_t &rid,
                                       const char *close_reason) noexcept
    {
        try {
            {
                const std::lock_guard lock (_core_pending_disconnects_mutex);
                _core_pending_disconnects.emplace_back (rid, close_reason);
            }
            _core_wake_timer.signal ();
        }
        catch (...) {
        }
    }

    void drain_pending_core_disconnects () noexcept
    {
        std::vector<std::pair<zlink::routing_id_t, const char *>> pending;
        {
            const std::lock_guard lock (_core_pending_disconnects_mutex);
            if (_core_pending_disconnects.empty ()) {
                return;
            }
            pending.swap (_core_pending_disconnects);
        }
        for (const auto &[rid, reason] : pending) {
            disconnect_core_peer (rid, reason);
        }
    }

    bool dispatch_core_packet (const zlink::routing_id_t &rid,
                              zlink::message_t payload,
                              stream_header_t header,
                              std::shared_ptr<application_job_queue_t::permit_t>
                                application_permit)
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
            + " payload_bytes=" + std::to_string (payload.size ()));
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
        if (current->replacement
            && current->replacement->closing.load (
              std::memory_order_acquire)) {
            return false;
        }

        bool submitted = false;
        bool protocol_error = false;
        std::optional<result_t<void>> rejected;
        try {
            std::lock_guard session_lock (current->gate);
            if (current->connected) {
                trace_stream_host (
                  "core-dispatch-submit", _stream, header,
                  "rid=" + rid.to_hex ());
                detail::session_actor_manager_access_t::set_codec (
                  *current->actors, header.codec ());
                auto dispatched = _runtime.dispatch_packet_async (
                  *current->session, current->stream, header, payload,
                  [this, current, rid, header]
                  (const result_t<void> &result) {
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
                      if (!result) {
                          report_packet_dispatch_error (header, result);
                          send_core_error_frame (rid, header, result);
                      }
                  },
                  [permit = std::move (application_permit)] () mutable {
                      if (!permit)
                          return;
                      permit->release_for_handler_entry ();
                      permit.reset ();
                  },
                  [this] { return _stop->load (std::memory_order_acquire); });
                submitted = static_cast<bool> (dispatched);
                if (!dispatched) {
                    /* Session-gate invariant (cpp-internals): no task may be
                     * completed and no close may be entered while
                     * current->gate is held. Only the rejection is recorded
                     * here; the report, the error-frame write, and the close
                     * all run below, after the gate is released. */
                    rejected.emplace (dispatched);
                    if (dispatched.error_kind ()
                        == framework_error_kind_t::protocol_error) {
                        protocol_error = true;
                    }
                }
            }
        }
        catch (...) {
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }
        static_cast<void> (submitted);
        if (rejected) {
            trace_stream_host (
              "core-dispatch-rejected", _stream, header,
              "rid=" + rid.to_hex () + " kind="
                + std::to_string (
                    static_cast<int> (rejected->error_kind ()))
                + " error="
                + (rejected->error () ? rejected->error ()->what ()
                                      : "unknown"));
            report_packet_dispatch_error (header, *rejected);
            /* error -> close order: the protocol close is sequenced behind
             * the error-frame task terminal (disconnect_rid tears the
             * connection down immediately, so submission order alone would
             * let the close overtake the pending error write). The
             * continuation only records the close intent; the Core loop
             * executes it outside every session lock, and
             * begin_core_session_close keeps the duplicate a no-op. */
            if (protocol_error) {
                send_core_error_frame (
                  rid, header, *rejected, [this, rid] {
                      request_core_peer_disconnect (rid, "protocol_error");
                  });
                return false;
            }
            send_core_error_frame (rid, header, *rejected);
        }
        return true;
    }

    bool process_core_packet (
      zlink::stream_packet_t &packet,
      application_job_queue_t::permit_t permit,
      receive_batch_budget_t &budget)
    {
        if (!packet.routing_id ()) {
            return false;
        }
        const auto rid = *packet.routing_id ();
        auto header_bytes = packet.header ().to_bytes ();
        auto decoded = _runtime.decode_header (header_bytes);
        if (!decoded) {
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }

        const auto frame_bytes = 6u + header_bytes.size () + packet.body ().size ();
        std::shared_ptr<application_job_queue_t::permit_t> application_permit;
        if (decoded.value ().kind () == stream_message_kind_t::send
            || decoded.value ().kind () == stream_message_kind_t::request) {
            permit.mark_queued ();
            application_permit = std::make_shared<application_job_queue_t::permit_t> (
              std::move (permit));
        }
        auto payload = std::move (packet.body ());
        const bool dispatched = dispatch_core_packet (
          rid, std::move (payload), std::move (decoded.value ()),
          std::move (application_permit));
        budget.account (frame_bytes);
        return dispatched;
    }

    void close_core_sessions (
      const char *close_reason = "server_stop",
      std::optional<stream_close_reason_t> notify_reason = std::nullopt,
      std::string_view diagnostic = {}) noexcept
    {
        struct retiring_core_session_t
        {
            std::string retirement_id;
            std::shared_ptr<core_session_t> session;
        };
        std::vector<retiring_core_session_t> sessions;
        {
            std::lock_guard lock (_core_sessions_mutex);
            sessions.reserve (_core_sessions.size ());
            for (auto &[key, current] : _core_sessions) {
                if (!current) {
                    continue;
                }
                auto retirement_id = current->stream.session_id ();
                _retired_core_sessions.emplace (retirement_id, current);
                sessions.push_back (
                  retiring_core_session_t{std::move (retirement_id), current});
            }
            _core_sessions.clear ();
            ++_core_sessions_revision;
        }
        _core_sessions_changed.notify_all ();
        for (auto &retiring : sessions) {
            begin_core_session_close (
              std::move (retiring.retirement_id),
              std::move (retiring.session), close_reason,
              notify_reason, diagnostic);
        }
        wait_for_retired_core_sessions ();
    }

    struct stream_write_wait_state_t
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::atomic_bool cancel_requested{false};
        boost::system::error_code error;

        void complete (const boost::system::error_code &completion_error)
        {
            {
                const std::lock_guard<std::mutex> lock (mutex);
                if (completed) {
                    return;
                }
                error = completion_error;
                completed = true;
            }
            condition.notify_all ();
        }

        boost::system::error_code result ()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return error;
        }
    };

    using stream_write_completion_t =
      std::function<void (const boost::system::error_code &, std::size_t)>;
    using stream_write_start_t = std::function<void (stream_write_completion_t)>;
    using stream_write_cancel_t = std::function<void ()>;

    struct stream_write_operation_t
    {
        std::shared_ptr<stream_write_wait_state_t> state;
        stream_write_start_t start;
        stream_write_cancel_t cancel;
    };

    struct stream_write_queue_t
    {
        std::mutex mutex;
        std::deque<std::shared_ptr<stream_write_operation_t>> pending;
        bool active = false;
    };

    struct tcp_connection_t : std::enable_shared_from_this<tcp_connection_t>
    {
        asio::io_context io;
        tcp::socket socket;
        std::atomic_bool closing{false};
        std::shared_ptr<stream_write_queue_t> write_queue;

        tcp_connection_t () : socket (io), write_queue (std::make_shared<stream_write_queue_t> ())
        {
        }
    };

    void finish_stream_write (const std::shared_ptr<tcp_connection_t> &owner,
                              const std::shared_ptr<stream_write_operation_t> &operation,
                              const boost::system::error_code &error)
    {
        bool start_next = false;
        {
            const std::lock_guard<std::mutex> lock (owner->write_queue->mutex);
            /* A cancelled Asio operation can report its completion after the
             * stop path has already finalized the queue entry. Only the
             * front entry owns the queue transition; late callbacks must not
             * start the following write twice. */
            if (owner->write_queue->pending.empty ()
                || owner->write_queue->pending.front () != operation) {
                return;
            }
            owner->write_queue->pending.pop_front ();
            if (owner->write_queue->pending.empty ()) {
                owner->write_queue->active = false;
            } else {
                start_next = true;
            }
        }
        operation->state->complete (error);
        if (start_next) {
            start_next_stream_write (owner);
        }
    }

    void start_next_stream_write (const std::shared_ptr<tcp_connection_t> &owner)
    {
        std::shared_ptr<stream_write_operation_t> operation;
        {
            const std::lock_guard<std::mutex> lock (owner->write_queue->mutex);
            if (owner->write_queue->pending.empty ()) {
                owner->write_queue->active = false;
                return;
            }
            operation = owner->write_queue->pending.front ();
        }
        if (stream_stop_requested (*_stop, &owner->closing)) {
            if (!operation->state->cancel_requested.exchange (true, std::memory_order_acq_rel)) {
                operation->cancel ();
            }
            finish_stream_write (owner, operation, asio::error::operation_aborted);
            return;
        }
        const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
        operation->start ([this, weak_owner, operation] (
                            const boost::system::error_code &error, std::size_t) {
            if (const auto owner = weak_owner.lock ()) {
                finish_stream_write (owner, operation, error);
            } else {
                operation->state->complete (asio::error::operation_aborted);
            }
        });
    }

    template <typename Start, typename Cancel>
    boost::system::error_code run_stream_write_operation (
      const std::shared_ptr<tcp_connection_t> &owner,
      const std::atomic_bool &stop,
      Start &&start,
      Cancel &&cancel)
    {
        if (stream_stop_requested (stop, &owner->closing)) {
            return asio::error::operation_aborted;
        }
        auto operation = std::make_shared<stream_write_operation_t> ();
        operation->state = std::make_shared<stream_write_wait_state_t> ();
        operation->start = std::forward<Start> (start);
        operation->cancel = std::forward<Cancel> (cancel);
        bool schedule = false;
        {
            const std::lock_guard<std::mutex> lock (owner->write_queue->mutex);
            owner->write_queue->pending.push_back (operation);
            if (!owner->write_queue->active) {
                owner->write_queue->active = true;
                schedule = true;
            }
        }
        if (schedule) {
            const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
            asio::post (owner->io, [this, weak_owner] {
                if (const auto owner = weak_owner.lock ()) {
                    start_next_stream_write (owner);
                }
            });
        }

        std::unique_lock<std::mutex> lock (operation->state->mutex);
        while (!operation->state->completed) {
            if (stream_stop_requested (stop, &owner->closing)
                && !operation->state->cancel_requested.exchange (
                  true, std::memory_order_acq_rel)) {
                /* Cancellation is submitted to the connection's executor. The
                 * same caller pumps that executor below, so a write that was
                 * started by the session dispatch executor is drained before
                 * this synchronous boundary returns. */
                operation->cancel ();
            }
            lock.unlock ();
            owner->io.restart ();
            if (owner->io.run_one () == 0) {
                lock.lock ();
                operation->state->condition.wait_for (
                  lock, std::chrono::milliseconds (100));
            } else {
                lock.lock ();
            }
        }
        lock.unlock ();
        return operation->state->result ();
    }

    struct frame_t
    {
        stream_header_t header;
        zlink::message_t payload;
    };

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
                _accept_retry_timer.expires_after (std::chrono::milliseconds (100));
                _accept_retry_timer.async_wait ([this] (const boost::system::error_code &retry_error) {
                    if (!retry_error) {
                        start_boost_accept ();
                    }
                });
            }
            return;
        }
        if (_stop->load (std::memory_order_acquire)) {
            close_connection (connection->socket);
            return;
        }
        trace_stream_host ("accept", _stream);
        track_socket (connection);
        if (stream_uses_websocket (_stream)) {
            auto websocket_connection =
              std::make_shared<websocket_stream_t> (std::move (connection->socket));
            {
                const std::lock_guard<std::mutex> lock (_sockets_mutex);
                _sockets.erase (&connection->socket);
            }
            track_stream (connection, websocket_connection);
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
            }
            track_stream (connection, tls_connection);
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

    void track_socket (const std::shared_ptr<tcp_connection_t> &owner)
    {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
        _sockets[&owner->socket] = [weak_owner] {
            if (const auto owner = weak_owner.lock ()) {
                owner->closing.store (true, std::memory_order_release);
                /* A synchronous read must be interrupted from the stopping
                 * thread; the completion and final close still run through
                 * the connection worker's normal teardown. */
                cancel_stream (owner->socket);
                asio::post (owner->io, [weak_owner] {
                    if (const auto owner = weak_owner.lock ())
                        close_stream (owner->socket);
                });
            }
        };
    }

    template <typename TStream>
    void track_stream (const std::shared_ptr<tcp_connection_t> &owner,
                       const std::shared_ptr<TStream> &stream)
    {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
        const auto weak_stream = std::weak_ptr<TStream> (stream);
        _sockets[&stream->next_layer ()] = [weak_owner, weak_stream] {
            if (const auto owner = weak_owner.lock ()) {
                owner->closing.store (true, std::memory_order_release);
                if (const auto stream = weak_stream.lock ()) {
                    /* Interrupt a synchronous read immediately. Pending
                     * async writes observe owner->closing and drain their
                     * completion on owner->io before teardown finishes. */
                    cancel_stream (*stream);
                }
                asio::post (owner->io, [weak_stream] {
                    if (const auto stream = weak_stream.lock ())
                        close_stream (*stream);
                });
            }
        };
    }

    void request_close (tcp::socket *socket) noexcept
    {
        std::function<void ()> cancel;
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            const auto found = _sockets.find (socket);
            if (found != _sockets.end ()) {
                cancel = found->second;
            }
        }
        if (cancel) {
            cancel ();
        }
    }

    void request_close (tcp::socket &socket) noexcept { request_close (&socket); }

    template <typename TStream> void request_close (TStream &stream) noexcept
    {
        request_close (&stream.next_layer ());
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

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    void close_connection (ssl::stream<tcp::socket> &stream) noexcept
    {
        close_tcp_socket (stream.next_layer ());
    }
#endif

    template <typename TStream>
    std::optional<frame_t>
    read_frame (TStream &socket,
                const std::function<bool (const stream_header_t &)> &admit_payload,
                asio::io_context &io,
                const std::atomic_bool *connection_stop)
    {
        auto prefix = read_exact (socket, 6, io, *_stop, connection_stop);
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8)
                                  | static_cast<std::size_t> (prefix[5]);
        validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
        auto header_bytes = read_exact (socket, header_size, io, *_stop, connection_stop);
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
        auto payload_bytes = read_exact (socket, payload_size, io, *_stop, connection_stop);
        trace_stream_host ("read-frame", _stream, decoded_header,
                           "payload_bytes=" + std::to_string (payload_bytes.size ()));
        return frame_t{decoded_header, message_from_bytes (payload_bytes)};
    }

    std::optional<frame_t>
    read_frame (websocket_stream_t &socket,
                const std::function<bool (const stream_header_t &)> &admit_payload,
                asio::io_context &io,
                const std::atomic_bool *connection_stop)
    {
        beast::flat_buffer buffer;
        const auto error = run_stream_async_operation (
          io, *_stop,
          [&] (auto completion) { socket.async_read (buffer, std::move (completion)); },
          [&socket] { cancel_stream (socket.next_layer ()); },
          connection_stop);
        if (error) {
            throw boost::system::system_error (error);
        }
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
    write_frame (const std::shared_ptr<tcp_connection_t> &owner,
                 const std::shared_ptr<TStream> &connection,
                 const stream_header_t &header,
                 const zlink::message_t &payload)
    {
        auto encoded_frame = _runtime.encode_frame (header, payload);
        if (!encoded_frame) {
            throw framework_exception_t (encoded_frame.error_kind (),
                                         encoded_frame.error () ? encoded_frame.error ()->what ()
                                                                : "STREAM frame encode failed");
        }
        auto frame = std::make_shared<std::vector<std::uint8_t>> (
          std::move (encoded_frame.value ()));
        trace_stream_host ("write-frame", _stream, header,
                           "payload_bytes=" + std::to_string (payload.size ()));
        const auto weak_connection = std::weak_ptr<TStream> (connection);
        const auto error = run_stream_write_operation (
          owner, *_stop,
          [weak_connection, frame] (auto completion) mutable {
              if (const auto connection = weak_connection.lock ()) {
                  asio::async_write (*connection,
                                     asio::buffer (*frame),
                                     [frame, completion = std::move (completion)] (
                                       const boost::system::error_code &write_error,
                                       std::size_t bytes) mutable {
                                         completion (write_error, bytes);
                                     });
              } else {
                  completion (asio::error::operation_aborted, 0);
              }
          },
          [weak_owner = std::weak_ptr<tcp_connection_t> (owner), weak_connection] {
              if (const auto connection_owner = weak_owner.lock ()) {
                  asio::post (connection_owner->io, [weak_connection] {
                      if (const auto connection = weak_connection.lock ())
                          cancel_stream (*connection);
                  });
              }
          });
        if (error) {
            throw boost::system::system_error (error);
        }
        trace_stream_host ("write-completion", _stream, header, "result=success");
    }

    void write_frame (const std::shared_ptr<tcp_connection_t> &owner,
                      const std::shared_ptr<websocket_stream_t> &connection,
                      const stream_header_t &header,
                      const zlink::message_t &payload)
    {
        auto encoded_frame = _runtime.encode_frame (header, payload);
        if (!encoded_frame) {
            throw framework_exception_t (encoded_frame.error_kind (),
                                         encoded_frame.error () ? encoded_frame.error ()->what ()
                                                                : "STREAM frame encode failed");
        }
        auto frame = std::make_shared<std::vector<std::uint8_t>> (
          std::move (encoded_frame.value ()));
        trace_stream_host ("write-frame", _stream, header,
                           "payload_bytes=" + std::to_string (payload.size ()));
        const auto weak_connection = std::weak_ptr<websocket_stream_t> (connection);
        const auto error = run_stream_write_operation (
          owner, *_stop,
          [weak_connection, frame] (auto completion) mutable {
              if (const auto connection = weak_connection.lock ()) {
                  connection->binary (true);
                  connection->async_write (
                    asio::buffer (*frame),
                    [frame, completion = std::move (completion)] (
                      const boost::system::error_code &write_error, std::size_t bytes) mutable {
                        completion (write_error, bytes);
                    });
              } else {
                  completion (asio::error::operation_aborted, 0);
              }
          },
          [weak_owner = std::weak_ptr<tcp_connection_t> (owner), weak_connection] {
              if (const auto connection_owner = weak_owner.lock ()) {
                  asio::post (connection_owner->io, [weak_connection] {
                      if (const auto connection = weak_connection.lock ())
                          cancel_stream (connection->next_layer ());
                  });
              }
          });
        if (error) {
            throw boost::system::system_error (error);
        }
        trace_stream_host ("write-completion", _stream, header, "result=success");
    }

    template <typename TStream>
    void write_error_frame (const std::shared_ptr<tcp_connection_t> &owner,
                            const std::shared_ptr<TStream> &connection,
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
        write_frame (owner, connection, error_header, stream_error_payload (error));
    }

    template <typename TStream>
    void
    flush_writes (const std::shared_ptr<tcp_connection_t> &owner,
                  const std::shared_ptr<TStream> &connection,
                  stream_t &stream,
                  std::size_t &flushed)
    {
        const auto headers = _runtime.written_headers (stream);
        const auto payloads = _runtime.written_payloads (stream);
        for (; flushed < headers.size (); ++flushed) {
            write_frame (owner, connection, headers[flushed], payloads[flushed]);
        }
    }

    template <typename TStream>
    void handle_stream_connection (std::shared_ptr<tcp_connection_t> owner,
                                   std::shared_ptr<TStream> connection,
                                   tcp::socket *tracked_socket,
                                   bool attach_immediate_writer,
                                   asio::io_context &io)
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
                write_frame (owner, connection, closing, closing_payload);
            }
            catch (...) {
            }
            return;
        }
        auto scope = detail::service_scope_t::create (
          *_services, detail::service_scope_kind_t::stream_session);
        auto &session = _session_factory (scope.provider ());
        auto stream_instance = _runtime.open_session (_stream.name);
        const auto session_rid = zlink::routing_id_t::from (
          stream_instance.session_id ());
        _runtime.set_session_identity (
          stream_instance, session_rid, local_endpoint_text (tcp_socket (*connection)),
          remote_endpoint_text (tcp_socket (*connection)));
        auto &session_actors = scope.provider ().get_required<session_actor_manager_t> ();
        detail::session_actor_manager_access_t::attach (session_actors, stream_instance);
        std::optional<runtime::stateful::stream_connection_t>
          transport_connection;
        runtime::stateful::stream_session_registry_t *session_registry = nullptr;
        if (_mesh_node) {
            session_registry = &_mesh_node->native_node ().sessions ();
            transport_connection = session_registry->open (
              stream_instance.session_id (),
              [this, connection] { request_close (*connection); });
        }
        auto connection_state = std::make_shared<stream_connection_state_t> (
          std::move (scope), &session, std::move (stream_instance),
          &session_actors, session_registry,
          std::move (transport_connection));
        auto &stream = connection_state->stream;
        if (_mesh_node) {
            auto replacement = register_replacement_session (
              session_rid, stream,
              [this, connection] { request_close (*connection); },
              [weak = std::weak_ptr<stream_connection_state_t> (
                 connection_state)] (
                stream_t &dispatch_stream,
                std::string actor_id) {
                  const auto current = weak.lock ();
                  if (!current || current->session == nullptr) {
                      return task_t<void> (result_t<void>::success ());
                  }
                  return current->session->on_actor_binding_replaced (
                    dispatch_stream, std::move (actor_id));
              });
            connection_state->replacement = replacement;
            detail::session_actor_manager_access_t::bind_native (
              session_actors,
              [this, connection = *connection_state->transport_connection,
               session_rid, replacement] (actor_ref_t actor,
                                          std::uint64_t binding_generation) -> task_t<void> {
                  return bind_actor_session (
                    connection, session_rid, actor, replacement,
                    binding_generation);
              });
        }
        if (attach_immediate_writer) {
            _runtime.attach_transport_writer (
              stream,
              [this, owner, connection] (const stream_header_t &header,
                                         const zlink::message_t &payload,
                                         std::optional<std::chrono::milliseconds>) -> task_t<void> {
                  try {
                      write_frame (owner, connection, header, payload);
                  }
                  catch (const framework_exception_t &) {
                      throw;
                  }
                  catch (const std::exception &error) {
                      throw framework_exception_t (
                        framework_error_kind_t::unavailable, error.what ());
                  }
                  co_return;
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
                                    [this, connection] { request_close (*connection); });
            flush_writes (owner, connection, stream, flushed);
            auto receive_lease = _receive_scheduler.register_connection ();
            while (!_stop->load (std::memory_order_acquire)
                   && (!connection_state->replacement
                       || !connection_state->replacement->closing.load (
                            std::memory_order_acquire))) {
                receive_batch_budget_t batch;
                while (!_stop->load (std::memory_order_acquire)
                       && (!connection_state->replacement
                           || !connection_state->replacement->closing.load (
                                std::memory_order_acquire))
                       && batch.can_receive ()) {
                    if (!receive_lease.wait_turn (
                          [this] { return _stop->load (std::memory_order_acquire); })) {
                        break;
                    }
                    /* The synchronous Asio read may wait for the remainder of
                     * a frame. Do not keep the listener-wide receive lease
                     * while waiting; one partial connection must not block
                     * the other connections. */
                    receive_lease.release_turn ();
                    auto frame = read_frame (*connection,
                                             [] (const stream_header_t &) { return true; },
                                             io, &owner->closing);
                    if (!frame) {
                        break;
                    }
                    if (connection_state->replacement
                        && connection_state->replacement->closing.load (
                             std::memory_order_acquire)) {
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
                            write_frame (owner, connection, pong, zlink::message_t{});
                        }
                        continue;
                      }
                    std::shared_ptr<application_job_queue_t::permit_t>
                      application_permit;
                    if (received_frame.header.kind ()
                          == stream_message_kind_t::send
                        || received_frame.header.kind ()
                             == stream_message_kind_t::request) {
                        auto reserved =
                          _application_jobs->wait_for_supply_blocking ();
                        if (!reserved)
                            break;
                        application_permit = std::make_shared<
                          application_job_queue_t::permit_t> (
                            std::move (*reserved));
                        application_permit->mark_queued ();
                    }
                    detail::session_actor_manager_access_t::set_codec (
                      session_actors, received_frame.header.codec ());
                    const auto header = received_frame.header;
                    trace_stream_host ("dispatch-submit", _stream, header);
                    auto dispatched = _runtime.dispatch_packet_async (
                      session, stream, header, received_frame.payload,
                      [this, owner, connection, connection_state, header, liveness]
                      (const result_t<void> &result) {
                          trace_stream_host (
                            "dispatch-complete", _stream, header,
                            std::string ("result=") + (result ? "success" : "failure"));
                          if (!result) {
                              report_packet_dispatch_error (header, result);
                              if (header.kind () == stream_message_kind_t::request) {
                                  try {
                                      write_error_frame (owner, connection, header, result);
                                  }
                                  catch (...) {
                                  }
                              }
                          }
                          (void) connection_state;
                          (void) liveness;
                      },
                      [liveness,
                       permit = std::move (application_permit)] () mutable {
                          if (permit) {
                              permit->release_for_handler_entry ();
                              permit.reset ();
                          }
                          liveness->record_application_inbound ();
                      },
                      [this] { return _stop->load (std::memory_order_acquire); });
                    if (!dispatched) {
                        report_packet_dispatch_error (header, dispatched);
                        if (header.kind () == stream_message_kind_t::request) {
                            try {
                                write_error_frame (owner, connection, header, dispatched);
                            }
                            catch (...) {
                            }
                      }
                    }
                    flush_writes (owner, connection, stream, flushed);
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
            owner->closing.store (true, std::memory_order_release);
            if (connection_state->replacement) {
                connection_state->replacement->deactivate ();
            }
            _runtime.mark_disconnected (stream);
            auto dispatch_disconnect = [this, &session, &stream, connection_state] {
                auto completed = [this, connection_state] (const result_t<void> &) {
                    trace_stream_host ("dispatch-disconnected-end", _stream);
                    if (!connection_state->transport_connection)
                        return;
                    const auto session_rid =
                      connection_state->stream.routing_id ().value_or (
                        zlink::routing_id_t::from (
                          connection_state->stream.session_id ()));
                    retire_actor_session_bindings_async (
                      *connection_state->transport_connection,
                      session_rid,
                      [connection_state] { (void) connection_state; });
                };
                auto submitted = _runtime.dispatch_disconnected_async (
                  session, stream, completed);
                if (!submitted) {
                    completed (submitted);
                }
            };
            if (_stop->load (std::memory_order_acquire)) {
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
            _runtime.drain_async_dispatch (stream);
        }
        close_connection (*connection);
    }

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    void handle_tls_connection (std::shared_ptr<tcp_connection_t> owner,
                                std::shared_ptr<ssl::stream<tcp::socket>> connection)
    {
        try {
            const auto error = run_stream_async_operation (
              owner->io, *_stop,
              [&] (auto completion) {
                  connection->async_handshake (
                    ssl::stream_base::server,
                    [completion = std::move (completion)] (
                      const boost::system::error_code &handshake_error) mutable {
                        completion (handshake_error, 0);
                    });
              },
              [connection] { cancel_stream (connection->next_layer ()); },
              &owner->closing);
            if (error) {
                throw boost::system::system_error (error);
            }
        }
        catch (const boost::system::system_error &) {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            _sockets.erase (&connection->next_layer ());
            return;
        }
        handle_stream_connection (owner, connection, &connection->next_layer (), true, owner->io);
    }
#endif

    void handle_websocket_connection (std::shared_ptr<tcp_connection_t> owner,
                                      std::shared_ptr<websocket_stream_t> connection)
    {
        try {
            const auto error = run_stream_async_operation (
              owner->io, *_stop,
              [&] (auto completion) {
                  connection->async_accept (
                    [completion = std::move (completion)] (
                      const boost::system::error_code &accept_error) mutable {
                        completion (accept_error, 0);
                    });
              },
              [connection] { cancel_stream (connection->next_layer ()); },
              &owner->closing);
            if (error) {
                throw boost::system::system_error (error);
            }
            connection->binary (true);
        }
        catch (const boost::system::system_error &) {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            _sockets.erase (&connection->next_layer ());
            return;
        }
        handle_stream_connection (owner, connection, &connection->next_layer (), true, owner->io);
    }

    void handle_connection (std::shared_ptr<tcp_connection_t> connection)
    {
        handle_stream_connection (connection,
                                  std::shared_ptr<tcp::socket> (connection, &connection->socket),
                                  &connection->socket, true, connection->io);
    }

    detail::stream_runtime_t _runtime;
    stream_snapshot_t _stream;
    std::optional<std::string> _advertise_host;
    detail::stream_session_factory_t _session_factory;
    service_provider_t *_services;
    std::atomic_bool *_stop;
    std::shared_ptr<std::atomic_bool> _drain_flag;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::shared_ptr<detail::mesh_node_runtime_t> _mesh_node;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::chrono::milliseconds _session_replacement_callback_timeout;
    runtime::offload_executor_t _active_streams_lane_executor;
    runtime::state_lane_t _active_streams_lane{_active_streams_lane_executor};
    std::vector<active_session_t> _active_streams;
    asio::io_context _io;
    std::mutex _io_mutex;
    tcp::acceptor _acceptor;
    asio::steady_timer _accept_retry_timer;
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    std::optional<ssl::context> _tls_context;
#endif
    std::mutex _sockets_mutex;
    std::unordered_map<tcp::socket *, std::function<void ()>> _sockets;
    std::mutex _core_socket_mutex;
    std::unique_ptr<zlink::stream_socket_t> _core_socket;
    std::unique_ptr<zlink::socket_monitor_t> _core_monitor;
    eventing::runtime_wake_timer_t _core_wake_timer;
    std::unique_ptr<application_supply_slot_t> _core_application_supply;
    std::mutex _core_pending_disconnects_mutex;
    std::vector<std::pair<zlink::routing_id_t, const char *>>
      _core_pending_disconnects;
    std::mutex _core_sessions_mutex;
    std::map<std::string, std::shared_ptr<core_session_t>> _core_sessions;
    std::map<std::string, std::shared_ptr<core_session_t>>
      _retired_core_sessions;
    std::condition_variable _core_sessions_changed;
    std::uint64_t _core_sessions_revision = 0;
    stream_receive_scheduler_t _receive_scheduler;
    std::mutex _workers_mutex;
    std::vector<std::thread> _workers;
    mutable std::mutex _ready_mutex;
    std::condition_variable _ready_cv;
    std::string _bound_endpoint;
    bool _started = false;
    bool _start_failed = false;
    std::string _start_error;
};

stream_host_service_t::stream_host_service_t (
  detail::stream_runtime_t runtime,
  std::vector<stream_snapshot_t> streams,
  std::map<std::string, detail::stream_session_factory_t> session_factories,
  std::chrono::milliseconds session_replacement_callback_timeout,
  std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
  std::map<std::string, std::optional<std::string>> advertise_hosts,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    _runtime (std::move (runtime)),
    _streams (std::move (streams)),
    _session_factories (std::move (session_factories)),
    _session_replacement_callback_timeout (session_replacement_callback_timeout),
    _advertise_hosts (std::move (advertise_hosts)),
    _listener_statuses (std::move (listener_statuses)),
    _mesh_node (std::move (mesh_node)),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (
            application_job_queue_configuration_t{
              application_job_queue_profile_t::balanced,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ()),
              1,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ())}))
{
}

stream_host_service_t::~stream_host_service_t ()
{
    stop ();
}

task_t<void> stream_host_service_t::start (service_provider_t &services)
{
    _services = &services;
    _stop.store (false, std::memory_order_release);
    try {
    for (const auto &stream : _streams) {
        auto factory = _session_factories.find (stream.packet_session_name);
        if (factory == _session_factories.end ()) {
            continue;
        }
        const auto advertise = _advertise_hosts.find (stream.name);
        const auto advertise_host =
          advertise == _advertise_hosts.end () ? std::optional<std::string>{}
                                               : advertise->second;
        validate_stream_listener_identity (stream, advertise_host);
        auto listener =
          std::make_unique<listener_t> (_runtime, stream, advertise_host, factory->second,
                                        services, _stop, _drain_flag, _monitoring, _mesh_node,
                                        _application_jobs,
                                        _session_replacement_callback_timeout);
        auto *raw = listener.get ();
        _listeners.push_back (std::move (listener));
        _threads.emplace_back ([raw] { raw->run_guarded (); });
        raw->wait_started ();
        if (_listener_statuses)
            _listener_statuses->update (
              listener_kind_t::stream,
              stream.name,
              raw->listener_endpoint ());
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
    catch (...) {
        stop ();
        throw;
    }
    return task_t<void> (result_t<void>::success ());
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
    if (_application_jobs)
        _application_jobs->stop ();
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
        if (_listener_statuses)
            _listener_statuses->remove (
              listener_kind_t::stream, listener->name ());
    }
    _listeners.clear ();
    _services = nullptr;
}

} // namespace zlink::framework::runtime
