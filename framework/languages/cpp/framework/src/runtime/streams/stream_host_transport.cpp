/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/streams/stream_host_service_internal.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_ingress.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace zlink::framework::runtime
{

namespace stream_host_detail
{
struct stream_receive_scheduler_t::slot_t
{
    explicit slot_t (std::uint64_t sequence_) : sequence (sequence_) {}

    std::uint64_t sequence;
    bool granted = false;
    bool closed = false;
    std::condition_variable changed;
};

class stream_receive_scheduler_t::lease_t
{
  public:
    lease_t () noexcept = default;

    lease_t (stream_receive_scheduler_t *owner, std::shared_ptr<slot_t> slot) :
        _owner (owner), _slot (std::move (slot))
    {
    }

    ~lease_t () { reset (); }

    lease_t (const lease_t &) = delete;
    lease_t &operator= (const lease_t &) = delete;

    lease_t (lease_t &&other) noexcept :
        _owner (std::exchange (other._owner, nullptr)), _slot (std::move (other._slot))
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

    explicit operator bool () const noexcept { return _owner != nullptr && _slot != nullptr; }

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

stream_receive_scheduler_t::~stream_receive_scheduler_t ()
{
    stop ();
}

void stream_receive_scheduler_t::start ()
{
    std::lock_guard lock (_mutex);
    if (_thread.joinable ()) {
        return;
    }
    _stopped = false;
    _thread = std::thread ([this] { run (); });
}

stream_receive_scheduler_t::lease_t stream_receive_scheduler_t::register_connection ()
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

void stream_receive_scheduler_t::request_stop () noexcept
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

void stream_receive_scheduler_t::stop () noexcept
{
    request_stop ();
    if (_thread.joinable ()) {
        _thread.join ();
    }
}

bool stream_receive_scheduler_t::wait_for_turn (const std::shared_ptr<slot_t> &slot,
                                                const std::function<bool ()> &stop_requested) const
{
    std::unique_lock lock (_mutex);
    slot->changed.wait (lock, [&] {
        return slot->granted || slot->closed || _stopped || (stop_requested && stop_requested ());
    });
    return slot->granted && !slot->closed && !_stopped && !(stop_requested && stop_requested ());
}

void stream_receive_scheduler_t::release_turn (const std::shared_ptr<slot_t> &slot) noexcept
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

void stream_receive_scheduler_t::unregister_slot (const std::shared_ptr<slot_t> &slot) noexcept
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

void stream_receive_scheduler_t::run () noexcept
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
                return _stopped
                       || std::any_of (_slots.begin (), _slots.end (), [] (const auto &slot) {
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
            while (start < candidates.size () && candidates[start]->sequence < _cursor_sequence) {
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

} // namespace stream_host_detail

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using detail::stream_error_payload;
using stream_host_detail::session_creation_failed_diagnostic;
using stream_host_detail::socket_endpoint_text;
using stream_host_detail::stream_trace_enabled;
using stream_host_detail::stream_uses_tls;
using stream_host_detail::stream_uses_websocket;

namespace
{

bool is_expected_session_disconnect (const boost::system::error_code &error)
{
    return error == asio::error::eof || error == asio::error::operation_aborted
           || error == websocket::error::closed;
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

struct stream_async_operation_state_t : std::enable_shared_from_this<stream_async_operation_state_t>
{
    explicit stream_async_operation_state_t (asio::io_context &io) : timer (io) {}

    asio::steady_timer timer;
    std::function<void ()> cancel;
    std::optional<result_t<boost::system::error_code>> result;

    void arm_timer (const std::atomic_bool &stop, const std::atomic_bool *connection_stop)
    {
        timer.expires_after (std::chrono::milliseconds (100));
        const auto weak_state = weak_from_this ();
        timer.async_wait (
          [weak_state, &stop, connection_stop] (const boost::system::error_code &timer_error) {
              const auto state = weak_state.lock ();
              if (!state || timer_error || state->result) {
                  return;
              }
              const auto stopped =
                stop.load (std::memory_order_acquire)
                || (connection_stop && connection_stop->load (std::memory_order_acquire));
              if (stopped) {
                  state->cancel ();
                  return;
              }
              state->arm_timer (stop, connection_stop);
          });
    }

    void complete (result_t<boost::system::error_code> completion)
    {
        result.emplace (std::move (completion));
        // Boost >= 1.87 removed cancel(error_code&) from waitable timers.
        try {
            timer.cancel ();
        }
        catch (const boost::system::system_error &) {
        }
    }
};

bool stream_stop_requested (const std::atomic_bool &stop,
                            const std::atomic_bool *connection_stop) noexcept
{
    return stop.load (std::memory_order_acquire)
           || (connection_stop && connection_stop->load (std::memory_order_acquire));
}

template <typename Start, typename Cancel, typename Progress>
boost::system::error_code run_stream_async_operation (asio::io_context &io,
                                                      const std::atomic_bool &stop,
                                                      Start &&start,
                                                      Cancel &&cancel,
                                                      const std::atomic_bool *connection_stop,
                                                      const Progress &progress)
{
    if (stream_stop_requested (stop, connection_stop)) {
        return asio::error::operation_aborted;
    }
    auto state = std::make_shared<stream_async_operation_state_t> (io);
    state->cancel = std::forward<Cancel> (cancel);
    state->arm_timer (stop, connection_stop);
    try {
        start ([state] (const boost::system::error_code &error, std::size_t) {
            state->complete (result_t<boost::system::error_code>::success (error));
        });
    }
    catch (...) {
        state->complete (detail::current_exception_result<boost::system::error_code> ());
    }
    io.restart ();
    while (!state->result)
        progress ();
    return state->result->value ();
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

template <typename TStream> void close_stream (TStream &stream) noexcept
{
    close_stream (stream.next_layer ());
}

struct tcp_read_progress_t
{
    // A peek has no bytes to consume. An empty value means the socket already
    // held enough bytes to finish this read when the deadline was checked.
    std::optional<std::size_t> remaining = 0;

    struct posted_settle_t
    {
    };
    std::shared_ptr<posted_settle_t> posted_settle;
};

template <typename TStream, typename Progress>
void read_exact (TStream &socket,
                 std::span<std::uint8_t> bytes,
                 asio::io_context &io,
                 const std::atomic_bool &stop,
                 const std::atomic_bool *connection_stop,
                 const Progress &progress)
{
    if (bytes.empty ())
        return;
    const auto cancel = [&socket] { cancel_stream (socket); };
    boost::system::error_code error;
    if constexpr (std::is_same_v<TStream, tcp::socket>) {
        tcp_read_progress_t read{bytes.size (), {}};
        error = run_stream_async_operation (
          io, stop,
          [&] (auto completion) {
              asio::async_read (
                socket, asio::buffer (bytes.data (), bytes.size ()),
                [&] (const boost::system::error_code &error, std::size_t transferred) {
                    if (read.remaining)
                        read.remaining = bytes.size () - transferred;
                    return asio::transfer_all () (error, transferred);
                },
                std::move (completion));
          },
          cancel, connection_stop, [&] { progress (&read); });
    } else {
        error = run_stream_async_operation (
          io, stop,
          [&] (auto completion) {
              asio::async_read (socket, asio::buffer (bytes.data (), bytes.size ()),
                                std::move (completion));
          },
          cancel, connection_stop, progress);
    }
    if (error)
        throw boost::system::system_error (error);
}

template <typename TStream, typename Progress>
std::vector<std::uint8_t> read_exact (TStream &socket,
                                      std::size_t size,
                                      asio::io_context &io,
                                      const std::atomic_bool &stop,
                                      const std::atomic_bool *connection_stop,
                                      const Progress &progress)
{
    std::vector<std::uint8_t> bytes (size);
    read_exact (socket, std::span (bytes), io, stop, connection_stop, progress);
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

using stream_write_completion_t =
  std::function<void (const boost::system::error_code &, std::size_t)>;
using stream_write_start_t = std::function<void (stream_write_completion_t)>;

} // namespace

struct stream_host_service_t::listener_t::stream_write_operation_t
{
    task_completion_source_t<boost::system::error_code> completion;
    stream_write_start_t start;

    void complete (result_t<boost::system::error_code> result)
    {
        completion.complete (std::move (result));
    }
};

struct stream_host_service_t::listener_t::stream_write_queue_t
{
    std::mutex mutex;
    std::deque<std::shared_ptr<stream_write_operation_t>> pending;
    bool active = false;
};

struct stream_host_service_t::listener_t::tcp_connection_t
{
    asio::io_context io;
    tcp::socket socket;
    std::atomic_bool closing{false};
    session_liveness_t::reader_t reader_liveness;
    std::shared_ptr<stream_write_queue_t> write_queue;

    tcp_connection_t () : socket (io), write_queue (std::make_shared<stream_write_queue_t> ()) {}

    void wake_reader ()
    {
        asio::post (io, [] {});
    }

    void request_close ()
    {
        const std::lock_guard lock (write_queue->mutex);
        closing.store (true, std::memory_order_release);
        // Socket close completion이 없어도 permit 대기 reader를 깨웁니다.
        wake_reader ();
    }
};

struct stream_host_service_t::listener_t::frame_t
{
    stream_header_t header;
    zlink::message_t payload;
};

void stream_host_service_t::listener_t::finish_stream_write (
  const std::shared_ptr<tcp_connection_t> &owner,
  const std::shared_ptr<stream_write_operation_t> &operation,
  result_t<boost::system::error_code> result)
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
    operation->complete (std::move (result));
    if (start_next) {
        start_next_stream_write (owner);
    }
}

void stream_host_service_t::listener_t::start_next_stream_write (
  const std::shared_ptr<tcp_connection_t> &owner)
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
        finish_stream_write (
          owner, operation,
          result_t<boost::system::error_code>::success (asio::error::operation_aborted));
        return;
    }
    const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
    try {
        operation->start (
          [this, weak_owner, operation] (const boost::system::error_code &error, std::size_t) {
              if (const auto owner = weak_owner.lock ()) {
                  finish_stream_write (owner, operation,
                                       result_t<boost::system::error_code>::success (error));
              } else {
                  operation->complete (
                    result_t<boost::system::error_code>::success (asio::error::operation_aborted));
              }
          });
    }
    catch (...) {
        finish_stream_write (owner, operation,
                             detail::current_exception_result<boost::system::error_code> ());
    }
}

/* 연결별 송신은 listener executor가 시작하고 completion으로 완료한다. */
template <typename Start>
std::shared_ptr<stream_host_service_t::listener_t::stream_write_operation_t>
stream_host_service_t::listener_t::queue_stream_write_operation (
  const std::shared_ptr<tcp_connection_t> &owner, Start &&start)
{
    auto operation = std::make_shared<stream_write_operation_t> ();
    operation->start = std::forward<Start> (start);
    {
        const std::lock_guard<std::mutex> lock (owner->write_queue->mutex);
        if (stream_stop_requested (*_stop, &owner->closing))
            return {};
        owner->write_queue->pending.push_back (operation);
        if (!owner->write_queue->active) {
            owner->write_queue->active = true;
            const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
            asio::post (owner->io, [this, weak_owner] {
                if (const auto owner = weak_owner.lock ())
                    start_next_stream_write (owner);
            });
        }
    }
    return operation;
}

void stream_host_service_t::listener_t::configure_tls_context ()
{
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    if (_stream.tls_certificate_file.empty () || _stream.tls_private_key_file.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM TLS endpoint requires certificate and private key");
    }
    _tls_context.emplace (ssl::context::tls_server);
    _tls_context->use_certificate_chain_file (_stream.tls_certificate_file);
    _tls_context->use_private_key_file (_stream.tls_private_key_file, ssl::context::pem);
    if (_stream.tls_require_client_certificate) {
        _tls_context->set_default_verify_paths ();
        _tls_context->set_verify_mode (ssl::verify_peer | ssl::verify_fail_if_no_peer_cert
                                       | ssl::verify_client_once);
    } else {
        _tls_context->set_verify_mode (ssl::verify_none);
    }
#else
    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 "STREAM TLS support requires OpenSSL");
#endif
}

template <typename T, typename Owner, typename... Args>
std::shared_ptr<T>
stream_host_service_t::listener_t::make_transport (std::shared_ptr<Owner> lifetime, Args &&...args)
{
    // Destroy the socket before the connection io and TLS context.
    using owner_t = std::pair<std::shared_ptr<Owner>, T>;
    auto owner = std::make_shared<owner_t> (std::piecewise_construct,
                                            std::forward_as_tuple (std::move (lifetime)),
                                            std::forward_as_tuple (std::forward<Args> (args)...));
    return {owner, &owner->second};
}

void stream_host_service_t::listener_t::start_boost_accept ()
{
    if (_stop->load (std::memory_order_acquire)) {
        return;
    }
    auto connection = make_transport<tcp_connection_t> (shared_from_this ());
    _acceptor.async_accept (connection->socket,
                            [this, connection] (const boost::system::error_code &error) {
                                handle_boost_accept (connection, error);
                            });
}

void stream_host_service_t::listener_t::handle_boost_accept (
  const std::shared_ptr<tcp_connection_t> &connection, const boost::system::error_code &error)
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
          make_transport<websocket_stream_t> (connection, std::move (connection->socket));
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
        auto tls_connection = make_transport<ssl::stream<tcp::socket>> (
          connection, std::move (connection->socket), *_tls_context);
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

void stream_host_service_t::listener_t::close_tcp_socket (tcp::socket &socket) noexcept
{
    boost::system::error_code ignored;
    socket.shutdown (tcp::socket::shutdown_both, ignored);
    socket.close (ignored);
}

void stream_host_service_t::listener_t::track_socket (
  const std::shared_ptr<tcp_connection_t> &owner)
{
    const std::lock_guard<std::mutex> lock (_sockets_mutex);
    const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
    _sockets[&owner->socket] = [weak_owner] {
        if (const auto owner = weak_owner.lock ()) {
            owner->request_close ();
            asio::post (owner->io, [weak_owner] {
                if (const auto owner = weak_owner.lock ())
                    close_stream (owner->socket);
            });
        }
    };
}

template <typename TStream>
void stream_host_service_t::listener_t::track_stream (
  const std::shared_ptr<tcp_connection_t> &owner, const std::shared_ptr<TStream> &stream)
{
    const std::lock_guard<std::mutex> lock (_sockets_mutex);
    const auto weak_owner = std::weak_ptr<tcp_connection_t> (owner);
    const auto weak_stream = std::weak_ptr<TStream> (stream);
    _sockets[&stream->next_layer ()] = [weak_owner, weak_stream] {
        if (const auto owner = weak_owner.lock ()) {
            owner->request_close ();
            asio::post (owner->io, [weak_stream] {
                if (const auto stream = weak_stream.lock ())
                    close_stream (*stream);
            });
        }
    };
}

template <typename TStream>
task_t<void>
stream_host_service_t::listener_t::close_transport (std::shared_ptr<tcp_connection_t> owner,
                                                    std::shared_ptr<TStream> connection)
{
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    auto pending = completion->task ();
    {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        if (!_sockets.contains (&tcp_socket (*connection)))
            co_return;
        asio::post (owner->io, [owner, connection, completion] {
            owner->request_close ();
            boost::system::error_code error;
            auto &socket = tcp_socket (*connection);
            socket.cancel (error);
            socket.close (error);
            if (error)
                completion->complete (
                  result_t<void>::failure (framework_error_kind_t::unavailable, error.message ()));
            else
                completion->complete (result_t<void>::success ());
        });
    }
    co_await pending;
}

void stream_host_service_t::listener_t::request_close (tcp::socket *socket) noexcept
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

void stream_host_service_t::listener_t::request_close (tcp::socket &socket) noexcept
{
    request_close (&socket);
}

template <typename TStream>
void stream_host_service_t::listener_t::request_close (TStream &stream) noexcept
{
    request_close (&stream.next_layer ());
}

void stream_host_service_t::listener_t::close_connection (tcp::socket &socket) noexcept
{
    close_tcp_socket (socket);
}

template <typename TStream, typename Reserve, typename Progress>
std::optional<stream_host_service_t::listener_t::frame_t>
stream_host_service_t::listener_t::read_frame (TStream &socket,
                                               const Reserve &reserve,
                                               asio::io_context &io,
                                               const std::atomic_bool *connection_stop,
                                               const Progress &progress)
{
    std::array<std::uint8_t, 6> prefix;
    std::span<std::uint8_t> unread (prefix);
    if constexpr (std::is_same_v<TStream, tcp::socket>) {
        // A peek distinguishes an idle peer's EOF from packet readiness.
        tcp_read_progress_t peek;
        const auto ready = run_stream_async_operation (
          io, *_stop,
          [&] (auto completion) {
              socket.async_receive (asio::buffer (prefix.data (), 1),
                                    asio::socket_base::message_peek, std::move (completion));
          },
          [&socket] { cancel_stream (socket); }, connection_stop, [&] { progress (&peek); });
        if (ready)
            throw boost::system::system_error (ready);
    } else {
        // TLS may already hold decrypted bytes after the last read.
        read_exact (socket, unread.first (1), io, *_stop, connection_stop, progress);
        unread = unread.subspan (1);
    }
    if (!reserve ())
        return std::nullopt;
    read_exact (socket, unread, io, *_stop, connection_stop, progress);
    const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
    const auto payload_size =
      (static_cast<std::size_t> (prefix[2]) << 24) | (static_cast<std::size_t> (prefix[3]) << 16)
      | (static_cast<std::size_t> (prefix[4]) << 8) | static_cast<std::size_t> (prefix[5]);
    validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
    auto header_bytes = read_exact (socket, header_size, io, *_stop, connection_stop, progress);
    auto payload_bytes = read_exact (socket, payload_size, io, *_stop, connection_stop, progress);
    auto header = _runtime.decode_header (header_bytes);
    if (!header) {
        throw framework_exception_t (header.error_kind (), header.error ()
                                                             ? header.error ()->what ()
                                                             : "STREAM header decode failed");
    }
    auto decoded_header = header.value ();
    trace_stream_host ("read-frame", _stream, decoded_header,
                       "payload_bytes=" + std::to_string (payload_bytes.size ()));
    return frame_t{decoded_header, message_from_bytes (payload_bytes)};
}

template <typename Reserve, typename Progress>
std::optional<stream_host_service_t::listener_t::frame_t>
stream_host_service_t::listener_t::read_frame (websocket_stream_t &socket,
                                               const Reserve &reserve,
                                               asio::io_context &io,
                                               const std::atomic_bool *connection_stop,
                                               const Progress &progress)
{
    beast::flat_buffer buffer;
    // Beast can buffer another message in the previous read. Acquire a
    // permit after one framing byte is ready and before reading the packet.
    auto error = run_stream_async_operation (
      io, *_stop,
      [&] (auto completion) { socket.async_read_some (buffer, 1, std::move (completion)); },
      [&socket] { cancel_stream (socket.next_layer ()); }, connection_stop, progress);
    if (error)
        throw boost::system::system_error (error);
    if (!reserve ())
        return std::nullopt;
    if (!socket.is_message_done ()) {
        error = run_stream_async_operation (
          io, *_stop, [&] (auto completion) { socket.async_read (buffer, std::move (completion)); },
          [&socket] { cancel_stream (socket.next_layer ()); }, connection_stop, progress);
        if (error)
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
    const auto payload_size =
      (static_cast<std::size_t> (bytes[2]) << 24) | (static_cast<std::size_t> (bytes[3]) << 16)
      | (static_cast<std::size_t> (bytes[4]) << 8) | static_cast<std::size_t> (bytes[5]);
    validate_stream_frame_size (header_size, payload_size, _stream.max_message_size);
    if (bytes.size () != 6 + header_size + payload_size) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM WebSocket message size does not match its prefix");
    }
    std::vector<std::uint8_t> header_bytes (bytes.begin () + 6, bytes.begin () + 6 + header_size);
    std::vector<std::uint8_t> payload_bytes (bytes.begin () + 6 + header_size, bytes.end ());
    auto header = _runtime.decode_header (header_bytes);
    if (!header) {
        throw framework_exception_t (header.error_kind (), header.error ()
                                                             ? header.error ()->what ()
                                                             : "STREAM header decode failed");
    }
    auto decoded_header = header.value ();
    trace_stream_host ("read-frame", _stream, decoded_header,
                       "payload_bytes=" + std::to_string (payload_bytes.size ()));
    return frame_t{decoded_header, message_from_bytes (payload_bytes)};
}

// Encodes one frame for queue_frame.
std::shared_ptr<std::vector<std::uint8_t>>
stream_host_service_t::listener_t::encode_frame_bytes (const stream_header_t &header,
                                                       const zlink::message_t &payload)
{
    auto encoded_frame = _runtime.encode_frame (header, payload);
    if (!encoded_frame) {
        throw framework_exception_t (encoded_frame.error_kind (),
                                     encoded_frame.error () ? encoded_frame.error ()->what ()
                                                            : "STREAM frame encode failed");
    }
    trace_stream_host ("write-frame", _stream, header,
                       "payload_bytes=" + std::to_string (payload.size ()));
    return std::make_shared<std::vector<std::uint8_t>> (std::move (encoded_frame.value ()));
}

/* Queues one frame on the connection's write queue without waiting for
 * it; empty once the connection stops. */
template <typename TStream>
std::shared_ptr<stream_host_service_t::listener_t::stream_write_operation_t>
stream_host_service_t::listener_t::queue_frame (const std::shared_ptr<tcp_connection_t> &owner,
                                                const std::shared_ptr<TStream> &connection,
                                                const stream_header_t &header,
                                                const zlink::message_t &payload)
{
    auto frame = encode_frame_bytes (header, payload);
    const auto weak_connection = std::weak_ptr<TStream> (connection);
    return queue_stream_write_operation (owner, [weak_connection, frame] (auto completion) mutable {
        if (const auto connection = weak_connection.lock ()) {
            asio::async_write (*connection, asio::buffer (*frame),
                               [frame, completion = std::move (completion)] (
                                 const boost::system::error_code &write_error,
                                 std::size_t bytes) mutable { completion (write_error, bytes); });
        } else {
            completion (asio::error::operation_aborted, 0);
        }
    });
}

std::shared_ptr<stream_host_service_t::listener_t::stream_write_operation_t>
stream_host_service_t::listener_t::queue_frame (
  const std::shared_ptr<tcp_connection_t> &owner,
  const std::shared_ptr<websocket_stream_t> &connection,
  const stream_header_t &header,
  const zlink::message_t &payload)
{
    auto frame = encode_frame_bytes (header, payload);
    const auto weak_connection = std::weak_ptr<websocket_stream_t> (connection);
    return queue_stream_write_operation (owner, [weak_connection, frame] (auto completion) mutable {
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
    });
}

template <typename TStream>
task_t<void>
stream_host_service_t::listener_t::submit_frame (std::shared_ptr<tcp_connection_t> owner,
                                                 std::shared_ptr<TStream> connection,
                                                 stream_header_t header,
                                                 zlink::message_t payload,
                                                 detail::stream_reply_submission_t *admission_reply)
{
    auto operation = queue_frame (owner, connection, header, payload);
    if (!operation)
        throw framework_exception_t (
          framework_error_kind_t::unavailable,
          asio::error::make_error_code (asio::error::operation_aborted).message ());
    if (admission_reply)
        admission_reply->complete ();
    const auto error = co_await operation->completion.task ();
    if (error)
        throw framework_exception_t (framework_error_kind_t::unavailable, error.message ());
    trace_stream_host ("write-completion", _stream, header, "result=success");
    co_return;
}

template <typename TStream>
void stream_host_service_t::listener_t::write_error_frame (
  const std::shared_ptr<tcp_connection_t> &owner,
  const std::shared_ptr<TStream> &connection,
  const stream_t &stream,
  const stream_header_t &request_header,
  const result_t<void> &error,
  detail::stream_metadata_t admission_metadata)
{
    if (!request_header.request_seq ()) {
        return;
    }
    auto error_header = detail::stream_runtime_t::make_terminal_header (
      stream_message_kind_t::error, request_header, std::move (admission_metadata));
    auto send = detail::session_actor_manager_access_t::submit_terminal (
      stream, error_header, stream_error_payload (error),
      [&] (const stream_header_t &header, const zlink::message_t &payload) {
          if (!queue_frame (owner, connection, header, payload))
              throw framework_exception_t (framework_error_kind_t::unavailable,
                                           "STREAM error reply connection is closing");
          return task_t<void> (result_t<void>::success ());
      });
    detail::task_result_access_t::read (send);
}

template <typename TStream>
void stream_host_service_t::listener_t::handle_stream_connection (
  std::shared_ptr<tcp_connection_t> owner,
  std::shared_ptr<TStream> connection,
  tcp::socket *tracked_socket,
  asio::io_context &io)
{
    const auto established = session_liveness_t::clock_t::now ();
    auto cleanup = std::unique_ptr<tcp::socket, std::function<void (tcp::socket *)>> (
      tracked_socket, [this] (tcp::socket *socket) {
          const std::lock_guard<std::mutex> lock (_sockets_mutex);
          _sockets.erase (socket);
      });
    const auto finish_transport = [&] {
        owner->request_close ();
        close_stream (*connection);
        cleanup.reset ();
        io.restart ();
        io.run ();
    };
    auto write = [this, owner, connection] (const stream_header_t &header,
                                            const zlink::message_t &payload) {
        return submit_frame (owner, connection, header, payload);
    };
    const auto close_before_admission = [&] (stream_close_reason_t reason,
                                             std::string_view diagnostic) {
        stream_t transport;
        _runtime.attach_transport_writer (transport, write);
        _runtime.send_session_closing (
          transport, reason, diagnostic,
          [work = asio::make_work_guard (io)] () mutable { work.reset (); });
        io.restart ();
        io.run ();
        finish_transport ();
    };
    const auto close_draining_connection = [&] {
        /* graceful-drain-handoff §5: brand-new connections on a draining
             * node receive session-closing(server_drain) and are closed. */
        close_before_admission (stream_close_reason_t::server_drain, "node is draining");
    };
    if (draining ()) {
        close_draining_connection ();
        return;
    }
    auto scope =
      detail::service_scope_t::create (*_services, detail::service_scope_kind_t::stream_session);
    auto *created = create_session (scope.provider ());
    if (!created) {
        close_before_admission (stream_close_reason_t::transport_error,
                                session_creation_failed_diagnostic);
        return;
    }
    auto &session = *created;
    auto stream_instance = _runtime.open_session (_stream.name, _shutdown_cancellation);
    const auto session_rid = zlink::routing_id_t::from (stream_instance.session_id ());
    _runtime.set_session_identity (stream_instance, session_rid,
                                   local_endpoint_text (tcp_socket (*connection)),
                                   remote_endpoint_text (tcp_socket (*connection)));
    auto &session_actors = scope.provider ().get_required<session_actor_manager_t> ();
    detail::session_actor_manager_access_t::attach (session_actors, stream_instance);
    std::optional<runtime::stateful::stream_connection_t> transport_connection;
    if (_mesh_node) {
        transport_connection = _mesh_node->native_node ().sessions ().open (
          runtime::stateful::stream_session_registry_t::connection_key (session_rid),
          [this, connection] { request_close (*connection); }, _mesh_node->routing_id (),
          _mesh_node->native_node ().status ().lifecycle_generation (),
          [this, owner, connection] { return close_transport (owner, connection); });
    }
    auto connection_state = std::make_shared<stream_connection_state_t> (
      std::move (scope), &session, std::move (stream_instance), &session_actors,
      std::move (transport_connection));
    auto &stream = connection_state->stream;
    if (_mesh_node) {
        auto replacement = register_replacement_session (
          session_rid, stream, [this, connection] { request_close (*connection); },
          [weak = std::weak_ptr<stream_connection_state_t> (connection_state)] (
            stream_t &dispatch_stream, std::string actor_id) {
              const auto current = weak.lock ();
              if (!current || current->session == nullptr) {
                  return task_t<void> (result_t<void>::success ());
              }
              return current->session->on_actor_binding_replaced (dispatch_stream,
                                                                  std::move (actor_id));
          });
        connection_state->replacement = replacement;
        detail::session_actor_manager_access_t::bind_native (
          session_actors,
          [this, connection = *connection_state->transport_connection, session_rid,
           replacement] (actor_ref_t actor, std::uint64_t binding_generation, std::uint16_t,
                         std::function<void ()> publish) -> request_task_t<void> {
              return bind_actor_session (connection, session_rid, actor, replacement,
                                         binding_generation, std::move (publish));
          });
    }
    if (connection_state->transport_connection)
        configure_disconnect (session_actors, *connection_state->transport_connection, session_rid);
    _runtime.attach_transport_writer (
      stream,
      [this, owner, connection] (const stream_header_t &header, const zlink::message_t &payload,
                                 const auto &admission_reply) {
          return submit_frame (owner, connection, header, payload, admission_reply.get ());
      });
    bool connected_session = false;
    std::optional<stream_error_t> session_transport_error;
    auto liveness = std::make_shared<session_liveness_t> (
      established, session_liveness_t::clock_t::now (), _stream.liveness);
    asio::steady_timer liveness_timer (io);
    try {
        const auto close = [this,
                            weak = std::weak_ptr<TStream> (connection)] (stream_close_reason_t) {
            if (const auto connection = weak.lock ())
                request_close (*connection);
        };
        std::optional<result_t<void>> connected;
        {
            const std::lock_guard lock (_core_sessions_mutex);
            if (!draining ()) {
                register_active_stream (
                  stream, [this, weak = std::weak_ptr<tcp_connection_t> (owner), stream, liveness,
                           close] (stream_close_reason_t reason, std::string diagnostic) {
                      if (const auto owner = weak.lock ())
                          asio::post (owner->io, [this, weak, stream, liveness, close, reason,
                                                  diagnostic = std::move (diagnostic)] () mutable {
                              if (const auto owner = weak.lock ())
                                  terminate_session (stream, liveness, close, reason, diagnostic);
                          });
                  });
                connected.emplace (_runtime.dispatch_connected_async (
                  session, stream, [this, connection] (const result_t<void> &result) {
                      if (!result && !_shutdown_cancellation.stop_requested ())
                          request_close (*connection);
                  }));
            }
        }
        if (!connected || !*connected) {
            begin_connection_retirement (connection_state);
            if (!connected) {
                close_draining_connection ();
            } else {
                finish_transport ();
            }
            retire_connection (connection_state);
            return;
        }
        connected_session = true;
        record_connection_opened ();
        const auto arm_liveness = [&] {
            schedule_liveness (liveness_timer, *liveness, owner->reader_liveness, [] {});
        };
        const auto settle_liveness = [&] {
            if (stream_stop_requested (*_stop, &owner->closing)
                || liveness_timer.expiry () > session_liveness_t::clock_t::now ())
                return;
            evaluate_liveness (stream, liveness, owner->reader_liveness, close);
            arm_liveness ();
        };
        arm_liveness ();
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
        if (_shutdown_test_hooks.registered)
            _shutdown_test_hooks.registered (shared_from_this (), owner);
#endif
        auto receive_lease = _receive_scheduler.register_connection ();
        while (!_stop->load (std::memory_order_acquire)
               && (!connection_state->replacement
                   || !connection_state->replacement->closing.load (std::memory_order_acquire))) {
            receive_batch_budget_t batch;
            std::optional<session_liveness_t::clock_t::time_point> ready_read_started;
            const auto progress = [&] (tcp_read_progress_t *read = nullptr) {
                const auto now = session_liveness_t::clock_t::now ();
                if (liveness_timer.expiry () <= now) {
                    if (!ready_read_started)
                        ready_read_started = now;
                    // A partial first frame need not reach batch.account().
                    // Bound the ready I/O pass by the same receive-round time limit.
                    if (!batch.exhausted () && now - *ready_read_started < batch.max_elapsed
                        && io.poll_one () != 0)
                        return;
                    const auto settle_receive_turn = [&] {
                        settle_liveness ();
                        ready_read_started.reset ();
                    };
                    if (!read) {
                        settle_receive_turn ();
                    } else if (read->remaining && !read->posted_settle) {
                        const auto remaining = *read->remaining;
                        boost::system::error_code error;
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
                        if (remaining != 0 && _shutdown_test_hooks.before_tcp_read_expiry_check)
                            _shutdown_test_hooks.before_tcp_read_expiry_check (remaining);
#endif
                        if (remaining != 0
                            && tcp_socket (*connection).available (error) >= remaining && !error) {
                            read->remaining.reset ();
                        } else {
                            read->posted_settle =
                              std::make_shared<tcp_read_progress_t::posted_settle_t> ();
                            asio::post (io, [read, settle_receive_turn,
                                             pending = std::weak_ptr (read->posted_settle)] {
                                // A completed read returns to the reader before the next
                                // handler. Its queued settle no longer owns this decision.
                                if (pending.expired ())
                                    return;
                                read->posted_settle.reset ();
                                settle_receive_turn ();
                            });
                        }
                    }
                }
                io.run_one ();
            };
            while (!_stop->load (std::memory_order_acquire)
                   && (!connection_state->replacement
                       || !connection_state->replacement->closing.load (std::memory_order_acquire))
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
                application_job_queue_t::permit_t application_permit;
                const auto reserve = [&] {
                    application_job_queue_t::supply_request_t supply;
                    const auto wake = [weak_owner = std::weak_ptr<tcp_connection_t> (owner)] {
                        if (const auto connection_owner = weak_owner.lock ())
                            connection_owner->wake_reader ();
                    };
                    for (;;) {
                        auto reserved =
                          supply.take (*_application_jobs, std::chrono::milliseconds::zero (), wake,
                                       application_job_queue_t::origin_t::remote);
                        owner->reader_liveness.observe_permit (reserved.has_value ());
                        if (reserved) {
                            if (!*reserved)
                                return false;
                            application_permit = std::move (**reserved);
                            return true;
                        }
                        if (stream_stop_requested (*_stop, &owner->closing))
                            return false;
                        io.restart ();
                        const auto running = asio::make_work_guard (io);
                        progress ();
                    }
                };
                auto frame = read_frame (*connection, reserve, io, &owner->closing, progress);
                if (!frame) {
                    break;
                }
                if (connection_state->replacement
                    && connection_state->replacement->closing.load (std::memory_order_acquire)) {
                    break;
                }
                auto received_frame = std::move (*frame);
                liveness->record_inbound (session_liveness_t::clock_t::now (),
                                          received_frame.header.kind ()
                                            != stream_message_kind_t::control);
                batch.account (received_frame.payload.size ());
                if (received_frame.header.kind () == stream_message_kind_t::control) {
                    _runtime.dispatch_control_frame (stream, received_frame.header,
                                                     received_frame.payload);
                    continue;
                }
                const auto header = received_frame.header;
                trace_stream_host ("dispatch-submit", _stream, header);
                auto dispatched = _ingress.publish (
                  _runtime, session, stream, header, received_frame.payload,
                  std::move (application_permit),
                  [this, owner, connection, connection_state, header, stream,
                   liveness] (const result_t<void> &result) {
                      trace_stream_host ("dispatch-complete", _stream, header,
                                         std::string ("result=")
                                           + (result ? "success" : "failure"));
                      if (!result) {
                          report_packet_dispatch_error (header, result);
                          if (header.kind () == stream_message_kind_t::request
                              && !_shutdown_cancellation.stop_requested ()) {
                              try {
                                  write_error_frame (owner, connection, stream, header, result);
                              }
                              catch (...) {
                                  report_packet_dispatch_error (
                                    header, detail::current_exception_result<void> ());
                                  request_close (*connection);
                              }
                          }
                      }
                      (void) connection_state;
                      (void) liveness;
                  },
                  [this] { return _stop->load (std::memory_order_acquire); },
                  [&] (const result_t<void> &error, detail::stream_metadata_t metadata) {
                      report_packet_dispatch_error (header, error);
                      write_error_frame (owner, connection, stream, header, error,
                                         std::move (metadata));
                  });
                if (!dispatched) {
                    report_packet_dispatch_error (header, dispatched);
                    if (header.kind () == stream_message_kind_t::request) {
                        try {
                            write_error_frame (owner, connection, stream, header, dispatched);
                        }
                        catch (...) {
                            report_packet_dispatch_error (
                              header, detail::current_exception_result<void> ());
                            request_close (*connection);
                        }
                    }
                }
            }
            if (batch.exhausted ())
                settle_liveness ();
        }
    }
    catch (const boost::system::system_error &error) {
        if (connected_session && !_stop->load (std::memory_order_acquire) && !liveness->forced ()
            && !is_expected_session_disconnect (error.code ())) {
            session_transport_error.emplace (stream_session_error_t::transport_error,
                                             error.what ());
        }
        if (stream_trace_enabled ()) {
            std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                      << _stream.name << " endpoint=" << _stream.bind_endpoint << " error=\""
                      << error.what () << "\"" << std::endl;
        }
    }
    catch (const stream_message_size_exceeded_t &error) {
        trace_stream_host ("message-size-rejected", _stream, std::nullopt,
                           "errno=EMSGSIZE limit=" + std::to_string (error.max_message_size ())
                             + " header_bytes=" + std::to_string (error.header_size ())
                             + " payload_bytes=" + std::to_string (error.payload_size ()));
    }
    catch (const framework_exception_t &error) {
        report_control_failure ([] { return std::current_exception (); });
        if (stream_trace_enabled ()) {
            std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                      << _stream.name << " endpoint=" << _stream.bind_endpoint << " error=\""
                      << error.what () << "\"" << std::endl;
        }
    }
    catch (const std::exception &error) {
        report_control_failure ([] { return std::current_exception (); });
        if (stream_trace_enabled ()) {
            std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                      << _stream.name << " endpoint=" << _stream.bind_endpoint << " error=\""
                      << error.what () << "\"" << std::endl;
        }
    }
    catch (...) {
        report_control_failure ([] { return std::current_exception (); });
        if (stream_trace_enabled ()) {
            std::cerr << "zlink-cpp-stream-trace side=server stage=connection-error stream="
                      << _stream.name << " endpoint=" << _stream.bind_endpoint
                      << " error=\"unknown\"" << std::endl;
        }
    }
    liveness_timer.cancel ();
    finish_transport ();
    begin_connection_retirement (connection_state);
    if (connected_session) {
        // The registry lane barrier excludes further forced-close submissions.
        // Settle their queued owner work before reading the close reason.
        owner->io.restart ();
        owner->io.run ();
        const auto forced = liveness->forced ();
        const bool stopping = _stop->load (std::memory_order_acquire);
        auto dispatch_disconnect = [self = shared_from_this (), connection_state,
                                    session_transport_error, forced,
                                    stopping] (std::function<void ()> retired) {
            if (!stopping) {
                trace_stream_host ("dispatch-disconnected-begin", self->_stream);
            }
            auto disconnected = [self, connection_state,
                                 retired = std::move (retired)] (const result_t<void> &) {
                auto completed = [self, connection_state, retired] (const result_t<void> &) {
                    trace_stream_host ("dispatch-disconnected-end", self->_stream);
                    retired ();
                };
                auto submitted = self->_runtime.dispatch_disconnected_async (
                  *connection_state->session, connection_state->stream, completed);
                if (!submitted)
                    completed (submitted);
            };
            if (!stopping && session_transport_error && !forced) {
                auto submitted = self->_runtime.dispatch_error_async (
                  *connection_state->session, connection_state->stream, *session_transport_error,
                  disconnected);
                if (!submitted)
                    disconnected (submitted);
            } else {
                disconnected (result_t<void>::success ());
            }
        };
        record_connection_closed (stopping                  ? "server_shutdown"
                                  : forced                  ? liveness_close_label (*forced)
                                  : session_transport_error ? "transport_error"
                                                            : "client_close");
        retire_connection (connection_state, std::move (dispatch_disconnect));
        _runtime.drain_async_dispatch (stream);
        // Connected/packet/disconnected callbacks have settled, and the
        // active entry is gone. Release their final IO posts on this owner.
        owner->io.restart ();
        owner->io.run ();
    } else {
        retire_connection (connection_state);
    }
}

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
void stream_host_service_t::listener_t::handle_tls_connection (
  std::shared_ptr<tcp_connection_t> owner, std::shared_ptr<ssl::stream<tcp::socket>> connection)
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
          [connection] { cancel_stream (connection->next_layer ()); }, &owner->closing,
          [&] { owner->io.run_one (); });
        if (error) {
            throw boost::system::system_error (error);
        }
    }
    catch (const boost::system::system_error &) {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        _sockets.erase (&connection->next_layer ());
        return;
    }
    handle_stream_connection (owner, connection, &connection->next_layer (), owner->io);
}

#endif

void stream_host_service_t::listener_t::handle_websocket_connection (
  std::shared_ptr<tcp_connection_t> owner, std::shared_ptr<websocket_stream_t> connection)
{
    try {
        const auto error = run_stream_async_operation (
          owner->io, *_stop,
          [&] (auto completion) {
              connection->async_accept ([completion = std::move (completion)] (
                                          const boost::system::error_code &accept_error) mutable {
                  completion (accept_error, 0);
              });
          },
          [connection] { cancel_stream (connection->next_layer ()); }, &owner->closing,
          [&] { owner->io.run_one (); });
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
    handle_stream_connection (owner, connection, &connection->next_layer (), owner->io);
}

void stream_host_service_t::listener_t::handle_connection (
  std::shared_ptr<tcp_connection_t> connection)
{
    handle_stream_connection (connection,
                              std::shared_ptr<tcp::socket> (connection, &connection->socket),
                              &connection->socket, connection->io);
}

} // namespace zlink::framework::runtime
