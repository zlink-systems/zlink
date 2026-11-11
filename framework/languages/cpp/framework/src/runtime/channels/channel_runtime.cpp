/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "channel_runtime.hpp"

#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include "runtime/channels/channel_outbound_exchange.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::detail
{

namespace
{

task_t<std::optional<runtime::spot_address_t>>
resolve_request_spot_address (const std::shared_ptr<channel_runtime_state_t> &state,
                              const std::shared_ptr<runtime::spot_address_resolver_t> &resolver,
                              const spot_id_t &target,
                              bool instance,
                              std::chrono::steady_clock::time_point deadline)
{
    auto resolved = resolver->resolve_spot_address ({}, target, instance);
    if (resolved.await_ready ())
        return resolved;
    const auto wait_owner = current_wait_owner ();
    auto pending_resolution =
      std::make_shared<task_t<std::optional<runtime::spot_address_t>>> (std::move (resolved));
    return state->lane.run_task ([state, pending_resolution, deadline, wait_owner] () mutable {
        using address_t = std::optional<runtime::spot_address_t>;
        state->ensure_transport_open ();
        auto completion = std::make_shared<task_completion_source_t<address_t>> ();
        auto pending = completion->task ();
        const auto operations = state->resolver_operations;
        const runtime::call_id_t id{0, state->pending_requests.next_request_seq ()};
        if (!operations->register_operation (
              id, deadline,
              [completion] (auto terminal, auto) {
                  const bool expired =
                    terminal == runtime::foundation::operation_terminal_t::timed_out;
                  completion->complete (result_t<address_t>::failure (
                    expired ? framework_error_kind_t::deadline_exceeded
                            : framework_error_kind_t::shutting_down,
                    expired ? "Spot resolution deadline elapsed"
                            : "Spot resolution wait owner stopped"));
              },
              {}, {}, {}, wait_owner))
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "channel runtime is shutting down");
        try {
            observe_task_terminal (
              *pending_resolution, [owner = std::weak_ptr (state), id,
                                    weak = std::weak_ptr (completion)] (const auto &result) {
                  // The callback may outlive the runtime and its borrowed registry lane.
                  const auto state = owner.lock ();
                  const auto source = weak.lock ();
                  if (state && source && state->resolver_operations->unregister (id))
                      source->complete (result);
              });
        }
        catch (...) {
            operations->unregister (id);
            throw;
        }
        return pending;
    });
}

bool is_internal_spot_route_packet (std::string_view packet_name)
{
    return packet_name == spot_actor_packet_route_request_t::packet_name
           || packet_name == actor_bound_session_route_request_t::packet_name;
}

std::optional<channel_runtime_state_t::spot_mesh_send_t>
spot_mesh_sender (const std::shared_ptr<channel_runtime_state_t> &state,
                  const std::string &mesh_name)
{
    const auto index =
      std::atomic_load_explicit (&state->node_transports, std::memory_order_acquire);
    const auto found = index->spot_mesh_senders.find (mesh_name);
    return found == index->spot_mesh_senders.end ()
             ? std::nullopt
             : std::optional<channel_runtime_state_t::spot_mesh_send_t> (found->second);
}

using node_transport_t = channel_runtime_state_t::node_transport_t;

node_transport_t resolve_node_transport (const std::shared_ptr<channel_runtime_state_t> &state,
                                         const std::string &mesh_name)
{
    const auto index =
      std::atomic_load_explicit (&state->node_transports, std::memory_order_acquire);
    const auto found = index->nodes.find (mesh_name);
    return found == index->nodes.end () ? node_transport_t{} : found->second.transport;
}

template <typename TClientServerTransports, typename TRouteMeshTransports>
auto resolve_route_client_channel_transport (
  const std::shared_ptr<channel_runtime_state_t> &state,
  const std::string &channel_name,
  const TClientServerTransports &client_server_transports,
  const TRouteMeshTransports &route_mesh_transports)
{
    using route_client_channel_transport_t =
      std::variant<typename TClientServerTransports::mapped_type,
                   typename TRouteMeshTransports::mapped_type>;
    return state->lane
      .run_checked ([&] () -> std::optional<route_client_channel_transport_t> {
          const auto client_server_transport = client_server_transports.find (channel_name);
          if (client_server_transport != client_server_transports.end ()) {
              return route_client_channel_transport_t (std::in_place_index<0>,
                                                       client_server_transport->second);
          }
          const auto route_mesh_transport = route_mesh_transports.find (channel_name);
          if (route_mesh_transport != route_mesh_transports.end ()) {
              return route_client_channel_transport_t (std::in_place_index<1>,
                                                       route_mesh_transport->second);
          }
          return std::nullopt;
      })
      .get ();
}

std::optional<channel_runtime_state_t::spot_mesh_request_t>
spot_mesh_requester (const std::shared_ptr<channel_runtime_state_t> &state,
                     const std::string &mesh_name)
{
    return state->lane
      .run_checked ([&] {
          const auto found = state->spot_mesh_requesters.find (mesh_name);
          return found == state->spot_mesh_requesters.end ()
                   ? std::nullopt
                   : std::optional<channel_runtime_state_t::spot_mesh_request_t> (found->second);
      })
      .get ();
}

bool has_route_channel (const std::shared_ptr<channel_runtime_state_t> &state,
                        const std::string &channel_name)
{
    const auto index =
      std::atomic_load_explicit (&state->node_transports, std::memory_order_acquire);
    const auto found = index->nodes.find (channel_name);
    return found != index->nodes.end ()
           && std::holds_alternative<std::shared_ptr<route_channel_runtime_t>> (
             found->second.transport);
}

} // namespace

route_client_state_t::route_client_state_t (std::shared_ptr<channel_runtime_state_t> runtime,
                                            serializer_registry_t &serializers) :
    runtime (std::move (runtime)), serializers (&serializers)
{
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

std::optional<result_t<zlink::message_t>>
request_capability_failure (const channel_runtime_state_t &state, const std::string &channel_name)
{
    if (is_enabled (server_capability (state, channel_name))) {
        return std::nullopt;
    }
    return result_t<zlink::message_t>::failure (framework_error_kind_t::unavailable,
                                                "channel server capability is not enabled");
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
    /* Reply validation only inspects the kind — flow fields are unused. */
    auto header = envelope.decode_header (parts, false);
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
    void decrement_pending () noexcept
    {
        if (_state.pending > 0) {
            --_state.pending;
        }
    }

    channel_runtime_state_t &_state;
};

void channel_runtime_state_t::ensure_transport_open () const
{
    if (shutdown)
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                               "channel runtime is shutting down");
    if (closed)
        throw detail::make_boundary_exception (detail::boundary_error_t::closed,
                                               "channel runtime is closed");
}

void channel_runtime_state_t::admit_outbound () const
{
    if (!runtime_admission)
        return;
    const auto [draining, started] = runtime_admission ();
    if (draining)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Framework runtime is shutting down");
    if (!started)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Framework runtime has not started");
}

void route_client_state_t::admit_outbound (const std::shared_ptr<route_client_state_t> &state)
{
    if (state && state->runtime)
        channel_runtime_t (state->runtime).admit_outbound ();
}

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

void drain_zlink_builder_state_runtime (zlink_builder_state_t &state) noexcept
{
    if (state.runtime
        && state.runtime->lane
             .run_checked ([&] {
                 return !state.runtime->native_clients.empty ()
                        || !state.runtime->native_publishers.empty ();
             })
             .get ()) {
        auto failures = runtime_failures_for (state.runtime->monitoring);
        if (!failures->capture ([&] { close_native_channel_transports (state.runtime); }))
            return;
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

std::shared_ptr<zlink::context_t>
zlink_builder_access_t::shared_core_context (const zlink_builder_t &builder)
{
    return builder._state->core_context;
}

void zlink_builder_access_t::bind_shared_core_context (zlink_builder_t &builder,
                                                       std::shared_ptr<zlink::context_t> context)
{
    if (!context) {
        throw std::invalid_argument ("shared Core context is required");
    }
    if (builder._state->core_context) {
        throw std::logic_error ("shared Core context is already configured");
    }
    builder._state->core_context = std::move (context);
}

void drain_zlink_builder_runtime (zlink_builder_t &builder) noexcept
{
    if (!builder._state) {
        return;
    }
    drain_zlink_builder_state_runtime (*builder._state);
}

void bind_zlink_monitoring (zlink_builder_t &builder,
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

result_t<zlink::message_t>
channel_runtime_t::dispatch_request (std::string channel_name,
                                     std::string topic,
                                     std::string packet_name,
                                     service_provider_t &services,
                                     serializer_registry_t &serializers,
                                     const handler_registry_t &handlers,
                                     const zlink::message_t &message,
                                     const detail::inbound_message_context_t &inbound) const
{
    if (auto failure = request_capability_failure (*_state, channel_name)) {
        return std::move (*failure);
    }
    return handlers.invoke (channel_name, topic, packet_name, services, serializers, message,
                            inbound);
}

task_t<result_t<zlink::message_t>>
channel_runtime_t::dispatch_request_async (std::string channel_name,
                                           std::string topic,
                                           std::string packet_name,
                                           service_provider_t &services,
                                           serializer_registry_t &serializers,
                                           const handler_registry_t &handlers,
                                           zlink::message_t message,
                                           detail::inbound_message_context_t inbound) const
{
    if (auto failure = request_capability_failure (*_state, channel_name)) {
        co_return std::move (*failure);
    }
    try {
        auto reply = co_await handlers.invoke_async (channel_name, topic, packet_name, services,
                                                     serializers, message, inbound);
        co_return result_t<zlink::message_t>::success (std::move (reply));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
}

task_t<zlink::message_t>
channel_runtime_t::dispatch_request_in_handler (std::string channel_name,
                                                std::string topic,
                                                std::string packet_name,
                                                service_provider_t &services,
                                                serializer_registry_t &serializers,
                                                const handler_registry_t &handlers,
                                                zlink::message_t message,
                                                detail::inbound_message_context_t inbound) const
{
    if (auto failure = request_capability_failure (*_state, channel_name))
        return task_t<zlink::message_t> (std::move (*failure));
    return handlers.invoke_on_handler_executor (std::move (channel_name), std::move (topic),
                                                std::move (packet_name), services, serializers,
                                                std::move (message), std::move (inbound));
}

task_t<result_t<zlink::message_t>> channel_runtime_t::dispatch_request_on_handler_executor (
  std::string channel_name,
  std::string topic,
  std::string packet_name,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  zlink::message_t message,
  detail::inbound_message_context_t inbound) const
{
    try {
        auto reply = co_await dispatch_request_in_handler (
          std::move (channel_name), std::move (topic), std::move (packet_name), services,
          serializers, handlers, std::move (message), std::move (inbound));
        co_return result_t<zlink::message_t>::success (std::move (reply));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
}

result_t<void>
channel_runtime_t::dispatch_send (std::string channel_name,
                                  std::string topic,
                                  std::string packet_name,
                                  service_provider_t &services,
                                  serializer_registry_t &serializers,
                                  const handler_registry_t &handlers,
                                  const zlink::message_t &message,
                                  const detail::inbound_message_context_t &inbound) const
{
    auto result =
      handlers.invoke (channel_name, topic, packet_name, services, serializers, message, inbound);
    if (!result) {
        return result_t<void>::failure (result.error_kind (), result.error ()
                                                                ? result.error ()->what ()
                                                                : "channel dispatch failed");
    }
    return result_t<void>::success ();
}

task_t<void>
channel_runtime_t::dispatch_send_async (std::string channel_name,
                                        std::string topic,
                                        std::string packet_name,
                                        service_provider_t &services,
                                        serializer_registry_t &serializers,
                                        const handler_registry_t &handlers,
                                        zlink::message_t message,
                                        detail::inbound_message_context_t inbound) const
{
    static_cast<void> (co_await handlers.invoke_async (channel_name, topic, packet_name, services,
                                                       serializers, message, inbound));
    co_return;
}

task_t<zlink::message_t>
channel_runtime_t::dispatch_send_in_handler (std::string channel_name,
                                             std::string topic,
                                             std::string packet_name,
                                             service_provider_t &services,
                                             serializer_registry_t &serializers,
                                             const handler_registry_t &handlers,
                                             zlink::message_t message,
                                             detail::inbound_message_context_t inbound) const
{
    return handlers.invoke_on_handler_executor (std::move (channel_name), std::move (topic),
                                                std::move (packet_name), services, serializers,
                                                std::move (message), std::move (inbound));
}

task_t<void> channel_runtime_t::dispatch_send_on_handler_executor (
  std::string channel_name,
  std::string topic,
  std::string packet_name,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  zlink::message_t message,
  detail::inbound_message_context_t inbound) const
{
    (void) co_await dispatch_send_in_handler (std::move (channel_name), std::move (topic),
                                              std::move (packet_name), services, serializers,
                                              handlers, std::move (message), std::move (inbound));
    co_return;
}

result_t<std::uint64_t> channel_runtime_t::reserve_outbound_request (std::string channel_name)
{
    try {
        _state->admit_outbound ();
    }
    catch (...) {
        return detail::current_exception_result<std::uint64_t> ();
    }
    return _state->lane
      .run_checked ([&] {
          try {
              _state->ensure_transport_open ();
          }
          catch (...) {
              return detail::current_exception_result<std::uint64_t> ();
          }
          const auto *client = client_capability (*_state, channel_name);
          if (client == nullptr || !client->enabled) {
              return result_t<std::uint64_t>::failure (
                framework_error_kind_t::not_configured,
                "ClientServer Client role is not registered for this channel");
          }
          if (!has_connection (client)) {
              return detail::boundary_failure<std::uint64_t> (
                detail::boundary_error_t::disconnected, "channel client is not connected");
          }
          return outbound_request_controller_t (*_state).reserve_request (std::move (channel_name));
      })
      .get ();
}

result_t<void> channel_runtime_t::complete_outbound_reply (std::uint64_t request_seq)
{
    return _state->lane
      .run_checked (
        [&] { return outbound_request_controller_t (*_state).complete_request (request_seq); })
      .get ();
}

result_t<void> channel_runtime_t::cancel_outbound_request (std::uint64_t request_seq)
{
    return _state->lane
      .run_checked (
        [&] { return outbound_request_controller_t (*_state).cancel_request (request_seq); })
      .get ();
}

void channel_runtime_t::close ()
{
    close_native_channel_transports (_state);
    _state->lane.run_checked ([&] { _state->closed = true; }).get ();
    drain ();
}

void channel_runtime_t::shutdown ()
{
    std::vector<std::shared_ptr<route_channel_runtime_t>> route_channels;
    _state->lane
      .run_checked ([&] {
          _state->shutdown = true;
          for (auto &[_, route_channel] : _state->route_channels) {
              if (route_channel) {
                  route_channels.push_back (route_channel);
              }
          }
      })
      .get ();
    _state->resolver_operations->shutdown ();
    for (auto &route_channel : route_channels) {
        route_channel->stop ();
    }
    close_native_channel_transports (_state);
    drain ();
}

std::size_t channel_runtime_t::pending_count () const noexcept
{
    return _state->lane.run_checked ([&] { return _state->pending; }).get ();
}

void channel_runtime_t::bind_serializers (serializer_registry_t &serializers) noexcept
{
    _state->serializers = &serializers;
}

void channel_runtime_t::bind_listener_statuses (
  std::shared_ptr<runtime::listener_status_registry_t> statuses) noexcept
{
    _state->lane.run_checked ([&] { _state->listener_statuses = std::move (statuses); }).get ();
}

void channel_runtime_t::bind_core_context (std::shared_ptr<zlink::context_t> context)
{
    if (!context) {
        throw std::invalid_argument ("shared Core context is required");
    }
    _state->lane
      .run_checked ([&] {
          if (_state->core_context && _state->core_context != context) {
              throw std::logic_error ("shared Core context is already configured");
          }
          _state->core_context = std::move (context);
      })
      .get ();
}

std::shared_ptr<zlink::context_t> channel_runtime_t::core_context () const
{
    return _state->lane.run_checked ([&] { return _state->core_context; }).get ();
}

void channel_runtime_t::bind_fanout_advertise_hosts (
  std::map<std::string, std::string> hosts) noexcept
{
    _state->lane.run_checked ([&] { _state->fanout_publisher_advertise_hosts = std::move (hosts); })
      .get ();
}

void channel_runtime_t::initialize_manual_channel_publishers ()
{
    detail::initialize_manual_channel_publishers (_state);
}

void channel_runtime_t::close_manual_channel_publishers ()
{
    detail::close_manual_channel_publishers (_state);
}

void channel_runtime_t::bind_spot_mesh_transport (
  std::string mesh_name,
  channel_runtime_state_t::spot_mesh_send_t send,
  channel_runtime_state_t::spot_mesh_request_t request)
{
    _state->lane
      .run_checked ([&] {
          _state->spot_mesh_requesters.insert_or_assign (mesh_name, std::move (request));
          _state->publish_node_transports ([&] (auto &index) {
              // Publish after requester preparation; a CAS retry copies the same sender.
              index.spot_mesh_senders.insert_or_assign (mesh_name, send);
          });
      })
      .get ();
}

void channel_runtime_t::bind_spot_address_resolver (
  std::shared_ptr<runtime::spot_address_resolver_t> resolver) noexcept
{
    _state->lane.run_checked ([&] { _state->spot_resolver = std::move (resolver); }).get ();
}

void channel_runtime_t::bind_instance_spot_activator (
  channel_runtime_state_t::instance_spot_send_t send,
  channel_runtime_state_t::instance_spot_request_t request)
{
    _state->lane
      .run_checked ([&] {
          _state->instance_spot_sender = std::move (send);
          _state->instance_spot_requester = std::move (request);
      })
      .get ();
}

void channel_runtime_t::bind_mesh_node_transport (
  std::string mesh_name,
  channel_runtime_state_t::mesh_node_send_t send,
  channel_runtime_state_t::mesh_node_request_t request)
{
    _state->lane
      .run_checked ([&] {
          auto transport = std::make_shared<const channel_runtime_state_t::mesh_node_transport_t> (
            channel_runtime_state_t::mesh_node_transport_t{std::move (send), std::move (request)});
          _state->publish_node_transports ([&] (auto &index) {
              // A publication can retry, so the change copies its inputs.
              const auto found = index.nodes.find (mesh_name);
              if (found == index.nodes.end ()
                  || !std::holds_alternative<std::shared_ptr<route_channel_runtime_t>> (
                    found->second.transport))
                  index.nodes.insert_or_assign (
                    mesh_name,
                    channel_runtime_state_t::node_transport_index_t::entry_t{
                      transport, prepare_request_channel (mesh_name),
                      &runtime::messaging::client_call_codec_t::encode_service_payload_parts});
          });
      })
      .get ();
}

void channel_runtime_t::bind_mesh_channel_transport (
  std::string channel_name,
  channel_runtime_state_t::mesh_channel_send_t send,
  channel_runtime_state_t::mesh_channel_request_t request)
{
    _state->lane
      .run_checked ([&] {
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
      })
      .get ();
}

void channel_runtime_t::bind_client_server_transport (
  std::string channel_name,
  channel_runtime_state_t::client_server_send_t send,
  channel_runtime_state_t::client_server_request_t request)
{
    _state->lane
      .run_checked ([&] {
          if (_state->mesh_channel_senders.contains (channel_name)) {
              throw framework_exception_t (
                framework_error_kind_t::protocol_error,
                "ChannelName is registered by both ClientServer and RouteMesh: " + channel_name);
          }
          if (_state->client_server_senders.contains (channel_name)) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "ClientServer ChannelName is registered more than once: "
                                             + channel_name);
          }
          _state->client_server_senders.emplace (channel_name, std::move (send));
          _state->client_server_requesters.emplace (std::move (channel_name), std::move (request));
      })
      .get ();
}

void channel_runtime_t::unbind_client_server_transport (const std::string &channel_name) noexcept
{
    _state->lane
      .run_checked ([&] {
          _state->client_server_senders.erase (channel_name);
          _state->client_server_requesters.erase (channel_name);
      })
      .get ();
}

void channel_runtime_t::bind_fanout_transport (std::string channel_name,
                                               channel_runtime_state_t::fanout_publish_t publish)
{
    _state->lane
      .run_checked ([&] {
          if (_state->fanout_publishers.contains (channel_name)) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "fanout ChannelName is registered more than once: "
                                             + channel_name);
          }
          _state->fanout_publishers.emplace (std::move (channel_name), std::move (publish));
      })
      .get ();
}

void channel_runtime_t::unbind_fanout_transport (const std::string &channel_name) noexcept
{
    _state->lane.run_checked ([&] { _state->fanout_publishers.erase (channel_name); }).get ();
}

dispatch_options_t channel_runtime_t::dispatch_options () const
{
    return _state->dispatch;
}

void channel_runtime_t::mark_auto_connect_active ()
{
    _state->lane.run_checked ([&] { _state->auto_connect_active = true; }).get ();
}

bool channel_runtime_t::auto_connect_active () const
{
    return _state->lane.run_checked ([&] { return _state->auto_connect_active; }).get ();
}

void channel_runtime_t::drain () noexcept
{
    _state->lane.run_checked ([&] { outbound_request_controller_t (*_state).drain (); }).get ();
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
      .publish_socket (socket_event_payload_t{channel_name, event});
}

void channel_runtime_t::set_server_weight (const std::string &channel_name, int value)
{
    if (value < 0 || value > 10000)
        throw std::invalid_argument ("service weight must be in range 0..10000");
    _state->lane
      .run_checked ([&] {
          auto overrides =
            std::make_shared<std::map<std::string, int>> (*std::atomic_load_explicit (
              &_state->server_peer_weight_overrides, std::memory_order_acquire));
          overrides->insert_or_assign (channel_name, value);
          std::atomic_store_explicit (
            &_state->server_peer_weight_overrides,
            std::shared_ptr<const std::map<std::string, int>> (std::move (overrides)),
            std::memory_order_release);
          if (_state->server_options_changed)
              _state->server_options_changed ();
      })
      .get ();
}

void channel_runtime_t::bind_server_options_changed (std::function<void ()> notify)
{
    _state->lane
      .run_checked ([&] {
          _state->server_options_changed = std::move (notify);
          if (_state->server_options_changed)
              _state->server_options_changed ();
      })
      .get ();
}

std::optional<int>
channel_runtime_t::server_peer_weight_override (const std::string &channel_name) const
{
    const auto overrides =
      std::atomic_load_explicit (&_state->server_peer_weight_overrides, std::memory_order_acquire);
    const auto found = overrides->find (channel_name);
    return found == overrides->end () ? std::optional<int>{} : std::optional<int>{found->second};
}

channel_runtime_t channel_runtime_t::from (const message_bus_t &bus)
{
    return channel_runtime_t (bus._state);
}

const std::string &client_server_bind_endpoint (const channel_capability_snapshot_t &server)
{
    if (server.bind_endpoints.size () != 1) {
        throw std::invalid_argument ("ClientServer server requires one bind endpoint");
    }
    return server.bind_endpoints.front ();
}

} // namespace zlink::framework::detail

namespace zlink::framework
{


namespace
{

void invalidate_stale_spot_route (runtime::spot_address_resolver_t &resolver,
                                  std::string_view target,
                                  const framework_exception_t *error)
{
    // Routing §2.2: only a stale route result invalidates the next call's cache.
    if (error
        && (error->kind () == framework_error_kind_t::not_found
            || error->kind () == framework_error_kind_t::unavailable
            || detail::failure_code (*error)
                 == static_cast<std::uint32_t> (
                   runtime::protocol::framework_error_code::spotGenerationStale))
        && detail::error_origin (*error) != detail::error_origin_t::application)
        resolver.invalidate_spot_address (target);
}

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
        throw std::invalid_argument ("service weight must be in range 0..10000");
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.service_weight = value;
    return *this;
}

capability_builder_t &capability_builder_t::set_no_drop (bool no_drop)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.no_drop = no_drop;
    return *this;
}

capability_builder_t &capability_builder_t::set_send_timeout (std::chrono::milliseconds timeout)
{
    auto &snapshot = capability_snapshot (*_state);
    snapshot.enabled = true;
    snapshot.send_timeout = timeout;
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

capability_builder_t channel_builder_t::enable_publisher (bool discovery)
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::publisher));
    detail::select_capability (*_state, channel_capability_t::publisher).discovery = discovery;
    return builder;
}

capability_builder_t channel_builder_t::enable_subscriber ()
{
    auto builder =
      enable_capability (detail::select_capability (*_state, channel_capability_t::subscriber));
    detail::select_capability (*_state, channel_capability_t::subscriber).discovery = true;
    return builder;
}

channel_builder_t &channel_builder_t::subscriber_subscriptions (std::vector<std::string> topics)
{
    detail::select_capability (*_state, channel_capability_t::subscriber).subscription_topics =
      std::move (topics);
    return *this;
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

void detail::connect_route_channel_peer (route_channel_builder_t &builder,
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

message_bus_t::message_bus_t () :
    message_bus_t (std::make_shared<detail::channel_runtime_state_t> ())
{
}

message_bus_t::message_bus_t (std::shared_ptr<detail::channel_runtime_state_t> state) :
    _state (std::move (state))
{
    _preflight = [state = _state] {
        try {
            state->admit_outbound ();
            return result_t<void>::success ();
        }
        catch (...) {
            return detail::current_exception_result<void> ();
        }
    };
}

message_bus_t::message_bus_t (std::shared_ptr<detail::channel_runtime_state_t> state,
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
    return _state->lane
      .run_checked ([&] {
          const auto found = _state->channels.find (channel_name);
          if (found != _state->channels.end () && found->second.default_request_timeout) {
              return *found->second.default_request_timeout;
          }
          return _state->default_request_timeout;
      })
      .get ();
}

serializer_registry_t *message_bus_t::serializers () const noexcept
{
    return _state ? _state->serializers : nullptr;
}

request_task_t<zlink::message_t>
message_bus_t::submit_request_message_async (std::string channel_name,
                                             std::string packet_name,
                                             std::type_index request_type,
                                             payload_encoder_t encode_payload,
                                             std::chrono::milliseconds timeout,
                                             channel_request_call_t::metadata_map_t metadata)
{
    return detail::channel_outbound_exchange_t (_state).submit_request (
      std::move (channel_name), std::move (packet_name), request_type, std::move (encode_payload),
      timeout, metadata);
}

task_t<void> message_bus_t::submit_send (std::string channel_name,
                                         std::string packet_name,
                                         std::type_index message_type,
                                         payload_encoder_t encode_payload,
                                         const send_call_t::metadata_map_t &metadata)
{
    return detail::channel_outbound_exchange_t (_state).submit_send (
      std::move (channel_name), std::move (packet_name), message_type, std::move (encode_payload),
      metadata);
}

task_t<void> message_bus_t::submit_publish (std::string channel_name,
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
    detail::channel_runtime_t (_state).set_server_weight (_channel_name,
                                                          static_cast<int> (value.value ()));
    return *this;
}

channel_server_socket_runtime_options_t &channel_server_socket_runtime_options_t::weight (int value)
{
    detail::channel_runtime_t (_state).set_server_weight (_channel_name, value);
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

route_send_call_t &route_send_call_t::metadata (std::string key, std::string value)
{
    _metadata[std::move (key)] = std::move (value);
    return *this;
}

task_t<void> route_send_call_t::async ()
{
    if (!_submit) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "route send call is not bound to a route client"));
    }
    const auto submission = _submit;
    if (submission->once.claimed ()) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "route send call has already been submitted"));
    }
    try {
        detail::route_client_state_t::admit_outbound (_state);
    }
    catch (...) {
        return task_t<void> (detail::current_exception_result<void> ());
    }
    if (!submission->once.try_claim ()) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "route send call has already been submitted"));
    }
    // Copies share the callable and its single-use decision, without a second allocation.
    return submission->submit (_packet_name, _metadata);
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

task_t<void>
route_client_t::submit_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                    const std::string &router_channel_id,
                                    const zlink::routing_id_t &target_node_rid,
                                    const std::string &packet_name,
                                    std::type_index message_type,
                                    payload_encoder_t encode_payload,
                                    const route_send_call_t::metadata_map_t &metadata)
{
    try {
        detail::route_client_state_t::admit_outbound (state);
        auto submit_flow = runtime::flow_context_t::enter_current_or_create (
          flow_origin_t::application,
          state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                                  : message_flow_log_mode_t::off);
        if (!state || !state->runtime || state->serializers == nullptr) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "route client is not configured");
        }
        const auto transport = detail::resolve_node_transport (state->runtime, router_channel_id);
        if (std::holds_alternative<std::monostate> (transport)) {
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "route channel or SPOT mesh '" + router_channel_id
                                           + "' is not registered");
        }
        const auto finish = [transport] (const result_t<result_t<void>> &result) {
            // The selected callable remains owned through its physical terminal.
            (void) transport;
            try {
                result.value ().value ();
                return result_t<void>::success ();
            }
            catch (const framework_exception_t &) {
                throw;
            }
            catch (const std::exception &error) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             error.what ());
            }
        };
        // Encoding finishes before submission, so the encoder remains borrowed.
        try {
            runtime::messaging::client_call_codec_t codec;
            auto parts = codec.encode_node_send_parts (
              router_channel_id, packet_name, target_node_rid, metadata, state->runtime->dispatch,
              *state->serializers, encode_payload);
            if (const auto mesh =
                  std::get_if<detail::channel_runtime_state_t::mesh_node_transport_ptr_t> (
                    &transport)) {
                auto submitted = (*mesh)->send (target_node_rid, std::move (parts));
                return detail::project_task_terminal<void> (submitted, finish);
            }
            auto &runtime = *std::get<std::shared_ptr<detail::route_channel_runtime_t>> (transport);
            runtime.submit_send_parts (target_node_rid, std::move (parts)).value ();
            return task_t<void> (result_t<void>::success ());
        }
        catch (...) {
            return task_t<void> (finish (detail::current_exception_result<result_t<void>> ()));
        }
    }
    catch (...) {
        return task_t<void> (detail::current_exception_result<void> ());
    }
}

task_t<void>
route_client_t::submit_channel_send_erased (std::shared_ptr<detail::route_client_state_t> state,
                                            std::string channel_name,
                                            std::string packet_name,
                                            std::type_index message_type,
                                            payload_encoder_t encode_payload,
                                            const route_send_call_t::metadata_map_t &metadata)
{
    detail::route_client_state_t::admit_outbound (state);

    // The call can be destroyed while the sender suspends. Own the values used
    // by the sent trace after completion; metadata and the encoder are consumed before it.
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
    if (!state || !state->runtime || state->serializers == nullptr) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route client is not configured");
    }
    const auto channel_transport = detail::resolve_route_client_channel_transport (
      state->runtime, channel_name, state->runtime->client_server_senders,
      state->runtime->mesh_channel_senders);
    if (!channel_transport) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "ChannelName '" + channel_name + "' is not registered");
    }
    // The transport boundary: this task is the public route send result. Both paths
    // run the encoder before the first suspension, so callers may pass a borrowed encoder.
    try {
        if (const auto *client_server = std::get_if<0> (&*channel_transport)) {
            auto serialized = encode_payload (*state->serializers);
            co_await (*client_server) (packet_name, std::move (serialized.content_type),
                                       detail::encoded_payload_to_raw (serialized.payload),
                                       metadata);
            if (detail::message_flow_tracer_t (state->runtime->dispatch)
                  .enabled_for (message_flow_outcome_t::sent)) {
                detail::message_flow_tracer_t (state->runtime->dispatch)
                  .trace (message_flow_outcome_t::sent, [&] {
                      return message_flow_event_t{.outcome = message_flow_outcome_t::sent,
                                                  .surface = dispatch_error_surface_t::channel,
                                                  .message_kind = dispatch_message_kind_t::send,
                                                  .packet_name = packet_name,
                                                  .channel_name = channel_name,
                                                  .channel_route_kind =
                                                    std::string ("client_server")};
                  });
            }
            co_return;
        }
        const auto &sender = std::get<1> (*channel_transport);
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             channel_name, packet_name);
        header.metadata = metadata;
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
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
        }
        auto parts =
          codec.encode_payload_parts (std::move (header), encode_payload (*state->serializers));
        (co_await sender (std::move (parts))).value ();
    }
    catch (const framework_exception_t &) {
        throw;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
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
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
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
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
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
        }
        parts =
          codec.encode_payload_parts (std::move (header), encode_payload (*state->serializers));
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (detail::result_access_t::failure<std::uint64_t> (error));
    }
    try {
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        return task_t<std::uint64_t> (
          runtime.submit_request_parts (target_node_rid, std::move (parts)));
    }
    catch (const framework_exception_t &error) {
        return task_t<std::uint64_t> (detail::result_access_t::failure<std::uint64_t> (error));
    }
    catch (const std::exception &error) {
        return task_t<std::uint64_t> (result_t<std::uint64_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
}

task_t<result_t<void>>
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
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
    if (!state || !state->runtime || state->serializers == nullptr) {
        co_return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                           "route client is not configured");
    }
    runtime::messaging::message_parts_t parts;
    const auto &spot_id = target_spot_id;
    const auto use_route_channel = detail::has_route_channel (state->runtime, router_channel_id);
    const auto mesh_sender = use_route_channel
                               ? std::nullopt
                               : detail::spot_mesh_sender (state->runtime, router_channel_id);
    if (!use_route_channel && !mesh_sender) {
        co_return result_t<void>::failure (framework_error_kind_t::unavailable,
                                           "route channel or SPOT mesh '" + router_channel_id
                                             + "' is not registered");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::command,
                                             router_channel_id, packet_name);
        header.metadata = metadata;
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
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
        }
        parts =
          codec.encode_payload_parts (std::move (header), encode_payload (*state->serializers));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<void> (error);
    }
    try {
        if (mesh_sender) {
            co_return co_await (*mesh_sender) (target_node_rid, spot_id, target_spot_generation,
                                               std::move (parts));
        }
        detail::channel_runtime_manager_t manager (state->runtime);
        auto &runtime = manager.get_route_channel (router_channel_id);
        co_await runtime.submit_spot_send_parts (target_node_rid, spot_id, std::move (parts));
        co_return result_t<void>::success ();
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

void route_client_t::submit_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state_ref,
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  std::string packet_name,
  node_payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata,
  detail::request_receiver_t<encoded_payload_t> receiver)
{
    detail::route_client_state_t::admit_outbound (state_ref);

    const auto request_started = std::chrono::steady_clock::now ();
    // Retain the runtime through the physical reply conversion.
    const auto state = state_ref;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route client is not configured");
    }
    runtime::messaging::message_parts_t parts;
    const auto index =
      std::atomic_load_explicit (&state->runtime->node_transports, std::memory_order_acquire);
    const auto endpoint = index->nodes.find (router_channel_id);
    if (endpoint == index->nodes.end ())
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "route channel or MeshNode '" + router_channel_id
                                       + "' is not registered");
    const auto &transport = endpoint->second.transport;
    const auto route = std::get_if<std::shared_ptr<detail::route_channel_runtime_t>> (&transport);
    const auto mesh =
      std::get_if<detail::channel_runtime_state_t::mesh_node_transport_ptr_t> (&transport);
    using reply_parts_t = runtime::messaging::message_parts_t;
    using node_reply_t = result_t<reply_parts_t>;
    struct request_decoder_t
    {
        std::shared_ptr<detail::route_client_state_t> state;
        detail::node_transport_t transport;
        std::shared_ptr<const zlink::routing_id_t> target;
        std::string packet_name;
        std::string router_channel_id;

        result_t<encoded_payload_t> decode (const result_t<reply_parts_t> &reply)
        {
            // Retain the selected callable and its borrowed target until terminal.
            (void) transport;
            try {
                runtime::messaging::envelope_codec_t envelope;
                if (!reply)
                    return detail::propagate_failure<encoded_payload_t> (reply,
                                                                         "route request failed");
                auto reply_header = envelope.decode_header (reply.value (), false);
                if (!reply_header) {
                    return result_t<encoded_payload_t>::failure (
                      reply_header.error_kind (), reply_header.error ()
                                                    ? reply_header.error ()->what ()
                                                    : "route reply header decode failed");
                }
                if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                    runtime::messaging::request_failure_mapper_t failure_mapper;
                    return detail::result_access_t::failure<encoded_payload_t> (
                      failure_mapper.error_header_exception (
                        reply_header.value ().error_code.value_or ("request_failed"),
                        reply_header.value ().error_message.value_or ("route request failed"),
                        "route request"));
                }
                auto body = envelope.decode_body_view (reply.value ());
                if (!body)
                    return detail::propagate_failure<encoded_payload_t> (
                      body, "route reply body decode failed");
                if (detail::message_flow_tracer_t (state->runtime->dispatch)
                      .enabled_for (message_flow_outcome_t::reply_received)) {
                    detail::message_flow_tracer_t (state->runtime->dispatch)
                      .trace (message_flow_outcome_t::reply_received, [&] {
                          return message_flow_event_t{message_flow_outcome_t::reply_received,
                                                      dispatch_error_surface_t::route_mesh_channel,
                                                      dispatch_message_kind_t::response,
                                                      packet_name,
                                                      router_channel_id,
                                                      std::nullopt,
                                                      reply_header.value ().correlation_id,
                                                      target->to_string (),
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt};
                      });
                }
                return result_t<encoded_payload_t>::success (std::move (body.value ()));
            }
            catch (const framework_exception_t &error) {
                return detail::result_access_t::failure<encoded_payload_t> (error);
            }
            catch (const std::exception &error) {
                return result_t<encoded_payload_t>::failure (
                  framework_error_kind_t::internal_failure, error.what ());
            }
        }

        result_t<encoded_payload_t> operator() (const result_t<reply_parts_t> &reply)
        {
            return decode (reply);
        }
        result_t<encoded_payload_t> operator() (const result_t<node_reply_t> &reply)
        {
            return decode (reply.value ());
        }
    };
    auto target_owner = receiver.complete.retain (std::move (target_node_rid));
    auto target_view = receiver.complete.has_request_storage ()
                         ? std::shared_ptr<const zlink::routing_id_t> (
                             std::shared_ptr<const zlink::routing_id_t>{}, target_owner.get ())
                         : std::shared_ptr<const zlink::routing_id_t> (target_owner);
    request_decoder_t decode{state, transport, std::move (target_view), std::move (packet_name),
                             std::move (router_channel_id)};

    auto effective_timeout = timeout;
    if (route) {
        auto &runtime = **route;
        effective_timeout = timeout > std::chrono::milliseconds::zero ()
                              ? timeout
                              : runtime.default_request_timeout ();
    } else if (effective_timeout <= std::chrono::milliseconds::zero ()) {
        effective_timeout = state->runtime->default_request_timeout;
    }
    const auto deadline = request_started + effective_timeout;
    {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (
          runtime::messaging::message_kind_t::request, decode.router_channel_id, decode.packet_name,
          runtime::messaging::remaining_request_timeout (deadline), std::nullopt, std::nullopt, {});
        header.metadata = std::move (metadata);
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
            detail::message_flow_tracer_t (state->runtime->dispatch)
              .trace (message_flow_outcome_t::sent, [&] {
                  return message_flow_event_t{message_flow_outcome_t::sent,
                                              dispatch_error_surface_t::route_mesh_channel,
                                              dispatch_message_kind_t::request,
                                              decode.packet_name,
                                              decode.router_channel_id,
                                              std::nullopt,
                                              header.correlation_id,
                                              decode.target->to_string (),
                                              std::nullopt,
                                              std::nullopt,
                                              std::nullopt};
              });
        }
        auto [payload, type_plan] = encode_payload (*state->serializers);
        parts = (codec.*endpoint->second.encode_request) (
          std::move (header), std::move (payload), std::move (type_plan),
          std::shared_ptr<const detail::prepared_header_fragment_t> (index,
                                                                     &endpoint->second.channel));
    }
    const auto ambient_guard = detail::enter_ambient_context (ambient_context);
    try {
        if (mesh) {
            const auto remaining = runtime::messaging::remaining_request_timeout (deadline);
            if (remaining <= std::chrono::milliseconds::zero ())
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "node request deadline elapsed before submission");
            using projection_t =
              detail::request_receiver_t<encoded_payload_t>::projection_t<node_reply_t,
                                                                          request_decoder_t>;
            auto complete = receiver.complete.template capture<const result_t<node_reply_t> &> (
              projection_t{receiver.complete.borrow (), std::move (decode)});
            detail::request_receiver_t<node_reply_t> physical{std::move (receiver.admitted),
                                                              std::move (complete),
                                                              std::move (receiver.fail_public)};
            (*mesh)->request (*target_owner, std::move (parts), remaining, std::move (physical));
        } else {
            using projection_t =
              detail::request_receiver_t<encoded_payload_t>::projection_t<reply_parts_t,
                                                                          request_decoder_t>;
            auto complete = receiver.complete.template capture<const result_t<reply_parts_t> &> (
              projection_t{receiver.complete.borrow (), std::move (decode)});
            detail::request_receiver_t<reply_parts_t> physical{std::move (receiver.admitted),
                                                               std::move (complete),
                                                               std::move (receiver.fail_public)};
            (*route)->request_reply_parts (*target_owner, std::nullopt, std::move (parts), deadline,
                                           std::move (physical));
        }
    }
    catch (const framework_exception_t &) {
        throw;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

request_task_t<zlink::message_t> route_client_t::submit_channel_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state_ref,
  std::string channel_name,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata)
try {
    detail::route_client_state_t::admit_outbound (state_ref);

    // The reply owns this copy: the referenced state lives in the call closure.
    const auto state = state_ref;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::protocol_error,
                                                    "route client is not configured");
    }
    auto channel_transport = detail::resolve_route_client_channel_transport (
      state->runtime, channel_name, state->runtime->client_server_requesters,
      state->runtime->mesh_channel_requesters);
    if (!channel_transport) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::not_found,
                                                    "ChannelName '" + channel_name
                                                      + "' is not registered");
    }
    const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                     ? timeout
                                     : state->runtime->default_request_timeout;
    if (const auto *client_server = std::get_if<0> (&*channel_transport)) {
        try {
            auto serialized = encode_payload (*state->serializers);
            const auto ambient_guard = detail::enter_ambient_context (ambient_context);
            if (detail::message_flow_tracer_t (state->runtime->dispatch)
                  .enabled_for (message_flow_outcome_t::sent)) {
                detail::message_flow_tracer_t (state->runtime->dispatch)
                  .trace (message_flow_outcome_t::sent, [&] {
                      return message_flow_event_t{.outcome = message_flow_outcome_t::sent,
                                                  .surface = dispatch_error_surface_t::channel,
                                                  .message_kind = dispatch_message_kind_t::request,
                                                  .packet_name = packet_name,
                                                  .channel_name = channel_name,
                                                  .channel_route_kind =
                                                    std::string ("client_server")};
                  });
            }
            auto selected = (*client_server) (packet_name, std::move (serialized.content_type),
                                              detail::encoded_payload_to_raw (serialized.payload),
                                              effective_timeout, std::move (metadata));
            // The selected callable stays owned until its terminal.
            return detail::project_request_reply<zlink::message_t> (
              selected, [transport = std::move (*channel_transport)] (
                          const result_t<zlink::message_t> &reply) {
                  (void) transport;
                  return reply;
              });
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<zlink::message_t> (error);
        }
        catch (const std::exception &error) {
            return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                        error.what ());
        }
    }
    const auto &requester = std::get<1> (*channel_transport);
    runtime::messaging::message_parts_t parts;
    try {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (runtime::messaging::message_kind_t::request,
                                             channel_name, packet_name, effective_timeout);
        header.metadata = std::move (metadata);
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
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
        }
        parts =
          codec.encode_payload_parts (std::move (header), encode_payload (*state->serializers));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<zlink::message_t> (error);
    }

    const auto ambient_guard = detail::enter_ambient_context (ambient_context);
    try {
        auto selected = requester (std::move (parts), effective_timeout);
        // The reply decodes the selected operation's terminal once (hot path
        // E5) and owns the selected callable until that terminal.
        return detail::project_request_reply<zlink::message_t> (
          selected,
          [state, transport = std::move (*channel_transport), packet_name = std::move (packet_name),
           channel_name = std::move (channel_name)] (
            const result_t<result_t<runtime::messaging::message_parts_t>> &terminal)
            -> result_t<zlink::message_t> {
              (void) transport;
              try {
                  const auto &reply = terminal.value ();
                  runtime::messaging::envelope_codec_t envelope;
                  if (!reply) {
                      return detail::propagate_failure<zlink::message_t> (
                        reply, "RouteMesh channel request failed");
                  }
                  auto reply_header = envelope.decode_header (reply.value (), false);
                  if (!reply_header) {
                      return result_t<zlink::message_t>::failure (
                        reply_header.error_kind (),
                        reply_header.error () ? reply_header.error ()->what ()
                                              : "RouteMesh channel reply header decode failed");
                  }
                  if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                      runtime::messaging::request_failure_mapper_t failure_mapper;
                      return detail::result_access_t::failure<zlink::message_t> (
                        failure_mapper.error_header_exception (
                          reply_header.value ().error_code.value_or ("request_failed"),
                          reply_header.value ().error_message.value_or (
                            "RouteMesh channel request failed"),
                          "RouteMesh channel request"));
                  }
                  auto body = envelope.decode_body (reply.value ());
                  if (!body) {
                      return detail::propagate_failure<zlink::message_t> (
                        body, "RouteMesh channel reply body decode failed");
                  }
                  if (detail::message_flow_tracer_t (state->runtime->dispatch)
                        .enabled_for (message_flow_outcome_t::reply_received)) {
                      detail::message_flow_tracer_t (state->runtime->dispatch)
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
                  }
                  return result_t<zlink::message_t>::success (body.value ());
              }
              catch (const framework_exception_t &error) {
                  return detail::result_access_t::failure<zlink::message_t> (error);
              }
              catch (const std::exception &error) {
                  return result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, error.what ());
              }
          });
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (const std::exception &error) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                    error.what ());
    }
}
catch (...) {
    return request_task_t<zlink::message_t> (detail::current_exception_result<zlink::message_t> ());
}

void route_client_t::submit_spot_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state_ref,
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  spot_id_t target_spot_id,
  std::uint64_t target_spot_generation,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::steady_clock::time_point deadline,
  std::map<std::string, std::string> metadata,
  detail::request_receiver_t<encoded_payload_t> receiver)
{
    // The reply owns this copy: the referenced state lives in the caller.
    const auto state = state_ref;
    auto submit_flow = runtime::flow_context_t::enter_current_or_create (
      flow_origin_t::application,
      state && state->runtime ? detail::message_flow_tracer_t (state->runtime->dispatch).mode ()
                              : message_flow_log_mode_t::off);
    const auto ambient_context = detail::capture_ambient_context ();
    if (!state || !state->runtime || state->serializers == nullptr) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route client is not configured");
    }
    runtime::messaging::message_parts_t parts;
    const auto effective_timeout = runtime::messaging::remaining_request_timeout (deadline);
    if (effective_timeout <= std::chrono::milliseconds::zero ())
        throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                     "Spot request deadline elapsed before encoding");
    const auto use_route_channel = detail::has_route_channel (state->runtime, router_channel_id);
    auto mesh_requester = use_route_channel
                            ? std::nullopt
                            : detail::spot_mesh_requester (state->runtime, router_channel_id);
    if (!use_route_channel && !mesh_requester) {
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "route channel or SPOT mesh '" + router_channel_id
                                       + "' is not registered");
    }
    {
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (
          runtime::messaging::message_kind_t::request, router_channel_id, packet_name,
          runtime::messaging::remaining_request_timeout (deadline));
        header.metadata = std::move (metadata);
        if (detail::message_flow_tracer_t (state->runtime->dispatch)
              .enabled_for (message_flow_outcome_t::sent)) {
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
                                              target_spot_id,
                                              std::nullopt,
                                              std::nullopt};
              });
        }
        parts =
          codec.encode_payload_parts (std::move (header), encode_payload (*state->serializers));
    }
    // A selected transport may borrow its target until the physical terminal.
    const auto target = std::make_shared<const std::pair<zlink::routing_id_t, spot_id_t>> (
      std::move (target_node_rid), std::move (target_spot_id));
    const auto &target_rid = target->first;
    const auto &spot_id = target->second;
    const auto ambient_guard = detail::enter_ambient_context (ambient_context);
    try {
        auto decode = [state, requester = mesh_requester, packet_name, router_channel_id,
                       target] (const result_t<runtime::messaging::message_parts_t> &reply)
          -> result_t<encoded_payload_t> {
            (void) requester;
            const auto &target_node_rid = target->first;
            const auto &spot_id = target->second;
            try {
                runtime::messaging::envelope_codec_t envelope;
                if (!reply) {
                    if (const auto *error = reply.error ();
                        error && error->kind () == framework_error_kind_t::not_found) {
                        detail::dispatch_error_reporter_t (state->runtime->dispatch)
                          .report_lazy ([&] {
                              return message_dispatch_error_event_t{
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
                                               : std::exception_ptr{}};
                          });
                    }
                    return detail::propagate_failure<encoded_payload_t> (
                      reply, "route spot request failed");
                }
                auto reply_header = envelope.decode_header (reply.value (), false);
                if (!reply_header) {
                    return result_t<encoded_payload_t>::failure (
                      reply_header.error_kind (), reply_header.error ()
                                                    ? reply_header.error ()->what ()
                                                    : "route spot reply header decode failed");
                }
                if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                    runtime::messaging::request_failure_mapper_t failure_mapper;
                    /* Stale-route contract: only framework-generated remote errors
                       * (zlink.origin=framework marker) may drive route-cache
                       * invalidation. Unmarked remote errors came from the application
                       * handler and are classified so the caller skips invalidation. */
                    return detail::result_access_t::failure<encoded_payload_t> (
                      detail::with_error_origin (
                        failure_mapper.error_header_exception (
                          reply_header.value ().error_code.value_or ("request_failed"),
                          reply_header.value ().error_message.value_or (
                            "route spot request failed"),
                          "route spot request"),
                        runtime::messaging::has_framework_origin (reply_header.value ().metadata)
                          ? detail::error_origin_t::framework
                          : detail::error_origin_t::application));
                }
                auto body = envelope.decode_body_view (reply.value ());
                if (!body) {
                    return result_t<encoded_payload_t>::failure (
                      body.error_kind (), body.error () ? body.error ()->what ()
                                                        : "route spot reply body decode failed");
                }
                if (detail::message_flow_tracer_t (state->runtime->dispatch)
                      .enabled_for (message_flow_outcome_t::reply_received)) {
                    detail::message_flow_tracer_t (state->runtime->dispatch)
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
                }
                return body;
            }
            catch (const framework_exception_t &error) {
                return detail::result_access_t::failure<encoded_payload_t> (error);
            }
            catch (const std::exception &error) {
                return result_t<encoded_payload_t>::failure (
                  framework_error_kind_t::internal_failure, error.what ());
            }
        };
        if (mesh_requester) {
            const auto remaining = runtime::messaging::remaining_request_timeout (deadline);
            if (remaining <= std::chrono::milliseconds::zero ())
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "Spot request deadline elapsed before submission");
            receiver
              .project<result_t<runtime::messaging::message_parts_t>> (
                [decode = std::move (decode)] (const auto &terminal) {
                    return decode (terminal.value ());
                })
              .observe ((*mesh_requester) (target_rid, spot_id, target_spot_generation,
                                           std::move (parts), remaining));
        } else {
            detail::channel_runtime_manager_t manager (state->runtime);
            manager.get_route_channel (router_channel_id)
              .request_reply_parts (
                target_rid,
                detail::is_internal_spot_route_packet (packet_name)
                  ? std::nullopt
                  : std::optional<std::string> (spot_id),
                std::move (parts), deadline,
                receiver.project<runtime::messaging::message_parts_t> (std::move (decode)));
        }
    }
    catch (const framework_exception_t &) {
        throw;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<void> route_client_t::submit_spot_id_send_erased (
  const std::shared_ptr<detail::route_client_state_t> &state_ref,
  spot_id_t target,
  detail::spot_activation_intent_t intent,
  std::string packet_name,
  std::type_index message_type,
  payload_encoder_t encode_payload,
  route_send_call_t::metadata_map_t metadata)
{
    detail::route_client_state_t::admit_outbound (state_ref);

    const auto started_at = std::chrono::system_clock::now ();
    /* The state reference belongs to the call object's closure, which can
     * unwind while this coroutine is suspended: keep a frame-owned copy
     * (session-reconnect-and-coroutine-lifetime doc). */
    const auto state = state_ref;
    /* The frame owns the resolver it starts with: a late terminal may run after
     * the caller observed its result and released the resolver's owner. */
    const auto resolver = state && state->runtime ? state->runtime->spot_resolver : nullptr;
    if (!resolver) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot location resolver is not configured");
    }
    if (intent.mesh_name && !intent.instance) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "in_mesh requires Instance Spot intent");
    }
    // The transport boundary: this task is the public Spot send result.
    try {
        const auto address = co_await resolver->resolve_spot_address ({}, target, intent.instance);

        auto submitted = result_t<void>::success ();
        if (intent.instance) {
            detail::channel_runtime_state_t::instance_spot_send_t activate;
            activate = state->runtime->lane
                         .run_checked ([&] {
                             activate = state->runtime->instance_spot_sender;
                             return activate;
                         })
                         .get ();
            if (!activate) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "Instance Spot activation runtime is not configured");
            }
            submitted = co_await activate (target, intent, address, packet_name, message_type,
                                           std::move (encode_payload), metadata, started_at);
        } else if (!address) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "Spot route was not found");
        } else {
            submitted = co_await submit_spot_send_erased (
              state, address->mesh_name, address->node_rid, spot_id_t (address->spot_id),
              address->spot_generation, packet_name, message_type, std::move (encode_payload),
              metadata);
        }
        invalidate_stale_spot_route (*resolver, target, submitted.error ());
        // The resolved transport already classified its failure; publish it unchanged.
        submitted.value ();
    }
    catch (const framework_exception_t &) {
        throw;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<void> route_client_t::submit_spot_id_request_reply_message_erased (
  const std::shared_ptr<detail::route_client_state_t> &state_ref,
  spot_id_t target,
  detail::spot_activation_intent_t intent,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  std::map<std::string, std::string> metadata,
  detail::request_receiver_t<encoded_payload_t> receiver)
{
    const auto request_started = std::chrono::steady_clock::now ();
    try {
        detail::route_client_state_t::admit_outbound (state_ref);
        // The call closure can end while suspended; this frame owns the state copy.
        const auto state = state_ref;
        // Frame-owned resolver, as in submit_spot_id_send_erased.
        const auto resolver = state && state->runtime ? state->runtime->spot_resolver : nullptr;
        if (!resolver) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Spot location resolver is not configured");
        }
        if (intent.mesh_name && !intent.instance) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "in_mesh requires Instance Spot intent");
        }
        const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                         ? timeout
                                         : state->runtime->default_request_timeout;
        const auto deadline = request_started + effective_timeout;
        auto address = co_await detail::resolve_request_spot_address (
          state->runtime, resolver, target, intent.instance, deadline);
        receiver = receiver.project<encoded_payload_t> (
          [resolver, target] (const result_t<encoded_payload_t> &terminal) {
              if (!terminal) {
                  invalidate_stale_spot_route (*resolver, target, terminal.error ());
                  return detail::result_access_t::failure<encoded_payload_t> (
                    terminal.exception ());
              }
              return result_t<encoded_payload_t>::success (
                detail::encoded_payload_from_raw (terminal.value ().bytes ()));
          });

        if (intent.instance) {
            detail::channel_runtime_state_t::instance_spot_request_t activate;
            activate = state->runtime->lane
                         .run_checked ([&] {
                             activate = state->runtime->instance_spot_requester;
                             return activate;
                         })
                         .get ();
            if (!activate) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "Instance Spot activation runtime is not configured");
            }
            const auto admission = receiver.admitted->task ();
            const auto remaining_timeout = [&deadline] {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
                  deadline - std::chrono::steady_clock::now ());
                if (remaining <= std::chrono::milliseconds::zero ())
                    throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                                 "Instance Spot request deadline elapsed");
                return remaining;
            };
            for (;;) {
                const auto remaining = remaining_timeout ();
                try {
                    auto pending = activate (target, intent, address, packet_name, request_type,
                                             encode_payload, remaining, metadata);
                    auto accepted = pending.admitted ();
                    detail::observe_task_terminal (
                      accepted, [source = receiver.admitted] (const auto &terminal) {
                          source->complete (terminal);
                      });
                    receiver.complete (
                      result_t<encoded_payload_t>::success (detail::encoded_payload_from_raw (
                        co_await detail::request_task_access_t::physical_reply (pending))));
                    co_return;
                }
                catch (const framework_exception_t &error) {
                    // §9 follows authority only for a refusal before target admission.
                    if (!admission.await_ready () || !detail::task_result_access_t::read (admission)
                        || (detail::failure_code (error)
                              != static_cast<std::uint32_t> (
                                runtime::protocol::framework_error_code::spotMoving)
                            && detail::failure_code (error)
                                 != static_cast<std::uint32_t> (
                                   runtime::protocol::framework_error_code::actorLocationStale)
                            && detail::failure_code (error)
                                 != static_cast<std::uint32_t> (
                                   runtime::protocol::framework_error_code::spotGenerationStale)))
                        throw;
                }
                resolver->invalidate_spot_address (target);
                (void) remaining_timeout ();
                address = co_await detail::resolve_request_spot_address (state->runtime, resolver,
                                                                         target, true, deadline);
            }
        }
        if (!address) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "Spot route was not found");
        }
        submit_spot_request_reply_message_erased (
          state, address->mesh_name, address->node_rid, spot_id_t (address->spot_id),
          address->spot_generation, std::move (packet_name), request_type,
          std::move (encode_payload), deadline, std::move (metadata), receiver);
    }
    catch (...) {
        const auto failure = detail::current_exception_result<encoded_payload_t> ();
        receiver.admitted->complete (detail::result_access_t::failure<void> (failure.exception ()));
        receiver.complete (failure);
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
    state->spot_builder._state->channel_runtime = _state->runtime;
    state->spot_builder._state->dispatch = _state->runtime->dispatch;
    state->spot_builder._state->snapshot.discovery_channel_name = mesh_name;
    _state->mesh_nodes.emplace (std::move (mesh_name), state);
    return mesh_node_builder_t (std::move (state));
}

mesh_node_builder_t zlink_framework_options_t::add_route_mesh (std::string mesh_name)
{
    auto builder = _zlink->add_route_mesh (std::move (mesh_name));
    builder._state->framework_options = _options;
    builder._state->handler_groups = _handler_groups;
    std::weak_ptr<detail::framework_options_state_t> options = _options;
    builder._state->lane
      .run_checked ([&] {
          builder._state->channel_name_observer = [options] (const std::string &channel_name) {
              if (const auto state = options.lock ()) {
                  state->mesh_node_channel_names.insert (channel_name);
              }
          };
      })
      .get ();
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
