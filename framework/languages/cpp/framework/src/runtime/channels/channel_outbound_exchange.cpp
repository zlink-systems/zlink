/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_outbound_exchange.hpp"

#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_socket_options.hpp"
#include "runtime/channels/socket_monitor_event.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/transport/listener_identity.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Messaging/subscription_event.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace zlink::framework::detail
{

namespace
{

constexpr auto default_send_wait_timeout = std::chrono::milliseconds (1000);
constexpr auto maximum_channel_ready_wait = std::chrono::seconds (5);

const channel_capability_snapshot_t *client_capability (const channel_runtime_state_t &state,
                                                        const std::string &channel_name)
{
    const auto found = state.channels.find (channel_name);
    if (found == state.channels.end ()) {
        return nullptr;
    }
    return &found->second.client;
}

const channel_capability_snapshot_t *publisher_capability (const channel_runtime_state_t &state,
                                                           const std::string &channel_name)
{
    const auto found = state.channels.find (channel_name);
    if (found == state.channels.end ()) {
        return nullptr;
    }
    return &found->second.publisher;
}

bool has_connection (const channel_capability_snapshot_t *capability)
{
    return capability != nullptr && capability->enabled
           && (capability->discovery || !capability->bind_endpoints.empty ()
               || !capability->connect_endpoints.empty ());
}

bool exceeds_configured_max_message_size (
  const runtime::messaging::message_parts_t &parts,
  const channel_capability_snapshot_t &capability) noexcept
{
    if (!capability.max_message_size) {
        return false;
    }
    const auto limit = capability.max_message_size->bytes ();
    if (limit <= 0) {
        return false;
    }
    const auto &items = parts.items ();
    return std::any_of (items.begin (), items.end (), [limit] (const zlink::message_t &part) {
        return static_cast<std::int64_t> (part.size ()) > limit;
    });
}

bool can_wait_for_client_endpoint (const std::shared_ptr<channel_runtime_state_t> &state,
                                   const channel_capability_snapshot_t *capability)
{
    if (capability == nullptr || !capability->enabled) {
        return false;
    }
    if (!capability->bind_endpoints.empty () || !capability->connect_endpoints.empty ()) {
        return true;
    }
    if (!capability->discovery) {
        return false;
    }
    std::lock_guard lock (state->mutex);
    return state->auto_connect_active;
}

std::optional<channel_runtime_state_t::client_server_send_t>
client_server_sender (const std::shared_ptr<channel_runtime_state_t> &state,
                      const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->client_server_senders.find (channel_name);
    return found == state->client_server_senders.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::client_server_send_t> (
                 found->second);
}

std::optional<channel_runtime_state_t::client_server_request_t>
client_server_requester (const std::shared_ptr<channel_runtime_state_t> &state,
                         const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->client_server_requesters.find (channel_name);
    return found == state->client_server_requesters.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::client_server_request_t> (
                 found->second);
}

std::optional<channel_runtime_state_t::fanout_publish_t>
fanout_publisher (const std::shared_ptr<channel_runtime_state_t> &state,
                  const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    const auto found =
      state->fanout_publishers.find (channel_name);
    return found == state->fanout_publishers.end ()
             ? std::nullopt
             : std::optional<
                 channel_runtime_state_t::fanout_publish_t> (
                 found->second);
}

bool channel_runtime_accepts_outbound_locked (const channel_runtime_state_t &state) noexcept
{
    return !state.shutdown && !state.closed;
}

detail::boundary_error_t channel_runtime_outbound_error_state_locked (
  const channel_runtime_state_t &state) noexcept
{
    return state.shutdown ? detail::boundary_error_t::shutdown : detail::boundary_error_t::closed;
}

const char *channel_runtime_outbound_error_message_locked (
  const channel_runtime_state_t &state) noexcept
{
    return state.shutdown ? "channel runtime is shutting down" : "channel runtime is closed";
}

runtime::messaging::message_parts_t
encode_channel_payload_parts (runtime::messaging::envelope_header_t header,
                              std::type_index payload_type,
                              const message_bus_t::payload_encoder_t &encode_payload,
                              serializer_registry_t &serializers)
{
    header.content_type = serializers.content_type (payload_type);
    runtime::messaging::envelope_codec_t envelope;
    return envelope.encode_raw_body_parts (
      header, detail::encoded_payload_to_raw (encode_payload (serializers)));
}

framework_exception_t map_native_request_exception (const std::exception &error)
{
    if (const auto *request_error = dynamic_cast<const zlink::request_error_t *> (&error);
        request_error != nullptr) {
        if (request_error->result () == zlink::request_result_t::timed_out) {
            return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                          "channel request timed out");
        }
        if (request_error->result () == zlink::request_result_t::not_connected) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel request target is not connected");
        }
        if (request_error->internal_errno () == ECONNREFUSED
            || request_error->internal_errno () == ENOTCONN
            || request_error->internal_errno () == EHOSTUNREACH
            || request_error->internal_errno () == ENETUNREACH) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel request target is not connected");
        }
        return runtime::messaging::map_request_result_exception (
          request_error->result (), request_error->what ());
    }
    if (const auto *recv_error = dynamic_cast<const zlink::recv_error_t *> (&error);
        recv_error != nullptr) {
        if (recv_error->result () == zlink::recv_result_t::no_data
            || recv_error->internal_errno () == EAGAIN) {
            return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                          "channel request timed out");
        }
        return framework_exception_t (framework_error_kind_t::internal_failure, recv_error->what ());
    }
    if (const auto *submit_error = dynamic_cast<const zlink::submit_error_t *> (&error);
        submit_error != nullptr) {
        if (submit_error->result () == zlink::submit_result_t::backpressured
            && (submit_error->internal_errno () == EAGAIN
                || submit_error->internal_errno () == ETIMEDOUT)) {
            return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                          "channel request timed out");
        }
        if (submit_error->result () == zlink::submit_result_t::not_connected) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel request target is not connected");
        }
        if (submit_error->internal_errno () == ECONNREFUSED
            || submit_error->internal_errno () == ENOTCONN
            || submit_error->internal_errno () == EHOSTUNREACH
            || submit_error->internal_errno () == ENETUNREACH) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel request target is not connected");
        }
        return runtime::messaging::map_submit_result_exception (
          submit_error->result (), submit_error->what ());
    }
    return framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
}

framework_exception_t map_native_send_exception (const std::exception &error)
{
    if (const auto *submit_error =
          dynamic_cast<const zlink::submit_error_t *> (&error);
        submit_error != nullptr) {
        if (submit_error->result () == zlink::submit_result_t::backpressured
            && (submit_error->internal_errno () == EAGAIN
                || submit_error->internal_errno () == ETIMEDOUT)) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::timed_out,
              "channel send timed out");
        }
        if (submit_error->result () == zlink::submit_result_t::not_connected
            || submit_error->internal_errno () == ECONNREFUSED
            || submit_error->internal_errno () == ENOTCONN
            || submit_error->internal_errno () == EHOSTUNREACH
            || submit_error->internal_errno () == ENETUNREACH) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel send target is not connected");
        }
        return runtime::messaging::map_submit_result_exception (
          submit_error->result (), submit_error->what ());
    }
    return framework_exception_t (
      framework_error_kind_t::internal_failure, error.what ());
}

void trace_channel_backpressure (const dispatch_options_t &dispatch,
                                 const std::string &channel_name,
                                 const std::string &packet_name,
                                 const std::optional<std::string> &correlation_id = std::nullopt)
{
    message_flow_tracer_t (dispatch).trace (
      message_flow_outcome_t::backpressured, [&] {
          return message_flow_event_t{
            .outcome = message_flow_outcome_t::backpressured,
            .surface = dispatch_error_surface_t::channel,
            .message_kind = dispatch_message_kind_t::send,
            .packet_name = packet_name,
            .channel_name = channel_name,
            .correlation_id = correlation_id,
            .channel_route_kind = std::string ("client_server"),
            .result = message_flow_result_t::backpressured,
            .reason = message_flow_reason_t::backpressure};
      });
}

struct channel_endpoint_snapshot_t
{
    std::vector<std::string> endpoints;
    std::uint64_t version = 0;
};

std::chrono::milliseconds
resolve_channel_wait_timeout (const std::shared_ptr<channel_runtime_state_t> &state,
                              const std::string &channel_name,
                              std::chrono::milliseconds timeout)
{
    if (timeout > std::chrono::milliseconds::zero ()) {
        return timeout;
    }
    std::lock_guard lock (state->mutex);
    const auto found = state->channels.find (channel_name);
    if (found != state->channels.end () && found->second.default_request_timeout) {
        return *found->second.default_request_timeout;
    }
    return state->default_request_timeout;
}

std::chrono::milliseconds resolve_send_wait_timeout (std::chrono::milliseconds timeout)
{
    return timeout > std::chrono::milliseconds::zero () ? timeout : default_send_wait_timeout;
}

std::function<channel_endpoint_snapshot_t ()>
make_client_endpoint_provider (std::shared_ptr<channel_runtime_state_t> state,
                               std::string channel_name)
{
    return [state = std::move (state), channel_name = std::move (channel_name)] {
        std::lock_guard lock (state->mutex);
        detail::channel_runtime_manager_t manager (state);
        auto &bundle = manager.get_or_create_client_bundle (channel_name);
        return channel_endpoint_snapshot_t{.endpoints = bundle.list_manual_connections (),
                                           .version = bundle.connection_version ()};
    };
}

} // namespace

class channel_native_client_t
{
  public:
    using endpoint_provider_t = std::function<channel_endpoint_snapshot_t ()>;

    channel_native_client_t (std::string channel_name,
                             const channel_capability_snapshot_t &client,
                             channel_runtime_t runtime,
                             std::shared_ptr<zlink::context_t> core_context) :
        _channel_name (std::move (channel_name)),
        _client (client),
        _runtime (std::move (runtime)),
        _core_context (std::move (core_context)),
        _readiness_executor (std::make_shared<runtime::offload_executor_t> (
          1, _runtime.pending_limit (), "zlink-channel-ready"))
    {
        initialize_transport ();
    }

    ~channel_native_client_t () { close (); }

    task_t<runtime::messaging::message_parts_t>
    request (const runtime::messaging::message_parts_t &parts,
             const endpoint_provider_t &endpoints,
             std::chrono::milliseconds timeout)
    {
        if (_closed.load (std::memory_order_acquire)) {
            co_return detail::boundary_failure<runtime::messaging::message_parts_t> (
              detail::boundary_error_t::shutdown,
              "channel native client is closed");
        }
        try {
            const auto operation_deadline = std::chrono::steady_clock::now () + timeout;
            const auto current = endpoints ();
            if (current.endpoints.empty ()) {
                co_return detail::boundary_failure<
                  runtime::messaging::message_parts_t> (
                  detail::boundary_error_t::disconnected,
                  "channel client has no connected server endpoint");
            }
            std::optional<
              zlink::async_result_t<std::vector<zlink::message_t>>>
              pending;
            std::shared_ptr<transport_t> transport;
            {
                std::lock_guard lock (_mutex);
                if (_closed.load (std::memory_order_acquire)) {
                    co_return detail::boundary_failure<
                      runtime::messaging::message_parts_t> (
                      detail::boundary_error_t::shutdown,
                      "channel native client is closed");
                }
                transport = sync_connections (current);
            }
            const auto ready_deadline = std::min (
              operation_deadline,
              std::chrono::steady_clock::now () + maximum_channel_ready_wait);
            if (!co_await wait_for_connection_ready (ready_deadline)) {
                co_return detail::boundary_failure<
                  runtime::messaging::message_parts_t> (
                  detail::boundary_error_t::timed_out,
                  "channel request target did not become ready before the deadline");
            }
            const auto now = std::chrono::steady_clock::now ();
            if (now >= operation_deadline) {
                co_return detail::boundary_failure<
                  runtime::messaging::message_parts_t> (
                  detail::boundary_error_t::timed_out,
                  "channel request timed out before native admission");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              operation_deadline - now);
            {
                zlink::message_t request_header = parts[0];
                zlink::message_t request_body = parts[1];
                std::lock_guard transport_lock (transport->mutex);
                pending.emplace (
                  transport->socket->request ()
                    .message (request_header)
                    .message (request_body)
                    .timeout (remaining > std::chrono::milliseconds::zero ()
                                ? remaining
                                : std::chrono::milliseconds (1))
                    .async ());
            }
            auto reply = co_await std::move (*pending);
            co_return result_t<runtime::messaging::message_parts_t>::success (
              runtime::messaging::message_parts_t (std::move (reply)));
        }
        catch (const std::exception &error) {
            const auto mapped = map_native_request_exception (error);
            co_return detail::result_access_t::failure<
              runtime::messaging::message_parts_t> (mapped);
        }
        catch (...) {
            co_return result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::internal_failure,
              "channel native request failed");
        }
    }

    task_t<void> send (const runtime::messaging::message_parts_t &parts,
                       const endpoint_provider_t &endpoints,
                       std::chrono::milliseconds timeout,
                       const std::string &packet_name,
                       std::optional<std::string> correlation_id)
    {
        if (_closed.load (std::memory_order_acquire)) {
            throw detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              "channel native client is closed");
        }
        const auto operation_deadline = std::chrono::steady_clock::now () + timeout;
        const auto current = endpoints ();
        if (current.endpoints.empty ()) {
            throw detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "channel client has no connected server endpoint");
        }
        /* Coroutine reference parameters must not cross a suspension point:
         * copy the frames (and the trace identifiers used by the catch
         * blocks) into the coroutine frame before the first co_await
         * (session-reconnect-and-coroutine-lifetime doc). */
        zlink::message_t send_header = parts[0];
        zlink::message_t send_body = parts[1];
        const std::string trace_packet_name = packet_name;
        try {
            std::shared_ptr<transport_t> transport;
            {
                std::lock_guard lock (_mutex);
                transport = sync_connections (current);
            }
            const auto ready_deadline = std::min (
              operation_deadline,
              std::chrono::steady_clock::now () + maximum_channel_ready_wait);
            if (!co_await wait_for_connection_ready (ready_deadline)) {
                throw detail::make_boundary_exception (
                  detail::boundary_error_t::timed_out,
                  "channel send target did not become ready before the deadline");
            }
            const auto now = std::chrono::steady_clock::now ();
            if (now >= operation_deadline) {
                throw detail::make_boundary_exception (
                  detail::boundary_error_t::timed_out,
                  "channel send timed out before native admission");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              operation_deadline - now);
            std::optional<zlink::async_result_t<void>> pending;
            {
                std::lock_guard transport_lock (transport->mutex);
                const auto configured_timeout =
                  transport->socket->options ().send_timeout ();
                transport->socket->options ().send_timeout (
                  remaining > std::chrono::milliseconds::zero ()
                    ? remaining
                    : std::chrono::milliseconds (1));
                try {
                    pending.emplace (
                      transport->socket->send ()
                        .message (send_header)
                        .message (send_body)
                        .async ());
                    transport->socket->options ().send_timeout (
                      configured_timeout);
                }
                catch (...) {
                    transport->socket->options ().send_timeout (
                      configured_timeout);
                    throw;
                }
            }
            co_await std::move (*pending);
            co_return;
        }
        catch (const framework_exception_t &) {
            throw;
        }
        catch (const zlink::submit_error_t &error) {
            if (error.result () == zlink::submit_result_t::backpressured) {
                trace_channel_backpressure (
                  _runtime.dispatch_options_ref (), _channel_name,
                  trace_packet_name, correlation_id);
            }
            const auto mapped = map_native_send_exception (error);
            throw mapped;
        }
        catch (const std::exception &error) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "channel native send failed");
        }
    }

    void close () noexcept
    {
        const std::lock_guard lock (_mutex);
        const bool was_closed = _closed.exchange (true, std::memory_order_acq_rel);
        if (was_closed) {
            return;
        }
        if (_transport) {
            _transport->close_noexcept ();
            _transport.reset ();
        }
        {
            std::lock_guard readiness_lock (_readiness->mutex);
            _readiness->closed = true;
        }
        _readiness->changed.notify_all ();
        _readiness_executor->request_stop ();
    }

  private:
    struct transport_t
    {
        transport_t (const channel_capability_snapshot_t &client,
                     std::shared_ptr<zlink::context_t> context) :
            context (std::move (context)),
            socket (std::make_unique<zlink::dealer_socket_t> (*this->context))
        {
            apply_weighted_channel_socket_options (*socket, client);
            if (client.routing_id) {
                socket->set_routing_id (*client.routing_id);
            }
            socket->options ().immediate (true);
            monitor = socket->monitor_open (zlink::monitor_event::connected
                                            | zlink::monitor_event::connection_ready
                                            | zlink::monitor_event::disconnected);
        }

        ~transport_t () { close_noexcept (); }

        void close_noexcept () noexcept
        {
            const std::lock_guard lock (mutex);
            if (socket) {
                try {
                    monitor.close ();
                }
                catch (...) {
                }
                try {
                    socket->close ();
                }
                catch (...) {
                }
                socket.reset ();
            }
        }

        std::shared_ptr<zlink::context_t> context;
        std::unique_ptr<zlink::dealer_socket_t> socket;
        zlink::socket_monitor_t monitor;
        std::set<std::string> connected;
        std::uint64_t connection_version = 0;
        std::mutex mutex;
    };

    struct readiness_state_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::set<std::pair<std::uint64_t, std::uint64_t>> ready_pairs;
        bool closed = false;
    };

    task_t<bool> wait_for_connection_ready (
      std::chrono::steady_clock::time_point deadline)
    {
        {
            std::lock_guard lock (_readiness->mutex);
            if (_readiness->closed) {
                return task_t<bool> (result_t<bool>::success (false));
            }
            if (!_readiness->ready_pairs.empty ()) {
                return task_t<bool> (result_t<bool>::success (true));
            }
        }
        auto completion =
          std::make_shared<detail::task_completion_source_t<bool>> ();
        auto output = completion->task ();
        const auto readiness = _readiness;
        const bool accepted = _readiness_executor->try_submit_cancellable (
          [readiness, deadline, completion] (std::stop_token stop) {
              std::stop_callback wake_on_stop (
                stop, [readiness] { readiness->changed.notify_all (); });
              std::unique_lock lock (readiness->mutex);
              readiness->changed.wait_until (lock, deadline, [&] {
                  return readiness->closed || stop.stop_requested ()
                         || !readiness->ready_pairs.empty ();
              });
              const bool ready = !readiness->closed
                                 && !stop.stop_requested ()
                                 && !readiness->ready_pairs.empty ();
              lock.unlock ();
              completion->complete (result_t<bool>::success (ready));
          });
        if (!accepted)
            completion->complete (result_t<bool>::success (false));
        return output;
    }

    std::shared_ptr<transport_t> sync_connections (const channel_endpoint_snapshot_t &snapshot)
    {
        std::set<std::string> desired;
        for (const auto &endpoint : snapshot.endpoints) {
            if (!endpoint.empty ()) {
                desired.insert (endpoint);
            }
        }
        if (!_transport) {
            _transport = make_transport ();
        }
        auto transport = _transport;
        for (auto it = transport->connected.begin (); it != transport->connected.end ();) {
            if (desired.find (*it) != desired.end ()) {
                ++it;
                continue;
            }
            try {
                std::lock_guard transport_lock (transport->mutex);
                transport->socket->disconnect (*it);
            }
            catch (...) {
            }
            it = transport->connected.erase (it);
        }
        for (const auto &endpoint : desired) {
            if (transport->connected.insert (endpoint).second) {
                std::lock_guard transport_lock (transport->mutex);
                transport->socket->connect (endpoint);
            }
        }
        transport->connection_version = snapshot.version;
        return transport;
    }

    void initialize_transport ()
    {
        _transport = make_transport ();
    }

    std::shared_ptr<transport_t> make_transport ()
    {
        auto transport = std::make_shared<transport_t> (
          _client, _core_context);
        const auto readiness = _readiness;
        const auto runtime = _runtime;
        const auto channel_name = _channel_name;
        transport->monitor.on_event (
          [readiness, runtime, channel_name] (const zlink::monitor_event_t &event) mutable {
              bool changed = false;
              const auto pair = std::make_pair (
                event.transport_pair_id, event.transport_pair_generation);
              {
                  std::lock_guard lock (readiness->mutex);
                  if (event.event == zlink::monitor_event::connection_ready) {
                      changed = readiness->ready_pairs.insert (pair).second;
                  } else if (event.event == zlink::monitor_event::disconnected) {
                      changed = readiness->ready_pairs.erase (pair) != 0;
                  }
              }
              if (changed)
                  readiness->changed.notify_all ();
              if (const auto kind = map_socket_monitor_event (event.event)) {
                  runtime.publish_socket_event (
                    channel_name, *kind, event.local_addr, event.remote_addr);
              }
          });
        return transport;
    }

    std::string _channel_name;
    channel_capability_snapshot_t _client;
    channel_runtime_t _runtime;
    std::shared_ptr<zlink::context_t> _core_context;
    std::shared_ptr<readiness_state_t> _readiness =
      std::make_shared<readiness_state_t> ();
    std::shared_ptr<runtime::offload_executor_t> _readiness_executor;
    std::shared_ptr<transport_t> _transport;
    std::mutex _mutex;
    std::atomic_bool _closed{false};
};

class channel_native_publisher_t
{
  public:
    channel_native_publisher_t (std::string channel_name,
                                const channel_capability_snapshot_t &publisher,
                                std::shared_ptr<zlink::context_t> context,
                                std::optional<std::string> advertise_host,
                                std::shared_ptr<runtime::listener_status_registry_t>
                                  listener_statuses) :
        _channel_name (std::move (channel_name)),
        _context (context ? std::move (context)
                          : std::make_shared<zlink::context_t> ()),
        _advertise_host (std::move (advertise_host)),
        _listener_statuses (std::move (listener_statuses)),
        _socket (*_context)
    {
        apply_common_channel_socket_options (_socket, publisher);
        std::string listener_endpoint;
        for (const auto &endpoint : publisher.bind_endpoints) {
            _socket.bind (endpoint);
            listener_endpoint = _socket.options ().last_endpoint ();
        }
        for (const auto &endpoint : publisher.connect_endpoints) {
            _socket.connect (endpoint);
        }
        _poller.add (_socket, zlink::poll_event_flag_t::pollin, 1);
        if (_listener_statuses && !listener_endpoint.empty ())
            _listener_statuses->update (
              listener_kind_t::fanout,
              _channel_name,
              runtime::transport::advertised_tcp_endpoint (
                std::move (listener_endpoint), _advertise_host, "Fanout"));
    }

    ~channel_native_publisher_t () { close (); }

    task_t<void> publish (const std::string &topic,
                          const runtime::messaging::message_parts_t &parts,
                          std::chrono::milliseconds timeout)
    {
        if (_closed.load (std::memory_order_acquire)) {
            throw detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              "channel native publisher is closed");
        }
        std::optional<zlink::async_result_t<void>> submitted;
        {
            std::lock_guard lock (_mutex);
            if (_closed.load (std::memory_order_acquire)) {
                throw detail::make_boundary_exception (
                  detail::boundary_error_t::shutdown,
                  "channel native publisher is closed");
            }
            drain_subscription_events ();
            zlink::message_t header = parts[0];
            zlink::message_t body = parts[1];
            auto operation = _socket.publish (topic)
                               .message (header)
                               .message (body);
            submitted.emplace (
              timeout > std::chrono::milliseconds::zero ()
                ? std::move (operation).timeout (timeout).async ()
                : std::move (operation).async ());
        }
        co_await std::move (*submitted);
        co_return;
    }

    void close () noexcept
    {
        const std::lock_guard lock (_mutex);
        if (_closed.exchange (true, std::memory_order_acq_rel))
            return;
        try {
            _poller.close ();
        }
        catch (...) {
        }
        try {
            _socket.close ();
        }
        catch (...) {
        }
        if (_listener_statuses)
            _listener_statuses->remove (
              listener_kind_t::fanout, _channel_name);
    }

  private:
    void drain_subscription_events ()
    {
        constexpr std::size_t max_events_per_publish = 32;
        for (std::size_t event_count = 0;
             event_count < max_events_per_publish;
             ++event_count) {
            zlink::poll_event_t readiness;
            try {
                if (_poller.wait (&readiness, 1, std::chrono::milliseconds (0)) != 1
                    || readiness.slot != 1
                    || (static_cast<short> (readiness.revents)
                        & static_cast<short> (zlink::poll_event_flag_t::pollin))
                         == 0) {
                    return;
                }
            }
            catch (...) {
                return;
            }
            zlink::subscription_event_t subscription;
            (void) _socket.receive_subscription_event (
              subscription, zlink::recv_flags_t::dontwait);
        }
    }

    std::string _channel_name;
    std::shared_ptr<zlink::context_t> _context;
    std::optional<std::string> _advertise_host;
    std::shared_ptr<runtime::listener_status_registry_t> _listener_statuses;
    zlink::xpub_socket_t _socket;
    zlink::poller_t _poller;
    std::mutex _mutex;
    std::atomic_bool _closed{false};
};

void initialize_manual_channel_publishers (
  const std::shared_ptr<channel_runtime_state_t> &state)
{
    if (!state)
        return;
    std::vector<std::pair<std::string, channel_capability_snapshot_t>>
      configurations;
    std::map<std::string, std::string> advertise_hosts;
    std::shared_ptr<runtime::listener_status_registry_t> listener_statuses;
    std::shared_ptr<zlink::context_t> core_context;
    {
        std::lock_guard lock (state->mutex);
        advertise_hosts = state->fanout_publisher_advertise_hosts;
        listener_statuses = state->listener_statuses;
        core_context = state->core_context;
        for (const auto &[channel_name, channel] : state->channels) {
            const auto &publisher = channel.publisher;
            if (!publisher.enabled || publisher.discovery
                || (publisher.bind_endpoints.empty ()
                    && publisher.connect_endpoints.empty ())
                || state->native_publishers.contains (channel_name)) {
                continue;
            }
            configurations.emplace_back (channel_name, publisher);
        }
    }
    for (auto &[channel_name, publisher] : configurations) {
        std::optional<std::string> advertise_host;
        if (const auto found = advertise_hosts.find (channel_name);
            found != advertise_hosts.end ()) {
            advertise_host = found->second;
        }
        auto native = std::make_shared<channel_native_publisher_t> (
          channel_name, publisher, core_context,
          std::move (advertise_host), listener_statuses);
        std::lock_guard lock (state->mutex);
        if (state->closed || state->shutdown) {
            native->close ();
            return;
        }
        state->native_publishers.emplace (channel_name, std::move (native));
    }
}

void close_manual_channel_publishers (
  const std::shared_ptr<channel_runtime_state_t> &state) noexcept
{
    if (!state)
        return;
    std::vector<std::shared_ptr<channel_native_publisher_t>> publishers;
    {
        std::lock_guard lock (state->mutex);
        for (auto &[_, publisher] : state->native_publishers) {
            if (publisher)
                publishers.push_back (std::move (publisher));
        }
        state->native_publishers.clear ();
    }
    for (auto &publisher : publishers)
        publisher->close ();
}

void close_native_channel_transports (
  const std::shared_ptr<channel_runtime_state_t> &state) noexcept
{
    std::vector<std::shared_ptr<channel_native_client_t>> clients;
    std::vector<std::shared_ptr<channel_native_publisher_t>> publishers;
    std::set<channel_native_client_t *> seen_clients;
    {
        std::lock_guard lock (state->mutex);
        for (auto &[_, client] : state->native_clients) {
            if (client && seen_clients.insert (client.get ()).second) {
                clients.push_back (client);
            }
        }
        for (auto &[_, publisher] : state->native_publishers) {
            if (publisher) {
                publishers.push_back (publisher);
            }
        }
        state->native_clients.clear ();
        state->native_publishers.clear ();
    }
    for (auto &client : clients) {
        client->close ();
    }
    for (auto &publisher : publishers) {
        publisher->close ();
    }
}

channel_outbound_exchange_t::channel_outbound_exchange_t (
  std::shared_ptr<channel_runtime_state_t> state) :
    _state (std::move (state))
{
}

namespace
{

/* channel.request.* catalog instruments (runtime-metrics §4.4). The guard is
 * armed only when a metric subscriber exists, so the disabled path skips the
 * clock read entirely (§7.2). */
struct channel_request_metrics_guard_t
{
    channel_request_metrics_guard_t (runtime::runtime_metrics_t metrics_surface,
                                     std::string channel_name) :
        metrics (std::move (metrics_surface)),
        armed (metrics.enabled ()),
        channel (std::move (channel_name))
    {
        if (armed) {
            started = std::chrono::steady_clock::now ();
            metrics.updown ("zlink.channel.request.inflight", "{request}", 1,
                            {{"channel", channel}});
        }
    }

    channel_request_metrics_guard_t (const channel_request_metrics_guard_t &) = delete;
    channel_request_metrics_guard_t &operator= (const channel_request_metrics_guard_t &) = delete;

    ~channel_request_metrics_guard_t ()
    {
        if (!armed) {
            return;
        }
        metrics.updown ("zlink.channel.request.inflight", "{request}", -1,
                        {{"channel", channel}});
        const auto elapsed =
          std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count ();
        metrics.histogram ("zlink.channel.request.duration", "s", elapsed,
                           {{"channel", channel}});
        if (timed_out) {
            metrics.counter ("zlink.channel.request.timeouts", "{request}", 1,
                             {{"channel", channel}});
        }
    }

    runtime::runtime_metrics_t metrics;
    bool armed = false;
    std::string channel;
    std::chrono::steady_clock::time_point started{};
    bool timed_out = false;
};

message_flow_result_t request_terminal_result (const framework_exception_t &error) noexcept
{
    if (error.kind () == framework_error_kind_t::shutting_down
        || detail::boundary_state (error) == detail::boundary_error_t::shutdown) {
        return message_flow_result_t::shutdown;
    }
    if (detail::boundary_state (error) == detail::boundary_error_t::cancelled) {
        return message_flow_result_t::cancelled;
    }
    if (error.kind () == framework_error_kind_t::capacity_exceeded) {
        return message_flow_result_t::backpressured;
    }
    return message_flow_result_t::failed;
}

struct channel_request_terminal_trace_t
{
    channel_request_terminal_trace_t (const dispatch_options_t &dispatch_options,
                                      const std::string &channel,
                                      const std::string &packet) :
        dispatch (&dispatch_options),
        channel_name (channel),
        packet_name (packet)
    {
        const auto mode = message_flow_tracer_t (*dispatch).mode ();
        if (mode == message_flow_log_mode_t::detailed) {
            duration_started = true;
            started = std::chrono::steady_clock::now ();
        }
    }

    channel_request_terminal_trace_t (const channel_request_terminal_trace_t &) = delete;
    channel_request_terminal_trace_t &operator= (const channel_request_terminal_trace_t &) = delete;

    ~channel_request_terminal_trace_t () noexcept
    {
        message_flow_tracer_t (*dispatch).trace (
          message_flow_outcome_t::reply_received, result, [&] {
              message_flow_event_t event{
                .outcome = message_flow_outcome_t::reply_received,
                .surface = dispatch_error_surface_t::channel,
                .message_kind = dispatch_message_kind_t::request,
                .packet_name = packet_name,
                .channel_name = channel_name,
                .correlation_id = correlation_id,
                .channel_route_kind = std::string ("client_server"),
                .result = result};
              if (duration_started) {
                  event.duration_seconds = std::chrono::duration<double> (
                    std::chrono::steady_clock::now () - started).count ();
              }
              return event;
          });
    }

    void succeeded () noexcept { result = message_flow_result_t::succeeded; }
    void backpressured () noexcept
    {
        result = message_flow_result_t::backpressured;
    }
    void failed_as (const framework_exception_t &error) noexcept
    {
        if (result == message_flow_result_t::backpressured) {
            return;
        }
        result = request_terminal_result (error);
    }

    const dispatch_options_t *dispatch;
    const std::string &channel_name;
    const std::string &packet_name;
    std::optional<std::string> correlation_id;
    message_flow_result_t result = message_flow_result_t::failed;
    bool duration_started = false;
    std::chrono::steady_clock::time_point started{};
};

} // namespace

task_t<zlink::message_t>
channel_outbound_exchange_t::submit_request (std::string channel_name,
                                             std::string packet_name,
                                             std::type_index request_type,
                                             message_bus_t::payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             const channel_request_call_t::metadata_map_t &metadata)
{
    /* Coroutine on a caller-owned object: copy the shared state into the
     * frame so resumes after the owner unwinds never touch `this`. */
    const auto state = _state;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (state->dispatch).mode ());
    channel_runtime_t runtime (state);
    const auto *client = client_capability (*state, channel_name);
    const auto call_packet_name = std::move (packet_name);
    channel_request_terminal_trace_t terminal_trace (
      state->dispatch, channel_name, call_packet_name);
    {
        std::lock_guard lock (state->mutex);
        state->outbound_calls.push_back (
          {"request", channel_name, "", call_packet_name, timeout, metadata});
    }
    auto reservation = runtime.reserve_outbound_request (channel_name);
    if (!reservation) {
        if (reservation.error () != nullptr) {
            terminal_trace.failed_as (*reservation.error ());
        } else {
            terminal_trace.failed_as (framework_exception_t (
              reservation.error_kind (), "channel request reservation failed"));
        }
        co_return reservation.error () != nullptr
                    ? detail::result_access_t::failure<zlink::message_t> (
                        *reservation.error ())
                    : result_t<zlink::message_t>::failure (
                        framework_error_kind_t::internal_failure,
                        "channel request failed");
    }
    channel_request_metrics_guard_t request_metrics (
      runtime::runtime_metrics_t (state->monitoring), channel_name);
    if (!can_wait_for_client_endpoint (state, client)) {
        (void) runtime.cancel_outbound_request (reservation.value ());
        const auto error = detail::make_boundary_exception (
          detail::boundary_error_t::disconnected,
          "channel client is not connected");
        terminal_trace.failed_as (error);
        co_return detail::result_access_t::failure<zlink::message_t> (
          error);
    }
    if (state->serializers != nullptr && client != nullptr) {
        if (const auto requester = client_server_requester (state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (encode_payload (*state->serializers));
                if (client->max_message_size
                    && client->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (payload.size ())
                         > client->max_message_size->bytes ()) {
                    (void) runtime.cancel_outbound_request (reservation.value ());
                    co_return result_t<zlink::message_t>::failure (
                        framework_error_kind_t::internal_failure,
                        "channel message exceeds configured max message size");
                }
                const auto effective_timeout =
                  resolve_channel_wait_timeout (state, channel_name, timeout);
                auto reply = co_await (*requester) (
                  call_packet_name,
                  state->serializers->content_type (request_type),
                  std::move (payload),
                  effective_timeout);
                auto completion =
                  runtime.complete_outbound_reply (reservation.value ());
                if (!completion) {
                    co_return completion.error () != nullptr
                                ? detail::result_access_t::failure<
                                    zlink::message_t> (*completion.error ())
                                : result_t<zlink::message_t>::failure (
                                    framework_error_kind_t::internal_failure,
                                    "channel request failed");
                }
                terminal_trace.succeeded ();
                co_return reply;
            }
            catch (const framework_exception_t &error) {
                terminal_trace.failed_as (error);
                (void) runtime.cancel_outbound_request (reservation.value ());
                request_metrics.timed_out =
                  detail::boundary_state (error)
                  == detail::boundary_error_t::timed_out;
                co_return detail::result_access_t::failure<zlink::message_t> (
                  error);
            }
            catch (const std::exception &error) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                if (const auto *submit_error = dynamic_cast<const zlink::submit_error_t *> (&error);
                    submit_error != nullptr
                    && submit_error->result () == zlink::submit_result_t::backpressured) {
                    terminal_trace.backpressured ();
                }
                const auto mapped = map_native_request_exception (error);
                terminal_trace.failed_as (mapped);
                request_metrics.timed_out =
                  detail::boundary_state (mapped)
                  == detail::boundary_error_t::timed_out;
                co_return detail::result_access_t::failure<zlink::message_t> (
                  mapped);
            }
        }
        try {
            runtime::messaging::client_call_codec_t codec;
            const auto effective_timeout =
              resolve_channel_wait_timeout (state, channel_name, timeout);
            auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                                 channel_name, call_packet_name,
                                                 effective_timeout);
            header.metadata = metadata;
            if (detail::message_flow_tracer_t (state->dispatch).capture_enabled ()) {
                terminal_trace.correlation_id = header.correlation_id;
            }
            detail::message_flow_tracer_t (state->dispatch)
              .trace (message_flow_outcome_t::sent, [&] {
                  return message_flow_event_t{message_flow_outcome_t::sent,
                                              dispatch_error_surface_t::channel,
                                              dispatch_message_kind_t::request,
                                              call_packet_name,
                                              channel_name,
                                              std::nullopt,
                                              header.correlation_id,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt};
              });
            runtime::messaging::envelope_codec_t envelope;
            auto parts = encode_channel_payload_parts (header, request_type, encode_payload,
                                                       *state->serializers);
            if (exceeds_configured_max_message_size (parts, *client)) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                co_return result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size");
            }
            std::shared_ptr<channel_native_client_t> native_client;
            {
                std::lock_guard lock (state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*state)) {
                    (void) runtime.cancel_outbound_request (reservation.value ());
                    co_return detail::result_access_t::failure<zlink::message_t> (
                      detail::make_boundary_exception (
                        channel_runtime_outbound_error_state_locked (*state),
                        channel_runtime_outbound_error_message_locked (*state)));
                }
                auto &slot = state->native_clients[channel_name];
                if (!slot) {
                    slot = std::make_shared<channel_native_client_t> (
                      channel_name, *client, runtime, state->core_context);
                }
                native_client = slot;
            }
            auto endpoints = make_client_endpoint_provider (state, channel_name);
            auto native_reply = co_await native_client->request (
              parts, endpoints, effective_timeout);
            auto validation = validate_channel_native_reply (native_reply);
            if (!validation) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                co_return validation.error () != nullptr
                            ? detail::result_access_t::failure<zlink::message_t> (
                                *validation.error ())
                            : result_t<zlink::message_t>::failure (
                                framework_error_kind_t::internal_failure,
                                "channel reply decode failed");
            }
            auto completion = runtime.complete_outbound_reply (reservation.value ());
            if (!completion) {
                co_return completion.error () != nullptr
                            ? detail::result_access_t::failure<zlink::message_t> (
                                *completion.error ())
                            : result_t<zlink::message_t>::failure (
                                framework_error_kind_t::internal_failure,
                                "channel request failed");
            }

            auto reply_header = envelope.decode_header (native_reply, false);
            if (!reply_header) {
                co_return result_t<zlink::message_t>::failure (
                  reply_header.error_kind (),
                  reply_header.error ()
                    ? reply_header.error ()->what ()
                    : "channel reply header decode failed");
            }
            if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                runtime::messaging::request_failure_mapper_t failure_mapper;
                co_return detail::result_access_t::failure<zlink::message_t> (
                  failure_mapper.error_header_exception (
                    reply_header.value ().error_code.value_or ("request_failed"),
                    reply_header.value ().error_message.value_or ("channel request failed"),
                    "channel request"));
            }
            auto body = envelope.decode_body (native_reply);
            if (!body) {
                co_return body.error () != nullptr
                            ? detail::result_access_t::failure<zlink::message_t> (
                                *body.error ())
                            : result_t<zlink::message_t>::failure (
                                framework_error_kind_t::internal_failure,
                                "channel reply body decode failed");
            }
            terminal_trace.succeeded ();
            co_return body.value ();
        }
        catch (const framework_exception_t &error) {
            terminal_trace.failed_as (error);
            (void) runtime.cancel_outbound_request (reservation.value ());
            request_metrics.timed_out =
              detail::boundary_state (error)
              == detail::boundary_error_t::timed_out;
            co_return detail::result_access_t::failure<zlink::message_t> (error);
        }
        catch (const std::exception &error) {
            (void) runtime.cancel_outbound_request (reservation.value ());
            if (const auto *submit_error = dynamic_cast<const zlink::submit_error_t *> (&error);
                submit_error != nullptr
                && submit_error->result () == zlink::submit_result_t::backpressured) {
                terminal_trace.backpressured ();
            }
            const auto mapped = map_native_request_exception (error);
            terminal_trace.failed_as (mapped);
            request_metrics.timed_out =
              detail::boundary_state (mapped)
              == detail::boundary_error_t::timed_out;
            co_return detail::result_access_t::failure<zlink::message_t> (
              mapped);
        }
        catch (...) {
            (void) runtime.cancel_outbound_request (reservation.value ());
            co_return result_t<zlink::message_t>::failure (
              framework_error_kind_t::internal_failure,
              "channel native request failed");
        }
    }
    (void) runtime.cancel_outbound_request (reservation.value ());
    request_metrics.timed_out = true;
    co_return detail::result_access_t::failure<zlink::message_t> (
      detail::make_boundary_exception (
        detail::boundary_error_t::timed_out,
        "channel request reply was not completed by a backend"));
}

task_t<void>
channel_outbound_exchange_t::submit_send (std::string channel_name,
                                          std::string packet_name,
                                          std::type_index message_type,
                                          message_bus_t::payload_encoder_t encode_payload,
                                          std::chrono::milliseconds timeout,
                                          const send_call_t::metadata_map_t &metadata)
{
    /* Coroutine on a caller-owned object: copy the shared state into the
     * frame so resumes after the owner unwinds never touch `this`. */
    const auto state = _state;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (state->dispatch).mode ());
    const auto call_packet_name = std::move (packet_name);
    {
        std::lock_guard lock (state->mutex);
        if (!channel_runtime_accepts_outbound_locked (*state)) {
            throw detail::make_boundary_exception (
              channel_runtime_outbound_error_state_locked (*state),
              channel_runtime_outbound_error_message_locked (*state));
        }
        state->outbound_calls.push_back (
          {"send", channel_name, "", call_packet_name, timeout, metadata});
    }
    const auto *client = client_capability (*state, channel_name);
    if (client == nullptr || !client->enabled) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "ClientServer Client role is not registered for this channel");
    }
    if (!can_wait_for_client_endpoint (state, client)) {
        throw detail::make_boundary_exception (
          detail::boundary_error_t::disconnected,
          "channel client is not connected");
    }
    if (state->serializers != nullptr && client != nullptr) {
        if (const auto sender = client_server_sender (state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (encode_payload (*state->serializers));
                if (client->max_message_size
                    && client->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (payload.size ())
                         > client->max_message_size->bytes ()) {
                    throw framework_exception_t (
                      framework_error_kind_t::internal_failure,
                      "channel message exceeds configured max message size");
                }
                co_await (*sender) (
                  call_packet_name,
                  state->serializers->content_type (message_type),
                  std::move (payload),
                  resolve_send_wait_timeout (timeout));
                detail::message_flow_tracer_t (state->dispatch)
                  .trace (message_flow_outcome_t::sent, [&] {
                      return message_flow_event_t{
                        .outcome = message_flow_outcome_t::sent,
                        .surface = dispatch_error_surface_t::channel,
                        .message_kind = dispatch_message_kind_t::send,
                        .packet_name = call_packet_name,
                        .channel_name = channel_name,
                        .channel_route_kind = std::string ("client_server")};
                  });
                co_return;
            }
            catch (const framework_exception_t &error) {
                if (error.kind () == framework_error_kind_t::capacity_exceeded) {
                    trace_channel_backpressure (
                      state->dispatch, channel_name, call_packet_name);
                }
                throw;
            }
            catch (const std::exception &error) {
                if (const auto *submit_error = dynamic_cast<const zlink::submit_error_t *> (&error);
                    submit_error != nullptr
                    && submit_error->result () == zlink::submit_result_t::backpressured) {
                    trace_channel_backpressure (
                      state->dispatch, channel_name, call_packet_name);
                }
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure, error.what ());
            }
        }
        try {
            runtime::messaging::client_call_codec_t codec;
            auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                                 channel_name, call_packet_name, timeout);
            header.metadata = metadata;
            auto parts = encode_channel_payload_parts (header, message_type, encode_payload,
                                                       *state->serializers);
            if (exceeds_configured_max_message_size (parts, *client)) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size");
            }
            std::shared_ptr<channel_native_client_t> native_client;
            {
                std::lock_guard lock (state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*state)) {
                    throw detail::make_boundary_exception (
              channel_runtime_outbound_error_state_locked (*state),
              channel_runtime_outbound_error_message_locked (*state));
                }
                auto &slot = state->native_clients[channel_name];
                if (!slot) {
                    slot = std::make_shared<channel_native_client_t> (
                      channel_name, *client, channel_runtime_t (state),
                      state->core_context);
                }
                native_client = slot;
            }
            auto endpoints = make_client_endpoint_provider (state, channel_name);
            const auto effective_timeout = resolve_send_wait_timeout (timeout);
            co_await native_client->send (
              parts, endpoints, effective_timeout, call_packet_name,
              header.correlation_id.empty ()
                ? std::nullopt
                : std::make_optional (header.correlation_id));
            detail::message_flow_tracer_t (state->dispatch)
              .trace (message_flow_outcome_t::sent, [&] {
                  return message_flow_event_t{
                    .outcome = message_flow_outcome_t::sent,
                    .surface = dispatch_error_surface_t::channel,
                    .message_kind = dispatch_message_kind_t::send,
                    .packet_name = call_packet_name,
                    .channel_name = channel_name,
                    .correlation_id = header.correlation_id,
                    .channel_route_kind = std::string ("client_server")};
              });
            co_return;
        }
        catch (const framework_exception_t &) {
            throw;
        }
        catch (const std::exception &error) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "channel native send failed");
        }
    }
    co_return;
}

task_t<void>
channel_outbound_exchange_t::submit_publish (std::string channel_name,
                                             std::string topic,
                                             std::string packet_name,
                                             std::type_index event_type,
                                             message_bus_t::payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             const send_call_t::metadata_map_t &metadata)
{
    /* Coroutine on a caller-owned object: copy the shared state into the
     * frame so resumes after the owner unwinds never touch `this`. */
    const auto state = _state;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (state->dispatch).mode ());
    const auto call_packet_name = std::move (packet_name);
    {
        std::lock_guard lock (state->mutex);
        if (!channel_runtime_accepts_outbound_locked (*state)) {
            throw detail::make_boundary_exception (
              channel_runtime_outbound_error_state_locked (*state),
              channel_runtime_outbound_error_message_locked (*state));
        }
        state->outbound_calls.push_back (
          {"publish", channel_name, topic, call_packet_name, timeout, metadata});
    }
    const auto *publisher = publisher_capability (*state, channel_name);
    if (!has_connection (publisher)) {
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "channel publisher is not connected");
    }
    if (state->serializers != nullptr && publisher != nullptr
        && (!publisher->bind_endpoints.empty () || !publisher->connect_endpoints.empty ())) {
        if (const auto publish =
              fanout_publisher (state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (
                    encode_payload (*state->serializers));
                if (publisher->max_message_size
                    && publisher->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (
                         payload.size ())
                         > publisher->max_message_size->bytes ()) {
                    throw framework_exception_t (
                      framework_error_kind_t::internal_failure,
                      "channel message exceeds configured max message size");
                }
                co_await (*publish) (
                  topic,
                  call_packet_name,
                  state->serializers->content_type (
                    event_type),
                  std::move (payload),
                  resolve_send_wait_timeout (timeout));
            }
            catch (const framework_exception_t &) { throw; }
            catch (const std::exception &error) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  error.what ());
            }
        }
        try {
            runtime::messaging::client_call_codec_t codec;
            auto header = codec.create_envelope (runtime::messaging::message_kind_t::publish,
                                                 channel_name, call_packet_name, timeout, topic);
            header.metadata = metadata;
            auto parts = encode_channel_payload_parts (header, event_type, encode_payload,
                                                       *state->serializers);
            if (exceeds_configured_max_message_size (parts, *publisher)) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size");
            }
            std::shared_ptr<detail::channel_native_publisher_t> native_publisher;
            {
                std::lock_guard lock (state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*state)) {
                    throw detail::make_boundary_exception (
              channel_runtime_outbound_error_state_locked (*state),
              channel_runtime_outbound_error_message_locked (*state));
                }
                auto &stored = state->native_publishers[channel_name];
                if (!stored) {
                    std::optional<std::string> advertise_host;
                    if (const auto configured =
                          state->fanout_publisher_advertise_hosts.find (channel_name);
                        configured
                          != state->fanout_publisher_advertise_hosts.end ()) {
                        advertise_host = configured->second;
                    }
                    stored = std::make_shared<detail::channel_native_publisher_t> (
                      channel_name, *publisher, state->core_context,
                      std::move (advertise_host), state->listener_statuses);
                }
                native_publisher = stored;
            }
            co_await native_publisher->publish (
              topic, parts, resolve_send_wait_timeout (timeout));
        }
        catch (const framework_exception_t &) { throw; }
        catch (const std::exception &error) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "channel native publish failed");
        }
    }
    co_return;
}

} // namespace zlink::framework::detail
