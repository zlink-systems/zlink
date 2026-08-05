/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "connector_runtime.hpp"

#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/packet_name_resolver.hpp"
#include "runtime/transport/stream_connection.hpp"
#include "runtime/transport/stream_transport_factory.hpp"
#include "runtime/transport/websocket_connection.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
#include <openssl/crypto.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zlink::stream_connector::detail
{

void cancel_timer (const std::shared_ptr<boost::asio::steady_timer> &timer)
{
    if (!timer) {
        return;
    }
    try {
        (void) timer->cancel ();
    } catch (const boost::system::system_error &) {
    }
}

namespace
{

bool stream_trace_enabled ()
{
    static const bool enabled = [] {
        const char *value = std::getenv ("ZLINK_CPP_STREAM_TRACE");
        return value != nullptr && value[0] != '\0' && std::string (value) != "0";
    }();
    return enabled;
}

const char *connection_state_name (connection_state_t state) noexcept
{
    switch (state) {
        case connection_state_t::created:
            return "created";
        case connection_state_t::connecting:
            return "connecting";
        case connection_state_t::connected:
            return "connected";
        case connection_state_t::reconnecting:
            return "reconnecting";
        case connection_state_t::disconnected:
            return "disconnected";
        case connection_state_t::closed:
            return "closed";
    }
    return "unknown";
}

std::mutex &shared_runtime_config_mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::size_t &shared_runtime_worker_count ()
{
    static std::size_t worker_count = 4;
    return worker_count;
}

bool &shared_runtime_started ()
{
    static bool started = false;
    return started;
}

class shared_operation_runner_t
{
  public:
    explicit shared_operation_runner_t (std::size_t worker_count, bool runtime_runner = false)
    {
        if (runtime_runner) {
            std::lock_guard<std::mutex> lock (shared_runtime_config_mutex ());
            worker_count = shared_runtime_worker_count ();
            shared_runtime_started () = true;
        }
        for (auto index = 0u; index < worker_count; ++index) {
            _workers.emplace_back ([this] {
                _io_context.run ();
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
                OPENSSL_thread_stop ();
#endif
            });
        }
    }

    ~shared_operation_runner_t ()
    {
        _work.reset ();
        _io_context.stop ();
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    }

    void post (std::function<void ()> operation)
    {
        boost::asio::post (_io_context, [operation = std::move (operation)] () mutable {
            try {
                operation ();
            }
            catch (const std::exception &) {
            }
            catch (...) {
            }
        });
    }

    std::shared_ptr<boost::asio::steady_timer>
    post_after (std::chrono::milliseconds delay, std::function<void ()> operation)
    {
        auto timer = std::make_shared<boost::asio::steady_timer> (_io_context);
        timer->expires_after (delay);
        timer->async_wait (
          [timer, operation = std::move (operation)] (const boost::system::error_code &error) mutable {
              if (error) {
                  return;
              }
              try {
                  operation ();
              }
              catch (const std::exception &) {
              }
              catch (...) {
              }
          });
        return timer;
    }

    boost::asio::io_context &io_context () noexcept
    {
        return _io_context;
    }

  private:
    boost::asio::io_context _io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work{
      boost::asio::make_work_guard (_io_context)};
    std::vector<std::thread> _workers;
};

shared_operation_runner_t &shared_operation_runner ()
{
    static shared_operation_runner_t runner (4, true);
    return runner;
}

shared_operation_runner_t &shared_callback_runner ()
{
    static shared_operation_runner_t runner (4);
    return runner;
}

shared_operation_runner_t &shared_connect_runner ()
{
    static shared_operation_runner_t runner (4);
    return runner;
}

} // namespace

boost::asio::io_context &shared_io_context ()
{
    /* connector_state_t owns strands that reference both shared contexts.
     * Initialize the callback runner first so its io_context and strand
     * service outlive the operation runner's pending handlers during static
     * teardown. Without this ordering, a handler released while the
     * operation runner is shutting down can destroy a delivery strand after
     * its callback io_context has already destroyed the strand service. */
    (void) shared_callback_runner ();
    return shared_operation_runner ().io_context ();
}

boost::asio::io_context &shared_callback_io_context ()
{
    return shared_callback_runner ().io_context ();
}

bool configure_shared_runtime_worker_count (std::size_t worker_count)
{
    if (worker_count == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock (shared_runtime_config_mutex ());
    if (shared_runtime_started ()) {
        return false;
    }
    shared_runtime_worker_count () = worker_count;
    return true;
}

std::shared_ptr<connector_state_t> state_from (const std::shared_ptr<void> &state)
{
    return std::static_pointer_cast<connector_state_t> (state);
}

connector_runtime_t::connector_runtime_t (std::shared_ptr<connector_state_t> state) :
    _state (std::move (state))
{
}

connector_runtime_t connector_runtime_t::from (const connector_t &connector)
{
    return connector_runtime_t (state_from (connector_internal_handle (connector)));
}

void deliver_received_packet (connector_state_t &state, packet_t packet)
{
    if (packet.name.rfind ("$zlink.", 0) == 0) {
        return;
    }
    if (state.options.dispatch_mode == dispatch_mode_t::immediate) {
        dispatch_packet (state, packet);
        state.state_changed.notify_all ();
        return;
    }
    if (state.dispatch_queue.size () >= state.options.max_received_messages) {
        publish_error (
          state, error_t{error_code_t::received_message_dropped,
                         "Received message was dropped because the receive queue is full."});
        state.state_changed.notify_all ();
        return;
    }
    state.dispatch_queue.push_back (std::move (packet));
    state.state_changed.notify_all ();
}

void schedule_delivery (std::shared_ptr<connector_state_t> state, std::function<void ()> callback)
{
    if (!callback) {
        return;
    }
    if (state->options.dispatch_mode == dispatch_mode_t::immediate) {
        // Deliver on the shared runner instead of the calling thread. Deliveries
        // are frequently invoked from the connector's own read pump; running user
        // callbacks inline there lets a slow or blocking callback starve the pump
        // (and deadlock when the callback waits on a later inbound frame).
        boost::asio::post (state->delivery_strand,
                           [callback = std::move (callback)] () mutable {
                               try {
                                   callback ();
                               }
                               catch (...) {
                               }
                           });
        return;
    }
    std::lock_guard<std::mutex> lock (state->delivery_mutex);
    state->delivery_queue.push_back (std::move (callback));
    state->state_changed.notify_all ();
}

void publish_error (connector_state_t &state, error_t error) noexcept
{
    std::vector<std::function<void (const error_t &)>> handlers;
    {
        std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
        handlers = state.error_handlers;
    }
    if (handlers.empty ()) {
        return;
    }
    schedule_lifecycle_delivery (
      state.shared_from_this (),
      [handlers = std::move (handlers), error = std::move (error)] {
          for (const auto &handler : handlers) {
              try {
                  handler (error);
              }
              catch (...) {
              }
          }
      });
}

void schedule_delivery (std::shared_ptr<void> state, std::function<void ()> callback)
{
    schedule_delivery (state_from (state), std::move (callback));
}

void schedule_lifecycle_delivery (std::shared_ptr<connector_state_t> state,
                                  std::function<void ()> callback)
{
    if (!callback) {
        return;
    }
    auto callback_state = state;
    schedule_delivery (
      state,
      [state = std::move (callback_state), callback = std::move (callback)] () mutable {
          if (state->lifecycle_callbacks_enabled.load (std::memory_order_acquire)) {
              callback ();
          }
      });
}

void post_runtime_operation (std::function<void ()> operation)
{
    shared_operation_runner ().post (std::move (operation));
}

void post_connect_operation (std::function<void ()> operation)
{
    shared_connect_runner ().post (std::move (operation));
}

std::shared_ptr<boost::asio::steady_timer>
post_runtime_operation_after (std::chrono::milliseconds delay,
                              std::function<void ()> operation)
{
    return shared_operation_runner ().post_after (delay, std::move (operation));
}

void connector_runtime_t::receive_packet (packet_t packet)
{
    deliver_received_packet (*_state, std::move (packet));
}

const std::vector<packet_t> &connector_runtime_t::sent_packets () const noexcept
{
    return _state->sent_packets;
}

std::size_t connector_runtime_t::pending_request_count () const noexcept
{
    return _state->pending_requests.size ();
}

void change_state (std::shared_ptr<connector_state_t> state,
                   connection_state_t next,
                   std::optional<error_t> error)
{
    connection_state_t previous;
    std::vector<std::function<void (const connection_state_changed_t &)>> state_handlers;
    std::vector<std::function<void ()>> disconnected_handlers;
    std::optional<close_reason_t> close_reason;
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        // close() is terminal. A read error, reconnect timer, or late
        // connect completion can otherwise publish a non-closed state after
        // the caller has already crossed the synchronous close boundary.
        if (next != connection_state_t::closed
            && (state->state == connection_state_t::closed
                || state->close_requested.load (std::memory_order_acquire))) {
            return;
        }
        previous = state->state;
        state->state = next;
        if (error
            && (next == connection_state_t::disconnected || next == connection_state_t::closed)) {
            state->last_disconnect_error = error;
        }
        if (next == connection_state_t::disconnected || next == connection_state_t::closed) {
            if (!state->last_close_reason) {
                state->last_close_reason =
                  error ? close_reason_t::transport_error : close_reason_t::client_close;
            }
            close_reason = state->last_close_reason;
            disconnected_handlers = state->disconnected_handlers;
        } else if (next == connection_state_t::connected) {
            state->last_close_reason.reset ();
        }
        state_handlers = state->state_handlers;
    }
    if (stream_trace_enabled ()) {
        std::cerr << "zlink-cpp-stream-trace side=client connector=" << state->connector_id
                  << " stage=state previous=" << connection_state_name (previous)
                  << " next=" << connection_state_name (next);
        if (error) {
            std::cerr << " error_code=" << static_cast<int> (error->code)
                      << " error=\"" << error->message << "\"";
        }
        std::cerr << '\n';
    }
    connection_state_changed_t changed{previous, next, error, close_reason};
    if (!state_handlers.empty () || !disconnected_handlers.empty ()) {
        detail::schedule_lifecycle_delivery (
          state, [state_handlers = std::move (state_handlers),
                  disconnected_handlers = std::move (disconnected_handlers),
                  changed = std::move (changed)] {
              for (const auto &handler : state_handlers) {
                  handler (changed);
              }
              for (const auto &handler : disconnected_handlers) {
                  handler ();
              }
          });
    }
    // Publish the state-change notifications after the lifecycle delivery is
    // queued. A caller that observes the new state can then dispatch the full
    // callback set for that transition.
    state->lifecycle_changed.notify_all ();
    state->state_changed.notify_all ();
}

} // namespace zlink::stream_connector::detail

namespace zlink::stream_connector
{

std::shared_ptr<void> connector_internal_handle (const connector_t &connector)
{
    return connector._state;
}

codec_registry_t::codec_registry_t () :
    _state (std::make_shared<detail::connector_state_t> (connector_options_t{}))
{
}

codec_registry_t::codec_registry_t (std::shared_ptr<void> state) : _state (std::move (state))
{
}

codec_registry_t::~codec_registry_t () = default;
codec_registry_t::codec_registry_t (codec_registry_t &&) noexcept = default;
codec_registry_t &codec_registry_t::operator= (codec_registry_t &&) noexcept = default;

codec_registry_t &codec_registry_t::enable_codec (codec_t codec)
{
    auto state = detail::state_from (_state);
    state->enabled_codecs.insert (codec);
    return *this;
}

codec_registry_t &codec_registry_t::use_default_codec (codec_t codec)
{
    if (!supports (codec)) {
        throw std::invalid_argument ("stream connector codec is not enabled");
    }
    auto state = detail::state_from (_state);
    state->default_codec = codec;
    return *this;
}

bool codec_registry_t::supports (codec_t codec) const
{
    auto state = detail::state_from (_state);
    switch (codec) {
        case codec_t::raw:
            return true;
        case codec_t::message_pack:
        case codec_t::protobuf:
        case codec_t::json:
            return state->enabled_codecs.find (codec) != state->enabled_codecs.end ();
    }
    return false;
}

send_call_t::send_call_t () = default;
send_call_t::send_call_t (std::shared_ptr<void> state, packet_t packet) :
    _state (std::move (state)), _packet (std::move (packet))
{
}
send_call_t::~send_call_t () = default;
send_call_t::send_call_t (send_call_t &&) noexcept = default;
send_call_t &send_call_t::operator= (send_call_t &&) noexcept = default;

send_call_t &send_call_t::packet_name (std::string name)
{
    _packet.name = std::move (name);
    return *this;
}

send_call_t &send_call_t::metadata (std::string key, std::string value)
{
    _packet.metadata.with (std::move (key), std::move (value));
    return *this;
}

send_call_t &send_call_t::metadata (metadata_t metadata)
{
    _packet.metadata = std::move (metadata);
    return *this;
}

send_call_t &send_call_t::codec (codec_t codec)
{
    _packet.codec = codec;
    return *this;
}

send_call_t &send_call_t::compress ()
{
    _packet.compressed = true;
    return *this;
}

void send_call_t::submit ()
{
    if (!_state) {
        return;
    }
    (void) detail::submit_send (detail::state_from (_state), std::move (_packet));
}

connector_t::connector_t () : connector_t (connector_options_t{})
{
}

connector_t::connector_t (connector_options_t options) :
    _state (std::make_shared<detail::connector_state_t> (std::move (options))), _codecs (_state)
{
    auto state = detail::state_from (_state);
    if (state->options.compression == compression_t::none) {
        state->options.compression_codec.reset ();
    } else if (!state->options.compression_codec) {
        state->options.compression_codec = lz4_compression_codec ();
    }
    state->compression_codec = state->options.compression_codec;
#ifndef ZLINK_STREAM_CONNECTOR_WITH_LZ4
    state->lz4_enabled = false;
#else
    state->lz4_enabled = true;
#endif
}

connector_t::~connector_t () = default;
connector_t::connector_t (connector_t &&) noexcept = default;
connector_t &connector_t::operator= (connector_t &&) noexcept = default;

inbound_observer_registration_t::inbound_observer_registration_t (
  std::shared_ptr<void> state, std::uint64_t id) :
    _state (std::move (state)), _id (id)
{
}

inbound_observer_registration_t::~inbound_observer_registration_t ()
{
    close ();
}

inbound_observer_registration_t::inbound_observer_registration_t (
  inbound_observer_registration_t &&other) noexcept :
    _state (std::move (other._state)), _id (std::exchange (other._id, 0))
{
}

inbound_observer_registration_t &
inbound_observer_registration_t::operator= (inbound_observer_registration_t &&other) noexcept
{
    if (this != &other) {
        close ();
        _state = std::move (other._state);
        _id = std::exchange (other._id, 0);
    }
    return *this;
}

void inbound_observer_registration_t::close ()
{
    if (!_state || _id == 0) {
        return;
    }
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    for (auto &observer : state->inbound_observers) {
        if (observer && observer->id == _id) {
            observer->active.store (false);
            break;
        }
    }
    _state.reset ();
    _id = 0;
}

inbound_observer_registration_t::operator bool () const noexcept
{
    return _state && _id != 0;
}

bool connector_t::is_connected () const
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    return state->state == connection_state_t::connected;
}

connection_state_t connector_t::state () const
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    return state->state;
}

connector_options_t connector_t::options () const
{
    return detail::state_from (_state)->options;
}

std::size_t connector_t::pending_dispatch_count () const
{
    auto state = detail::state_from (_state);
    std::size_t packets;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        packets = state->dispatch_queue.size ();
    }
    std::lock_guard<std::mutex> lock (state->delivery_mutex);
    return packets + state->delivery_queue.size ();
}

codec_registry_t &connector_t::codecs ()
{
    return _codecs;
}

namespace
{

struct parsed_endpoint_options_t
{
    std::optional<detail::endpoint_parts_t> tcp;
    std::optional<detail::endpoint_parts_t> tls;
    std::optional<detail::websocket_endpoint_parts_t> websocket;
    std::optional<detail::websocket_endpoint_parts_t> websocket_secure;
};

result_t<parsed_endpoint_options_t>
parse_connect_options (const std::shared_ptr<detail::connector_state_t> &state)
{
    if (state->options.endpoint.empty ()) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error, "stream connector endpoint is required");
    }
    if (!detail::stream_transport_factory_t::is_supported (state->options.transport)) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector does not support the configured transport in this build");
    }
    if (state->options.max_received_messages == 0) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector max_received_messages must be greater than zero");
    }
    if (state->options.connect_timeout <= std::chrono::milliseconds::zero ()) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector connect_timeout must be greater than zero");
    }
    parsed_endpoint_options_t parsed;
    parsed.tcp = state->options.transport == transport_t::tcp
                   ? detail::parse_tcp_endpoint (state->options.endpoint)
                   : std::optional<detail::endpoint_parts_t>{};
    parsed.tls = state->options.transport == transport_t::tls
                   ? detail::parse_tls_endpoint (state->options.endpoint)
                   : std::optional<detail::endpoint_parts_t>{};
    parsed.websocket = state->options.transport == transport_t::websocket
                         ? detail::parse_websocket_endpoint (state->options.endpoint)
                         : std::optional<detail::websocket_endpoint_parts_t>{};
    parsed.websocket_secure = state->options.transport == transport_t::websocket_secure
                                ? detail::parse_websocket_secure_endpoint (state->options.endpoint)
                                : std::optional<detail::websocket_endpoint_parts_t>{};
    if (state->options.transport == transport_t::tcp && !parsed.tcp) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error, "stream connector endpoint must use tcp://host:port");
    }
    if (state->options.transport == transport_t::tls && !parsed.tls) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error, "stream connector endpoint must use tls://host:port");
    }
    if (state->options.transport == transport_t::websocket && !parsed.websocket) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector endpoint must use ws://host:port/path");
    }
    if (state->options.transport == transport_t::websocket_secure && !parsed.websocket_secure) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector endpoint must use wss://host:port/path");
    }
    return result_t<parsed_endpoint_options_t>::success (std::move (parsed));
}

std::optional<result_t<void>>
begin_connect_attempt (const std::shared_ptr<detail::connector_state_t> &state,
                       bool automatic_reconnect)
{
    std::unique_lock<std::mutex> lock (state->lifecycle_mutex);
    if (automatic_reconnect) {
        state->reconnect_scheduled = false;
        state->reconnect_timer.reset ();
    } else if (state->connect_attempt_active || state->reconnect_scheduled) {
        state->lifecycle_changed.wait (lock, [&] {
            return !state->connect_attempt_active && !state->reconnect_scheduled;
        });
        if (state->state == connection_state_t::connected) {
            return result_t<void>::success ();
        }
        if (state->state == connection_state_t::closed) {
            return result_t<void>::failure (error_code_t::closed,
                                            "stream connector is closed");
        }
        const auto error = state->last_disconnect_error;
        return result_t<void>::failure (
          error ? error->code : error_code_t::disconnected,
          error ? error->message : "stream connector is not connected");
    }
    if (state->state == connection_state_t::connected) {
        return result_t<void>::success ();
    }
    if (state->state == connection_state_t::closed || state->close_requested.load ()) {
        return result_t<void>::failure (error_code_t::closed, "stream connector is closed");
    }
    state->connect_attempt_active = true;
    return std::nullopt;
}

class connect_attempt_guard_t
{
  public:
    explicit connect_attempt_guard_t (std::shared_ptr<detail::connector_state_t> state) :
        _state (std::move (state))
    {
    }

    ~connect_attempt_guard_t ()
    {
        std::lock_guard<std::mutex> lock (_state->lifecycle_mutex);
        _state->connect_attempt_active = false;
        _state->lifecycle_changed.notify_all ();
    }

  private:
    std::shared_ptr<detail::connector_state_t> _state;
};

class bounded_transport_connect_t : public std::enable_shared_from_this<bounded_transport_connect_t>
{
  public:
    void complete (boost::system::error_code error,
                   std::unique_ptr<detail::stream_connection_t> connection)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        if (_completed) {
            lock.unlock ();
            if (connection) {
                std::shared_ptr<detail::stream_connection_t> owned (std::move (connection));
                owned->shutdown_and_close_async ();
            }
            return;
        }
        _completed = true;
        _error = error;
        _connection = std::move (connection);
        lock.unlock ();
        _ready.notify_all ();
    }

    std::pair<boost::system::error_code, std::unique_ptr<detail::stream_connection_t>> wait ()
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _ready.wait (lock, [&] { return _completed; });
        return {_error, std::move (_connection)};
    }

  private:
    std::mutex _mutex;
    std::condition_variable _ready;
    bool _completed = false;
    boost::system::error_code _error;
    std::unique_ptr<detail::stream_connection_t> _connection;
};

result_t<std::unique_ptr<detail::stream_connection_t>>
connect_transport (const std::shared_ptr<detail::connector_state_t> &state,
                   const parsed_endpoint_options_t &parsed,
                   std::chrono::milliseconds timeout)
{
    auto operation = std::make_shared<bounded_transport_connect_t> ();
    auto control = std::make_shared<detail::transport_connect_control_t> ();
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->connect_control = control;
    }
    auto timeout_timer = detail::post_runtime_operation_after (timeout, [operation, control] {
        control->cancel ();
        operation->complete (boost::asio::error::timed_out, nullptr);
    });
    auto completion = [operation] (
                        boost::system::error_code error,
                        std::unique_ptr<detail::stream_connection_t> connection) mutable {
        operation->complete (error, std::move (connection));
    };

    if (state->options.transport == transport_t::websocket) {
        detail::connect_websocket_async (state->io_context, *parsed.websocket,
                                         control,
                                         std::move (completion));
    } else if (state->options.transport == transport_t::websocket_secure) {
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
        detail::connect_websocket_secure_async (
          state->io_context, *parsed.websocket_secure,
          state->options.skip_server_certificate_validation, control,
          std::move (completion));
#else
        operation->complete (boost::asio::error::operation_not_supported, nullptr);
#endif
    } else if (state->options.transport == transport_t::tls) {
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
        detail::connect_tls_async (state->io_context, *parsed.tls,
                                   state->options.skip_server_certificate_validation,
                                   control,
                                   std::move (completion));
#else
        operation->complete (boost::asio::error::operation_not_supported, nullptr);
#endif
    } else {
        auto resolver = std::make_shared<boost::asio::ip::tcp::resolver> (state->io_context);
        auto socket = std::make_shared<boost::asio::ip::tcp::socket> (state->io_context);
        control->set_cancel_handler ([resolver, socket] {
            boost::system::error_code ignored;
            resolver->cancel ();
            socket->cancel (ignored);
            socket->close (ignored);
        });
        resolver->async_resolve (
          parsed.tcp->host, parsed.tcp->port,
          [resolver, socket, state, control, completion = std::move (completion)] (
            boost::system::error_code error,
            boost::asio::ip::tcp::resolver::results_type endpoints) mutable {
              if (control->cancelled ()) {
                  completion (boost::asio::error::operation_aborted, nullptr);
                  return;
              }
              if (error) {
                  completion (error, nullptr);
                  return;
              }
              boost::asio::async_connect (
                *socket, endpoints,
                [socket, state, control, completion = std::move (completion)] (
                  boost::system::error_code connect_error,
                  const boost::asio::ip::tcp::endpoint &) mutable {
                    if (control->cancelled ()) {
                        completion (boost::asio::error::operation_aborted, nullptr);
                        return;
                    }
                    if (connect_error) {
                        completion (connect_error, nullptr);
                        return;
                    }
                    completion ({}, detail::make_tcp_connection (state->io_context,
                                                                  std::move (*socket)));
                });
          });
    }

    auto [error, connection] = operation->wait ();
    detail::cancel_timer (timeout_timer);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        if (state->connect_control == control)
            state->connect_control.reset ();
    }
    if (error || !connection) {
        return result_t<std::unique_ptr<detail::stream_connection_t>>::failure (
          error_code_t::connect_timeout,
          error ? error.message () : "stream connector transport connect failed");
    }
    return result_t<std::unique_ptr<detail::stream_connection_t>>::success (
      std::move (connection));
}

void schedule_start_read_loop (std::shared_ptr<detail::connector_state_t> state)
{
    detail::post_runtime_operation ([state = std::move (state)] {
        detail::start_read_loop (state);
    });
}

result_t<void> connect_state (std::shared_ptr<detail::connector_state_t> state,
                              bool automatic_reconnect = false)
{
    if (auto existing = begin_connect_attempt (state, automatic_reconnect)) {
        return std::move (*existing);
    }
    connect_attempt_guard_t attempt (state);
    state->connect_started = true;
    auto parsed = parse_connect_options (state);
    if (!parsed) {
        const auto message = parsed.error () ? parsed.error ()->message
                                             : "stream connector endpoint is invalid";
        detail::change_state (state, connection_state_t::disconnected,
                              error_t{parsed.error_code (), message});
        return result_t<void>::failure (parsed.error_code (), message);
    }

    const auto max_attempts = state->options.reconnect.enabled
                                ? state->options.reconnect.max_attempts
                                : std::optional<int> (1);
    auto retry_delay = state->options.reconnect.initial_delay;
    std::string last_error;
    const auto deadline = std::chrono::steady_clock::now () + state->options.connect_timeout;

    for (int attempt_number = 1;
         !max_attempts || attempt_number <= std::max (1, *max_attempts);
         ++attempt_number) {
        if (state->close_requested.load (std::memory_order_acquire))
            return result_t<void>::failure (
              error_code_t::closed, "stream connector is closed");
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline) {
            last_error = "stream connector connect timed out";
            break;
        }
        if (!automatic_reconnect || attempt_number != 1) {
            detail::change_state (state, attempt_number == 1 ? connection_state_t::connecting
                                                             : connection_state_t::reconnecting);
        }
        const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
        auto connected = connect_transport (state, parsed.value (), remaining);
        if (connected) {
            if (state->close_requested.load (std::memory_order_acquire)) {
                if (connected.value ()) {
                    std::shared_ptr<detail::stream_connection_t> owned (
                      std::move (connected.value ()));
                    owned->shutdown_and_close_async ();
                }
                return result_t<void>::failure (
                    error_code_t::closed, "stream connector is closed");
            }
            connected.value ()->set_read_message_limit (
              state->options.max_receive_payload_size
              + static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ())
              + 6u);
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                state->connection = std::move (connected.value ());
                const auto connected_at = std::chrono::steady_clock::now ();
                state->last_heartbeat_sent = connected_at;
                state->last_inbound_received = connected_at;
            }
            detail::change_state (state, connection_state_t::connected);
            detail::resume_pending_writes_after_connect (state);
            schedule_start_read_loop (state);
            detail::start_heartbeat_monitor (state);
            return result_t<void>::success ();
        }
        last_error = connected.error () ? connected.error ()->message
                                        : "stream connector transport connect failed";
        if (state->close_requested.load (std::memory_order_acquire))
            return result_t<void>::failure (
              error_code_t::closed, "stream connector is closed");
        if (max_attempts && attempt_number >= std::max (1, *max_attempts)) {
            break;
        }
        const auto delay = std::min (
          retry_delay,
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - std::chrono::steady_clock::now ()));
        if (delay > std::chrono::milliseconds::zero ()) {
            std::mutex retry_mutex;
            std::condition_variable retry_ready;
            std::unique_lock<std::mutex> retry_lock (retry_mutex);
            retry_ready.wait_for (retry_lock, delay);
        }
        retry_delay =
          std::min (state->options.reconnect.max_delay,
                    std::chrono::milliseconds (static_cast<int> (
                      retry_delay.count () * state->options.reconnect.backoff_factor)));
    }

    detail::change_state (state, connection_state_t::disconnected,
                          error_t{error_code_t::connect_timeout, last_error});
    return result_t<void>::failure (error_code_t::connect_timeout, last_error);
}

} // namespace

void detail::schedule_reconnect (std::shared_ptr<detail::connector_state_t> state)
{
    if (!state->options.reconnect.enabled || state->close_requested.load ()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        if (state->state == connection_state_t::closed
            || state->close_requested.load (std::memory_order_acquire)
            || state->connect_attempt_active || state->reconnect_scheduled) {
            return;
        }
        state->reconnect_scheduled = true;
    }
    detail::change_state (state, connection_state_t::reconnecting);
    auto timer = detail::post_runtime_operation_after (
      state->options.reconnect.initial_delay, [state] {
          detail::post_connect_operation ([state] {
              if (state->close_requested.load ()) {
                  std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
                  state->reconnect_scheduled = false;
                  state->reconnect_timer.reset ();
                  state->lifecycle_changed.notify_all ();
                  return;
              }
              (void) connect_state (state, true);
          });
      });
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    if (state->reconnect_scheduled) {
        state->reconnect_timer = std::move (timer);
    } else {
        detail::cancel_timer (timer);
    }
}

result_t<void> connector_t::connect ()
{
    return connect_state (detail::state_from (_state));
}

void connector_t::connect (std::function<void (result_t<void>)> callback)
{
    auto state = detail::state_from (_state);
    detail::post_connect_operation ([state, callback = std::move (callback)] () mutable {
        auto result = connect_state (state);
        detail::schedule_delivery (
          state, [callback = std::move (callback), result = std::move (result)] () mutable {
              if (callback) {
                  callback (std::move (result));
              }
          });
    });
}

namespace
{

result_t<void> close_state (std::shared_ptr<detail::connector_state_t> state)
{
    // Operation completions still run below with a closed result. Registered
    // lifecycle callbacks are different: a queued callback can hold a
    // caller-owned capture, so it must not cross the synchronous close
    // boundary.
    state->lifecycle_callbacks_enabled.store (false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->state_handlers.clear ();
        state->error_handlers.clear ();
        state->disconnected_handlers.clear ();
    }
    std::shared_ptr<boost::asio::steady_timer> reconnect_timer;
    std::shared_ptr<detail::transport_connect_control_t> connect_control;
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->close_requested.store (true, std::memory_order_release);
        state->reconnect_scheduled = false;
        reconnect_timer = std::move (state->reconnect_timer);
        connect_control = state->connect_control;
    }
    if (connect_control)
        connect_control->cancel ();
    {
        std::unique_lock<std::mutex> lock (state->lifecycle_mutex);
        state->lifecycle_changed.wait (lock, [&] {
            return !state->connect_attempt_active;
        });
    }
    detail::cancel_timer (reconnect_timer);
    state->lifecycle_changed.notify_all ();
    detail::stop_heartbeat_monitor (state);
    state->state_changed.notify_all ();
    std::vector<std::function<void ()>> closed_write_callbacks;
    std::vector<std::function<void ()>> closed_send_callbacks;
    std::vector<std::function<void ()>> closed_request_callbacks;
    std::vector<std::function<void ()>> closed_wait_callbacks;
    std::shared_ptr<detail::stream_connection_t> connection;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        connection = state->connection;
    }
    if (connection) {
        // The public close contract is synchronous: wait until the transport
        // strand has completed the socket shutdown before returning. This is
        // outside transport_mutex so pending completion handlers can still
        // finish their state transition without a lock inversion.
        connection->shutdown_and_close ();
    }
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        detail::change_state (state, connection_state_t::closed);
        state->connection.reset ();
        while (!state->pending_sends.empty ()) {
            auto send = std::move (state->pending_sends.front ());
            state->pending_sends.pop_front ();
            if (send.callback) {
                closed_send_callbacks.push_back (
                  [callback = std::move (send.callback)] () mutable {
                      callback (result_t<void>::failure (error_code_t::closed,
                                                         "stream connector is closed"));
                  });
            }
        }
        while (!state->pending_writes.empty ()) {
            auto write = std::move (state->pending_writes.front ());
            state->pending_writes.pop_front ();
            if (write.callback) {
                closed_write_callbacks.push_back (
                  [callback = std::move (write.callback)] () mutable {
                      callback (result_t<void>::failure (error_code_t::closed,
                                                         "stream connector is closed"));
                  });
            }
        }
        for (auto &[_, request] : state->pending_requests) {
            detail::cancel_timer (request.timeout_timer);
            if (request.callback) {
                closed_request_callbacks.push_back (
                  [callback = std::move (request.callback)] () mutable {
                      callback (result_t<detail::request_reply_t>::failure (
                        error_code_t::closed, "stream connector is closed"));
                  });
            }
        }
        for (auto &[_, wait] : state->pending_waits) {
            detail::cancel_timer (wait.timeout_timer);
            if (wait.callback) {
                closed_wait_callbacks.push_back (
                  [callback = std::move (wait.callback)] () mutable {
                      callback (result_t<packet_t>::failure (error_code_t::closed,
                                                             "stream connector is closed"));
                  });
            }
        }
        state->pending_waits.clear ();
        state->pending_requests.clear ();
        state->pending_sends.clear ();
        state->pending_writes.clear ();
        for (auto &observer : state->inbound_observers) {
            if (observer) {
                observer->active.store (false);
            }
        }
        state->inbound_observers.clear ();
        state->read_in_progress = false;
        state->inbound_buffer.clear ();
        state->dispatch_queue.clear ();
        state->delivery_queue.clear ();
        state->state_changed.notify_all ();
    }
    for (auto &delivery : closed_write_callbacks) {
        delivery ();
    }
    for (auto &delivery : closed_send_callbacks) {
        detail::schedule_delivery (state, std::move (delivery));
    }
    for (auto &delivery : closed_request_callbacks) {
        detail::schedule_delivery (state, std::move (delivery));
    }
    for (auto &delivery : closed_wait_callbacks) {
        detail::schedule_delivery (state, std::move (delivery));
    }
    return result_t<void>::success ();
}

} // namespace

result_t<void> connector_t::close ()
{
    return close_state (detail::state_from (_state));
}

void connector_t::close (std::function<void (result_t<void>)> callback)
{
    auto state = detail::state_from (_state);
    detail::post_runtime_operation ([state, callback = std::move (callback)] () mutable {
        auto result = close_state (state);
        detail::schedule_delivery (
          state, [callback = std::move (callback), result = std::move (result)] () mutable {
              if (callback) {
                  callback (std::move (result));
              }
          });
    });
}

result_t<void> connector_t::dispatch ()
{
    return detail::dispatch_pending (detail::state_from (_state));
}

result_t<packet_t> connector_t::wait_for (std::string packet_name,
                                          std::chrono::milliseconds timeout)
{
    return detail::wait_for_packet (
      detail::state_from (_state), std::move (packet_name), nullptr, timeout);
}

connector_t &connector_t::on_connection_state_changed (
  std::function<void (const connection_state_changed_t &)> handler)
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    state->state_handlers.push_back (std::move (handler));
    return *this;
}

connector_t &connector_t::on_error (std::function<void (const error_t &)> handler)
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    state->error_handlers.push_back (std::move (handler));
    return *this;
}

inbound_observer_registration_t
connector_t::observe_inbound (std::function<void (const inbound_observation_t &)> observer)
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    if (state->connect_started) {
        return {};
    }
    auto entry = std::make_shared<detail::inbound_observer_entry_t> ();
    entry->id = state->next_inbound_observer_id++;
    entry->callback = std::move (observer);
    state->inbound_observers.push_back (entry);
    return inbound_observer_registration_t (_state, entry->id);
}

connector_t &connector_t::on_disconnected (std::function<void ()> handler)
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    state->disconnected_handlers.push_back (std::move (handler));
    return *this;
}

packet_t connector_t::make_packet (std::type_index type, std::string packet_name) const
{
    packet_t packet;
    packet.name = detail::packet_name_resolver_t{}.resolve (type, std::move (packet_name));
    const auto state = detail::state_from (_state);
    packet.codec = state->default_codec;
    packet.payload = zlink::message_t::from (std::string ("{}"));
    return packet;
}

connector_t &connector_t::on_packet_erased (std::string packet_name,
                                            std::function<void (const packet_t &)> handler)
{
    detail::state_from (_state)->packet_handlers[std::move (packet_name)].push_back (
      std::move (handler));
    return *this;
}

connector_t connector_factory_t::create (connector_options_t options)
{
    return connector_t (std::move (options));
}

} // namespace zlink::stream_connector
