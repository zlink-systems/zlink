/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_outbound_exchange.hpp"
#include "runtime/messaging/async_submit_runtime.hpp"

#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_socket_options.hpp"
#include "runtime/channels/socket_monitor_event.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
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
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace zlink::framework::detail
{

namespace
{

constexpr auto default_send_wait_timeout = std::chrono::milliseconds (1000);

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
            && submit_error->internal_errno () == EAGAIN) {
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

std::chrono::steady_clock::time_point
submit_deadline (std::chrono::milliseconds timeout)
{
    return timeout > std::chrono::milliseconds::zero ()
             ? std::chrono::steady_clock::now () + timeout
             : std::chrono::steady_clock::time_point::max ();
}

std::chrono::milliseconds
remaining_submit_timeout (std::chrono::steady_clock::time_point deadline)
{
    if (deadline == std::chrono::steady_clock::time_point::max ()) {
        return std::chrono::milliseconds::zero ();
    }
    const auto now = std::chrono::steady_clock::now ();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero ();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
}

bool submit_deadline_expired (std::chrono::steady_clock::time_point deadline)
{
    return deadline != std::chrono::steady_clock::time_point::max ()
           && std::chrono::steady_clock::now () >= deadline;
}

struct channel_endpoint_snapshot_t
{
    std::vector<std::string> endpoints;
    std::uint64_t version = 0;
};

bool is_channel_readiness_errno (int error) noexcept
{
    return error == EHOSTUNREACH || error == ENETUNREACH || error == ECONNREFUSED
           || error == ENOTCONN || error == EAGAIN;
}

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
                             channel_runtime_t runtime) :
        _channel_name (std::move (channel_name)), _client (client), _runtime (std::move (runtime))
    {
        runtime::messaging::activate_submit_owner (this);
        initialize_transport ();
    }

    result_t<runtime::messaging::message_parts_t>
    request (const runtime::messaging::message_parts_t &parts,
             const endpoint_provider_t &endpoints,
             std::chrono::milliseconds timeout)
    {
        if (_closed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::shutdown, "channel native client is closed");
        }
        const auto deadline = submit_deadline (timeout);

        for (;;) {
            try {
                if (_closed.load (std::memory_order_acquire)) {
                    return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::shutdown, "channel native client is closed");
                }
                if (submit_deadline_expired (deadline)) {
                    return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::timed_out, "channel request timed out");
                }
                const auto current = endpoints ();
                if (current.endpoints.empty ()) {
                    wait_for_readiness (deadline);
                    continue;
                }
                auto completion = std::make_shared<request_completion_t> ();
                {
                    std::lock_guard lock (_mutex);
                    if (_closed.load (std::memory_order_acquire)) {
                        return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::shutdown, "channel native client is closed");
                    }
                    auto transport = sync_connections (current);
                    completion->transport = transport;
                    const auto request_timeout = remaining_submit_timeout (deadline);
                    zlink::message_t request_header = parts[0];
                    zlink::message_t request_body = parts[1];
                    std::lock_guard transport_lock (transport->mutex);
                    const bool submitted =
                      transport->socket->request ()
                        .message (request_header)
                        .message (request_body)
                        .timeout (request_timeout)
                        .submit ([completion] (zlink::request_result_t result,
                                               std::vector<zlink::message_t> reply) {
                            std::lock_guard complete_lock (completion->mutex);
                            completion->result = result;
                            completion->reply = std::move (reply);
                            completion->completed = true;
                            completion->cv.notify_all ();
                        });
                    if (!submitted) {
                        return result_t<runtime::messaging::message_parts_t>::failure (
                          framework_error_kind_t::internal_failure,
                          "channel native request submit failed");
                    }
                }
                if (!wait_for_completion (completion, deadline)) {
                    if (_closed.load (std::memory_order_acquire)) {
                        return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::shutdown, "channel native client is closed");
                    }
                    return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::timed_out, "channel request timed out");
                }
                if (completion->result != zlink::request_result_t::ok) {
                    return detail::result_access_t::failure<
                      runtime::messaging::message_parts_t> (
                      runtime::messaging::map_request_result_exception (
                        completion->result, "channel native request failed"));
                }
                return result_t<runtime::messaging::message_parts_t>::success (
                  runtime::messaging::message_parts_t (std::move (completion->reply)));
            }
            catch (const zlink::submit_error_t &error) {
                if (is_channel_readiness_errno (error.internal_errno ())) {
                    if (submit_deadline_expired (deadline)) {
                        return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::timed_out, "channel request timed out");
                    }
                    wait_for_readiness (deadline);
                    continue;
                }
                const auto mapped = map_native_request_exception (error);
                return detail::result_access_t::failure<runtime::messaging::message_parts_t> (mapped);
            }
            catch (const std::exception &error) {
                const auto mapped = map_native_request_exception (error);
                return detail::result_access_t::failure<runtime::messaging::message_parts_t> (mapped);
            }
            catch (...) {
                return result_t<runtime::messaging::message_parts_t>::failure (
                  framework_error_kind_t::internal_failure, "channel native request failed");
            }
        }
    }

    result_t<void> send (const runtime::messaging::message_parts_t &parts,
                         const endpoint_provider_t &endpoints,
                         std::chrono::milliseconds timeout)
    {
        std::unique_lock operation_lock (_operation_mutex);
        runtime::messaging::note_submit_attempt (
          "channel:" + _channel_name, this, timeout, _runtime.pending_limit ());
        if (_closed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                            "channel native client is closed");
        }
        const auto current = endpoints ();
        if (current.endpoints.empty ()) {
            return detail::boundary_failure<void> (
              detail::boundary_error_t::disconnected,
              "channel client has no connected server endpoint");
        }
        try {
            std::shared_ptr<transport_t> transport;
            {
                std::lock_guard lock (_mutex);
                transport = sync_connections (current);
            }
            zlink::message_t send_header = parts[0];
            zlink::message_t send_body = parts[1];
            std::lock_guard transport_lock (transport->mutex);
            const bool sent =
              transport->socket->send ()
                .message (send_header)
                .message (send_body)
                .flags (static_cast<int> (zlink::send_flags_t::dontwait))
                .submit ();
            return sent
                     ? result_t<void>::success ()
                     : result_t<void>::failure (
                         framework_error_kind_t::capacity_exceeded,
                         "channel send is backpressured");
        }
        catch (const zlink::submit_error_t &error) {
            if (is_channel_readiness_errno (error.internal_errno ())) {
                return result_t<void>::failure (
                  framework_error_kind_t::capacity_exceeded,
                  "channel send is backpressured");
            }
            const auto mapped = map_native_request_exception (error);
            return detail::result_access_t::failure<void> (mapped);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "channel native send failed");
        }
    }

    void close () noexcept
    {
        runtime::messaging::shutdown_submit_owner (this);
        const bool was_closed = _closed.exchange (true, std::memory_order_acq_rel);
        notify_readiness_changed ();
        if (was_closed) {
            return;
        }
        std::lock_guard lock (_mutex);
        if (_transport) {
            _transport->close_noexcept ();
            _transport.reset ();
        }
    }

  private:
    struct transport_t
    {
        explicit transport_t (const channel_capability_snapshot_t &client) :
            context (std::make_unique<zlink::context_t> ()),
            socket (std::make_unique<zlink::dealer_socket_t> (*context))
        {
            apply_weighted_channel_socket_options (*socket, client);
            if (client.routing_id) {
                socket->set_routing_id (*client.routing_id);
            }
            socket->options ().immediate (true);
            monitor = socket->monitor_open (zlink::monitor_event::connected
                                            | zlink::monitor_event::connection_ready
                                            | zlink::monitor_event::disconnected);
            poller.add (*socket, zlink::poll_event_flag_t::pollcompletion, 1);
            monitor_poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
        }

        ~transport_t () { close_noexcept (); }

        void close_noexcept () noexcept
        {
            if (socket) {
                try {
                    poller.close ();
                }
                catch (...) {
                }
                try {
                    monitor_poller.close ();
                }
                catch (...) {
                }
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
            if (context) {
                try {
                    context->shutdown ();
                }
                catch (...) {
                }
                context.reset ();
            }
        }

        std::unique_ptr<zlink::context_t> context;
        std::unique_ptr<zlink::dealer_socket_t> socket;
        zlink::socket_monitor_t monitor;
        zlink::poller_t poller;
        zlink::poller_t monitor_poller;
        std::set<std::string> connected;
        std::uint64_t connection_version = 0;
        std::mutex mutex;
    };

    struct request_completion_t
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
        zlink::request_result_t result = zlink::request_result_t::ok;
        std::vector<zlink::message_t> reply;
        std::shared_ptr<transport_t> transport;

    };

    static std::chrono::milliseconds poll_timeout (
      std::chrono::steady_clock::time_point deadline)
    {
        if (deadline == std::chrono::steady_clock::time_point::max ()) {
            return std::chrono::milliseconds::max ();
        }
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline) {
            return std::chrono::milliseconds::zero ();
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
        return remaining > std::chrono::milliseconds::zero ()
                 ? remaining
                 : std::chrono::milliseconds (1);
    }

    bool wait_for_completion (const std::shared_ptr<request_completion_t> &completion,
                              std::chrono::steady_clock::time_point deadline)
    {
        for (;;) {
            {
                std::lock_guard completion_lock (completion->mutex);
                if (completion->completed) {
                    return true;
                }
            }
            if (_closed.load (std::memory_order_acquire)
                || submit_deadline_expired (deadline)) {
                return false;
            }
            const auto transport = completion->transport;
            if (!transport || !transport->poller.valid ()) {
                return false;
            }
            zlink::poll_event_t readiness;
            std::size_t ready = 0;
            try {
                std::lock_guard transport_lock (transport->mutex);
                ready = transport->poller.wait (&readiness, 1, poll_timeout (deadline));
            }
            catch (...) {
                return false;
            }
            if (ready == 0) {
                return false;
            }
        }
    }

    void wait_for_readiness (std::chrono::steady_clock::time_point deadline)
    {
        std::shared_ptr<transport_t> transport;
        {
            std::lock_guard lock (_mutex);
            transport = _transport;
        }
        if (transport && transport->monitor_poller.valid ()) {
            zlink::poll_event_t readiness;
            try {
                std::lock_guard transport_lock (transport->mutex);
                if (transport->monitor_poller.wait (
                      &readiness, 1, poll_timeout (deadline)) == 1
                    && readiness.slot == 1
                    && (static_cast<short> (readiness.revents)
                        & static_cast<short> (zlink::poll_event_flag_t::pollin))
                         != 0) {
                    drain_monitor_events_locked (*transport, true);
                }
            }
            catch (...) {
            }
            return;
        }
        std::unique_lock lock (_readiness_mutex);
        const auto generation = _readiness_generation;
        const auto closed = [this] {
            return _closed.load (std::memory_order_acquire);
        };
        const auto changed = [this, generation, &closed] {
            return closed () || _readiness_generation != generation;
        };
        if (deadline == std::chrono::steady_clock::time_point::max ()) {
            _readiness_changed.wait (lock, changed);
        } else {
            _readiness_changed.wait_until (lock, deadline, changed);
        }
    }

    void notify_readiness_changed ()
    {
        {
            std::lock_guard lock (_readiness_mutex);
            ++_readiness_generation;
        }
        _readiness_changed.notify_all ();
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

    void drain_monitor_events (const std::shared_ptr<transport_t> &transport)
    {
        if (!transport || !transport->monitor.valid ()) {
            return;
        }
        std::lock_guard transport_lock (transport->mutex);
        drain_monitor_events_locked (*transport);
    }

    void drain_monitor_events_locked (transport_t &transport,
                                      bool monitor_ready = false)
    {
        for (;;) {
            if (!monitor_ready) {
                zlink::poll_event_t readiness;
                try {
                    if (transport.monitor_poller.wait (
                          &readiness, 1, std::chrono::milliseconds::zero ())
                          != 1
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
            }
            monitor_ready = false;
            std::optional<zlink::monitor_event_t> event;
            try {
                event = transport.monitor.recv (zlink::recv_flags_t::dontwait);
            }
            catch (...) {
                return;
            }
            if (!event) {
                return;
            }
            const auto kind = map_socket_monitor_event (event->event);
            if (kind) {
                _runtime.publish_socket_event (
                  _channel_name, *kind, event->local_addr, event->remote_addr);
            }
            notify_readiness_changed ();
        }
    }

    void initialize_transport ()
    {
        _transport = make_transport ();
    }

    std::shared_ptr<transport_t> make_transport ()
    {
        auto transport = std::make_shared<transport_t> (_client);
        transport->socket->set_send_ready_handler ([this] {
            notify_readiness_changed ();
            runtime::messaging::notify_submit_ready ("channel:" + _channel_name, this);
        });
        return transport;
    }

    std::string _channel_name;
    channel_capability_snapshot_t _client;
    channel_runtime_t _runtime;
    std::shared_ptr<transport_t> _transport;
    std::mutex _operation_mutex;
    std::mutex _mutex;
    std::mutex _readiness_mutex;
    std::condition_variable _readiness_changed;
    std::uint64_t _readiness_generation = 0;
    std::atomic_bool _closed{false};
};

class channel_native_publisher_t
{
  public:
    channel_native_publisher_t (std::string channel_name,
                                const channel_capability_snapshot_t &publisher,
                                std::size_t pending_capacity) :
        _channel_name (std::move (channel_name)),
        _pending_capacity (pending_capacity),
        _socket (_context)
    {
        runtime::messaging::activate_submit_owner (this);
        apply_common_channel_socket_options (_socket, publisher);
        for (const auto &endpoint : publisher.bind_endpoints) {
            _socket.bind (endpoint);
        }
        for (const auto &endpoint : publisher.connect_endpoints) {
            _socket.connect (endpoint);
        }
        _poller.add (_socket, zlink::poll_event_flag_t::pollin, 1);
        _socket.set_send_ready_handler ([this] {
            runtime::messaging::notify_submit_ready (
              "fanout:" + _channel_name, this);
        });
    }

    result_t<void> publish (const std::string &topic,
                            const runtime::messaging::message_parts_t &parts,
                            std::chrono::milliseconds timeout)
    {
        runtime::messaging::note_submit_attempt (
          "fanout:" + _channel_name, this, timeout, _pending_capacity);
        if (_closed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                            "channel native publisher is closed");
        }
        std::lock_guard lock (_mutex);
        drain_subscription_events ();
        zlink::message_t publish_header = parts[0];
        zlink::message_t publish_body = parts[1];
        const bool sent =
          _socket.publish (topic)
            .message (publish_header)
            .message (publish_body)
            .flags (static_cast<int> (zlink::send_flags_t::dontwait))
            .submit ();
        if (!sent) {
            return result_t<void>::failure (
              framework_error_kind_t::capacity_exceeded,
              "channel native publish is backpressured");
        }
        return result_t<void>::success ();
    }

    void close () noexcept
    {
        runtime::messaging::shutdown_submit_owner (this);
        _closed.store (true, std::memory_order_release);
        try {
            _poller.close ();
        }
        catch (...) {
        }
        try {
            _context.shutdown ();
        }
        catch (...) {
        }
    }

  private:
    void drain_subscription_events ()
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (200);
        while (std::chrono::steady_clock::now () < deadline) {
            zlink::poll_event_t readiness;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              deadline - std::chrono::steady_clock::now ());
            try {
                if (_poller.wait (&readiness, 1, remaining) != 1
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
    std::size_t _pending_capacity;
    zlink::context_t _context;
    zlink::xpub_socket_t _socket;
    zlink::poller_t _poller;
    std::mutex _mutex;
    std::atomic_bool _closed{false};
};

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
        for (const auto &weak_client : state->native_request_clients) {
            if (auto client = weak_client.lock ()
                ; client && seen_clients.insert (client.get ()).second) {
                clients.push_back (std::move (client));
            }
        }
        for (auto &[_, publisher] : state->native_publishers) {
            if (publisher) {
                publishers.push_back (publisher);
            }
        }
        state->native_clients.clear ();
        state->native_request_clients.clear ();
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

} // namespace

message_bus_t::erased_request_result_t
channel_outbound_exchange_t::submit_request (std::string channel_name,
                                             std::string packet_name,
                                             std::type_index request_type,
                                             message_bus_t::payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             const channel_request_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled ());
    channel_runtime_t runtime (_state);
    const auto *client = client_capability (*_state, channel_name);
    const auto call_packet_name = std::move (packet_name);
    {
        std::lock_guard lock (_state->mutex);
        _state->outbound_calls.push_back (
          {"request", channel_name, "", call_packet_name, timeout, metadata});
    }
    auto reservation = runtime.reserve_outbound_request (channel_name);
    if (!reservation) {
        return message_bus_t::erased_request_result_t (
                  reservation.error () != nullptr
                    ? *reservation.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "channel request failed"));
    }
    channel_request_metrics_guard_t request_metrics (
      runtime::runtime_metrics_t (_state->monitoring), channel_name);
    if (!can_wait_for_client_endpoint (_state, client)) {
        (void) runtime.cancel_outbound_request (reservation.value ());
        return message_bus_t::erased_request_result_t (detail::make_boundary_exception (detail::boundary_error_t::disconnected, "channel client is not connected"));
    }
    if (_state->serializers != nullptr && client != nullptr) {
        if (const auto requester = client_server_requester (_state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (encode_payload (*_state->serializers));
                if (client->max_message_size
                    && client->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (payload.size ())
                         > client->max_message_size->bytes ()) {
                    (void) runtime.cancel_outbound_request (reservation.value ());
                    return message_bus_t::erased_request_result_t (
                      framework_exception_t (
                        framework_error_kind_t::internal_failure,
                        "channel message exceeds configured max message size"));
                }
                const auto effective_timeout =
                  resolve_channel_wait_timeout (_state, channel_name, timeout);
                auto reply = (*requester) (
                  call_packet_name,
                  _state->serializers->content_type (request_type),
                  std::move (payload),
                  effective_timeout);
                if (!reply) {
                    (void) runtime.cancel_outbound_request (reservation.value ());
                    return message_bus_t::erased_request_result_t (
                      reply.error () != nullptr
                        ? *reply.error ()
                        : framework_exception_t (
                            framework_error_kind_t::internal_failure,
                            "ClientServer request failed"));
                }
                auto completion =
                  runtime.complete_outbound_reply (reservation.value ());
                if (!completion) {
                    return message_bus_t::erased_request_result_t (
                      completion.error () != nullptr
                        ? *completion.error ()
                        : framework_exception_t (
                            framework_error_kind_t::internal_failure,
                            "channel request failed"));
                }
                return message_bus_t::erased_request_result_t (
                  reply.value (), *_state->serializers);
            }
            catch (const framework_exception_t &error) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                return message_bus_t::erased_request_result_t (error);
            }
            catch (const std::exception &error) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                return message_bus_t::erased_request_result_t (
                  map_native_request_exception (error));
            }
        }
        try {
            runtime::messaging::client_call_codec_t codec;
            const auto effective_timeout =
              resolve_channel_wait_timeout (_state, channel_name, timeout);
            auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                                 channel_name, call_packet_name,
                                                 effective_timeout);
            header.metadata = metadata;
            detail::message_flow_tracer_t (_state->dispatch)
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
                                                       *_state->serializers);
            if (exceeds_configured_max_message_size (parts, *client)) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                return message_bus_t::erased_request_result_t (framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size"));
            }
            std::shared_ptr<channel_native_client_t> native_client;
            {
                std::lock_guard lock (_state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*_state)) {
                    (void) runtime.cancel_outbound_request (reservation.value ());
                    return message_bus_t::erased_request_result_t (
                      detail::make_boundary_exception (
                        channel_runtime_outbound_error_state_locked (*_state),
                        channel_runtime_outbound_error_message_locked (*_state)));
                }
                _state->native_request_clients.erase (
                  std::remove_if (
                    _state->native_request_clients.begin (),
                    _state->native_request_clients.end (),
                    [] (const auto &weak_client) { return weak_client.expired (); }),
                  _state->native_request_clients.end ());
                native_client = std::make_shared<channel_native_client_t> (
                  channel_name, *client, runtime);
                _state->native_request_clients.emplace_back (native_client);
                auto &send_client = _state->native_clients[channel_name];
                if (!send_client) {
                    send_client = native_client;
                }
            }
            auto endpoints = make_client_endpoint_provider (_state, channel_name);

            auto native_reply = native_client->request (parts, endpoints, effective_timeout);
            if (!native_reply) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                request_metrics.timed_out =
                  native_reply.error () != nullptr
                  && detail::boundary_state (*native_reply.error ())
                       == detail::boundary_error_t::timed_out;
                return message_bus_t::erased_request_result_t (
                  native_reply.error () != nullptr
                    ? *native_reply.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "channel native request failed"));
            }
            auto validation = validate_channel_native_reply (native_reply.value ());
            if (!validation) {
                (void) runtime.cancel_outbound_request (reservation.value ());
                return message_bus_t::erased_request_result_t (
                  validation.error () != nullptr
                    ? *validation.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "channel reply decode failed"));
            }
            auto completion = runtime.complete_outbound_reply (reservation.value ());
            if (!completion) {
                return message_bus_t::erased_request_result_t (
                  completion.error () != nullptr
                    ? *completion.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "channel request failed"));
            }

            auto reply_header = envelope.decode_header (native_reply.value ());
            if (!reply_header) {
                return message_bus_t::erased_request_result_t (framework_exception_t (
                  reply_header.error_kind (), reply_header.error ()
                                                ? reply_header.error ()->what ()
                                                : "channel reply header decode failed"));
            }
            if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                runtime::messaging::request_failure_mapper_t failure_mapper;
                return message_bus_t::erased_request_result_t (
                  failure_mapper.error_header_exception (
                    reply_header.value ().error_code.value_or ("request_failed"),
                    reply_header.value ().error_message.value_or ("channel request failed"),
                    "channel request"));
            }
            auto body = envelope.decode_body (native_reply.value ());
            if (!body) {
                return message_bus_t::erased_request_result_t (
                  body.error () != nullptr
                    ? *body.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "channel reply body decode failed"));
            }
            detail::message_flow_tracer_t (_state->dispatch)
              .trace (message_flow_outcome_t::reply_received, [&] {
                  return message_flow_event_t{message_flow_outcome_t::reply_received,
                                              dispatch_error_surface_t::channel,
                                              dispatch_message_kind_t::response,
                                              call_packet_name,
                                              channel_name,
                                              std::nullopt,
                                              header.correlation_id,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt};
              });
            return message_bus_t::erased_request_result_t (body.value (), *_state->serializers);
        }
        catch (const framework_exception_t &error) {
            (void) runtime.cancel_outbound_request (reservation.value ());
            return message_bus_t::erased_request_result_t (error);
        }
        catch (const std::exception &error) {
            (void) runtime.cancel_outbound_request (reservation.value ());
            return message_bus_t::erased_request_result_t (map_native_request_exception (error));
        }
        catch (...) {
            (void) runtime.cancel_outbound_request (reservation.value ());
            return message_bus_t::erased_request_result_t (framework_exception_t (
              framework_error_kind_t::internal_failure, "channel native request failed"));
        }
    }
    (void) runtime.cancel_outbound_request (reservation.value ());
    request_metrics.timed_out = true;
    return message_bus_t::erased_request_result_t (detail::make_boundary_exception (detail::boundary_error_t::timed_out, "channel request reply was not completed by a backend"));
}

result_t<void>
channel_outbound_exchange_t::submit_send (std::string channel_name,
                                          std::string packet_name,
                                          std::type_index message_type,
                                          message_bus_t::payload_encoder_t encode_payload,
                                          std::chrono::milliseconds timeout,
                                          const send_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled ());
    const auto call_packet_name = std::move (packet_name);
    {
        std::lock_guard lock (_state->mutex);
        if (!channel_runtime_accepts_outbound_locked (*_state)) {
            return detail::boundary_failure<void> (
              channel_runtime_outbound_error_state_locked (*_state),
              channel_runtime_outbound_error_message_locked (*_state));
        }
        _state->outbound_calls.push_back (
          {"send", channel_name, "", call_packet_name, timeout, metadata});
    }
    const auto *client = client_capability (*_state, channel_name);
    if (!can_wait_for_client_endpoint (_state, client)) {
        return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                        "channel client is not connected");
    }
    if (_state->serializers != nullptr && client != nullptr) {
        if (const auto sender = client_server_sender (_state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (encode_payload (*_state->serializers));
                if (client->max_message_size
                    && client->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (payload.size ())
                         > client->max_message_size->bytes ()) {
                    return result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      "channel message exceeds configured max message size");
                }
                return (*sender) (
                  call_packet_name,
                  _state->serializers->content_type (message_type),
                  std::move (payload),
                  resolve_send_wait_timeout (timeout));
            }
            catch (const framework_exception_t &error) {
                return detail::result_access_t::failure<void> (error);
            }
            catch (const std::exception &error) {
                return result_t<void>::failure (
                  framework_error_kind_t::internal_failure, error.what ());
            }
        }
        try {
            runtime::messaging::client_call_codec_t codec;
            auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                                 channel_name, call_packet_name, timeout);
            header.metadata = metadata;
            detail::message_flow_tracer_t (_state->dispatch)
              .trace (message_flow_outcome_t::sent, [&] {
                  return message_flow_event_t{message_flow_outcome_t::sent,
                                              dispatch_error_surface_t::channel,
                                              dispatch_message_kind_t::send,
                                              call_packet_name,
                                              channel_name,
                                              std::nullopt,
                                              header.correlation_id,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt};
            });
            auto parts = encode_channel_payload_parts (header, message_type, encode_payload,
                                                       *_state->serializers);
            if (exceeds_configured_max_message_size (parts, *client)) {
                return result_t<void>::failure (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size");
            }
            std::shared_ptr<channel_native_client_t> native_client;
            {
                std::lock_guard lock (_state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*_state)) {
                    return detail::boundary_failure<void> (
              channel_runtime_outbound_error_state_locked (*_state),
              channel_runtime_outbound_error_message_locked (*_state));
                }
                auto &slot = _state->native_clients[channel_name];
                if (!slot) {
                    slot = std::make_shared<channel_native_client_t> (
                      channel_name, *client, channel_runtime_t (_state));
                }
                native_client = slot;
            }
            auto endpoints = make_client_endpoint_provider (_state, channel_name);
            const auto effective_timeout = resolve_send_wait_timeout (timeout);
            return native_client->send (parts, endpoints, effective_timeout);
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "channel native send failed");
        }
    }
    return result_t<void>::success ();
}

result_t<void>
channel_outbound_exchange_t::submit_publish (std::string channel_name,
                                             std::string topic,
                                             std::string packet_name,
                                             std::type_index event_type,
                                             message_bus_t::payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             const send_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled ());
    const auto call_packet_name = std::move (packet_name);
    {
        std::lock_guard lock (_state->mutex);
        if (!channel_runtime_accepts_outbound_locked (*_state)) {
            return detail::boundary_failure<void> (
              channel_runtime_outbound_error_state_locked (*_state),
              channel_runtime_outbound_error_message_locked (*_state));
        }
        _state->outbound_calls.push_back (
          {"publish", channel_name, topic, call_packet_name, timeout, metadata});
    }
    const auto *publisher = publisher_capability (*_state, channel_name);
    if (!has_connection (publisher)) {
        return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                        "channel publisher is not connected");
    }
    if (_state->serializers != nullptr && publisher != nullptr
        && (!publisher->bind_endpoints.empty () || !publisher->connect_endpoints.empty ())) {
        if (const auto publish =
              fanout_publisher (_state, channel_name)) {
            try {
                auto payload =
                  detail::encoded_payload_to_raw (
                    encode_payload (*_state->serializers));
                if (publisher->max_message_size
                    && publisher->max_message_size->bytes () > 0
                    && static_cast<std::int64_t> (
                         payload.size ())
                         > publisher->max_message_size->bytes ()) {
                    return result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      "channel message exceeds configured max message size");
                }
                auto published = (*publish) (
                  topic,
                  call_packet_name,
                  _state->serializers->content_type (
                    event_type),
                  std::move (payload),
                  resolve_send_wait_timeout (timeout));
                return published;
            }
            catch (const framework_exception_t &error) {
                return detail::result_access_t::failure<void> (
                  error);
            }
            catch (const std::exception &error) {
                return result_t<void>::failure (
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
                                                       *_state->serializers);
            if (exceeds_configured_max_message_size (parts, *publisher)) {
                return result_t<void>::failure (
                  framework_error_kind_t::internal_failure,
                  "channel message exceeds configured max message size");
            }
            std::shared_ptr<detail::channel_native_publisher_t> native_publisher;
            {
                std::lock_guard lock (_state->mutex);
                if (!channel_runtime_accepts_outbound_locked (*_state)) {
                    return detail::boundary_failure<void> (
              channel_runtime_outbound_error_state_locked (*_state),
              channel_runtime_outbound_error_message_locked (*_state));
                }
                auto &stored = _state->native_publishers[channel_name];
                if (!stored) {
                    stored = std::make_shared<detail::channel_native_publisher_t> (
                      channel_name, *publisher, _state->max_pending);
                }
                native_publisher = stored;
            }
            auto published = native_publisher->publish (
              topic, parts, resolve_send_wait_timeout (timeout));
            if (!published) {
                return published;
            }
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "channel native publish failed");
        }
    }
    return result_t<void>::success ();
}

} // namespace zlink::framework::detail
