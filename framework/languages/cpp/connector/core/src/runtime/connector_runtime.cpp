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
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
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
    }
    catch (const boost::system::system_error &) {
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

class shared_operation_runner_t : public std::enable_shared_from_this<shared_operation_runner_t>
{
  public:
    explicit shared_operation_runner_t (
      std::shared_ptr<shared_operation_runner_t> context_dependency = {}) :
        _context_dependency (std::move (context_dependency))
    {
    }

    void start (std::size_t worker_count)
    {
        auto self = shared_from_this ();
        for (auto index = 0u; index < worker_count; ++index) {
            _workers.emplace_back ([self] {
                auto *runner = self.get ();
                runner->_io_context.run ();
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
                OPENSSL_thread_stop ();
#endif
            });
        }
    }

    ~shared_operation_runner_t ()
    {
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                if (worker.get_id () == std::this_thread::get_id ()) {
                    worker.detach ();
                } else {
                    worker.join ();
                }
            }
        }
    }

    void stop ()
    {
        _work.reset ();
        _io_context.stop ();
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

    std::shared_ptr<boost::asio::steady_timer> post_after (std::chrono::milliseconds delay,
                                                           std::function<void ()> operation)
    {
        auto timer = std::make_shared<boost::asio::steady_timer> (_io_context);
        timer->expires_after (delay);
        timer->async_wait ([timer, operation = std::move (operation)] (
                             const boost::system::error_code &error) mutable {
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

    boost::asio::io_context &io_context () noexcept { return _io_context; }

  private:
    std::shared_ptr<shared_operation_runner_t> _context_dependency;
    boost::asio::io_context _io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work{
      boost::asio::make_work_guard (_io_context)};
    std::vector<std::thread> _workers;
};

std::weak_ptr<shared_runtime_t> &shared_runtime_weak ()
{
    static std::weak_ptr<shared_runtime_t> runtime;
    return runtime;
}

} // namespace

class shared_runtime_t
{
  public:
    explicit shared_runtime_t (std::size_t worker_count)
    {
        callback = std::make_shared<shared_operation_runner_t> ();
        operation = std::make_shared<shared_operation_runner_t> (callback);
        connect = std::make_shared<shared_operation_runner_t> ();
        callback->start (4);
        operation->start (worker_count);
        connect->start (4);
    }

    ~shared_runtime_t ()
    {
        operation->stop ();
        connect->stop ();
        callback->stop ();
    }

    std::shared_ptr<shared_operation_runner_t> callback;
    std::shared_ptr<shared_operation_runner_t> operation;
    std::shared_ptr<shared_operation_runner_t> connect;
};

std::shared_ptr<shared_runtime_t> acquire_shared_runtime ()
{
    std::lock_guard<std::mutex> lock (shared_runtime_config_mutex ());
    auto runtime = shared_runtime_weak ().lock ();
    if (!runtime) {
        runtime = std::make_shared<shared_runtime_t> (shared_runtime_worker_count ());
        shared_runtime_weak () = runtime;
        shared_runtime_started () = true;
    }
    return runtime;
}

connector_state_t::connector_state_t (connector_options_t options) :
    connector_id (next_connector_id.fetch_add (1, std::memory_order_relaxed)),
    options (std::move (options)),
    runtime (acquire_shared_runtime ()),
    io_context (runtime->operation->io_context ()),
    write_strand (boost::asio::make_strand (io_context)),
    delivery_strand (boost::asio::make_strand (runtime->callback->io_context ()))
{
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

std::optional<transport_t> transport_from_scheme (const std::string &endpoint)
{
    const auto separator = endpoint.find ("://");
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    const auto scheme = endpoint.substr (0, separator);
    if (scheme == "tcp") {
        return transport_t::tcp;
    }
    if (scheme == "tls") {
        return transport_t::tls;
    }
    if (scheme == "ws") {
        return transport_t::websocket;
    }
    if (scheme == "wss") {
        return transport_t::websocket_secure;
    }
    return std::nullopt;
}

result_t<transport_t> resolve_transport (const connector_options_t &options)
{
    const auto scheme_transport = transport_from_scheme (options.endpoint);
    if (!scheme_transport) {
        return result_t<transport_t>::failure (
          error_code_t::configuration_error,
          "stream connector endpoint must use one of tcp://, tls://, ws:// or "
          "wss://");
    }
    /* §3.1: with no transport given the scheme decides; a given transport that
   * disagrees with the scheme leaves no way to choose between the two. */
    if (options.transport && *options.transport != *scheme_transport) {
        return result_t<transport_t>::failure (
          error_code_t::configuration_error,
          "stream connector transport does not match the endpoint scheme");
    }
    return result_t<transport_t>::success (*scheme_transport);
}

std::chrono::milliseconds first_reconnect_delay (const connector_options_t &options)
{
    return std::min (options.reconnect.initial_delay, options.reconnect.max_delay);
}

/* stream-connector §6.3: every option item is checked before a connection is
 * made. A value outside its range is validation_failed; items that disagree
 * with one another are configuration_error. */
result_t<transport_t> validate_options (const connector_options_t &options)
{
    if (options.endpoint.empty ()) {
        return result_t<transport_t>::failure (error_code_t::validation_failed,
                                               "stream connector endpoint is required");
    }
    auto transport = resolve_transport (options);
    if (!transport) {
        return transport;
    }
    if (!stream_transport_factory_t::is_supported (transport.value ())) {
        return result_t<transport_t>::failure (
          error_code_t::configuration_error,
          "stream connector does not support the configured transport in this "
          "build");
    }
    if (options.connect_timeout <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector connect_timeout must be greater than zero");
    }
    if (options.request_timeout <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector request_timeout must be greater than zero");
    }
    if (options.wait_timeout <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector wait_timeout must be greater than zero");
    }
    if (options.heartbeat.interval <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector heartbeat interval must be greater than zero");
    }
    if (options.heartbeat.timeout <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector heartbeat timeout must be greater than zero");
    }
    if (options.reconnect.initial_delay <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector reconnect initial_delay must be greater than zero");
    }
    if (options.reconnect.max_delay <= std::chrono::milliseconds::zero ()) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector reconnect max_delay must be greater than zero");
    }
    if (!std::isfinite (options.reconnect.backoff_factor)
        || options.reconnect.backoff_factor <= 0.0) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector reconnect backoff_factor must be finite and positive");
    }
    if (options.reconnect.max_attempts && *options.reconnect.max_attempts <= 0) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector reconnect max_attempts must be positive or empty "
          "for no limit");
    }
    if (options.max_send_payload_size == 0) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector max_send_payload_size must be greater than zero");
    }
    if (options.max_receive_payload_size == 0) {
        return result_t<transport_t>::failure (
          error_code_t::validation_failed,
          "stream connector max_receive_payload_size must be greater than zero");
    }
    switch (options.compression) {
        case compression_t::none:
            if (options.compression_codec) {
                return result_t<transport_t>::failure (
                  error_code_t::configuration_error,
                  "stream connector compression is off but a compression codec is "
                  "configured");
            }
            break;
        case compression_t::lz4:
            break;
        default:
            return result_t<transport_t>::failure (error_code_t::validation_failed,
                                                   "stream connector compression is out of range");
    }
    switch (options.dispatch_mode) {
        case dispatch_mode_t::manual:
        case dispatch_mode_t::immediate:
            break;
        default:
            return result_t<transport_t>::failure (
              error_code_t::validation_failed, "stream connector dispatch_mode is out of range");
    }
    if (options.typed_codec) {
        switch (options.typed_codec->codec_id ()) {
            case codec_t::raw:
            case codec_t::json:
            case codec_t::message_pack:
            case codec_t::protobuf:
                break;
            default:
                return result_t<transport_t>::failure (
                  error_code_t::configuration_error,
                  "stream connector typed codec reports an unknown wire codec number");
        }
    }
    return transport;
}

connector_runtime_t::connector_runtime_t (std::shared_ptr<connector_state_t> state) :
    _state (std::move (state))
{
}

connector_runtime_t connector_runtime_t::from (const connector_t &connector)
{
    return connector_runtime_t (state_from (connector_internal_handle (connector)));
}

void enqueue_received_message (connector_state_t &state, dispatch_envelope_t envelope)
{
    /* Precondition: the caller holds transport_mutex. dispatch_queue is a
   * std::deque shared with the read pump and with every synchronous
   * receive/wait caller, so an unguarded push_back is a data race. A packet
   * that was not decoded from a frame takes its arrival here. */
    if (envelope.arrival == 0) {
        envelope.arrival = state.next_arrival.fetch_add (1);
    }
    state.dispatch_queue.push_back (std::move (envelope));
    state.state_changed.notify_all ();
}

namespace
{
thread_local const connector_state_t *running_callbacks_of = nullptr;
}

callback_scope_t::callback_scope_t (const connector_state_t &state) noexcept :
    _previous (running_callbacks_of)
{
    running_callbacks_of = &state;
}

callback_scope_t::~callback_scope_t ()
{
    running_callbacks_of = _previous;
}

bool callback_scope_t::running_callback_of (const connector_state_t &state) noexcept
{
    return running_callbacks_of == &state;
}

void run_on_delivery_strand (std::shared_ptr<connector_state_t> state,
                             std::function<void ()> callback)
{
    // The callback runs on the shared callback runner, never on the calling
    // thread. Deliveries are frequently started from the connector's own read
    // pump; running user code there lets a slow or blocking callback starve
    // the pump (and deadlock when the callback waits on a later inbound frame).
    boost::asio::post (state->delivery_strand, [state, callback = std::move (callback)] () mutable {
        callback_scope_t scope (*state);
        invoke_user_callback (*state, "connector callback failed", callback);
    });
}

void schedule_delivery (std::shared_ptr<connector_state_t> state,
                        std::function<void ()> callback,
                        std::function<std::size_t ()> callbacks)
{
    if (!callback) {
        return;
    }
    if (state->options.dispatch_mode == dispatch_mode_t::immediate) {
        run_on_delivery_strand (std::move (state), std::move (callback));
        return;
    }
    if (!callbacks) {
        callbacks = [] { return std::size_t{1}; };
    }
    std::lock_guard<std::mutex> lock (state->delivery_mutex);
    /* Numbered under the queue lock, so the queue stays in arrival order. */
    state->delivery_queue.push_back (
      delivery_t{state->next_arrival.fetch_add (1), std::move (callback), std::move (callbacks)});
    state->state_changed.notify_all ();
}

void publish_error (connector_state_t &state, error_t error) noexcept
{
    std::vector<std::uint64_t> handler_ids;
    {
        std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
        handler_ids = registered_handler_ids_locked (state.error_handlers);
    }
    if (handler_ids.empty ()) {
        return;
    }
    auto shared = state.shared_from_this ();
    auto callbacks =
      registered_handler_count (shared, &connector_state_t::error_handlers, handler_ids);
    schedule_delivery (
      shared,
      [state = shared, handler_ids = std::move (handler_ids), error = std::move (error)] {
          for (const auto &entry :
               registered_handlers (*state, &connector_state_t::error_handlers, handler_ids)) {
              /* An error handler's own failure is not reported to the error
           * handlers again. */
              try {
                  entry.handler (error);
              }
              catch (...) {
              }
          }
      },
      std::move (callbacks));
}

void close_bound_actors (const std::shared_ptr<connector_state_t> &state)
{
    std::vector<std::shared_ptr<actor_t>> actors;
    std::vector<std::uint64_t> handler_ids;
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        actors.reserve (state->actors_by_slot.size ());
        for (const auto &[_, actor] : state->actors_by_slot) {
            actor_access_t::close (actor);
            actors.push_back (actor);
        }
        state->actors_by_slot.clear ();
        state->actors_by_id.clear ();
        handler_ids = registered_handler_ids_locked (state->actor_unbound_handlers);
    }
    for (const auto &actor : actors) {
        schedule_actor_delivery (state, &connector_state_t::actor_unbound_handlers, handler_ids,
                                 actor);
    }
}

void schedule_actor_delivery (const std::shared_ptr<connector_state_t> &state,
                              actor_handlers_t connector_state_t::*registry,
                              std::vector<std::uint64_t> handler_ids,
                              std::shared_ptr<actor_t> actor)
{
    auto callbacks = registered_handler_count (state, registry, handler_ids);
    schedule_delivery (
      state,
      [state, registry, handler_ids = std::move (handler_ids), actor = std::move (actor)] {
          for (const auto &entry : registered_handlers (*state, registry, handler_ids)) {
              invoke_user_callback (*state, "Actor lifecycle callback failed",
                                    [&] { entry.handler (actor); });
          }
      },
      std::move (callbacks));
}

std::vector<std::uint8_t> encode_typed_payload (const std::shared_ptr<void> &state_handle,
                                                const std::vector<std::uint8_t> &payload)
{
    if (!state_handle) {
        return payload;
    }
    const auto state = state_from (state_handle);
    if (!state->options.typed_codec) {
        return payload;
    }
    return state->options.typed_codec->encode (payload);
}

std::vector<std::uint8_t> decode_typed_payload (const std::shared_ptr<void> &state_handle,
                                                const packet_t &packet)
{
    if (!state_handle) {
        return packet.payload;
    }
    const auto state = state_from (state_handle);
    if (!state->options.typed_codec) {
        return packet.payload;
    }
    return state->options.typed_codec->decode (packet.payload);
}

std::vector<std::uint8_t> decode_typed_reply (const std::shared_ptr<void> &state_handle,
                                              codec_t,
                                              const std::vector<std::uint8_t> &payload)
{
    if (!state_handle) {
        return payload;
    }
    const auto state = state_from (state_handle);
    if (!state->options.typed_codec) {
        return payload;
    }
    return state->options.typed_codec->decode (payload);
}

void remove_subscription (const std::shared_ptr<void> &state_handle, std::uint64_t subscription_id)
{
    if (!state_handle || subscription_id == 0) {
        return;
    }
    auto state = state_from (state_handle);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    for (auto entry = state->packet_handlers.begin (); entry != state->packet_handlers.end ();) {
        auto &handlers = entry->second;
        handlers.erase (std::remove_if (handlers.begin (), handlers.end (),
                                        [subscription_id] (const packet_handler_entry_t &handler) {
                                            return handler.id == subscription_id;
                                        }),
                        handlers.end ());
        entry = handlers.empty () ? state->packet_handlers.erase (entry) : std::next (entry);
    }
    const auto drop = [subscription_id] (auto &handlers) {
        handlers.erase (std::remove_if (handlers.begin (), handlers.end (),
                                        [subscription_id] (const auto &handler) {
                                            return handler.id == subscription_id;
                                        }),
                        handlers.end ());
    };
    drop (state->state_handlers);
    drop (state->error_handlers);
    drop (state->request_sending_handlers);
    drop (state->reply_received_handlers);
    drop (state->disconnected_handlers);
    drop (state->actor_bound_handlers);
    drop (state->actor_unbound_handlers);
}

/* stream-connector §6: the wait between attempts is a value picked between 50%
 * and 100% of the base delay, so clients that dropped together do not come back
 * at the same instant. */
std::chrono::milliseconds jittered_delay (std::chrono::milliseconds base)
{
    if (base <= std::chrono::milliseconds::zero ()) {
        return base;
    }
    static thread_local std::mt19937_64 engine (
      std::random_device{}()
      ^ static_cast<std::uint64_t> (
        std::chrono::steady_clock::now ().time_since_epoch ().count ()));
    std::uniform_real_distribution<double> fraction (0.5, 1.0);
    const auto scaled =
      static_cast<std::int64_t> (static_cast<double> (base.count ()) * fraction (engine));
    return std::chrono::milliseconds (std::max<std::int64_t> (1, scaled));
}

std::size_t packet_handler_count (connector_state_t &state, const dispatch_envelope_t &envelope)
{
    std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
    const auto found = state.packet_handlers.find (envelope.packet.name);
    if (found == state.packet_handlers.end ()) {
        return 0;
    }
    return static_cast<std::size_t> (
      std::count_if (found->second.begin (), found->second.end (),
                     [&] (const packet_handler_entry_t &entry) { return entry.takes (envelope); }));
}

void count_received_locked (connector_state_t &state, const packet_t &packet)
{
    if (packet.name.empty () || packet.name.rfind ("$zlink.", 0) == 0) {
        return;
    }
    ++state.received_counts[packet.name];
}

void schedule_delivery (std::shared_ptr<void> state, std::function<void ()> callback)
{
    schedule_delivery (state_from (state), std::move (callback));
}

void run_request_sending (const std::shared_ptr<void> &state_handle,
                          request_sending_context_t &context)
{
    auto state = state_from (state_handle);
    std::vector<handler_entry_t<std::function<void (request_sending_context_t &)>>> handlers;
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        handlers = state->request_sending_handlers;
    }
    for (const auto &entry : handlers) {
        invoke_user_callback (*state, "request sending hook failed",
                              [&] { entry.handler (context); });
    }
}

std::vector<std::uint64_t> capture_reply_hook_ids (const std::shared_ptr<void> &state_handle)
{
    auto state = state_from (state_handle);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    return registered_handler_ids_locked (state->reply_received_handlers);
}

void run_reply_received (const std::shared_ptr<void> &state_handle,
                         const reply_received_context_t &context,
                         const std::vector<std::uint64_t> &handler_ids)
{
    auto state = state_from (state_handle);
    for (const auto &entry :
         registered_handlers (*state, &connector_state_t::reply_received_handlers, handler_ids)) {
        invoke_user_callback (*state, "reply received hook failed",
                              [&] { entry.handler (context); });
    }
}

void schedule_reply_received (const std::shared_ptr<void> &state_handle,
                              reply_received_context_t context)
{
    auto state = state_from (state_handle);
    auto handler_ids = capture_reply_hook_ids (state_handle);
    if (handler_ids.empty ()) {
        return;
    }
    auto callbacks =
      registered_handler_count (state, &connector_state_t::reply_received_handlers, handler_ids);
    schedule_delivery (
      state,
      [state_handle, handler_ids = std::move (handler_ids), context = std::move (context)] {
          run_reply_received (state_handle, context, handler_ids);
      },
      std::move (callbacks));
}

void post_runtime_operation (const std::shared_ptr<connector_state_t> &state,
                             std::function<void ()> operation)
{
    state->runtime->operation->post (std::move (operation));
}

void post_connect_operation (const std::shared_ptr<connector_state_t> &state,
                             std::function<void ()> operation)
{
    state->runtime->connect->post (std::move (operation));
}

std::shared_ptr<boost::asio::steady_timer>
post_runtime_operation_after (const std::shared_ptr<connector_state_t> &state,
                              std::chrono::milliseconds delay,
                              std::function<void ()> operation)
{
    return state->runtime->operation->post_after (delay, std::move (operation));
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

namespace
{

/* stream-connector §9: the one mapping from what ended a connection to its
 * close reason. A frame or header the connector cannot decode and a frame over
 * the receive limit are protocol violations; every other error ends the
 * connection as a transport error; no error means the client closed it. */
close_reason_t close_reason_for (const std::optional<error_t> &error) noexcept
{
    if (!error) {
        return close_reason_t::client_close;
    }
    switch (error->code) {
        case error_code_t::frame_decode_failed:
        case error_code_t::frame_too_large:
            return close_reason_t::protocol_error;
        default:
            return close_reason_t::transport_error;
    }
}

} // namespace

void change_state (std::shared_ptr<connector_state_t> state,
                   connection_state_t next,
                   std::optional<error_t> error)
{
    connection_state_t previous;
    std::vector<std::uint64_t> state_handler_ids;
    std::vector<std::uint64_t> disconnected_handler_ids;
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
            /* stream-connector §6.2: the reason of the last ending stays
       * readable, so it is replaced on every ending and never cleared.
       * A session-closing control announced before the socket went away
       * outranks the synthesized reason. */
            if (next == connection_state_t::closed && previous != connection_state_t::connected
                && state->last_close_reason) {
                /* Closing after an earlier ending keeps that ending's reason. */
            } else if (state->pending_close_reason) {
                state->last_close_reason = *state->pending_close_reason;
                state->pending_close_reason.reset ();
            } else {
                state->last_close_reason = close_reason_for (error);
            }
            close_reason = state->last_close_reason;
            disconnected_handler_ids = registered_handler_ids_locked (state->disconnected_handlers);
        } else if (next == connection_state_t::connected) {
            state->pending_close_reason.reset ();
        }
        state_handler_ids = registered_handler_ids_locked (state->state_handlers);
    }
    if (stream_trace_enabled ()) {
        std::cerr << "zlink-cpp-stream-trace side=client connector=" << state->connector_id
                  << " stage=state previous=" << connection_state_name (previous)
                  << " next=" << connection_state_name (next);
        if (error) {
            std::cerr << " error_code=" << static_cast<int> (error->code) << " error=\""
                      << error->message << "\"";
        }
        std::cerr << '\n';
    }
    connection_state_changed_t changed{previous, next, error, close_reason};
    if (!state_handler_ids.empty () || !disconnected_handler_ids.empty ()) {
        auto state_callbacks =
          registered_handler_count (state, &connector_state_t::state_handlers, state_handler_ids);
        auto disconnected_callbacks = registered_handler_count (
          state, &connector_state_t::disconnected_handlers, disconnected_handler_ids);
        schedule_delivery (
          state,
          [state, state_handler_ids = std::move (state_handler_ids),
           disconnected_handler_ids = std::move (disconnected_handler_ids),
           changed = std::move (changed)] {
              for (const auto &entry : registered_handlers (
                     *state, &connector_state_t::state_handlers, state_handler_ids)) {
                  invoke_user_callback (*state, "connection state handler failed",
                                        [&] { entry.handler (changed); });
              }
              for (const auto &entry : registered_handlers (
                     *state, &connector_state_t::disconnected_handlers, disconnected_handler_ids)) {
                  invoke_user_callback (*state, "disconnected handler failed",
                                        [&] { entry.handler (changed.close_reason); });
              }
          },
          [state_callbacks = std::move (state_callbacks),
           disconnected_callbacks = std::move (disconnected_callbacks)] {
              return state_callbacks () + disconnected_callbacks ();
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

namespace
{

result_t<void> close_state (std::shared_ptr<detail::connector_state_t> state);

/* Default typed codec (stream-connector §5.4): the payload types already
 * serialize themselves to JSON, so this codec names the wire codec and leaves
 * the bytes alone. */
class json_typed_codec_t final : public typed_codec_t
{
  public:
    codec_t codec_id () const noexcept override { return codec_t::json; }
    std::vector<std::uint8_t> encode (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }
    std::vector<std::uint8_t> decode (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }
};

} // namespace

std::shared_ptr<const typed_codec_t> json_typed_codec ()
{
    static const auto codec = std::make_shared<const json_typed_codec_t> ();
    return codec;
}

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
    detail::submit_send (detail::state_from (_state), std::move (_packet), _actor_binding);
}

connector_t::connector_t () : connector_t (connector_options_t{})
{
}

connector_t::connector_t (connector_options_t options) :
    _state (std::make_shared<detail::connector_state_t> (std::move (options))), _codecs (_state)
{
    auto state = detail::state_from (_state);
    _external_owner = std::shared_ptr<void> (state.get (), [state] (void *) {
        detail::post_connect_operation (state, [state] {
            (void) close_state (state);
            std::lock_guard<std::mutex> lock (state->delivery_mutex);
            state->delivery_queue.clear ();
        });
    });
    /* Only the built-in default is dropped here. A codec the caller installed
   * on a configuration whose compression is off stays as written, so
   * connect() rejects the pair instead of quietly dropping one of them
   * (stream-connector §6.3). */
    if (state->options.compression == compression_t::none
        && state->options.compression_codec == lz4_compression_codec ()) {
        state->options.compression_codec.reset ();
    } else if (state->options.compression == compression_t::lz4
               && !state->options.compression_codec) {
        state->options.compression_codec = lz4_compression_codec ();
    }
    state->compression_codec = state->options.compression_codec;
    if (state->options.typed_codec) {
        state->default_codec = state->options.typed_codec->codec_id ();
        state->enabled_codecs.insert (state->default_codec);
    }
#ifndef ZLINK_STREAM_CONNECTOR_WITH_LZ4
    state->lz4_enabled = false;
#else
    state->lz4_enabled = true;
#endif
}

connector_t::connector_t (std::shared_ptr<void> state) :
    _state (std::move (state)), _codecs (_state)
{
}

std::shared_ptr<actor_t> detail::actor_access_t::create (std::shared_ptr<void> connector_state,
                                                         std::string actor_id,
                                                         std::uint16_t actor_slot)
{
    return std::shared_ptr<actor_t> (new actor_t (connector_t (connector_state),
                                                  std::move (actor_id), actor_slot,
                                                  std::make_shared<std::atomic_bool> (true)));
}

void detail::actor_access_t::close (const std::shared_ptr<actor_t> &actor)
{
    if (actor)
        actor->_bound->store (false, std::memory_order_release);
}

std::uint16_t detail::actor_access_t::slot (const std::shared_ptr<actor_t> &actor)
{
    return actor ? actor->_actor_slot : 0;
}

connector_t::~connector_t () = default;
connector_t::connector_t (connector_t &&) noexcept = default;
connector_t &connector_t::operator= (connector_t &&) noexcept = default;

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

std::optional<close_reason_t> connector_t::close_reason () const
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    return state->last_close_reason;
}

std::size_t connector_t::received_count (std::string_view packet_name) const
{
    auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    const auto found = state->received_counts.find (packet_name);
    if (found == state->received_counts.end ()) {
        return 0;
    }
    return found->second;
}

connector_options_t connector_t::options () const
{
    auto state = detail::state_from (_state);
    return state->options;
}

std::size_t connector_t::pending_dispatch_count () const
{
    auto state = detail::state_from (_state);
    std::size_t packets;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        packets = 0;
        for (const auto &envelope : state->dispatch_queue) {
            if (detail::packet_handler_count (*state, envelope) > 0) {
                ++packets;
            }
        }
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
    /* stream-connector §6.3: every option item is checked here, before a
   * connection is made, and §3.1 resolves the transport from the endpoint
   * scheme when the option leaves it open. */
    auto validated = detail::validate_options (state->options);
    if (!validated) {
        return result_t<parsed_endpoint_options_t>::failure (
          validated.error_code ().value_or (error_code_t::configuration_error),
          validated.error ()->message);
    }
    const auto transport = validated.value ();
    state->effective_transport = transport;
    parsed_endpoint_options_t parsed;
    parsed.tcp = transport == transport_t::tcp
                   ? detail::parse_tcp_endpoint (state->options.endpoint)
                   : std::optional<detail::endpoint_parts_t>{};
    parsed.tls = transport == transport_t::tls
                   ? detail::parse_tls_endpoint (state->options.endpoint)
                   : std::optional<detail::endpoint_parts_t>{};
    parsed.websocket = transport == transport_t::websocket
                         ? detail::parse_websocket_endpoint (state->options.endpoint)
                         : std::optional<detail::websocket_endpoint_parts_t>{};
    parsed.websocket_secure = transport == transport_t::websocket_secure
                                ? detail::parse_websocket_secure_endpoint (state->options.endpoint)
                                : std::optional<detail::websocket_endpoint_parts_t>{};
    if (transport == transport_t::tcp && !parsed.tcp) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error, "stream connector endpoint must use tcp://host:port");
    }
    if (transport == transport_t::tls && !parsed.tls) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error, "stream connector endpoint must use tls://host:port");
    }
    if (transport == transport_t::websocket && !parsed.websocket) {
        return result_t<parsed_endpoint_options_t>::failure (
          error_code_t::configuration_error,
          "stream connector endpoint must use ws://host:port/path");
    }
    if (transport == transport_t::websocket_secure && !parsed.websocket_secure) {
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
        state->lifecycle_changed.wait (
          lock, [&] { return !state->connect_attempt_active && !state->reconnect_scheduled; });
        if (state->state == connection_state_t::connected) {
            return result_t<void>::success ();
        }
        if (state->state == connection_state_t::closed) {
            return result_t<void>::failure (error_code_t::disconnected,
                                            "stream connector is closed");
        }
        const auto error = state->last_disconnect_error;
        return result_t<void>::failure (error ? error->code : error_code_t::disconnected,
                                        error ? error->message
                                              : "stream connector is not connected");
    }
    if (state->state == connection_state_t::connected) {
        return result_t<void>::success ();
    }
    if (state->state == connection_state_t::closed || state->close_requested.load ()) {
        return result_t<void>::failure (error_code_t::disconnected, "stream connector is closed");
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
    auto timeout_timer =
      detail::post_runtime_operation_after (state, timeout, [operation, control] {
          control->cancel ();
          operation->complete (boost::asio::error::timed_out, nullptr);
      });
    auto completion =
      [operation] (boost::system::error_code error,
                   std::unique_ptr<detail::stream_connection_t> connection) mutable {
          operation->complete (error, std::move (connection));
      };

    if (state->effective_transport == transport_t::websocket) {
        detail::connect_websocket_async (state->io_context, *parsed.websocket, control,
                                         std::move (completion));
    } else if (state->effective_transport == transport_t::websocket_secure) {
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
        detail::connect_websocket_secure_async (state->io_context, *parsed.websocket_secure,
                                                state->options.skip_server_certificate_validation,
                                                control, std::move (completion));
#else
        operation->complete (boost::asio::error::operation_not_supported, nullptr);
#endif
    } else if (state->effective_transport == transport_t::tls) {
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
        detail::connect_tls_async (state->io_context, *parsed.tls,
                                   state->options.skip_server_certificate_validation, control,
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
                    completion (
                      {}, detail::make_tcp_connection (state->io_context, std::move (*socket)));
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
    return result_t<std::unique_ptr<detail::stream_connection_t>>::success (std::move (connection));
}

void schedule_start_read_loop (std::shared_ptr<detail::connector_state_t> state)
{
    detail::post_runtime_operation (state, [state] { detail::start_read_loop (state); });
}

result_t<void> connect_state (std::shared_ptr<detail::connector_state_t> state,
                              bool automatic_reconnect = false)
{
    if (auto existing = begin_connect_attempt (state, automatic_reconnect)) {
        return std::move (*existing);
    }
    connect_attempt_guard_t attempt (state);
    auto parsed = parse_connect_options (state);
    if (!parsed) {
        const auto message =
          parsed.error () ? parsed.error ()->message : "stream connector endpoint is invalid";
        /* stream-connector §9: configuration_error and validation_failed keep
     * the state the connector had before the attempt and leave no close
     * reason. Only the attempt fails. */
        return result_t<void>::failure (
          parsed.error_code ().value_or (error_code_t::configuration_error), message);
    }

    const auto max_attempts = state->options.reconnect.enabled
                                ? state->options.reconnect.max_attempts
                                : std::optional<int> (1);
    auto retry_delay = detail::first_reconnect_delay (state->options);
    std::string last_error;
    const auto deadline = std::chrono::steady_clock::now () + state->options.connect_timeout;

    for (int attempt_number = 1; !max_attempts || attempt_number <= std::max (1, *max_attempts);
         ++attempt_number) {
        if (state->close_requested.load (std::memory_order_acquire))
            return result_t<void>::failure (error_code_t::disconnected,
                                            "stream connector is closed");
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
                return result_t<void>::failure (error_code_t::disconnected,
                                                "stream connector is closed");
            }
            connected.value ()->set_read_message_limit (
              state->options.max_receive_payload_size
              + static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ()) + 6u);
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                state->connection = std::move (connected.value ());
                const auto connected_at = std::chrono::steady_clock::now ();
                state->last_heartbeat_sent = connected_at;
                state->last_inbound_received = connected_at;
                /* stream-connector §10: a connection that is established starts
         * the receive counts at zero and drops what the previous
         * connection left unconsumed. Resetting only the counts would
         * let the counts and the queue describe different connections,
         * and a wait would hand back a packet from before the drop. */
                state->dispatch_queue.clear ();
                ++state->dispatch_queue_generation;
                state->received_counts.clear ();
            }
            detail::change_state (state, connection_state_t::connected);
            schedule_start_read_loop (state);
            detail::start_heartbeat_monitor (state);
            return result_t<void>::success ();
        }
        last_error = connected.error () ? connected.error ()->message
                                        : "stream connector transport connect failed";
        if (state->close_requested.load (std::memory_order_acquire))
            return result_t<void>::failure (error_code_t::disconnected,
                                            "stream connector is closed");
        if (max_attempts && attempt_number >= std::max (1, *max_attempts)) {
            break;
        }
        const auto delay = std::min (detail::jittered_delay (retry_delay),
                                     std::chrono::duration_cast<std::chrono::milliseconds> (
                                       deadline - std::chrono::steady_clock::now ()));
        if (delay > std::chrono::milliseconds::zero ()) {
            std::mutex retry_mutex;
            std::condition_variable retry_ready;
            std::unique_lock<std::mutex> retry_lock (retry_mutex);
            retry_ready.wait_for (retry_lock, delay);
        }
        retry_delay =
          std::chrono::milliseconds (static_cast<std::chrono::milliseconds::rep> (std::max (
            1.0, std::min (static_cast<double> (state->options.reconnect.max_delay.count ()),
                           retry_delay.count () * state->options.reconnect.backoff_factor))));
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
    /* §6: the first wait after a drop is also randomized. */
    auto timer = detail::post_runtime_operation_after (
      state, detail::jittered_delay (detail::first_reconnect_delay (state->options)), [state] {
          detail::post_connect_operation (state, [state] {
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
    detail::post_connect_operation (
      state, [state, owner = _external_owner, callback = std::move (callback)] () mutable {
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

/* The close work (stream-connector §7). It closes the transport and fails
 * every operation that has not completed; a frame not yet written to the
 * transport is never written. The peer is not waited for: a write already in
 * progress is abandoned with the socket and its operation fails with the
 * others. The state and disconnect callbacks this close produces follow the
 * dispatch mode like every other callback: Immediate posts them to the
 * delivery strand, Manual leaves them to the next pump. The close work does
 * not wait for them, so a handler that never finishes does not hold it. */
void run_close_work (const std::shared_ptr<detail::connector_state_t> &state)
{
    std::shared_ptr<boost::asio::steady_timer> reconnect_timer;
    std::shared_ptr<detail::transport_connect_control_t> connect_control;
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->reconnect_scheduled = false;
        reconnect_timer = std::move (state->reconnect_timer);
        connect_control = state->connect_control;
    }
    if (connect_control)
        connect_control->cancel ();
    {
        std::unique_lock<std::mutex> lock (state->lifecycle_mutex);
        state->lifecycle_changed.wait (lock, [&] { return !state->connect_attempt_active; });
    }
    detail::cancel_timer (reconnect_timer);
    state->lifecycle_changed.notify_all ();
    detail::stop_heartbeat_monitor (state);
    detail::connection_operations_t operations;
    std::shared_ptr<detail::stream_connection_t> connection;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        operations = detail::take_connection_operations_locked (*state);
        connection = state->connection;
    }
    if (connection) {
        // Outside transport_mutex so pending completion handlers can still
        // finish their state transition without a lock inversion.
        connection->shutdown_and_close ();
    }
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        state->connection.reset ();
        state->read_in_progress = false;
        state->inbound_buffer.clear ();
    }
    detail::close_bound_actors (state);
    detail::change_state (state, connection_state_t::closed);
    state->state_changed.notify_all ();
    detail::fail_connection_operations (state, std::move (operations),
                                        "stream connector is closed");
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->close_completed = true;
    }
    state->lifecycle_changed.notify_all ();
}

/* One close work per connector. The close that sets close_requested starts
 * it; a close called inside a callback returns right after starting it, and a
 * close called outside waits for its result (stream-connector §7). */
result_t<void> close_state (std::shared_ptr<detail::connector_state_t> state)
{
    const bool starts = !state->close_requested.exchange (true, std::memory_order_acq_rel);
    if (detail::callback_scope_t::running_callback_of (*state)) {
        if (starts) {
            detail::post_connect_operation (state, [state] { run_close_work (state); });
        }
        return result_t<void>::success ();
    }
    if (starts) {
        run_close_work (state);
        return result_t<void>::success ();
    }
    std::unique_lock<std::mutex> lock (state->lifecycle_mutex);
    state->lifecycle_changed.wait (lock, [&] { return state->close_completed; });
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
    detail::post_connect_operation (state, [state, callback = std::move (callback)] () mutable {
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
    return detail::wait_for_packet (detail::state_from (_state), std::move (packet_name), nullptr,
                                    timeout);
}

subscription_t connector_t::on_connection_state_changed (
  std::function<void (const connection_state_changed_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->state_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

subscription_t connector_t::on_error (std::function<void (const error_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->error_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

subscription_t
connector_t::on_request_sending (std::function<void (request_sending_context_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->request_sending_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

subscription_t
connector_t::on_reply_received (std::function<void (const reply_received_context_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->reply_received_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

subscription_t
connector_t::on_disconnected (std::function<void (std::optional<close_reason_t>)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->disconnected_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

std::vector<std::shared_ptr<actor_t>> connector_t::actors () const
{
    const auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    std::vector<std::shared_ptr<actor_t>> actors;
    actors.reserve (state->actors_by_slot.size ());
    for (const auto &[_, actor] : state->actors_by_slot)
        actors.push_back (actor);
    return actors;
}

std::shared_ptr<actor_t> connector_t::actor (std::string_view actor_id) const
{
    const auto state = detail::state_from (_state);
    std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
    const auto found = state->actors_by_id.find (actor_id);
    return found == state->actors_by_id.end () ? nullptr : found->second;
}

subscription_t
connector_t::on_actor_bound (std::function<void (const std::shared_ptr<actor_t> &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->actor_bound_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

subscription_t
connector_t::on_actor_unbound (std::function<void (const std::shared_ptr<actor_t> &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->actor_unbound_handlers.push_back ({id, std::move (handler)});
    }
    return subscription_t (_state, id);
}

std::string connector_t::resolve_type_packet_name (std::string type_name) const
{
    const auto state = detail::state_from (_state);
    if (state->options.name_resolver) {
        auto resolved = state->options.name_resolver->resolve (type_name);
        if (!resolved.empty ()) {
            return resolved;
        }
    }
    return type_name;
}

packet_t connector_t::make_packet (std::type_index type, std::string packet_name) const
{
    packet_t packet;
    packet.name = detail::packet_name_resolver_t{}.resolve (type, std::move (packet_name));
    const auto state = detail::state_from (_state);
    packet.codec = state->default_codec;
    packet.payload = {'{', '}'};
    return packet;
}

subscription_t connector_t::on_packet_erased (std::string packet_name,
                                              std::function<void (const packet_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->packet_handlers[std::move (packet_name)].push_back (
          {id, [handler = std::move (handler)] (const detail::dispatch_envelope_t &envelope) {
               handler (envelope.packet);
           }});
    }
    detail::deliver_queued_to_handlers (state);
    return subscription_t (_state, id);
}

subscription_t connector_t::on_actor_packet_erased (std::string packet_name,
                                                    std::uint16_t actor_slot,
                                                    std::function<void (const packet_t &)> handler)
{
    auto state = detail::state_from (_state);
    const auto id = state->next_subscription_id.fetch_add (1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock (state->lifecycle_mutex);
        state->packet_handlers[std::move (packet_name)].push_back (
          {id,
           [handler = std::move (handler)] (const detail::dispatch_envelope_t &envelope) {
               handler (envelope.packet);
           },
           actor_slot});
    }
    detail::deliver_queued_to_handlers (state);
    return subscription_t (_state, id);
}

connector_t connector_factory_t::create (connector_options_t options)
{
    return connector_t (std::move (options));
}

} // namespace zlink::stream_connector
