/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/diagnostics/topology_projection.hpp"
#include "runtime/channels/channel_socket_options.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace zlink::framework::runtime::client_server
{

namespace
{

constexpr std::string_view default_security_identity = "default";
constexpr std::uint32_t default_effective_max_message_bytes =
  static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ());

std::atomic<std::uintptr_t> next_client_server_poller_slot{1};

std::uintptr_t next_transport_poller_slot () noexcept
{
    auto slot = next_client_server_poller_slot.fetch_add (1, std::memory_order_relaxed);
    if (slot == 0) {
        slot = next_client_server_poller_slot.fetch_add (1, std::memory_order_relaxed);
    }
    return slot;
}

std::string connection_key (const client_server_server_descriptor_t &descriptor)
{
    return descriptor.server_rid.to_hex () + "|" + std::to_string (descriptor.lifecycle_generation);
}

std::string stable_key (const client_server_server_descriptor_t &descriptor)
{
    return descriptor.server_rid.to_hex ();
}

void report_client_server_dispatch_error (const dispatch_options_t &options,
                                          const mesh::service_mailbox_record_t &record,
                                          std::string_view packet_name,
                                          dispatch_message_kind_t message_kind,
                                          dispatch_error_action_t action,
                                          const framework_exception_t &error) noexcept
{
    zlink::framework::detail::dispatch_error_reporter_t (options).report_lazy ([&] {
        std::optional<std::string> correlation;
        if (record.correlation)
            correlation = std::to_string (*record.correlation);
        return message_dispatch_error_event_t{
          dispatch_error_surface_t::channel,
          message_kind,
          zlink::framework::detail::dispatch_reason_from_error (&error),
          action,
          std::string (packet_name),
          record.owner,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::move (correlation),
          std::make_exception_ptr (error)};
    });
}

std::string manual_connection_key (std::string_view endpoint)
{
    return "manual|" + std::string (endpoint);
}

void trace_client_server_runtime_failure (std::string_view stage, std::string_view error) noexcept
{
    try {
        const auto *trace = std::getenv ("ZLINK_CPP_CLIENT_SERVER_TRACE");
        if (trace == nullptr || std::string_view (trace) == "0"
            || std::string_view (trace).empty ())
            return;
        std::cerr << "zlink-cpp-client-server-trace stage=" << stage << " error=" << error
                  << std::endl;
    }
    catch (...) {
    }
}

client_server_server_descriptor_t manual_descriptor (std::string_view channel_name,
                                                     std::string_view endpoint)
{
    return client_server_server_descriptor_t{.channel_name = std::string (channel_name),
                                             .endpoint = std::string (endpoint),
                                             .weight = 100,
                                             .state = framework_runtime_state_t::serving,
                                             .security_identity =
                                               std::string (default_security_identity)};
}

framework_runtime_state_t current_state (const location_runtime_t &locations)
{
    return locations.draining () ? framework_runtime_state_t::draining
                                 : framework_runtime_state_t::serving;
}

} // namespace

mesh::service_node_state_t client_server_service_state (framework_runtime_state_t state)
{
    switch (state) {
        case framework_runtime_state_t::preparing:
            return mesh::service_node_state_t::preparing;
        case framework_runtime_state_t::serving:
            return mesh::service_node_state_t::serving;
        case framework_runtime_state_t::relocating:
        case framework_runtime_state_t::relocated:
        case framework_runtime_state_t::draining:
            return mesh::service_node_state_t::draining;
        case framework_runtime_state_t::stopped:
            return mesh::service_node_state_t::stopped;
        case framework_runtime_state_t::error:
            return mesh::service_node_state_t::error;
        default:
            throw std::invalid_argument ("invalid framework ClientServer runtime state");
    }
}

framework_runtime_state_t client_server_framework_state (mesh::service_node_state_t state)
{
    switch (state) {
        case mesh::service_node_state_t::preparing:
            return framework_runtime_state_t::preparing;
        case mesh::service_node_state_t::serving:
            return framework_runtime_state_t::serving;
        case mesh::service_node_state_t::retiring:
            return framework_runtime_state_t::relocating;
        case mesh::service_node_state_t::draining:
            return framework_runtime_state_t::draining;
        case mesh::service_node_state_t::stopped:
            return framework_runtime_state_t::stopped;
        case mesh::service_node_state_t::error:
            return framework_runtime_state_t::error;
        default:
            throw std::invalid_argument ("invalid service ClientServer runtime state");
    }
}

struct client_server_location_runtime_t::server_entry_t
{
    channel_capability_snapshot_t capability;
    std::shared_ptr<raw_client_server_server_t> owner;
    std::optional<client_server_server_descriptor_t> published_descriptor;
    std::shared_ptr<pump_task_state_t> pump_task;
    std::shared_ptr<pump_task_state_t> dispatch_task;
};

struct client_server_location_runtime_t::client_connection_t
{
    client_server_server_descriptor_t descriptor;
    std::shared_ptr<raw_client_server_client_t> owner;
    bool selector_ready = false;
    std::shared_ptr<pump_task_state_t> pump_task;
};

struct client_server_location_runtime_t::snapshot_connection_t
{
    client_server_server_descriptor_t descriptor;
    std::shared_ptr<raw_client_server_client_t> owner;
    raw_client_server_client_t::pump_status_t transport{};
};

struct client_server_location_runtime_t::snapshot_source_t
{
    std::string channel_name;
    client_server_role_t role = client_server_role_t::client;
    framework_runtime_state_t host_state = framework_runtime_state_t::preparing;
    std::vector<snapshot_connection_t> connections;
    std::shared_ptr<raw_client_server_server_t> local_server;
    std::optional<protocol::client_server_server_admission_t> local_admission;
    std::uint64_t sequence = 0;
};

struct client_server_location_runtime_t::worker_lane_snapshot_t
{
    std::vector<client_connection_t *> connections;
    std::vector<std::shared_ptr<raw_client_server_client_t>> owners;
    std::vector<std::shared_ptr<raw_client_server_server_t>> servers;
    std::optional<std::chrono::steady_clock::time_point> ready_deadline;
    std::optional<mesh::service_liveness_registry_t::clock_t::time_point> next_activity;
};

struct client_server_location_runtime_t::pump_task_state_t
{
    std::mutex mutex;
    std::optional<result_t<void>> completion;
    std::shared_ptr<task_t<void>> task;
};

struct client_server_location_runtime_t::ready_waiter_t
{
    std::string channel_name;
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<task_completion_source_t<std::shared_ptr<raw_client_server_client_t>>>
      completion;
};

struct client_server_location_runtime_t::client_channel_t
{
    channel_snapshot_t snapshot;
    std::vector<std::uint8_t> routing_id;
    std::map<std::string, client_connection_t> connections;
    smooth_weighted_selector_t selector;
    std::vector<weighted_candidate_t> selector_candidates;
    bool selector_dirty = true;
};

namespace
{

task_t<void>
pump_server_transport (std::shared_ptr<raw_client_server_server_t> server,
                       mesh::service_liveness_registry_t::clock_t::time_point now,
                       std::shared_ptr<application_job_queue_t::permit_t> application_permit)
{
    (void) co_await server->drain_monitor_events_task (now);
    (void) co_await server->pump_one (now, std::move (application_permit));
    (void) co_await server->tick_liveness (now);
}

task_t<void> pump_client_transport (std::shared_ptr<raw_client_server_client_t> client,
                                    mesh::service_liveness_registry_t::clock_t::time_point now)
{
    (void) co_await client->drain_monitor_events (now);
    receive_batch_budget_t budget;
    while (budget.can_receive ()) {
        const auto result = co_await client->pump_one (now);
        if (result == client_server_pump_result_t::no_data
            || result == client_server_pump_result_t::backpressured)
            break;
        budget.account (co_await client->last_pump_bytes_task ());
        if (budget.exhausted ())
            break;
    }
    (void) co_await client->tick_liveness (now);
}

class client_server_observation_t final : public mesh_runtime_observation_t
{
  public:
    explicit client_server_observation_t (
      std::shared_ptr<client_server_location_runtime_t::observer_t> observer) :
        _observer (std::move (observer))
    {
    }

    ~client_server_observation_t () override { close (); }

    void close () override
    {
        if (_observer) {
            _observer->close ();
            _observer.reset ();
        }
    }

  private:
    std::shared_ptr<client_server_location_runtime_t::observer_t> _observer;
};

client_server_role_t local_role (const channel_snapshot_t &channel)
{
    if (channel.client.enabled && channel.server.enabled)
        return client_server_role_t::client_and_server;
    if (channel.server.enabled)
        return client_server_role_t::server;
    return client_server_role_t::client;
}

} // namespace

client_server_location_runtime_t::client_server_location_runtime_t (
  message_bus_t bus,
  std::vector<channel_snapshot_t> channels,
  location_runtime_t &locations,
  location_repository_t &store,
  location_repository_t &leases,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  std::map<std::string, std::string> advertise_hosts,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    _bus (std::move (bus)),
    _channel_runtime (detail::channel_runtime_t::from (_bus)),
    _channels (std::move (channels)),
    _locations (&locations),
    _store (&store),
    _leases (&leases),
    _services (services),
    _serializers (&serializers),
    _handlers (&handlers),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (application_job_queue_configuration_t{
            application_job_queue_profile_t::balanced,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ()), 1,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ())})),
    _advertise_hosts (std::move (advertise_hosts)),
    _listener_statuses (std::move (listener_statuses))
{
    std::erase_if (_channels, [] (const auto &channel) {
        return !channel.server.enabled && !channel.client.enabled;
    });
}

client_server_location_runtime_t::~client_server_location_runtime_t () noexcept
{
    stop ();
}

bool client_server_location_runtime_t::empty () const noexcept
{
    return std::none_of (_channels.begin (), _channels.end (), [] (const auto &channel) {
        return (channel.server.enabled && !channel.server.bind_endpoints.empty ())
               || (channel.client.enabled
                   && (channel.client.discovery || !channel.client.connect_endpoints.empty ()));
    });
}

client_server_location_runtime_t::snapshot_source_t
client_server_location_runtime_t::snapshot_source_locked (const std::string &channel_name) const
{
    snapshot_source_t source;
    source.channel_name = channel_name;
    const auto configured =
      std::find_if (_channels.begin (), _channels.end (),
                    [&] (const auto &channel) { return channel.name == channel_name; });
    if (configured == _channels.end ())
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "ClientServer channel is not configured: " + channel_name);

    source.role = local_role (*configured);
    auto services = _services;
    source.host_state = _stop.load (std::memory_order_acquire)
                          ? (_transport_poller ? framework_runtime_state_t::draining
                                               : framework_runtime_state_t::stopped)
                          : services.get_required<framework_runtime_t> ().status ().state;
    const auto client = _clients.find (channel_name);
    if (client != _clients.end ()) {
        source.connections.reserve (client->second->connections.size ());
        for (const auto &[key, connection] : client->second->connections)
            source.connections.push_back ({connection.descriptor, connection.owner});
    }
    const auto local_server = _servers.find (channel_name);
    if (local_server != _servers.end ())
        source.local_server = local_server->second->owner;
    const auto sequence = _snapshot_sequences.find (channel_name);
    source.sequence = sequence == _snapshot_sequences.end () ? 0 : sequence->second;
    return source;
}

task_t<client_server_channel_snapshot_t>
client_server_location_runtime_t::snapshot_task (std::string channel_name) const
{
    auto source = co_await _lane.run_task ([this, channel_name = std::move (channel_name)] {
        return snapshot_source_locked (channel_name);
    });
    auto current = co_await build_snapshot_task (std::move (source));
    co_return co_await _lane.run_task ([this, current = std::move (current)] () mutable {
        return publish_snapshot_locked (std::move (current));
    });
}

task_t<client_server_channel_snapshot_t>
client_server_location_runtime_t::build_snapshot_task (snapshot_source_t source) const
{
    for (auto &connection : source.connections)
        connection.transport = co_await connection.owner->pump_status_task ();
    if (source.local_server)
        source.local_admission = co_await source.local_server->descriptor_task ();
    co_return build_snapshot (std::move (source));
}

client_server_channel_snapshot_t
client_server_location_runtime_t::build_snapshot (snapshot_source_t source) const
{
#ifndef NDEBUG
    assert (!_lane.is_on_lane ());
#endif
    client_server_channel_snapshot_t result;
    result.channel_name = std::move (source.channel_name);
    result.observed_at = std::chrono::system_clock::now ();
    result.local_role = source.role;
    result.targets.reserve (source.connections.size () + source.local_admission.has_value ());
    for (const auto &connection : source.connections) {
        const auto peer = detail::project_topology_peer (
          connection.descriptor.server_rid, connection.descriptor.state, connection.transport.ready,
          connection.transport.connecting, topology_reason_t::no_ready_target);
        result.targets.push_back (
          {peer.node_rid, connection.descriptor.weight, peer.state, peer.unavailable_reason});
    }
    const auto host_state = source.host_state;
    if (source.local_admission) {
        const auto &admission = *source.local_admission;
        const auto local_state =
          detail::topology_state_for_host (host_state, topology_state_t::ready)
              == topology_state_t::stopping
            ? host_state
            : client_server_framework_state (admission.state);
        const auto peer = detail::project_topology_peer (
          zlink::routing_id_t::from (admission.server_routing_id), local_state,
          admission.state == mesh::service_node_state_t::serving,
          admission.state == mesh::service_node_state_t::preparing,
          topology_reason_t::no_ready_target);
        client_server_target_snapshot_t local{peer.node_rid, static_cast<int> (admission.weight),
                                              peer.state, peer.unavailable_reason};
        const auto existing = std::find_if (
          result.targets.begin (), result.targets.end (),
          [&local] (const auto &target) { return target.node_rid == local.node_rid; });
        if (existing == result.targets.end ())
            result.targets.push_back (std::move (local));
        else
            *existing = std::move (local);
    }
    const auto ready_count =
      std::count_if (result.targets.begin (), result.targets.end (),
                     [] (const auto &target) { return target.state == peer_state_t::ready; });
    result.ready_target_count = static_cast<std::uint32_t> (
      std::min<std::size_t> (ready_count, std::numeric_limits<std::uint32_t>::max ()));
    const bool eligible =
      std::any_of (result.targets.begin (), result.targets.end (), [] (const auto &target) {
          return target.state == peer_state_t::ready && target.weight > 0;
      });
    result.state = detail::topology_state_for_host (
      host_state, eligible ? topology_state_t::ready : topology_state_t::degraded);
    result.is_ready = result.state == topology_state_t::ready;
    result.sequence = source.sequence;
    return result;
}

bool client_server_location_runtime_t::snapshot_equivalent (
  const client_server_channel_snapshot_t &left,
  const client_server_channel_snapshot_t &right) noexcept
{
    return left.channel_name == right.channel_name && left.local_role == right.local_role
           && left.state == right.state && left.is_ready == right.is_ready
           && left.ready_target_count == right.ready_target_count
           && std::equal (left.targets.begin (), left.targets.end (), right.targets.begin (),
                          right.targets.end (), [] (const auto &a, const auto &b) {
                              return a.node_rid == b.node_rid && a.weight == b.weight
                                     && a.state == b.state
                                     && a.unavailable_reason == b.unavailable_reason;
                          });
}

client_server_channel_snapshot_t
client_server_location_runtime_t::snapshot (std::string channel_name) const
{
    return snapshot_task (std::move (channel_name)).result ().value ();
}

std::unique_ptr<mesh_runtime_observation_t> client_server_location_runtime_t::observe (
  std::string channel_name,
  std::size_t capacity,
  std::function<void (const observed_status_t<client_server_channel_snapshot_t> &)> observer)
{
    if (channel_name.empty () || capacity == 0 || !observer)
        throw std::invalid_argument ("ClientServer observation requires a channel and callback");
    auto value = std::make_shared<observer_t> (capacity, std::move (observer));
    value->start ();
    auto source = _lane
                    .run_checked ([this, &channel_name, &value] {
                        auto source = snapshot_source_locked (channel_name);
                        _observers[channel_name].push_back (value);
                        return source;
                    })
                    .get ();
    auto initial = build_snapshot_task (std::move (source)).result ().value ();
    _lane
      .run_checked (
        [this, &initial, &value] { publish_snapshot_locked (std::move (initial), value); })
      .get ();
    return std::make_unique<client_server_observation_t> (std::move (value));
}

bool client_server_location_runtime_t::is_ready (std::string channel_name) const
{
    return snapshot (std::move (channel_name)).is_ready;
}

client_server_channel_snapshot_t client_server_location_runtime_t::publish_snapshot_locked (
  client_server_channel_snapshot_t current,
  const std::shared_ptr<observer_t> &initial_observer) const
{
    const auto &channel_name = current.channel_name;
    const auto previous = _last_snapshots.find (channel_name);
    if (previous != _last_snapshots.end () && current.sequence < previous->second.sequence) {
        current = previous->second;
    }
    const bool changed =
      previous == _last_snapshots.end () || !snapshot_equivalent (previous->second, current);
    if (changed) {
        current.sequence = ++_snapshot_sequences[channel_name];
        current.observed_at = std::chrono::system_clock::now ();
        _last_snapshots.insert_or_assign (channel_name, current);
    } else if (previous != _last_snapshots.end ()) {
        current = previous->second;
    }
    const bool terminal = detail::topology_is_terminal (current.state);
    auto &registered = _observers[channel_name];
    auto write = registered.begin ();
    for (auto read = registered.begin (); read != registered.end (); ++read) {
        if (auto value = read->lock ()) {
            if (changed || value == initial_observer)
                value->enqueue (channel_name, current, terminal);
            *write++ = *read;
        }
    }
    registered.erase (write, registered.end ());
    return current;
}

task_t<void> client_server_location_runtime_t::publish_snapshot_changes ()
{
    auto sources = co_await _lane.run_task ([this] {
        std::vector<snapshot_source_t> result;
        result.reserve (_channels.size ());
        for (const auto &channel : _channels)
            result.push_back (snapshot_source_locked (channel.name));
        return result;
    });
    std::vector<client_server_channel_snapshot_t> snapshots;
    snapshots.reserve (sources.size ());
    for (auto &source : sources)
        snapshots.push_back (co_await build_snapshot_task (std::move (source)));
    co_await _lane.run_task ([this, snapshots = std::move (snapshots)] () mutable {
        for (auto &current : snapshots)
            current = publish_snapshot_locked (std::move (current));
        return std::move (snapshots);
    });
}

void client_server_location_runtime_t::start ()
{
    if (empty ())
        return;
    auto owner = _locations->current_owner_token ();

    _stop.store (false, std::memory_order_release);
    try {
        _lane.run_checked ([this] { _transport_poller = std::make_unique<zlink::poller_t> (); })
          .get ();
        _wake_timer->attach (*_transport_poller);
        _application_supply = std::make_unique<application_supply_slot_t> (
          _application_jobs, [wake = _wake_timer] { wake->signal (); });
        for (const auto &channel : _channels) {
            if (channel.server.enabled && !channel.server.bind_endpoints.empty ())
                start_server (channel, owner);
            if (channel.client.enabled
                && (channel.client.discovery || !channel.client.connect_endpoints.empty ()))
                start_client (channel);
        }
        reconcile ();
        _channel_runtime.mark_auto_connect_active ();
        _thread = std::thread ([this] {
#ifndef NDEBUG
            runtime::infrastructure_wait_guard::infrastructure_scope_t scope (this);
#endif
            run ();
        });
    }
    catch (...) {
        stop ();
        throw;
    }
}

void client_server_location_runtime_t::start_server (
  const channel_snapshot_t &channel, const std::optional<location_owner_token_t> &publication_owner)
{
    protocol::client_server_server_admission_t admission;
    admission.channel_name = channel.name;
    admission.server_routing_id = server_routing_id (channel);
    admission.lifecycle_generation = make_lifecycle_generation ();
    admission.weight = static_cast<std::uint32_t> (channel.server.service_weight);
    admission.state = mesh::service_node_state_t::preparing;
    admission.security_identity = std::string (default_security_identity);
    admission.effective_max_message_bytes = effective_max_message_bytes (channel.server);
    admission.advertised_endpoint =
      zlink::framework::detail::client_server_bind_endpoint (channel.server);

    const auto advertise = _advertise_hosts.find (channel.name);
    raw_client_server_server_options_t options{admission,
                                               advertise == _advertise_hosts.end ()
                                                 ? std::nullopt
                                                 : std::optional<std::string> (advertise->second)};
    options.transport_poller = _transport_poller.get ();
    options.transport_poller_slot = next_transport_poller_slot ();
    options.application_jobs = _application_jobs;
    auto raw = std::make_shared<raw_client_server_server_t> (std::move (options),
                                                             _channel_runtime.core_context ());
    raw->start ();
    auto entry = std::make_unique<server_entry_t> ();
    entry->capability = channel.server;
    entry->owner = std::move (raw);
    if (publication_owner) {
        auto descriptor = to_descriptor (entry->owner->descriptor (), *publication_owner);
        const auto stored =
          _store->update_client_server (descriptor, location_write_intent_t::new_claim)
            .result ()
            .value ();
        if (stored.status != location_write_status_t::stored) {
            entry->owner->close ();
            throw std::runtime_error ("ClientServer descriptor publication was fenced");
        }
        entry->published_descriptor = std::move (descriptor);
    }
    const auto endpoint = entry->owner->endpoint ();
    _servers.emplace (channel.name, std::move (entry));
    if (_listener_statuses)
        _listener_statuses->update (listener_kind_t::client_server, channel.name, endpoint);
}

void client_server_location_runtime_t::start_client (const channel_snapshot_t &channel)
{
    auto entry = std::make_unique<client_channel_t> ();
    entry->snapshot = channel;
    entry->routing_id = client_routing_id (channel);
    _clients.emplace (channel.name, std::move (entry));
    _channel_runtime.bind_client_server_transport (
      channel.name,
      [this, name = channel.name] (std::string packet_name, std::string content_type,
                                   zlink::message_t message, std::chrono::milliseconds,
                                   std::map<std::string, std::string> metadata) {
          return send (name, std::move (packet_name), std::move (content_type), std::move (message),
                       std::move (metadata));
      },
      [this, name = channel.name] (std::string packet_name, std::string content_type,
                                   zlink::message_t message, std::chrono::milliseconds timeout,
                                   std::map<std::string, std::string> metadata) {
          return request (name, std::move (packet_name), std::move (content_type),
                          std::move (message), timeout, std::move (metadata));
      });
}

bool client_server_location_runtime_t::publish_descriptor_state (
  framework_runtime_state_t state) noexcept
{
    if (state != framework_runtime_state_t::draining)
        return true;
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        if (_stop.load (std::memory_order_acquire))
            return false;
        _descriptor_publish_result = false;
        _descriptor_publish_pending = true;
    }
    _descriptor_publish_changed.notify_all ();
    _wake_timer->signal ();

    std::unique_lock lock (_descriptor_publish_mutex);
    if (!runtime::infrastructure_wait_guard::condition_wait_for (
          _descriptor_publish_changed, lock, std::chrono::seconds (5),
          [this] { return !_descriptor_publish_pending; }, "client-server/descriptor-publish",
          runtime::infrastructure_wait_guard::wait_relation_t::dependent_completion)) {
        return false;
    }
    return _descriptor_publish_result;
}

bool client_server_location_runtime_t::republish_after_store_recovery ()
{
    return publish_servers ();
}

void client_server_location_runtime_t::run ()
{
    auto next_reconcile = std::chrono::steady_clock::now ();
    std::shared_ptr<task_t<void>> pending_snapshot;
    std::shared_ptr<task_t<void>> pending_pump;
    std::shared_ptr<task_t<worker_lane_snapshot_t>> pending_worker_snapshot;
    std::shared_ptr<task_t<bool>> pending_maintenance;
    std::shared_ptr<task_t<void>> pending_reconcile;
    std::unique_lock<std::mutex> maintenance_lock;
    std::optional<std::chrono::steady_clock::time_point> ready_deadline;
    std::optional<mesh::service_liveness_registry_t::clock_t::time_point> next_activity;
    while (!_stop.load (std::memory_order_acquire) || pending_snapshot || pending_pump
           || pending_worker_snapshot || pending_maintenance || pending_reconcile) {
        if (pending_maintenance && pending_maintenance->await_ready ()) {
            auto completed = std::move (pending_maintenance);
            const auto &result = completed->result ();
            const bool published = result && result.value ();
            const bool reconcile_after_publish = !_descriptor_publish_pending;
            if (!result)
                _locations->record_store_error ();
            if (_descriptor_publish_pending) {
                _descriptor_publish_result = published;
                _descriptor_publish_pending = false;
            }
            maintenance_lock.unlock ();
            if (!reconcile_after_publish)
                _descriptor_publish_changed.notify_all ();
            if (reconcile_after_publish && result && !_stop.load (std::memory_order_acquire)) {
                pending_reconcile = std::make_shared<task_t<void>> (reconcile_task ());
                detail::observe_task_terminal (
                  *pending_reconcile,
                  [wake = _wake_timer] (const result_t<void> &) { wake->signal (); });
            } else if (reconcile_after_publish) {
                next_reconcile =
                  std::chrono::steady_clock::now () + _locations->options ().polling_interval;
            }
        }
        if (pending_reconcile && pending_reconcile->await_ready ()) {
            auto completed = std::move (pending_reconcile);
            if (!completed->result ())
                _locations->record_store_error ();
            next_reconcile =
              std::chrono::steady_clock::now () + _locations->options ().polling_interval;
        }
        if (pending_worker_snapshot && pending_worker_snapshot->await_ready ()
            && _stop.load (std::memory_order_acquire)) {
            auto completed = std::move (pending_worker_snapshot);
            if (!completed->result ())
                trace_client_server_runtime_failure ("runtime-loop-error",
                                                     completed->result ().error ()->what ());
        }
        if (pending_snapshot && pending_snapshot->await_ready ()) {
            auto completed = std::move (pending_snapshot);
            if (!completed->result ()) {
                trace_client_server_runtime_failure ("runtime-loop-error",
                                                     completed->result ().error ()->what ());
                _locations->record_store_error ();
            }
        }
        if (pending_pump && pending_pump->await_ready ()) {
            auto completed = std::move (pending_pump);
            if (!completed->result ()) {
                trace_client_server_runtime_failure ("runtime-loop-error",
                                                     completed->result ().error ()->what ());
                _locations->record_store_error ();
            }
        }
        if (_stop.load (std::memory_order_acquire)) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now ();
        if (!pending_maintenance && !pending_reconcile && !pending_worker_snapshot
            && !pending_pump) {
            maintenance_lock = std::unique_lock<std::mutex> (_descriptor_publish_mutex);
            if (_descriptor_publish_pending || now >= next_reconcile) {
                _client_pump_snapshot.clear ();
                pending_maintenance = std::make_shared<task_t<bool>> (publish_servers_task ());
                detail::observe_task_terminal (
                  *pending_maintenance,
                  [wake = _wake_timer] (const result_t<bool> &) { wake->signal (); });
            } else {
                maintenance_lock.unlock ();
            }
        }
        try {
            if (!pending_maintenance && !pending_reconcile && !pending_worker_snapshot
                && !pending_pump) {
                pending_worker_snapshot = std::make_shared<task_t<worker_lane_snapshot_t>> (
                  refresh_client_pump_snapshot ());
                detail::observe_task_terminal (
                  *pending_worker_snapshot,
                  [wake = _wake_timer] (const result_t<worker_lane_snapshot_t> &) {
                      wake->signal ();
                  });
            }
            if (pending_worker_snapshot && pending_worker_snapshot->await_ready ()) {
                auto completed = std::move (pending_worker_snapshot);
                auto current = completed->result ().value ();
                _client_pump_snapshot = std::move (current.connections);
                ready_deadline = current.ready_deadline;
                next_activity = current.next_activity;
                pending_pump = std::make_shared<task_t<void>> (pump ());
                detail::observe_task_terminal (
                  *pending_pump,
                  [wake = _wake_timer] (const result_t<void> &) { wake->signal (); });
                if (!pending_snapshot) {
                    pending_snapshot = std::make_shared<task_t<void>> (publish_snapshot_changes ());
                    detail::observe_task_terminal (
                      *pending_snapshot,
                      [wake = _wake_timer] (const result_t<void> &) { wake->signal (); });
                }
            }
        }
        catch (const std::exception &error) {
            trace_client_server_runtime_failure ("runtime-loop-error", error.what ());
            try {
                _locations->record_store_error ();
            }
            catch (...) {
            }
        }
        catch (...) {
            trace_client_server_runtime_failure ("runtime-loop-error", "unknown exception");
            try {
                _locations->record_store_error ();
            }
            catch (...) {
            }
        }
        if (_stop.load (std::memory_order_acquire))
            continue;

        auto wake_at = next_reconcile;
        if (next_activity)
            wake_at = std::min (wake_at, *next_activity);
        if (ready_deadline)
            wake_at = std::min (wake_at, *ready_deadline);

        const auto after_pump = std::chrono::steady_clock::now ();
        if (wake_at <= after_pump || !_transport_poller)
            continue;
        try {
            zlink::poll_event_t readiness;
            const auto count = _transport_poller->wait (
              &readiness, 1,
              std::chrono::duration_cast<std::chrono::milliseconds> (wake_at - after_pump));
            if (count == 1 && _wake_timer->is_event (readiness))
                _wake_timer->consume ();
        }
        catch (...) {
            if (!_stop.load (std::memory_order_acquire))
                continue;
            break;
        }
    }
}

bool client_server_location_runtime_t::publish_servers ()
{
    std::lock_guard publish_lock (_descriptor_publish_mutex);
    return publish_servers_task ().result ().value ();
}

task_t<bool> client_server_location_runtime_t::publish_servers_task ()
{
    const auto owner = co_await _locations->current_owner_token_task ();
    if (!owner) {
        co_return std::none_of (_servers.begin (), _servers.end (), [] (const auto &entry) {
            return entry.second->published_descriptor.has_value ();
        });
    }
    bool published = true;
    for (auto &[channel_name, server] : _servers) {
        const auto weight_override =
          co_await _channel_runtime.server_peer_weight_override_task (channel_name);
        const auto weight = weight_override.value_or (server->capability.service_weight);
        const auto state = current_state (*_locations);
        const bool new_owner =
          !server->published_descriptor || server->published_descriptor->owner_id != owner->owner_id
          || server->published_descriptor->lease_generation != owner->lease_generation;
        if (!new_owner && server->published_descriptor->weight == weight
            && server->published_descriptor->state == state)
            continue;

        auto admission = co_await server->owner->descriptor_task ();
        if (admission.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
            throw std::overflow_error ("ClientServer descriptor revision is exhausted");
        if (server->published_descriptor) {
            ++admission.descriptor_revision;
            admission.weight = static_cast<std::uint32_t> (weight);
            admission.state = client_server_service_state (state);
            co_await server->owner->update_descriptor_task (admission);
        }
        admission.weight = static_cast<std::uint32_t> (weight);
        admission.state = client_server_service_state (state);
        auto descriptor = to_descriptor (admission, *owner);
        const auto written = co_await _store->update_client_server (
          descriptor,
          new_owner ? location_write_intent_t::new_claim : location_write_intent_t::renew);
        if (written.status == location_write_status_t::stored)
            server->published_descriptor = std::move (descriptor);
        else
            published = false;
    }
    co_return published;
}

void client_server_location_runtime_t::reconcile ()
{
    reconcile_task ().result ().value ();
}

task_t<void> client_server_location_runtime_t::reconcile_task ()
{
    for (auto &[_, channel] : _clients)
        co_await reconcile_channel_task (*channel);
}

task_t<void> client_server_location_runtime_t::reconcile_channel_task (client_channel_t &channel)
{
    std::map<std::string, client_server_server_descriptor_t> desired;
    location_page_request_t page;
    do {
        const auto listed = co_await _store->list_client_servers (channel.snapshot.name, page);
        for (const auto &descriptor : listed.items) {
            if ((descriptor.state == framework_runtime_state_t::stopped)
                || descriptor.state == framework_runtime_state_t::error)
                continue;
            if (!(co_await owner_is_live_task (descriptor)))
                continue;
            desired.insert_or_assign (connection_key (descriptor), descriptor);
        }
        page.continuation_token = listed.continuation_token;
    } while (page.continuation_token);

    for (const auto &endpoint : channel.snapshot.client.connect_endpoints) {
        const auto discovered =
          std::find_if (desired.begin (), desired.end (),
                        [&] (const auto &entry) { return entry.second.endpoint == endpoint; });
        if (discovered == desired.end ()) {
            desired.emplace (manual_connection_key (endpoint),
                             manual_descriptor (channel.snapshot.name, endpoint));
        }
    }
    std::vector<std::shared_ptr<raw_client_server_client_t>> close;
    for (const auto &[key, descriptor] : desired) {
        bool exists = false;
        {
            co_await _lane.run_task ([&] {
                const auto found = channel.connections.find (key);
                if (found != channel.connections.end ()) {
                    if (found->second.descriptor.endpoint != descriptor.endpoint
                        || found->second.descriptor.server_rid != descriptor.server_rid
                        || found->second.descriptor.lifecycle_generation
                             != descriptor.lifecycle_generation
                        || found->second.descriptor.weight != descriptor.weight
                        || found->second.descriptor.state != descriptor.state) {
                        channel.selector_dirty = true;
                        found->second.descriptor = descriptor;
                    }
                    exists = true;
                } else if (!key.starts_with ("manual|")) {
                    const auto manual =
                      channel.connections.find (manual_connection_key (descriptor.endpoint));
                    if (manual != channel.connections.end ()) {
                        auto connection = std::move (manual->second);
                        channel.selector_dirty = true;
                        channel.connections.erase (manual);
                        connection.descriptor = descriptor;
                        channel.connections.emplace (key, std::move (connection));
                        exists = true;
                    }
                }
                return true;
            });
        }
        if (exists)
            continue;
        protocol::client_server_client_admission_t admission;
        admission.channel_name = channel.snapshot.name;
        admission.security_identity = std::string (default_security_identity);
        admission.effective_max_message_bytes =
          effective_max_message_bytes (channel.snapshot.client);
        auto expected =
          key.starts_with ("manual|")
            ? protocol::client_server_server_admission_t{.channel_name = channel.snapshot.name,
                                                         .security_identity =
                                                           std::string (default_security_identity),
                                                         .effective_max_message_bytes =
                                                           admission.effective_max_message_bytes,
                                                         .advertised_endpoint = descriptor.endpoint}
            : to_admission (descriptor, admission.effective_max_message_bytes);
        raw_client_server_client_options_t options{channel.routing_id, admission,
                                                   std::move (expected)};
        options.transport_poller = _transport_poller.get ();
        options.transport_poller_slot = next_transport_poller_slot ();
        options.application_jobs = _application_jobs;
        options.send_timeout = channel.snapshot.client.send_timeout;
        auto raw = std::make_shared<raw_client_server_client_t> (std::move (options),
                                                                 _channel_runtime.core_context ());
        co_await raw->start_task ();
        co_await _lane.run_task ([&] {
            channel.selector_dirty = true;
            channel.connections.emplace (key, client_connection_t{descriptor, std::move (raw)});
            return true;
        });
    }

    auto stale = co_await _lane.run_task ([&] {
        std::vector<std::pair<std::string, std::shared_ptr<raw_client_server_client_t>>> result;
        for (const auto &[key, connection] : channel.connections) {
            if (desired.contains (key))
                continue;
            const auto stable = stable_key (connection.descriptor);
            const auto replacement =
              std::find_if (channel.connections.begin (), channel.connections.end (),
                            [&] (const auto &candidate) {
                                return desired.contains (candidate.first)
                                       && stable_key (candidate.second.descriptor) == stable;
                            });
            result.emplace_back (
              key, replacement == channel.connections.end () ? nullptr : replacement->second.owner);
        }
        return result;
    });
    std::vector<bool> remove;
    remove.reserve (stale.size ());
    for (const auto &[_, replacement] : stale) {
        bool remove_stale = !replacement;
        if (replacement)
            remove_stale = co_await replacement->ready_task ();
        remove.push_back (remove_stale);
    }
    if (!stale.empty ()) {
        co_await _lane.run_task ([&] {
            for (std::size_t i = 0; i < stale.size (); ++i) {
                if (!remove[i])
                    continue;
                const auto found = channel.connections.find (stale[i].first);
                close.push_back (found->second.owner);
                channel.selector_dirty = true;
                channel.connections.erase (found);
            }
            return true;
        });
    }
    for (auto &owner : close)
        co_await owner->close_task ();
}

task_t<void> client_server_location_runtime_t::pump ()
{
    /* The worker uses the same client snapshot for pumping and liveness scheduling. */
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto take_completed =
      [] (const std::shared_ptr<pump_task_state_t> &state) -> std::optional<result_t<void>> {
        if (!state)
            return std::nullopt;
        std::lock_guard lock (state->mutex);
        return state->completion;
    };
    const auto start_task = [wake = _wake_timer] (task_t<void> pending) {
        auto state = std::make_shared<pump_task_state_t> ();
        state->task = std::make_shared<task_t<void>> (std::move (pending));
        detail::observe_task_terminal (*state->task, [state, wake] (const result_t<void> &result) {
            {
                std::lock_guard lock (state->mutex);
                state->completion = result;
            }
            wake->signal ();
        });
        return state;
    };

    _server_pump_snapshot.clear ();
    _server_pump_snapshot.reserve (_servers.size ());
    for (auto &[_, server] : _servers)
        _server_pump_snapshot.push_back (server.get ());
    if (!_server_pump_snapshot.empty ()) {
        const auto start = _server_pump_cursor % _server_pump_snapshot.size ();
        for (std::size_t offset = 0; offset < _server_pump_snapshot.size (); ++offset) {
            auto &server = *_server_pump_snapshot[(start + offset) % _server_pump_snapshot.size ()];
            if (const auto completed = take_completed (server.pump_task)) {
                if (!*completed)
                    _locations->record_store_error ();
                server.pump_task.reset ();
            }
            if (!server.pump_task) {
                std::shared_ptr<application_job_queue_t::permit_t> application_permit;
                _application_supply->ensure_waiter ();
                auto reserved = _application_supply->take ();
                const bool may_pump = reserved.has_value ();
                if (reserved) {
                    application_permit =
                      std::make_shared<application_job_queue_t::permit_t> (std::move (*reserved));
                }
                if (may_pump) {
                    server.pump_task = start_task (
                      pump_server_transport (server.owner, now, std::move (application_permit)));
                }
            }
            if (const auto completed = take_completed (server.dispatch_task)) {
                if (!*completed)
                    _locations->record_store_error ();
                server.dispatch_task.reset ();
            }
            if (!server.dispatch_task) {
                server.dispatch_task = start_task (dispatch_server (server.owner));
            }
        }
        _server_pump_cursor = (start + 1) % _server_pump_snapshot.size ();
    }
    if (!_client_pump_snapshot.empty ()) {
        const auto start = _client_pump_cursor % _client_pump_snapshot.size ();
        for (std::size_t offset = 0; offset < _client_pump_snapshot.size (); ++offset) {
            auto &connection =
              *_client_pump_snapshot[(start + offset) % _client_pump_snapshot.size ()];
            if (const auto completed = take_completed (connection.pump_task)) {
                if (!*completed)
                    _locations->record_store_error ();
                connection.pump_task.reset ();
            }
            if (!connection.pump_task) {
                connection.pump_task = start_task (pump_client_transport (connection.owner, now));
            }
        }
        _client_pump_cursor = (start + 1) % _client_pump_snapshot.size ();
    }
    return complete_ready_waiters ();
}

task_t<client_server_location_runtime_t::worker_lane_snapshot_t>
client_server_location_runtime_t::refresh_client_pump_snapshot ()
{
    auto snapshot = co_await _lane.run_task ([this] {
        worker_lane_snapshot_t result;
        for (auto &[_, channel] : _clients) {
            for (auto &[__, connection] : channel->connections) {
                result.connections.push_back (&connection);
                result.owners.push_back (connection.owner);
            }
        }
        for (auto &[_, server] : _servers)
            result.servers.push_back (server->owner);
        for (const auto &waiter : _ready_waiters) {
            if (!result.ready_deadline || waiter->deadline < *result.ready_deadline)
                result.ready_deadline = waiter->deadline;
        }
        return result;
    });
    std::vector<bool> ready;
    ready.reserve (snapshot.owners.size ());
    const auto include_activity = [&snapshot] (auto activity) {
        if (activity && (!snapshot.next_activity || *activity < *snapshot.next_activity))
            snapshot.next_activity = *activity;
    };
    for (const auto &owner : snapshot.owners) {
        const auto status = co_await owner->pump_status_task ();
        ready.push_back (status.ready);
        include_activity (status.next_activity);
    }
    for (const auto &server : snapshot.servers)
        include_activity (co_await server->next_liveness_activity_task ());
    co_await _lane.run_task ([this, &snapshot, ready = std::move (ready)] {
        std::map<raw_client_server_client_t *, bool> current_ready;
        for (std::size_t i = 0; i < snapshot.owners.size (); ++i)
            current_ready.emplace (snapshot.owners[i].get (), ready[i]);
        for (auto &[_, channel] : _clients) {
            for (auto &[__, connection] : channel->connections) {
                const auto status = current_ready.find (connection.owner.get ());
                if (status == current_ready.end ())
                    continue;
                const bool selectable = status->second;
                if (selectable != connection.selector_ready) {
                    connection.selector_ready = selectable;
                    channel->selector_dirty = true;
                }
            }
        }
        return true;
    });
    co_return snapshot;
}

task_t<void> client_server_location_runtime_t::dispatch_server (
  std::shared_ptr<raw_client_server_server_t> owner)
{
    auto &mailbox = owner->mailbox ();
    receive_batch_budget_t budget;
    for (;;) {
        if (!budget.can_receive ())
            co_return;
        auto claim = mailbox.try_claim (mesh::service_mailbox_domain_t::application,
                                        dispatch_limits::receive_batch_messages,
                                        dispatch_limits::receive_batch_bytes);
        if (!claim)
            co_return;
        for (const auto &record : claim->records) {
            std::size_t record_bytes = 0;
            for (const auto &part : record.parts)
                record_bytes += part.size ();
            budget.account (record_bytes);
        }
        const bool yield_after_claim = budget.exhausted ();
        for (const auto &record : claim->records) {
            if (record.parts.size () != 2)
                continue;
            std::optional<protocol::application_payload_t> pending_reply;
            std::optional<framework_exception_t> pending_failure_reply;
            try {
                /* ClientServer application records ride the channel
                 * envelope: [JSON header, payload]. flow-correlation §4: at
                 * Off the wire flow pair is neither validated nor
                 * materialized at this ingress. */
                auto envelope_header = runtime::messaging::envelope_codec_t{}.decode_header (
                  zlink::message_t::from (record.parts[0]),
                  detail::message_flow_tracer_t (_channel_runtime.dispatch_options_ref ())
                    .capture_enabled ());
                if (!envelope_header) {
                    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                 envelope_header.error () != nullptr
                                                   ? envelope_header.error ()->what ()
                                                   : "ClientServer request envelope is malformed");
                }
                auto &request_envelope = envelope_header.value ();
                const protocol::application_payload_t payload{
                  request_envelope.message_name, request_envelope.content_type, record.parts[1],
                  request_envelope.flow_id, request_envelope.flow_origin};
                const auto message = zlink::message_t::from (payload.payload_bytes ());
                detail::inbound_message_context_t inbound;
                inbound.before_application_handler = record.before_application_handler;
                inbound.message.channel_name = record.owner;
                inbound.message.packet_name = payload.packet_name;
                inbound.message.content_type = payload.content_type;
                inbound.message.metadata =
                  message_metadata_t (std::move (request_envelope.metadata));
                if (!request_envelope.correlation_id.empty ())
                    inbound.message.correlation_id = request_envelope.correlation_id;
                detail::message_flow_tracer_t flow (_channel_runtime.dispatch_options_ref ());
                auto flow_scope = runtime::flow_context_t::enter (
                  payload.flow_id, payload.flow_origin, flow.mode (), flow_origin_t::inbound,
                  std::nullopt);
                flow.trace (message_flow_outcome_t::received, [&] {
                    return message_flow_event_t{message_flow_outcome_t::received,
                                                dispatch_error_surface_t::channel,
                                                record.reply_token
                                                  ? dispatch_message_kind_t::request
                                                  : dispatch_message_kind_t::send,
                                                payload.packet_name,
                                                record.owner,
                                                std::nullopt,
                                                inbound.message.correlation_id,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt};
                });
                flow.trace (message_flow_outcome_t::admitted, [&] {
                    return message_flow_event_t{message_flow_outcome_t::admitted,
                                                dispatch_error_surface_t::channel,
                                                record.reply_token
                                                  ? dispatch_message_kind_t::request
                                                  : dispatch_message_kind_t::send,
                                                payload.packet_name,
                                                record.owner,
                                                std::nullopt,
                                                inbound.message.correlation_id,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt};
                });
                auto scope = zlink::framework::detail::service_scope_t::create (
                  _services, zlink::framework::detail::service_scope_kind_t::handler_invocation);
                if (record.reply_token) {
                    auto reply = co_await _channel_runtime.dispatch_request_async (
                      record.owner, {}, payload.packet_name, scope.provider (), *_serializers,
                      *_handlers, message, inbound);
                    if (reply) {
                        pending_reply.emplace (protocol::application_payload_t{
                          payload.packet_name,
                          std::string (runtime::messaging::envelope_codec_t::default_content_type),
                          reply.value ().to_bytes ()});
                    } else {
                        const framework_exception_t error (
                          reply.error_kind (), reply.error () != nullptr
                                                 ? reply.error ()->what ()
                                                 : "ClientServer request handler failed");
                        report_client_server_dispatch_error (
                          _channel_runtime.dispatch_options_ref (), record, payload.packet_name,
                          dispatch_message_kind_t::request, dispatch_error_action_t::reply_error,
                          error);
                        pending_failure_reply = error;
                    }
                } else {
                    try {
                        co_await _channel_runtime.dispatch_send_async (
                          record.owner, {}, payload.packet_name, scope.provider (), *_serializers,
                          *_handlers, message, inbound);
                        flow.trace (message_flow_outcome_t::completed, [&] {
                            return message_flow_event_t{message_flow_outcome_t::completed,
                                                        dispatch_error_surface_t::channel,
                                                        dispatch_message_kind_t::send,
                                                        payload.packet_name,
                                                        record.owner,
                                                        std::nullopt,
                                                        inbound.message.correlation_id,
                                                        std::nullopt,
                                                        std::nullopt,
                                                        std::nullopt,
                                                        std::nullopt};
                        });
                    }
                    catch (const framework_exception_t &error) {
                        report_client_server_dispatch_error (
                          _channel_runtime.dispatch_options_ref (), record, payload.packet_name,
                          dispatch_message_kind_t::send, dispatch_error_action_t::drop, error);
                    }
                }
            }
            catch (const framework_exception_t &error) {
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (), record,
                  record.parts.size () > 1 ? zlink::framework::detail::diagnostic_decoded_value
                                           : zlink::framework::detail::diagnostic_absent_value,
                  record.reply_token ? dispatch_message_kind_t::request
                                     : dispatch_message_kind_t::send,
                  record.reply_token ? dispatch_error_action_t::reply_error
                                     : dispatch_error_action_t::drop,
                  error);
                if (record.reply_token) {
                    pending_reply.reset ();
                    pending_failure_reply = error;
                }
            }
            catch (const std::exception &error) {
                const framework_exception_t failure (framework_error_kind_t::internal_failure,
                                                     error.what ());
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (), record,
                  record.parts.size () > 1 ? zlink::framework::detail::diagnostic_decoded_value
                                           : zlink::framework::detail::diagnostic_absent_value,
                  record.reply_token ? dispatch_message_kind_t::request
                                     : dispatch_message_kind_t::send,
                  record.reply_token ? dispatch_error_action_t::reply_error
                                     : dispatch_error_action_t::drop,
                  failure);
                if (record.reply_token) {
                    pending_reply.reset ();
                    pending_failure_reply = failure;
                }
            }
            catch (...) {
                const framework_exception_t failure (framework_error_kind_t::internal_failure,
                                                     "ClientServer dispatch failed");
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (), record,
                  record.parts.size () > 1 ? zlink::framework::detail::diagnostic_decoded_value
                                           : zlink::framework::detail::diagnostic_absent_value,
                  record.reply_token ? dispatch_message_kind_t::request
                                     : dispatch_message_kind_t::send,
                  record.reply_token ? dispatch_error_action_t::reply_error
                                     : dispatch_error_action_t::drop,
                  failure);
                if (record.reply_token) {
                    pending_reply.reset ();
                    pending_failure_reply = failure;
                }
            }
            try {
                if (pending_reply)
                    (void) co_await owner->reply (record, *pending_reply);
                else if (pending_failure_reply)
                    (void) co_await owner->reply (record, *pending_failure_reply);
            }
            catch (...) {
                detail::dispatch_error_reporter_t (_channel_runtime.dispatch_options_ref ())
                  .report_lazy ([&] {
                      return message_dispatch_error_event_t{
                        dispatch_error_surface_t::channel,
                        dispatch_message_kind_t::request,
                        dispatch_error_reason_t::reply_path_missing,
                        dispatch_error_action_t::drop,
                        std::nullopt,
                        record.owner,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::current_exception ()};
                  });
            }
        }
        (void) mailbox.release (*claim);
        if (yield_after_claim)
            co_return;
    }
}

task_t<void> client_server_location_runtime_t::send (const std::string &channel_name,
                                                     std::string packet_name,
                                                     std::string content_type,
                                                     zlink::message_t message,
                                                     std::map<std::string, std::string> metadata)
{
    auto selected = co_await select_ready (channel_name);
    const auto submitted = co_await selected->send (
      protocol::application_payload_t{std::move (packet_name), std::move (content_type),
                                      message.to_bytes ()},
      std::move (metadata));
    if (submitted == zlink::submit_result_t::backpressured) {
        throw detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                               "ClientServer send timed out");
    }
    if (submitted != zlink::submit_result_t::ok) {
        throw runtime::messaging::map_submit_result_exception (submitted,
                                                               "ClientServer send failed");
    }
    co_return;
}

task_t<zlink::message_t>
client_server_location_runtime_t::request (const std::string &channel_name,
                                           std::string packet_name,
                                           std::string content_type,
                                           zlink::message_t message,
                                           std::chrono::milliseconds timeout,
                                           std::map<std::string, std::string> metadata)
{
    const auto effective =
      timeout > std::chrono::milliseconds::zero () ? timeout : std::chrono::seconds (30);
    std::shared_ptr<raw_client_server_client_t> selected;
    try {
        selected = co_await select_ready (channel_name);
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
    const auto completion = co_await selected->request (
      protocol::application_payload_t{std::move (packet_name), std::move (content_type),
                                      message.to_bytes ()},
      effective, std::move (metadata));
    if (completion.terminal != foundation::operation_terminal_t::completed) {
        co_return detail::result_access_t::failure<zlink::message_t> (
          client_server_operation_exception (completion.terminal, "ClientServer request"));
    }
    if (completion.error_code) {
        const auto error = runtime::messaging::request_failure_mapper_t{}.error_header_exception (
          *completion.error_code, completion.error_message.value_or ("ClientServer request failed"),
          "ClientServer request");
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
    /* The response envelope body is the reply payload as-is. */
    co_return zlink::message_t::from (completion.payload);
}

task_t<std::shared_ptr<raw_client_server_client_t>>
client_server_location_runtime_t::select_ready (std::string channel_name)
{
    using client_t = std::shared_ptr<raw_client_server_client_t>;
    using completion_t = task_completion_source_t<client_t>;
    using selection_t = std::variant<result_t<client_t>, std::shared_ptr<completion_t>>;
    auto selected = co_await _lane.run_task (
      [this, channel_name = std::move (channel_name)] () mutable -> selection_t {
          const auto channel = select_channel_locked (channel_name);
          if (!channel)
              return selection_t (
                std::in_place_index<0>,
                result_t<client_t>::failure (channel.error_kind (), channel.error ()->what ()));
          const auto deadline =
            std::chrono::steady_clock::now ()
            + detail::channel_send_timeout (channel.value ()->snapshot.client.send_timeout);
          auto result = select_ready_locked (channel_name, deadline);
          if (result || result.error_kind () != framework_error_kind_t::not_found)
              return selection_t (std::in_place_index<0>, std::move (result));
          auto completion = std::make_shared<completion_t> ();
          auto waiter = std::make_unique<ready_waiter_t> ();
          waiter->channel_name = std::move (channel_name);
          waiter->deadline = deadline;
          waiter->completion = completion;
          _ready_waiters.push_back (std::move (waiter));
          return selection_t (std::in_place_index<1>, std::move (completion));
      });
    if (std::holds_alternative<result_t<client_t>> (selected)) {
        auto result = std::get<result_t<client_t>> (std::move (selected));
        if (!result)
            throw framework_exception_t (result.error_kind (), result.error ()->what ());
        co_return std::move (result.value ());
    }
    _wake_timer->signal ();
    co_return co_await std::get<std::shared_ptr<completion_t>> (selected)->task ();
}

result_t<client_server_location_runtime_t::client_channel_t *>
client_server_location_runtime_t::select_channel_locked (const std::string &channel_name)
{
    if (_stop.load (std::memory_order_acquire)) {
        return result_t<client_channel_t *>::failure (framework_error_kind_t::shutting_down,
                                                      "ClientServer runtime is stopping");
    }
    const auto channel_it = _clients.find (channel_name);
    if (channel_it == _clients.end ()) {
        return result_t<client_channel_t *>::failure (
          framework_error_kind_t::not_configured,
          "ClientServer Client role is not registered for this channel");
    }
    return result_t<client_channel_t *>::success (channel_it->second.get ());
}

result_t<std::shared_ptr<raw_client_server_client_t>>
client_server_location_runtime_t::select_ready_locked (
  const std::string &channel_name, std::chrono::steady_clock::time_point deadline)
{
    const auto found = select_channel_locked (channel_name);
    if (!found)
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          found.error_kind (), found.error ()->what ());
    if (std::chrono::steady_clock::now () >= deadline)
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          framework_error_kind_t::deadline_exceeded, "ClientServer admission deadline expired");
    auto &channel = *found.value ();
    if (channel.selector_dirty) {
        channel.selector_candidates.clear ();
        channel.selector_candidates.reserve (channel.connections.size ());
        for (auto &[key, connection] : channel.connections) {
            if (!connection.selector_ready
                || connection.descriptor.state != framework_runtime_state_t::serving)
                continue;
            channel.selector_candidates.push_back (
              {key, static_cast<std::uint32_t> (connection.descriptor.weight),
               stable_key (connection.descriptor)});
        }
        channel.selector.set_candidates (channel.selector_candidates);
        channel.selector_dirty = false;
    }
    const auto selected = channel.selector.select ();
    if (!selected) {
        const bool has_ready_target =
          std::any_of (channel.connections.begin (), channel.connections.end (),
                       [] (const auto &entry) { return entry.second.selector_ready; });
        const auto kind = has_ready_target ? framework_error_kind_t::unavailable
                                           : framework_error_kind_t::not_found;
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          kind, "ClientServer has no selectable target snapshot");
    }
    const auto connection = channel.connections.find (*selected);
    if (connection == channel.connections.end ()) {
        channel.selector_dirty = true;
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          framework_error_kind_t::unavailable,
          "ClientServer selected target is no longer registered");
    }
    return result_t<std::shared_ptr<raw_client_server_client_t>>::success (
      connection->second.owner);
}

task_t<void> client_server_location_runtime_t::complete_ready_waiters ()
{
    using client_t = std::shared_ptr<raw_client_server_client_t>;
    using completion_t = task_completion_source_t<client_t>;
    std::vector<std::pair<std::shared_ptr<completion_t>, result_t<client_t>>> completed;
    completed = co_await _lane.run_task ([this] {
        std::vector<std::pair<std::shared_ptr<completion_t>, result_t<client_t>>> result;
        auto write = _ready_waiters.begin ();
        for (auto read = _ready_waiters.begin (); read != _ready_waiters.end (); ++read) {
            auto selected = select_ready_locked ((*read)->channel_name, (*read)->deadline);
            const bool terminal =
              selected || selected.error_kind () != framework_error_kind_t::not_found;
            if (!terminal) {
                if (write != read)
                    *write = std::move (*read);
                ++write;
                continue;
            }
            result.emplace_back ((*read)->completion, std::move (selected));
        }
        _ready_waiters.erase (write, _ready_waiters.end ());
        return result;
    });
    for (auto &entry : completed)
        entry.first->complete (std::move (entry.second));
}

void client_server_location_runtime_t::stop () noexcept
{
    const bool was_stopped = _stop.exchange (true, std::memory_order_acq_rel);
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        _descriptor_publish_result = false;
        _descriptor_publish_pending = false;
    }
    _descriptor_publish_changed.notify_all ();
    _wake_timer->signal ();
    complete_ready_waiters ().result ().value ();
    if (_thread.joinable ())
        runtime::infrastructure_wait_guard::join (_thread, "client-server-location/worker");
    if (_application_supply) {
        _application_supply->close ();
        _application_supply.reset ();
    }
    _wake_timer->detach ();
    std::vector<std::string> client_channels;
    bool has_servers = false;
    bool has_clients = false;
    _lane
      .run_checked ([this, &client_channels, &has_servers, &has_clients] {
          client_channels.reserve (_clients.size ());
          for (const auto &[channel_name, _] : _clients)
              client_channels.push_back (channel_name);
          has_servers = !_servers.empty ();
          has_clients = !_clients.empty ();
      })
      .get ();
    if (!was_stopped || has_servers || has_clients) {
        stop_clients ();
        stop_servers ();
    }
    for (const auto &channel_name : client_channels)
        _channel_runtime.unbind_client_server_transport (channel_name);
    if (_transport_poller) {
        try {
            _transport_poller->close ();
        }
        catch (...) {
        }
    }
    _lane.run_checked ([this] { _transport_poller.reset (); }).get ();
    if (!was_stopped || has_servers || has_clients)
        publish_snapshot_changes ().result ().value ();
}

void client_server_location_runtime_t::stop_clients () noexcept
{
    std::vector<std::shared_ptr<raw_client_server_client_t>> clients;
    _lane
      .run_checked ([this, &clients] {
          for (auto &[_, channel] : _clients) {
              for (auto &[__, connection] : channel->connections)
                  clients.push_back (connection.owner);
              channel->connections.clear ();
          }
          _clients.clear ();
          _client_pump_snapshot.clear ();
      })
      .get ();
    for (auto &client : clients)
        client->close ();
}

void client_server_location_runtime_t::stop_servers () noexcept
{
    std::map<std::string, std::unique_ptr<server_entry_t>> servers;
    _lane
      .run_checked ([this, &servers] {
          servers.swap (_servers);
          _server_pump_snapshot.clear ();
      })
      .get ();
    for (auto &[channel_name, server] : servers) {
        if (!server->published_descriptor) {
            server->owner->close ();
            if (_listener_statuses)
                _listener_statuses->remove (listener_kind_t::client_server, channel_name);
            continue;
        }
        try {
            auto admission = server->owner->descriptor ();
            if (admission.state != mesh::service_node_state_t::draining) {
                ++admission.descriptor_revision;
                admission.state = mesh::service_node_state_t::draining;
                admission.weight = 0;
                auto draining = *server->published_descriptor;
                draining.descriptor_revision = admission.descriptor_revision;
                draining.state = framework_runtime_state_t::draining;
                draining.weight = 0;
                const auto written =
                  _store->update_client_server (draining, location_write_intent_t::renew)
                    .result ()
                    .value ();
                if (written.status == location_write_status_t::stored)
                    server->published_descriptor = std::move (draining);
            }
            (void) _store
              ->remove_client_server ({server->published_descriptor->channel_name,
                                       server->published_descriptor->server_rid},
                                      {server->published_descriptor->owner_id,
                                       server->published_descriptor->lease_generation})
              .result ()
              .value ();
        }
        catch (...) {
        }
        server->owner->close ();
        if (_listener_statuses)
            _listener_statuses->remove (listener_kind_t::client_server, channel_name);
    }
}

std::uint64_t client_server_location_runtime_t::make_lifecycle_generation ()
{
    static std::atomic_uint64_t counter{1};
    const auto random = (static_cast<std::uint64_t> (std::random_device{}()) << 32u)
                        ^ static_cast<std::uint64_t> (std::random_device{}());
    const auto time =
      static_cast<std::uint64_t> (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    auto value = (random ^ time ^ counter.fetch_add (1))
                 & static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    return value == 0 ? 1 : value;
}

std::uint32_t client_server_location_runtime_t::effective_max_message_bytes (
  const channel_capability_snapshot_t &capability)
{
    if (!capability.max_message_size || capability.max_message_size->bytes () <= 0)
        return default_effective_max_message_bytes;
    return static_cast<std::uint32_t> (std::min<std::int64_t> (
      capability.max_message_size->bytes (), std::numeric_limits<std::uint32_t>::max ()));
}

std::vector<std::uint8_t>
client_server_location_runtime_t::client_routing_id (const channel_snapshot_t &channel)
{
    if (channel.client.routing_id)
        return channel.client.routing_id->to_bytes ();
    return zlink::routing_id_t::from (channel.name
                                      + ":client:" + std::to_string (make_lifecycle_generation ()))
      .to_bytes ();
}

std::vector<std::uint8_t>
client_server_location_runtime_t::server_routing_id (const channel_snapshot_t &channel)
{
    if (channel.server.routing_id)
        return channel.server.routing_id->to_bytes ();
    return zlink::routing_id_t::from (channel.name
                                      + ":server:" + std::to_string (make_lifecycle_generation ()))
      .to_bytes ();
}

protocol::client_server_server_admission_t
client_server_location_runtime_t::to_admission (const client_server_server_descriptor_t &descriptor,
                                                std::uint32_t effective_max_message_bytes)
{
    protocol::client_server_server_admission_t admission;
    admission.channel_name = descriptor.channel_name;
    admission.server_routing_id = descriptor.server_rid.to_bytes ();
    admission.lifecycle_generation = descriptor.lifecycle_generation;
    admission.descriptor_revision = descriptor.descriptor_revision;
    admission.weight = static_cast<std::uint32_t> (descriptor.weight);
    admission.state = client_server_service_state (descriptor.state);
    admission.security_identity = descriptor.security_identity;
    admission.effective_max_message_bytes = effective_max_message_bytes;
    admission.advertised_endpoint = descriptor.endpoint;
    return admission;
}

client_server_server_descriptor_t client_server_location_runtime_t::to_descriptor (
  const protocol::client_server_server_admission_t &admission, const location_owner_token_t &owner)
{
    return client_server_server_descriptor_t{
      .channel_name = admission.channel_name,
      .server_rid = zlink::routing_id_t::from (admission.server_routing_id),
      .lifecycle_generation = admission.lifecycle_generation,
      .descriptor_revision = admission.descriptor_revision,
      .endpoint = admission.advertised_endpoint,
      .weight = static_cast<int> (admission.weight),
      .state = client_server_framework_state (admission.state),
      .security_identity = admission.security_identity,
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
}

task_t<bool> client_server_location_runtime_t::owner_is_live_task (
  client_server_server_descriptor_t descriptor) const
{
    const auto lease = co_await _leases->read_owner_lease (descriptor.owner_id);
    const auto *found = std::get_if<owner_lease_found_t> (&lease);
    co_return found != nullptr && found->token.owner_id == descriptor.owner_id
      && found->token.lease_generation == descriptor.lease_generation
      && found->lease_expires_at > found->store_now;
}

} // namespace zlink::framework::runtime::client_server
