/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "channel_runtime.hpp"

#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include "runtime/channels/channel_outbound_exchange.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

namespace
{

bool is_internal_spot_route_packet (std::string_view packet_name)
{
    return packet_name == spot_actor_join_route_request_t::packet_name
           || packet_name == spot_actor_packet_route_request_t::packet_name
           || packet_name == spot_actor_disconnect_route_request_t::packet_name
           || packet_name == actor_bound_session_route_request_t::packet_name;
}

std::optional<channel_runtime_state_t::spot_mesh_send_t>
spot_mesh_sender (const std::shared_ptr<channel_runtime_state_t> &state,
                  const std::string &mesh_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->spot_mesh_senders.find (mesh_name);
    return found == state->spot_mesh_senders.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::spot_mesh_send_t> (found->second);
}

std::optional<channel_runtime_state_t::mesh_node_send_t>
mesh_node_sender (const std::shared_ptr<channel_runtime_state_t> &state,
                  const std::string &mesh_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->mesh_node_senders.find (mesh_name);
    return found == state->mesh_node_senders.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::mesh_node_send_t> (found->second);
}

std::optional<channel_runtime_state_t::mesh_node_request_t>
mesh_node_requester (const std::shared_ptr<channel_runtime_state_t> &state,
                     const std::string &mesh_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->mesh_node_requesters.find (mesh_name);
    return found == state->mesh_node_requesters.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::mesh_node_request_t> (found->second);
}

std::optional<channel_runtime_state_t::mesh_channel_send_t>
mesh_channel_sender (const std::shared_ptr<channel_runtime_state_t> &state,
                     const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->mesh_channel_senders.find (channel_name);
    return found == state->mesh_channel_senders.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::mesh_channel_send_t> (found->second);
}

std::optional<channel_runtime_state_t::mesh_channel_request_t>
mesh_channel_requester (const std::shared_ptr<channel_runtime_state_t> &state,
                        const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->mesh_channel_requesters.find (channel_name);
    return found == state->mesh_channel_requesters.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::mesh_channel_request_t> (found->second);
}

std::optional<channel_runtime_state_t::spot_mesh_request_t>
spot_mesh_requester (const std::shared_ptr<channel_runtime_state_t> &state,
                     const std::string &mesh_name)
{
    std::lock_guard lock (state->mutex);
    const auto found = state->spot_mesh_requesters.find (mesh_name);
    return found == state->spot_mesh_requesters.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::spot_mesh_request_t> (found->second);
}

bool has_route_channel (const std::shared_ptr<channel_runtime_state_t> &state,
                        const std::string &channel_name)
{
    std::lock_guard lock (state->mutex);
    return state->route_channels.find (channel_name) != state->route_channels.end ();
}

} // namespace

route_client_state_t::route_client_state_t (std::shared_ptr<channel_runtime_state_t> runtime,
                                            serializer_registry_t &serializers) :
    runtime (std::move (runtime)), serializers (&serializers)
{
    const auto hardware_workers =
      static_cast<std::size_t> (std::max (1u, std::thread::hardware_concurrency ()));
    executor = std::make_shared<zlink::framework::runtime::offload_executor_t> (
      0, hardware_workers, 1024, std::chrono::milliseconds (100), "zlink-route-cli");
    {
        std::lock_guard lock (this->runtime->mutex);
        this->runtime->route_client_executors.push_back (executor);
    }
}

route_client_state_t::~route_client_state_t ()
{
    if (executor) {
        executor->drain ();
        executor.reset ();
    }
}

channel_capability_snapshot_t &select_capability (channel_builder_state_t &state,
                                                  channel_capability_t kind)
{
    auto &snapshot = state.target == nullptr ? state.snapshot : *state.target;
    switch (kind) {
        case channel_capability_t::server:
            return snapshot.server;
        case channel_capability_t::client:
            return snapshot.client;
        case channel_capability_t::publisher:
            return snapshot.publisher;
        case channel_capability_t::subscriber:
            return snapshot.subscriber;
    }
    return snapshot.client;
}

const channel_capability_snapshot_t *server_capability (const channel_runtime_state_t &state,
                                                        const std::string &channel_name)
{
    const auto found = state.channels.find (channel_name);
    if (found == state.channels.end ()) {
        return nullptr;
    }
    return &found->second.server;
}

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

bool is_enabled (const channel_capability_snapshot_t *capability)
{
    return capability != nullptr && capability->enabled;
}

bool has_connection (const channel_capability_snapshot_t *capability)
{
    return capability != nullptr && capability->enabled
           && (capability->discovery || !capability->bind_endpoints.empty ()
               || !capability->connect_endpoints.empty ());
}

result_t<void> validate_channel_native_reply (const runtime::messaging::message_parts_t &parts)
{
    if (parts.size () != 2) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "channel native request returned an invalid reply frame count");
    }
    runtime::messaging::envelope_codec_t envelope;
    auto header = envelope.decode_header (parts);
    if (!header) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        header.error () ? header.error ()->what ()
                                                        : "channel reply header decode failed");
    }
    if (header.value ().kind == runtime::messaging::message_kind_t::error) {
        return result_t<void>::success ();
    }
    if (header.value ().kind != runtime::messaging::message_kind_t::response) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "channel native request returned an invalid reply message kind");
    }
    auto body = envelope.decode_body (parts);
    if (!body) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        body.error () ? body.error ()->what ()
                                                      : "channel reply body decode failed");
    }
    return result_t<void>::success ();
}

class outbound_request_controller_t
{
  public:
    explicit outbound_request_controller_t (channel_runtime_state_t &state) : _state (state) {}

    result_t<std::uint64_t> reserve_request (std::string channel_name)
    {
        if (auto admission = ensure_admission (); !admission) {
            return detail::propagate_failure<std::uint64_t> (admission, "channel request was rejected");
        }

        const auto request_seq = _state.pending_requests.next_request_seq ();
        _state.pending_requests.register_request (request_seq, std::move (channel_name));
        ++_state.pending;
        return result_t<std::uint64_t>::success (request_seq);
    }

    result_t<void> complete_request (std::uint64_t request_seq)
    {
        if (!_state.pending_requests.remove (request_seq)) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "reply does not match a pending outbound request");
        }
        decrement_pending ();
        return result_t<void>::success ();
    }

    result_t<void> cancel_request (std::uint64_t request_seq)
    {
        if (_state.pending_requests.remove (request_seq)) {
            decrement_pending ();
        }
        return result_t<void>::success ();
    }

    void drain () noexcept
    {
        _state.pending_requests.clear ();
        _state.pending = 0;
    }

  private:
    result_t<void> ensure_admission () const
    {
        if (_state.shutdown) {
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                            "channel runtime is shutting down");
        }
        if (_state.closed) {
            return detail::boundary_failure<void> (detail::boundary_error_t::closed,
                                            "channel runtime is closed");
        }
        if (_state.pending >= _state.max_pending) {
            return result_t<void>::failure (framework_error_kind_t::rejected,
                                            "channel pending queue is full");
        }
        return result_t<void>::success ();
    }

    void decrement_pending () noexcept
    {
        if (_state.pending > 0) {
            --_state.pending;
        }
    }

    channel_runtime_state_t &_state;
};

channel_runtime_t::channel_runtime_t (std::shared_ptr<channel_runtime_state_t> state) :
    _state (std::move (state))
{
}

std::vector<channel_snapshot_t> channel_runtime_t::channel_snapshots () const
{
    std::vector<channel_snapshot_t> result;
    result.reserve (_state->channels.size ());
    for (const auto &[_, snapshot] : _state->channels) {
        result.push_back (snapshot);
    }
    return result;
}

namespace
{

void drain_route_client_executors (channel_runtime_state_t &state) noexcept
{
    std::vector<std::shared_ptr<runtime::offload_executor_t>> route_client_executors;
    {
        std::lock_guard lock (state.mutex);
        for (auto it = state.route_client_executors.begin ();
             it != state.route_client_executors.end ();) {
            if (auto executor = it->lock ()) {
                route_client_executors.push_back (std::move (executor));
                ++it;
            } else {
                it = state.route_client_executors.erase (it);
            }
        }
    }
    for (auto &executor : route_client_executors) {
        executor->drain ();
    }
}

void drain_zlink_builder_state_runtime (zlink_builder_state_t &state) noexcept
{
    if (state.runtime) {
        drain_route_client_executors (*state.runtime);
    }
    state.stream_runtime.reset ();
    state.route_channels.clear ();
    state.runtime.reset ();
}

} // namespace

zlink_builder_state_t::~zlink_builder_state_t ()
{
    drain_zlink_builder_state_runtime (*this);
}

void drain_zlink_builder_runtime (zlink_builder_t &builder) noexcept
{
    if (!builder._state) {
        return;
    }
    drain_zlink_builder_state_runtime (*builder._state);
}

void bind_zlink_monitoring (
  zlink_builder_t &builder,
  std::shared_ptr<monitoring_runtime_state_t> monitoring)
{
    if (!builder._state) {
        return;
    }
    builder._state->runtime->monitoring = std::move (monitoring);
}

void channel_runtime_t::add_client_manual_connection (const std::string &channel_name,
                                                      const std::string &endpoint)
{
    detail::channel_runtime_manager_t manager (_state);
    manager.get_or_create_client_bundle (channel_name).try_add_manual_connection (endpoint);
}

void channel_runtime_t::remove_client_manual_connection (const std::string &channel_name,
                                                         const std::string &endpoint)
{
    detail::channel_runtime_manager_t manager (_state);
    manager.get_or_create_client_bundle (channel_name).remove_manual_connection (endpoint);
}

void channel_runtime_t::add_subscriber_manual_connection (const std::string &channel_name,
                                                          const std::string &endpoint)
{
    detail::channel_runtime_manager_t manager (_state);
    manager.get_or_create_subscriber_bundle (channel_name).try_add_manual_connection (endpoint);
}

void channel_runtime_t::remove_subscriber_manual_connection (const std::string &channel_name,
                                                             const std::string &endpoint)
{
    detail::channel_runtime_manager_t manager (_state);
    manager.get_or_create_subscriber_bundle (channel_name).remove_manual_connection (endpoint);
}

void apply_dispatch_options (zlink_builder_t &builder, const dispatch_options_t &options)
{
    builder._state->runtime->dispatch = options;
    builder._state->stream_runtime->dispatch = options;
}

result_t<zlink::message_t> channel_runtime_t::dispatch_request (std::string channel_name,
                                                                std::string topic,
                                                                std::string packet_name,
                                                                service_provider_t &services,
                                                                serializer_registry_t &serializers,
                                                                const handler_registry_t &handlers,
                                                                const zlink::message_t &message,
                                                                const detail::
                                                                  inbound_message_context_t
                                                                    &inbound) const
{
    if (!is_enabled (server_capability (*_state, channel_name))) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::unavailable,
                                                    "channel server capability is not enabled");
    }
    return handlers.invoke (channel_name, topic, packet_name, services, serializers, message,
                            inbound);
}

result_t<void> channel_runtime_t::dispatch_send (std::string channel_name,
                                                 std::string topic,
                                                 std::string packet_name,
                                                 service_provider_t &services,
                                                 serializer_registry_t &serializers,
                                                 const handler_registry_t &handlers,
                                                 const zlink::message_t &message,
                                                 const detail::inbound_message_context_t
                                                   &inbound) const
{
    auto result = handlers.invoke (channel_name, topic, packet_name, services, serializers, message,
                                   inbound);
    if (!result) {
        return result_t<void>::failure (result.error_kind (), result.error ()
                                                                ? result.error ()->what ()
                                                                : "channel dispatch failed");
    }
    return result_t<void>::success ();
}

result_t<std::uint64_t> channel_runtime_t::reserve_outbound_request (std::string channel_name)
{
    std::lock_guard lock (_state->mutex);
    const auto *client = client_capability (*_state, channel_name);
    if (!has_connection (client)) {
        return detail::boundary_failure<std::uint64_t> (detail::boundary_error_t::disconnected,
                                                 "channel client is not connected");
    }
    return outbound_request_controller_t (*_state).reserve_request (std::move (channel_name));
}

result_t<void> channel_runtime_t::complete_outbound_reply (std::uint64_t request_seq)
{
    std::lock_guard lock (_state->mutex);
    return outbound_request_controller_t (*_state).complete_request (request_seq);
}

result_t<void> channel_runtime_t::cancel_outbound_request (std::uint64_t request_seq)
{
    std::lock_guard lock (_state->mutex);
    return outbound_request_controller_t (*_state).cancel_request (request_seq);
}

void channel_runtime_t::close () noexcept
{
    {
        std::lock_guard lock (_state->mutex);
        _state->closed = true;
    }
    close_native_channel_transports (_state);
    drain ();
}

void channel_runtime_t::shutdown () noexcept
{
    std::vector<std::shared_ptr<route_channel_runtime_t>> route_channels;
    {
        std::lock_guard lock (_state->mutex);
        _state->shutdown = true;
        for (auto &[_, route_channel] : _state->route_channels) {
            if (route_channel) {
                route_channels.push_back (route_channel);
            }
        }
    }
    for (auto &route_channel : route_channels) {
        route_channel->stop ();
    }
    close_native_channel_transports (_state);
    drain_route_client_executors (*_state);
    drain ();
}

std::size_t channel_runtime_t::pending_count () const noexcept
{
    std::lock_guard lock (_state->mutex);
    return _state->pending;
}

std::size_t channel_runtime_t::pending_limit () const noexcept
{
    return _state->max_pending;
}

std::vector<channel_runtime_state_t::outbound_call_record_t>
channel_runtime_t::outbound_calls () const
{
    std::lock_guard lock (_state->mutex);
    return _state->outbound_calls;
}

void channel_runtime_t::bind_serializers (serializer_registry_t &serializers) noexcept
{
    _state->serializers = &serializers;
}

void channel_runtime_t::bind_spot_mesh_transport (
  std::string mesh_name,
  channel_runtime_state_t::spot_mesh_send_t send,
  channel_runtime_state_t::spot_mesh_request_t request)
{
    std::lock_guard lock (_state->mutex);
    _state->spot_mesh_senders.insert_or_assign (mesh_name, std::move (send));
    _state->spot_mesh_requesters.insert_or_assign (std::move (mesh_name), std::move (request));
}

void channel_runtime_t::bind_spot_address_resolver (
  runtime::spot_address_resolver_t &resolver) noexcept
{
    std::lock_guard lock (_state->mutex);
    _state->spot_resolver = &resolver;
}

void channel_runtime_t::bind_instance_spot_activator (
  channel_runtime_state_t::instance_spot_send_t send,
  channel_runtime_state_t::instance_spot_request_t request)
{
    std::lock_guard lock (_state->mutex);
    _state->instance_spot_sender = std::move (send);
    _state->instance_spot_requester = std::move (request);
}

void channel_runtime_t::bind_mesh_node_transport (
  std::string mesh_name,
  channel_runtime_state_t::mesh_node_send_t send,
  channel_runtime_state_t::mesh_node_request_t request)
{
    std::lock_guard lock (_state->mutex);
    _state->mesh_node_senders.insert_or_assign (mesh_name, std::move (send));
    _state->mesh_node_requesters.insert_or_assign (std::move (mesh_name),
                                                   std::move (request));
}

void channel_runtime_t::bind_mesh_channel_transport (
  std::string channel_name,
  channel_runtime_state_t::mesh_channel_send_t send,
  channel_runtime_state_t::mesh_channel_request_t request)
{
    std::lock_guard lock (_state->mutex);
    if (_state->client_server_senders.contains (channel_name)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "ChannelName is registered by both RouteMesh and ClientServer: " + channel_name);
    }
    if (_state->mesh_channel_senders.contains (channel_name)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "RouteMesh ChannelName is registered by more than one MeshNode: " + channel_name);
    }
    _state->mesh_channel_senders.emplace (channel_name, std::move (send));
    _state->mesh_channel_requesters.emplace (std::move (channel_name), std::move (request));
}

void channel_runtime_t::bind_client_server_transport (
  std::string channel_name,
  channel_runtime_state_t::client_server_send_t send,
  channel_runtime_state_t::client_server_request_t request)
{
    std::lock_guard lock (_state->mutex);
    if (_state->mesh_channel_senders.contains (channel_name)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "ChannelName is registered by both ClientServer and RouteMesh: " + channel_name);
    }
    if (_state->client_server_senders.contains (channel_name)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "ClientServer ChannelName is registered more than once: " + channel_name);
    }
    _state->client_server_senders.emplace (channel_name, std::move (send));
    _state->client_server_requesters.emplace (std::move (channel_name), std::move (request));
}

void channel_runtime_t::unbind_client_server_transport (
  const std::string &channel_name) noexcept
{
    std::lock_guard lock (_state->mutex);
    _state->client_server_senders.erase (channel_name);
    _state->client_server_requesters.erase (channel_name);
}

void channel_runtime_t::bind_fanout_transport (
  std::string channel_name,
  channel_runtime_state_t::fanout_publish_t publish)
{
    std::lock_guard lock (_state->mutex);
    if (_state->fanout_publishers.contains (channel_name)) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "fanout ChannelName is registered more than once: "
            + channel_name);
    }
    _state->fanout_publishers.emplace (
      std::move (channel_name), std::move (publish));
}

void channel_runtime_t::unbind_fanout_transport (
  const std::string &channel_name) noexcept
{
    std::lock_guard lock (_state->mutex);
    _state->fanout_publishers.erase (channel_name);
}

dispatch_options_t channel_runtime_t::dispatch_options () const
{
    return _state->dispatch;
}

void channel_runtime_t::mark_auto_connect_active ()
{
    std::lock_guard lock (_state->mutex);
    _state->auto_connect_active = true;
}

bool channel_runtime_t::auto_connect_active () const
{
    std::lock_guard lock (_state->mutex);
    return _state->auto_connect_active;
}

void channel_runtime_t::drain () noexcept
{
    std::lock_guard lock (_state->mutex);
    outbound_request_controller_t (*_state).drain ();
}

void channel_runtime_t::publish_socket_event (const std::string &channel_name,
                                              socket_event_kind_t event,
                                              std::string local_address,
                                              std::string remote_address) const
{
    (void) local_address;
    (void) remote_address;
    if (!_state->monitoring) {
        return;
    }
    monitoring_runtime_t (_state->monitoring)
      .publish_socket (
        socket_event_payload_t{channel_name, event});
}

void channel_runtime_t::set_server_weight (const std::string &channel_name,
                                           int value)
{
    if (value < 0 || value > 10000)
        throw std::invalid_argument (
          "service weight must be in range 0..10000");
    {
        std::lock_guard lock (_state->mutex);
        _state->server_peer_weight_overrides.insert_or_assign (channel_name, value);
    }
}

std::optional<int>
channel_runtime_t::server_peer_weight_override (const std::string &channel_name) const
{
    std::lock_guard lock (_state->mutex);
    const auto found = _state->server_peer_weight_overrides.find (channel_name);
    if (found == _state->server_peer_weight_overrides.end ()) {
        return std::nullopt;
    }
    return found->second;
}

channel_runtime_t channel_runtime_t::from (const message_bus_t &bus)
{
    return channel_runtime_t (bus._state);
}

} // namespace zlink::framework::detail

namespace zlink::framework
{
namespace
{

runtime::messaging::message_parts_t
encode_route_payload_parts (runtime::messaging::envelope_header_t header,
                            std::type_index payload_type,
                            const route_client_t::payload_encoder_t &encode_payload,
                            serializer_registry_t &serializers)
{
    header.content_type = serializers.content_type (payload_type);
    runtime::messaging::envelope_codec_t envelope;
    return envelope.encode_raw_body_parts (
      header, detail::encoded_payload_to_raw (encode_payload (serializers)));
}

} // namespace


namespace
{

channel_capability_snapshot_t &capability_snapshot (detail::capability_builder_state_t &state)
{
    return state.target == nullptr ? state.snapshot : *state.target;
}

const channel_capability_snapshot_t &
capability_snapshot (const detail::capability_builder_state_t &state)
{
    return state.target == nullptr ? state.snapshot : *state.target;
}

} // namespace

capability_builder_t::capability_builder_t () :
    _state (std::make_shared<detail::capability_builder_state_t> ())
{
}

capability_builder_t::capability_builder_t (
  std::shared_ptr<detail::capability_builder_state_t> state) :
    _state (std::move (state))
{
}

capability_builder_t::~capability_builder_t () = default;

capability_builder_t::capability_builder_t (capability_builder_t &&) noexcept = default;

capability_builder_t &capability_builder_t::operator= (capability_builder_t &&) noexcept = default;

capability_builder_t &capability_builder_t::bind (std::string endpoint)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.bind_endpoints.push_back (std::move (endpoint));
    return *this;
}

capability_builder_t &capability_builder_t::connect (std::string endpoint)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.discovery = false;
    snapshot.enabled = true;
    snapshot.connect_endpoints.push_back (std::move (endpoint));
    return *this;
}

capability_builder_t &capability_builder_t::set_routing_id (zlink::routing_id_t routing_id)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.routing_id = std::move (routing_id);
    return *this;
}

capability_builder_t &capability_builder_t::send_high_water_mark (zlink::byte_count_t value)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.send_high_water_mark = value;
    return *this;
}

capability_builder_t &capability_builder_t::receive_high_water_mark (zlink::byte_count_t value)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.receive_high_water_mark = value;
    return *this;
}

capability_builder_t &capability_builder_t::max_message_size (zlink::byte_size_t value)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.max_message_size = value;
    return *this;
}

capability_builder_t &capability_builder_t::peer_weight (zlink::peer_weight_t value)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.peer_weight = value;
    snapshot.service_weight = static_cast<int> (value.value ());
    return *this;
}

capability_builder_t &capability_builder_t::service_weight (int value)
{
    if (value < 0 || value > 10000)
        throw std::invalid_argument (
          "service weight must be in range 0..10000");
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.service_weight = value;
    return *this;
}

channel_capability_snapshot_t capability_builder_t::snapshot () const
{
    return capability_snapshot (*_state);
}

channel_builder_t::channel_builder_t () :
    _state (std::make_shared<detail::channel_builder_state_t> (""))
{
}

channel_builder_t::channel_builder_t (std::shared_ptr<detail::channel_builder_state_t> state) :
    _state (std::move (state))
{
}

channel_builder_t::~channel_builder_t () = default;

channel_builder_t::channel_builder_t (channel_builder_t &&) noexcept = default;

channel_builder_t &channel_builder_t::operator= (channel_builder_t &&) noexcept = default;

capability_builder_t channel_builder_t::enable_capability (channel_capability_snapshot_t &target)
{
    auto state = std::make_shared<detail::capability_builder_state_t> ();
    state->target = &target;
    target.enabled = true;
    return capability_builder_t (state);
}

capability_builder_t channel_builder_t::enable_server ()
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::server));
    detail::select_capability (*_state, channel_capability_t::server).discovery = true;
    return builder;
}

capability_builder_t channel_builder_t::enable_client ()
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::client));
    detail::select_capability (*_state, channel_capability_t::client).discovery = true;
    return builder;
}

capability_builder_t channel_builder_t::enable_publisher ()
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::publisher));
    detail::select_capability (*_state, channel_capability_t::publisher).discovery = true;
    return builder;
}

capability_builder_t channel_builder_t::enable_subscriber ()
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::subscriber));
    detail::select_capability (*_state, channel_capability_t::subscriber).discovery = true;
    return builder;
}

channel_builder_t &channel_builder_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "channel request timeout must be greater than zero");
    }
    _state->snapshot.default_request_timeout = timeout;
    if (_state->target != nullptr) {
        _state->target->default_request_timeout = timeout;
    }
    return *this;
}

channel_snapshot_t channel_builder_t::snapshot () const
{
    return _state->target == nullptr ? _state->snapshot : *_state->target;
}

route_channel_builder_t::route_channel_builder_t ()
{
}

route_channel_builder_t::route_channel_builder_t (
  std::shared_ptr<detail::route_channel_builder_state_t> state) :
    _state (std::move (state))
{
}

route_channel_builder_t::~route_channel_builder_t () = default;

route_channel_builder_t::route_channel_builder_t (route_channel_builder_t &&) noexcept = default;

route_channel_builder_t &
route_channel_builder_t::operator= (route_channel_builder_t &&) noexcept = default;

route_channel_builder_t &route_channel_builder_t::bind (std::string endpoint)
{
    _state->registration.bind (std::move (endpoint));
    return *this;
}

route_channel_builder_t &route_channel_builder_t::set_routing_id (zlink::routing_id_t routing_id)
{
    _state->registration.set_routing_id (std::move (routing_id));
    return *this;
}

route_channel_builder_t &route_channel_builder_t::connect (std::string endpoint)
{
    _state->registration.connect (std::move (endpoint));
    return *this;
}

void
detail::connect_route_channel_peer (route_channel_builder_t &builder,
                                    zlink::routing_id_t peer_rid,
                                    std::string endpoint)
{
    builder._state->registration.connect (std::move (peer_rid), std::move (endpoint));
}

route_channel_builder_t &
route_channel_builder_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    _state->registration.default_request_timeout (timeout);
    return *this;
}

route_channel_builder_t &route_channel_builder_t::add_handler_group (std::string group_name)
{
    _state->registration.add_handler_group (std::move (group_name));
    return *this;
}

route_channel_builder_t &
route_channel_builder_t::add_handler (route_handler_registration_t registration)
{
    _state->registration.add_handler (std::move (registration));
    return *this;
}

message_bus_t::erased_request_result_t::erased_request_result_t (framework_exception_t error) :
    _error (std::move (error))
{
}

message_bus_t::erased_request_result_t::erased_request_result_t (
  zlink::message_t reply, serializer_registry_t &serializers) :
    _error (framework_error_kind_t::internal_failure, ""),
    _reply (std::move (reply)),
    _serializers (&serializers)
{
}

message_bus_t::message_bus_t () : _state (std::make_shared<detail::channel_runtime_state_t> ())
{
}

message_bus_t::message_bus_t (std::shared_ptr<detail::channel_runtime_state_t> state) :
    _state (std::move (state))
{
}

message_bus_t::message_bus_t (
  std::shared_ptr<detail::channel_runtime_state_t> state,
  std::function<result_t<void> ()> preflight) :
    _state (std::move (state)), _preflight (std::move (preflight))
{
}

message_bus_t::~message_bus_t () = default;

message_bus_t::message_bus_t (message_bus_t &&) noexcept = default;

message_bus_t &message_bus_t::operator= (message_bus_t &&) noexcept = default;

std::chrono::milliseconds
message_bus_t::default_request_timeout (const std::string &channel_name) const
{
    std::lock_guard lock (_state->mutex);
    const auto found = _state->channels.find (channel_name);
    if (found != _state->channels.end () && found->second.default_request_timeout) {
        return *found->second.default_request_timeout;
    }
    return _state->default_request_timeout;
}

serializer_registry_t *message_bus_t::serializers () const noexcept
{
    return _state ? _state->serializers : nullptr;
}

message_bus_t::erased_request_result_t
message_bus_t::submit_request (std::string channel_name,
                               std::string packet_name,
                               std::type_index request_type,
                               payload_encoder_t encode_payload,
                               std::chrono::milliseconds timeout,
                               const channel_request_call_t::metadata_map_t &metadata)
{
    return detail::channel_outbound_exchange_t (_state).submit_request (
      std::move (channel_name), std::move (packet_name), request_type, std::move (encode_payload),
      timeout, metadata);
}

task_t<zlink::message_t>
message_bus_t::submit_request_message_async (std::string channel_name,
                                             std::string packet_name,
                                             std::type_index request_type,
                                             payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             channel_request_call_t::metadata_map_t metadata)
{
    auto result =
      submit_request (std::move (channel_name), std::move (packet_name), request_type,
                      std::move (encode_payload), timeout, metadata)
        .message ();
    return task_t<zlink::message_t> (std::move (result));
}

result_t<void> message_bus_t::submit_send (std::string channel_name,
                                           std::string packet_name,
                                           std::type_index message_type,
                                           payload_encoder_t encode_payload,
                                           std::chrono::milliseconds timeout,
                                           const send_call_t::metadata_map_t &metadata)
{
    return detail::channel_outbound_exchange_t (_state).submit_send (
      std::move (channel_name), std::move (packet_name), message_type, std::move (encode_payload),
      timeout, metadata);
}

result_t<void> message_bus_t::submit_publish (std::string channel_name,
                                              std::string topic,
                                              std::string packet_name,
                                              std::type_index event_type,
                                              payload_encoder_t encode_payload,
                                              std::chrono::milliseconds timeout,
                                              const send_call_t::metadata_map_t &metadata)
{
    return detail::channel_outbound_exchange_t (_state).submit_publish (
      std::move (channel_name), std::move (topic), std::move (packet_name), event_type,
      std::move (encode_payload), timeout, metadata);
}

channel_server_socket_runtime_options_t::channel_server_socket_runtime_options_t () = default;

channel_server_socket_runtime_options_t::channel_server_socket_runtime_options_t (
  std::shared_ptr<detail::channel_runtime_state_t> state, std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

channel_server_socket_runtime_options_t::~channel_server_socket_runtime_options_t () = default;

channel_server_socket_runtime_options_t::channel_server_socket_runtime_options_t (
  channel_server_socket_runtime_options_t &&) noexcept = default;

channel_server_socket_runtime_options_t &channel_server_socket_runtime_options_t::operator= (
  channel_server_socket_runtime_options_t &&) noexcept = default;

channel_server_socket_runtime_options_t &
channel_server_socket_runtime_options_t::peer_weight (zlink::peer_weight_t value)
{
    detail::channel_runtime_t (_state).set_server_weight (
      _channel_name, static_cast<int> (value.value ()));
    return *this;
}

channel_server_socket_runtime_options_t &
channel_server_socket_runtime_options_t::weight (int value)
{
    detail::channel_runtime_t (_state).set_server_weight (
      _channel_name, value);
    return *this;
}

client_server_channel_runtime_options_t::client_server_channel_runtime_options_t () = default;

client_server_channel_runtime_options_t::client_server_channel_runtime_options_t (
  std::shared_ptr<detail::channel_runtime_state_t> state, std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

client_server_channel_runtime_options_t::~client_server_channel_runtime_options_t () = default;

client_server_channel_runtime_options_t::client_server_channel_runtime_options_t (
  client_server_channel_runtime_options_t &&) noexcept = default;

client_server_channel_runtime_options_t &client_server_channel_runtime_options_t::operator= (
  client_server_channel_runtime_options_t &&) noexcept = default;

channel_server_socket_runtime_options_t
client_server_channel_runtime_options_t::configure_server_socket () const
{
    return channel_server_socket_runtime_options_t (_state, _channel_name);
}

route_mesh_channel_runtime_options_t::route_mesh_channel_runtime_options_t () = default;

route_mesh_channel_runtime_options_t::route_mesh_channel_runtime_options_t (
  std::shared_ptr<detail::channel_runtime_state_t> state, std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

route_mesh_channel_runtime_options_t::~route_mesh_channel_runtime_options_t () = default;

route_mesh_channel_runtime_options_t::route_mesh_channel_runtime_options_t (
  route_mesh_channel_runtime_options_t &&) noexcept = default;

route_mesh_channel_runtime_options_t &route_mesh_channel_runtime_options_t::operator= (
  route_mesh_channel_runtime_options_t &&) noexcept = default;

channel_server_socket_runtime_options_t
route_mesh_channel_runtime_options_t::configure_server_socket () const
{
    return channel_server_socket_runtime_options_t (_state, _channel_name);
}

channel_runtime_options_t::channel_runtime_options_t () = default;

channel_runtime_options_t::channel_runtime_options_t (message_bus_t bus) : _state (bus._state)
{
}

channel_runtime_options_t::~channel_runtime_options_t () = default;

channel_runtime_options_t::channel_runtime_options_t (channel_runtime_options_t &&) noexcept =
  default;

channel_runtime_options_t &
channel_runtime_options_t::operator= (channel_runtime_options_t &&) noexcept = default;

client_server_channel_runtime_options_t
channel_runtime_options_t::client_server_channel (std::string channel_name) const
{
    return client_server_channel_runtime_options_t (_state, std::move (channel_name));
}

route_mesh_channel_runtime_options_t
channel_runtime_options_t::route_mesh_channel (std::string_view channel_name) const
{
    return route_mesh_channel_runtime_options_t (_state, std::string (channel_name));
}

request_client_t::request_client_t (message_bus_t bus, std::string channel_name) :
    _bus (std::move (bus)), _channel_name (std::move (channel_name))
{
}

publisher_t::publisher_t (message_bus_t bus) : _bus (std::move (bus))
{
}

route_send_call_t::route_send_call_t (std::string packet_name, submit_fn_t submit) :
    _packet_name (std::move (packet_name)), _submit (std::move (submit))
{
}

route_send_call_t &route_send_call_t::metadata (std::string key, std::string value)
{
    _metadata[std::move (key)] = std::move (value);
    return *this;
}

result_t<void> route_send_call_t::submit_now ()
{
    if (!_submit) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "route send call is not bound to a route client");
    }
    return _submit (_packet_name, _metadata);
}

task_t<void> route_send_call_t::submit ()
{
    if (!_submission->try_claim ()) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "route send call has already been submitted"));
    }
    return detail::submit_one_way_task (
      [packet_name = _packet_name, metadata = _metadata, submit = _submit] () {
          if (!submit) {
              return result_t<void>::failure (
                framework_error_kind_t::protocol_error,
                "route send call is not bound to a route client");
          }
          return submit (packet_name, metadata);
      });
}

route_client_t::route_client_t () = default;

route_client_t::route_client_t (std::shared_ptr<detail::route_client_state_t> state,
                                serializer_registry_t &serializers) :
    _state (std::move (state)), _serializers (&serializers)
{
}

route_client_t::~route_client_t () = default;

route_client_t::route_client_t (route_client_t &&) noexcept = default;

route_client_t &route_client_t::operator= (route_client_t &&) noexcept = default;

result_t<void>
route_client_t::submit_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                    const std::string &router_channel_id,
                                    const zlink::routing_id_t &target_node_rid,
                                    const std::string &packet_name,
                                    std::type_index message_type,
                                    payload_encoder_t encode_payload,
                                    const route_send_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    if (!state || !state->runtime || state->serializers == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "route client is not configured");
    }
    runtime::messaging::message_parts_t parts;
    const auto use_route_channel = detail::has_route_channel (state->runtime, router_channel_id);
    const auto mesh_sender = use_route_channel
                               ? std::nullopt
                               : detail::mesh_node_sender (state->runtime, router_channel_id);
    if (!use_route_channel && !mesh_sender) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "route channel or SPOT mesh '" + router_channel_id + "' is not registered");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             router_channel_id, packet_name);
        header.metadata = metadata;
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::route_mesh_channel,
                                          dispatch_message_kind_t::send,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), message_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    try {
        if (mesh_sender) {
            return (*mesh_sender) (target_node_rid, std::move (parts));
        }
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        return runtime.submit_send_parts (target_node_rid, std::move (parts));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

result_t<void> route_client_t::submit_channel_send_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  const std::string &channel_name,
  const std::string &packet_name,
  std::type_index message_type,
  payload_encoder_t encode_payload,
  const route_send_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    if (!state || !state->runtime || state->serializers == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "route client is not configured");
    }
    const auto sender = detail::mesh_channel_sender (state->runtime, channel_name);
    if (!sender) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "RouteMesh channel '" + channel_name + "' is not registered");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             channel_name, packet_name);
        header.metadata = metadata;
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::route_mesh_channel,
                                          dispatch_message_kind_t::send,
                                          packet_name,
                                          channel_name,
                                          std::nullopt,
                                          header.correlation_id,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
          });
        auto parts = encode_route_payload_parts (std::move (header), message_type,
                                                 std::move (encode_payload),
                                                 *state->serializers);
        return (*sender) (std::move (parts));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<std::uint64_t>
route_client_t::submit_request_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                       const std::string &router_channel_id,
                                       const zlink::routing_id_t &target_node_rid,
                                       const std::string &packet_name,
                                       std::type_index request_type,
                                       payload_encoder_t encode_payload,
                                       std::chrono::milliseconds timeout,
                                       const channel_request_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    if (!state || !state->runtime || state->serializers == nullptr) {
        return task_t<std::uint64_t> (result_t<std::uint64_t>::failure (
          framework_error_kind_t::protocol_error, "route client is not configured"));
    }
    runtime::messaging::message_parts_t parts;
    try {
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                         ? timeout
                                         : runtime.default_request_timeout ();
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             router_channel_id, packet_name, effective_timeout);
        header.metadata = metadata;
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::route_mesh_channel,
                                          dispatch_message_kind_t::request,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), request_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (
          detail::result_access_t::failure<std::uint64_t> (error));
    }
    try {
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        return task_t<std::uint64_t> (runtime.submit_request_parts (target_node_rid,
                                                                    std::move (parts)));
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (
          detail::result_access_t::failure<std::uint64_t> (error));
    }
    catch (const std::exception &error) {
        return task_t<std::uint64_t> (result_t<std::uint64_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
}

result_t<void>
route_client_t::submit_spot_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                         const std::string &router_channel_id,
                                         const zlink::routing_id_t &target_node_rid,
                                         const spot_id_t &target_spot_id,
                                         std::uint64_t target_spot_generation,
                                         const std::string &packet_name,
                                         std::type_index message_type,
                                         payload_encoder_t encode_payload,
                                         const route_send_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    if (!state || !state->runtime || state->serializers == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "route client is not configured");
    }
    runtime::messaging::message_parts_t parts;
    const auto &spot_id = target_spot_id;
    const auto use_route_channel = detail::has_route_channel (state->runtime, router_channel_id);
    const auto mesh_sender = use_route_channel
                               ? std::nullopt
                               : detail::spot_mesh_sender (state->runtime, router_channel_id);
    if (!use_route_channel && !mesh_sender) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "route channel or SPOT mesh '" + router_channel_id + "' is not registered");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             router_channel_id, packet_name);
        header.metadata = metadata;
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::spot_route,
                                          dispatch_message_kind_t::send,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          spot_id,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), message_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    try {
        if (mesh_sender) {
            return (*mesh_sender) (target_node_rid, spot_id,
                                   target_spot_generation, std::move (parts));
        }
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        return runtime.submit_spot_send_parts (target_node_rid, spot_id, std::move (parts));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<std::uint64_t> route_client_t::submit_spot_request_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  const std::string &router_channel_id,
  const zlink::routing_id_t &target_node_rid,
  const spot_id_t &target_spot_id,
  const std::string &packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  const channel_request_call_t::metadata_map_t &metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    if (!state || !state->runtime || state->serializers == nullptr) {
        return task_t<std::uint64_t> (result_t<std::uint64_t>::failure (
          framework_error_kind_t::protocol_error, "route client is not configured"));
    }
    runtime::messaging::message_parts_t parts;
    const auto &spot_id = target_spot_id;
    try {
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                         ? timeout
                                         : runtime.default_request_timeout ();
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             router_channel_id, packet_name, effective_timeout);
        header.metadata = metadata;
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::spot_route,
                                          dispatch_message_kind_t::request,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          spot_id,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), request_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (
          detail::result_access_t::failure<std::uint64_t> (error));
    }
    try {
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        return task_t<std::uint64_t> (
          runtime.request_to_spot_parts (target_node_rid, spot_id, std::move (parts)));
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (
          detail::result_access_t::failure<std::uint64_t> (error));
    }
    catch (const std::exception &error) {
        return task_t<std::uint64_t> (result_t<std::uint64_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
}

task_t<zlink::message_t> route_client_t::submit_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error, "route client is not configured"));
    }
    runtime::messaging::message_parts_t parts;
    const auto use_route_channel =
      detail::has_route_channel (state->runtime, router_channel_id);
    const auto mesh_requester =
      use_route_channel ? std::nullopt
                        : detail::mesh_node_requester (state->runtime,
                                                      router_channel_id);
    if (!use_route_channel && !mesh_requester) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::unavailable,
          "route channel or MeshNode '" + router_channel_id
            + "' is not registered"));
    }
    auto effective_timeout = timeout;
    try {
        if (use_route_channel) {
            detail::channel_runtime_manager_t manager (state->runtime);
            auto &runtime = manager.get_route_channel (router_channel_id);
            effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                  ? timeout
                                  : runtime.default_request_timeout ();
        } else if (effective_timeout <= std::chrono::milliseconds::zero ()) {
            effective_timeout = state->runtime->default_request_timeout;
        }
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             router_channel_id, packet_name, effective_timeout);
        header.metadata = std::move (metadata);
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::route_mesh_channel,
                                          dispatch_message_kind_t::request,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), request_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
    }
    auto source = std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
    auto output = source->task ();
    auto runtime_state = state->runtime;
    auto executor = state->executor;
    if (!executor
        || !executor->try_submit ([runtime_state, source,
                                   router_channel_id = std::move (router_channel_id),
                                   target_node_rid = std::move (target_node_rid),
                                   packet_name = std::move (packet_name), parts = std::move (parts),
                                   effective_timeout, mesh_requester,
                                   ambient_context] () mutable {
              const auto ambient_guard = detail::enter_ambient_context (ambient_context);
              try {
                  runtime::messaging::envelope_codec_t envelope;
                  result_t<runtime::messaging::message_parts_t> reply =
                    mesh_requester
                      ? (*mesh_requester) (target_node_rid, std::move (parts),
                                           effective_timeout)
                      : [&] {
                            detail::channel_runtime_manager_t manager (
                              runtime_state);
                            auto &runtime =
                              manager.get_route_channel (router_channel_id);
                            return runtime.request_reply_parts (
                              target_node_rid, std::move (parts),
                              effective_timeout);
                        } ();
                  if (!reply) {
                      source->complete (detail::propagate_failure<zlink::message_t> (reply, "route request failed"));
                      return;
                  }
                  auto reply_header = envelope.decode_header (reply.value ());
                  if (!reply_header) {
                      source->complete (result_t<zlink::message_t>::failure (
                        reply_header.error_kind (),
                        reply_header.error () ? reply_header.error ()->what ()
                                              : "route reply header decode failed"));
                      return;
                  }
                  if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                      runtime::messaging::request_failure_mapper_t failure_mapper;
                      source->complete (detail::result_access_t::failure<zlink::message_t> (
                        failure_mapper.error_header_exception (
                          reply_header.value ().error_code.value_or ("request_failed"),
                          reply_header.value ().error_message.value_or ("route request failed"),
                          "route request")));
                      return;
                  }
                  auto body = envelope.decode_body (reply.value ());
                  if (!body) {
                      source->complete (detail::propagate_failure<zlink::message_t> (body, "route reply body decode failed"));
                      return;
                  }
                  detail::message_flow_tracer_t (runtime_state->dispatch)
                    .trace (message_flow_outcome_t::reply_received, [&] {
                        return message_flow_event_t{message_flow_outcome_t::reply_received,
                                                    dispatch_error_surface_t::route_mesh_channel,
                                                    dispatch_message_kind_t::response,
                                                    packet_name,
                                                    router_channel_id,
                                                    std::nullopt,
                                                    reply_header.value ().correlation_id,
                                                    target_node_rid.to_string (),
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt};
                    });
                  source->complete (result_t<zlink::message_t>::success (body.value ()));
              }
              catch (const framework_exception_t &error) {
                  source->complete (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (const std::exception &error) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
              catch (...) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, "route request failed"));
              }
          })) {
        source->complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "route client executor is stopped"));
    }
    return output;
}

task_t<zlink::message_t> route_client_t::submit_channel_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  std::string channel_name,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata)
{
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error, "route client is not configured"));
    }
    const auto requester = detail::mesh_channel_requester (state->runtime, channel_name);
    if (!requester) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::unavailable,
          "RouteMesh channel '" + channel_name + "' is not registered"));
    }
    const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                     ? timeout
                                     : state->runtime->default_request_timeout;
    runtime::messaging::message_parts_t parts;
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             channel_name, packet_name, effective_timeout);
        header.metadata = std::move (metadata);
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::route_mesh_channel,
                                          dispatch_message_kind_t::request,
                                          packet_name,
                                          channel_name,
                                          std::nullopt,
                                          header.correlation_id,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), request_type,
                                            std::move (encode_payload), *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return task_t<zlink::message_t> (
          detail::result_access_t::failure<zlink::message_t> (error));
    }

    auto source = std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
    auto output = source->task ();
    auto runtime_state = state->runtime;
    auto executor = state->executor;
    if (!executor) {
        source->complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "route client executor is stopped"));
        return output;
    }
    try {
        executor->submit (
          [runtime_state, source, requester, channel_name = std::move (channel_name),
           packet_name = std::move (packet_name), parts = std::move (parts),
           effective_timeout, ambient_context] () mutable {
              const auto ambient_guard = detail::enter_ambient_context (ambient_context);
              try {
                  runtime::messaging::envelope_codec_t envelope;
                  auto reply = (*requester) (std::move (parts), effective_timeout);
                  if (!reply) {
                      source->complete (detail::propagate_failure<zlink::message_t> (
                        reply, "RouteMesh channel request failed"));
                      return;
                  }
                  auto reply_header = envelope.decode_header (reply.value ());
                  if (!reply_header) {
                      source->complete (result_t<zlink::message_t>::failure (
                        reply_header.error_kind (),
                        reply_header.error () ? reply_header.error ()->what ()
                                              : "RouteMesh channel reply header decode failed"));
                      return;
                  }
                  if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                      runtime::messaging::request_failure_mapper_t failure_mapper;
                      source->complete (detail::result_access_t::failure<zlink::message_t> (
                        failure_mapper.error_header_exception (
                          reply_header.value ().error_code.value_or ("request_failed"),
                          reply_header.value ().error_message.value_or (
                            "RouteMesh channel request failed"),
                          "RouteMesh channel request")));
                      return;
                  }
                  auto body = envelope.decode_body (reply.value ());
                  if (!body) {
                      source->complete (detail::propagate_failure<zlink::message_t> (
                        body, "RouteMesh channel reply body decode failed"));
                      return;
                  }
                  detail::message_flow_tracer_t (runtime_state->dispatch)
                    .trace (message_flow_outcome_t::reply_received, [&] {
                        return message_flow_event_t{
                          message_flow_outcome_t::reply_received,
                          dispatch_error_surface_t::route_mesh_channel,
                          dispatch_message_kind_t::response,
                          packet_name,
                          channel_name,
                          std::nullopt,
                          reply_header.value ().correlation_id,
                          std::nullopt,
                          std::nullopt,
                          std::nullopt,
                          std::nullopt};
                    });
                  source->complete (result_t<zlink::message_t>::success (body.value ()));
              }
              catch (const framework_exception_t &error) {
                  source->complete (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (const std::exception &error) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
              catch (...) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "RouteMesh channel request failed"));
              }
          });
    }
    catch (const std::exception &error) {
        source->complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    return output;
}

task_t<zlink::message_t> route_client_t::submit_spot_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  spot_id_t target_spot_id,
  std::uint64_t target_spot_generation,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata)
{
    const auto metrics_channel = router_channel_id;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime
        && detail::message_flow_tracer_t (state->runtime->dispatch).capture_enabled ());
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error, "route client is not configured"));
    }
    runtime::messaging::message_parts_t parts;
    auto effective_timeout = timeout;
    const auto &spot_id = target_spot_id;
    const auto use_route_channel = detail::has_route_channel (state->runtime, router_channel_id);
    const auto mesh_requester = use_route_channel
                                  ? std::nullopt
                                  : detail::spot_mesh_requester (state->runtime, router_channel_id);
    if (!use_route_channel && !mesh_requester) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::unavailable,
          "route channel or SPOT mesh '" + router_channel_id + "' is not registered"));
    }
    try {
        if (use_route_channel) {
            detail::channel_runtime_manager_t manager (state->runtime);
            auto &runtime = manager.get_route_channel (router_channel_id);
            effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                  ? timeout
                                  : runtime.default_request_timeout ();
        } else if (effective_timeout <= std::chrono::milliseconds::zero ()) {
            effective_timeout = state->runtime->default_request_timeout;
        }
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             router_channel_id, packet_name, effective_timeout);
        header.metadata = std::move (metadata);
        detail::message_flow_tracer_t (state->runtime->dispatch)
          .trace (message_flow_outcome_t::sent, [&] {
              return message_flow_event_t{message_flow_outcome_t::sent,
                                          dispatch_error_surface_t::spot_route,
                                          dispatch_message_kind_t::request,
                                          packet_name,
                                          router_channel_id,
                                          std::nullopt,
                                          header.correlation_id,
                                          target_node_rid.to_string (),
                                          spot_id,
                                          std::nullopt,
                                          std::nullopt};
          });
        parts = encode_route_payload_parts (std::move (header), request_type, encode_payload,
                                            *state->serializers);
    }
    catch (const framework_exception_t &error) {
        return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
    }
    auto source = std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
    auto output = source->task ();
    auto runtime_state = state->runtime;
    auto executor = state->executor;
    if (!executor
        || !executor->try_submit ([runtime_state, source,
                                   router_channel_id = std::move (router_channel_id),
                                   target_node_rid = std::move (target_node_rid),
                                   spot_id = std::move (spot_id),
                                   target_spot_generation,
                                   packet_name = std::move (packet_name), parts = std::move (parts),
                                   effective_timeout, mesh_requester,
                                   ambient_context] () mutable {
              const auto ambient_guard = detail::enter_ambient_context (ambient_context);
              try {
                  runtime::messaging::envelope_codec_t envelope;
                  auto reply = [&] () -> result_t<runtime::messaging::message_parts_t> {
                      if (mesh_requester) {
                          return (*mesh_requester) (
                            target_node_rid, spot_id, target_spot_generation,
                            std::move (parts), effective_timeout);
                      }
                      detail::channel_runtime_manager_t manager (runtime_state);
                      auto &runtime = manager.get_route_channel (router_channel_id);
                      return detail::is_internal_spot_route_packet (packet_name)
                               ? runtime.request_reply_parts (
                                   target_node_rid, std::move (parts), effective_timeout)
                               : runtime.request_reply_spot_parts (
                                   target_node_rid, spot_id, std::move (parts), effective_timeout);
                  } ();
                  if (!reply) {
                      if (reply.error_kind ()
                          == framework_error_kind_t::not_found) {
                          detail::dispatch_error_reporter_t (runtime_state->dispatch).report (
                            message_dispatch_error_event_t{
                              dispatch_error_surface_t::spot_route,
                              dispatch_message_kind_t::request,
                              dispatch_error_reason_t::handler_missing,
                              dispatch_error_action_t::reply_error,
                              packet_name,
                              router_channel_id,
                              std::nullopt,
                              spot_id,
                              std::nullopt,
                              target_node_rid.to_string (),
                              std::nullopt,
                              reply.error () ? std::make_exception_ptr (*reply.error ())
                                             : std::exception_ptr {}});
                      }
                      source->complete (detail::propagate_failure<zlink::message_t> (reply, "route spot request failed"));
                      return;
                  }
                  auto reply_header = envelope.decode_header (reply.value ());
                  if (!reply_header) {
                      source->complete (result_t<zlink::message_t>::failure (
                        reply_header.error_kind (),
                        reply_header.error () ? reply_header.error ()->what ()
                                              : "route spot reply header decode failed"));
                      return;
                  }
                  if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                      runtime::messaging::request_failure_mapper_t failure_mapper;
                      source->complete (detail::result_access_t::failure<zlink::message_t> (
                        failure_mapper.error_header_exception (
                          reply_header.value ().error_code.value_or ("request_failed"),
                          reply_header.value ().error_message.value_or (
                            "route spot request failed"),
                          "route spot request")));
                      return;
                  }
                  auto body = envelope.decode_body (reply.value ());
                  if (!body) {
                      source->complete (result_t<zlink::message_t>::failure (
                        body.error_kind (),
                        body.error () ? body.error ()->what ()
                                      : "route spot reply body decode failed"));
                      return;
                  }
                  detail::message_flow_tracer_t (runtime_state->dispatch)
                    .trace (message_flow_outcome_t::reply_received, [&] {
                        return message_flow_event_t{message_flow_outcome_t::reply_received,
                                                    dispatch_error_surface_t::spot_route,
                                                    dispatch_message_kind_t::response,
                                                    packet_name,
                                                    router_channel_id,
                                                    std::nullopt,
                                                    reply_header.value ().correlation_id,
                                                    target_node_rid.to_string (),
                                                    spot_id,
                                                    std::nullopt,
                                                    std::nullopt};
                    });
                  source->complete (result_t<zlink::message_t>::success (body.value ()));
              }
              catch (const framework_exception_t &error) {
                  source->complete (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (const std::exception &error) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
              catch (...) {
                  source->complete (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, "route spot request failed"));
              }
          })) {
        source->complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "route client executor is stopped"));
    }
    if (state->runtime && state->runtime->monitoring) {
        runtime::runtime_metrics_t metrics (state->runtime->monitoring);
        if (metrics.enabled ()) {
            const auto started = std::chrono::steady_clock::now ();
            metrics.updown ("zlink.channel.request.inflight", "{request}", 1,
                            {{"channel", metrics_channel}});
            detail::observe_task_completion (
              output, [metrics, started, metrics_channel] (
                        const result_t<zlink::message_t> &completed) mutable {
                  metrics.updown ("zlink.channel.request.inflight", "{request}", -1,
                                  {{"channel", metrics_channel}});
                  const auto elapsed = std::chrono::duration<double> (
                                         std::chrono::steady_clock::now () - started)
                                         .count ();
                  metrics.histogram ("zlink.channel.request.duration", "s", elapsed,
                                     {{"channel", metrics_channel}});
                  if (!completed && completed.error () != nullptr
                      && detail::boundary_state (*completed.error ())
                           == detail::boundary_error_t::timed_out) {
                      metrics.counter ("zlink.channel.request.timeouts", "{request}", 1,
                                       {{"channel", metrics_channel}});
                  }
              });
        }
    }
    return output;
}

result_t<void> route_client_t::submit_spot_id_send_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  const spot_id_t &target,
  const detail::spot_activation_intent_t &intent,
  const std::string &packet_name,
  std::type_index message_type,
  payload_encoder_t encode_payload,
  const route_send_call_t::metadata_map_t &metadata)
{
    if (!state || !state->runtime || state->runtime->spot_resolver == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::not_configured,
                                        "Spot location resolver is not configured");
    }
    if (intent.mesh_name && !intent.instance) {
        return result_t<void>::failure (
          framework_error_kind_t::not_configured,
          "in_mesh requires Instance Spot intent");
    }
    auto address = state->runtime->spot_resolver
                     ->resolve_spot_address ({}, target).result ().value ();
    if (!address && intent.instance) {
        detail::channel_runtime_state_t::instance_spot_send_t activate;
        {
            std::lock_guard lock (state->runtime->mutex);
            activate = state->runtime->instance_spot_sender;
        }
        if (!activate) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Instance Spot activation runtime is not configured");
        }
        return activate (target, intent, packet_name, message_type,
                         std::move (encode_payload), metadata);
    }
    if (!address) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "Spot route was not found");
    }
    auto submitted = submit_spot_send_erased (
      state, address->mesh_name, address->node_rid, spot_id_t (address->spot_id),
      address->spot_generation, packet_name, message_type, std::move (encode_payload), metadata);
    if (!submitted
        && (submitted.error_kind () == framework_error_kind_t::not_found
            || submitted.error_kind () == framework_error_kind_t::unavailable)) {
        state->runtime->spot_resolver->invalidate_spot_address (target);
    }
    return submitted;
}

task_t<zlink::message_t> route_client_t::submit_spot_id_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  spot_id_t target,
  detail::spot_activation_intent_t intent,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata)
{
    if (!state || !state->runtime || state->runtime->spot_resolver == nullptr) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot location resolver is not configured");
    }
    if (intent.mesh_name && !intent.instance) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "in_mesh requires Instance Spot intent");
    }
    auto address = state->runtime->spot_resolver
                     ->resolve_spot_address ({}, target).result ().value ();
    if (!address && intent.instance) {
        detail::channel_runtime_state_t::instance_spot_request_t activate;
        {
            std::lock_guard lock (state->runtime->mutex);
            activate = state->runtime->instance_spot_requester;
        }
        if (!activate) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot activation runtime is not configured");
        }
        const auto effective_timeout =
          timeout > std::chrono::milliseconds::zero ()
            ? timeout
            : state->runtime->default_request_timeout;
        co_return co_await activate (
          target, intent, std::move (packet_name), request_type,
          std::move (encode_payload), effective_timeout, std::move (metadata));
    }
    if (!address) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "Spot route was not found");
    }
    try {
        co_return co_await submit_spot_request_reply_message_erased (
          state, address->mesh_name, address->node_rid,
          spot_id_t (address->spot_id), address->spot_generation, std::move (packet_name),
          request_type, std::move (encode_payload), timeout, std::move (metadata));
    }
    catch (const framework_exception_t &error) {
        if (error.kind () == framework_error_kind_t::not_found
            || error.kind () == framework_error_kind_t::unavailable) {
            state->runtime->spot_resolver->invalidate_spot_address (target);
        }
        throw;
    }
}

zlink_builder_t::zlink_builder_t () : _state (std::make_shared<detail::zlink_builder_state_t> ())
{
}

zlink_builder_t::~zlink_builder_t () = default;

zlink_builder_t::zlink_builder_t (zlink_builder_t &&) noexcept = default;

zlink_builder_t &zlink_builder_t::operator= (zlink_builder_t &&) noexcept = default;

zlink_builder_t &zlink_builder_t::add_node (std::string node_name)
{
    _state->node_name = std::move (node_name);
    return *this;
}

zlink_builder_t &zlink_builder_t::max_pending (std::size_t count)
{
    _state->runtime->max_pending = count;
    _state->stream_runtime->max_pending = count;
    for (const auto &[_, mesh] : _state->mesh_nodes) {
        if (mesh) {
            mesh->max_pending = count;
        }
    }
    return *this;
}

zlink_builder_t &zlink_builder_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "request timeout must be greater than zero");
    }
    _state->runtime->default_request_timeout = timeout;
    return *this;
}

channel_builder_t zlink_builder_t::channel (std::string channel_name)
{
    auto state = std::make_shared<detail::channel_builder_state_t> (std::move (channel_name));
    auto [entry, _] =
      _state->runtime->channels.insert_or_assign (state->snapshot.name, state->snapshot);
    state->target = &entry->second;
    return channel_builder_t (state);
}

route_channel_builder_t zlink_builder_t::route_channel (std::string route_channel_name)
{
    if (route_channel_name.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel name is required");
    }
    if (auto found = _state->route_channels.find (route_channel_name);
        found != _state->route_channels.end ()) {
        return route_channel_builder_t (found->second);
    }
    auto state = std::make_shared<detail::route_channel_builder_state_t> (route_channel_name);
    _state->route_channels[route_channel_name] = state;
    return route_channel_builder_t (state);
}

mesh_node_builder_t zlink_builder_t::add_route_mesh (std::string mesh_name)
{
    if (mesh_name.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "MeshName is required");
    }
    if (const auto found = _state->mesh_nodes.find (mesh_name);
        found != _state->mesh_nodes.end ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "MeshName is already registered: " + mesh_name);
    }
    auto state = std::make_shared<detail::mesh_node_builder_state_t> (mesh_name);
    state->max_pending = _state->runtime->max_pending;
    state->spot_builder._state->channel_runtime = _state->runtime;
    state->spot_builder._state->dispatch = _state->runtime->dispatch;
    state->spot_builder._state->snapshot.discovery_channel_name = mesh_name;
    _state->mesh_nodes.emplace (std::move (mesh_name), state);
    return mesh_node_builder_t (std::move (state));
}

mesh_node_builder_t zlink_framework_options_t::add_route_mesh (
  std::string mesh_name)
{
    auto builder = _zlink->add_route_mesh (std::move (mesh_name));
    std::weak_ptr<detail::framework_options_state_t> options = _options;
    std::lock_guard lock (builder._state->mutex);
    builder._state->channel_name_observer =
      [options] (const std::string &channel_name) {
          if (const auto state = options.lock ()) {
              state->mesh_node_channel_names.insert (channel_name);
          }
      };
    return builder;
}

message_bus_t zlink_builder_t::message_bus () const
{
    return message_bus_t (_state->runtime);
}

request_client_t zlink_builder_t::request_client (std::string channel_name) const
{
    return request_client_t (message_bus (), std::move (channel_name));
}

publisher_t zlink_builder_t::publisher () const
{
    return publisher_t (message_bus ());
}

route_client_t zlink_builder_t::route_client (serializer_registry_t &serializers) const
{
    detail::channel_runtime_manager_t manager (_state->runtime);
    manager.initialize_route_channels (*this);
    return route_client_t (
      std::make_shared<detail::route_client_state_t> (_state->runtime, serializers), serializers);
}

} // namespace zlink::framework
