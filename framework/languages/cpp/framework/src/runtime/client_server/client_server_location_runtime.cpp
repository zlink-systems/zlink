/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/eventing/runtime_wake_pipe.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime::client_server
{

namespace
{

constexpr std::string_view default_security_identity = "default";
constexpr auto maximum_ready_wait = std::chrono::seconds (5);
constexpr std::uint32_t default_effective_max_message_bytes =
  static_cast<std::uint32_t> (
    std::numeric_limits<std::int32_t>::max ());

std::atomic<std::uintptr_t> next_client_server_poller_slot{1};

std::uintptr_t next_transport_poller_slot () noexcept
{
    auto slot = next_client_server_poller_slot.fetch_add (
      1, std::memory_order_relaxed);
    if (slot == 0) {
        slot = next_client_server_poller_slot.fetch_add (
          1, std::memory_order_relaxed);
    }
    return slot;
}

std::string connection_key (
  const client_server_server_descriptor_t &descriptor)
{
    return descriptor.server_rid.to_hex () + "|"
           + std::to_string (descriptor.lifecycle_generation);
}

std::string stable_key (
  const client_server_server_descriptor_t &descriptor)
{
    return descriptor.server_rid.to_hex ();
}

struct client_server_wire_failure_t
{
    std::uint32_t terminal_result;
    protocol::framework_error_code failure_code;
};

client_server_wire_failure_t
wire_failure (const framework_exception_t &error)
{
    switch (error.kind ()) {
        case framework_error_kind_t::unavailable:
            return {
              105, protocol::framework_error_code::routeNotConnected};
        case framework_error_kind_t::not_found:
            return {
              102,
              protocol::framework_error_code::requestTargetNotFound};
        case framework_error_kind_t::rejected:
            return {
              106, protocol::framework_error_code::requestRejected};
        case framework_error_kind_t::protocol_error:
            return {
              104,
              protocol::framework_error_code::requestProtocolError};
        default:
            return {105, protocol::framework_error_code::requestFailed};
    }
}

void report_client_server_dispatch_error (
  const dispatch_options_t &options,
  const mesh::service_mailbox_record_t &record,
  std::string_view packet_name,
  dispatch_message_kind_t message_kind,
  dispatch_error_action_t action,
  const framework_exception_t &error) noexcept
{
    std::optional<std::string> correlation;
    if (record.correlation)
        correlation = std::to_string (*record.correlation);
    zlink::framework::detail::dispatch_error_reporter_t (options)
      .report (message_dispatch_error_event_t{
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
        correlation,
        std::make_exception_ptr (error)});
}

std::string manual_connection_key (std::string_view endpoint)
{
    return "manual|" + std::string (endpoint);
}

void trace_client_server_runtime_failure (
  std::string_view stage,
  std::string_view error) noexcept
{
    try {
        const auto *trace = std::getenv ("ZLINK_CPP_CLIENT_SERVER_TRACE");
        if (trace == nullptr || std::string_view (trace) == "0"
            || std::string_view (trace).empty ())
            return;
        std::cerr << "zlink-cpp-client-server-trace stage=" << stage
                  << " error=" << error << std::endl;
    }
    catch (...) {
    }
}

client_server_server_descriptor_t manual_descriptor (
  std::string_view channel_name,
  std::string_view endpoint)
{
    return client_server_server_descriptor_t{
      .channel_name = std::string (channel_name),
      .endpoint = std::string (endpoint),
      .weight = 100,
      .state = framework_runtime_state_t::serving,
      .security_identity = std::string (default_security_identity)};
}

framework_runtime_state_t current_state (
  const location_runtime_t &locations)
{
    return locations.draining ()
             ? framework_runtime_state_t::draining
             : framework_runtime_state_t::serving;
}

} // namespace

mesh::service_node_state_t client_server_service_state (
  framework_runtime_state_t state)
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
            throw std::invalid_argument (
              "invalid framework ClientServer runtime state");
    }
}

framework_runtime_state_t client_server_framework_state (
  mesh::service_node_state_t state)
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
            throw std::invalid_argument (
              "invalid service ClientServer runtime state");
    }
}

struct client_server_location_runtime_t::server_entry_t
{
    channel_capability_snapshot_t capability;
    std::unique_ptr<raw_client_server_server_t> owner;
    std::optional<client_server_server_descriptor_t> published_descriptor;
};

struct client_server_location_runtime_t::client_connection_t
{
    client_server_server_descriptor_t descriptor;
    std::shared_ptr<raw_client_server_client_t> owner;
    bool selector_ready = false;
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

client_server_server_state_t snapshot_state (
  framework_runtime_state_t state,
  bool transport_ready)
{
    switch (state) {
        case framework_runtime_state_t::preparing:
            return client_server_server_state_t::configured;
        case framework_runtime_state_t::serving:
            return transport_ready ? client_server_server_state_t::ready
                                    : client_server_server_state_t::connecting;
        case framework_runtime_state_t::relocating:
        case framework_runtime_state_t::relocated:
        case framework_runtime_state_t::draining:
            return client_server_server_state_t::draining;
        case framework_runtime_state_t::stopped:
            return client_server_server_state_t::disconnected;
        case framework_runtime_state_t::error:
            return client_server_server_state_t::rejected;
    }
    return client_server_server_state_t::rejected;
}

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
  std::map<std::string, std::string> advertise_hosts) :
    _bus (std::move (bus)),
    _channel_runtime (detail::channel_runtime_t::from (_bus)),
    _channels (std::move (channels)),
    _locations (&locations),
    _store (&store),
    _leases (&leases),
    _services (services),
    _serializers (&serializers),
    _handlers (&handlers),
    _advertise_hosts (std::move (advertise_hosts))
{
}

client_server_location_runtime_t::~client_server_location_runtime_t () noexcept
{
    stop ();
}

bool client_server_location_runtime_t::empty () const noexcept
{
    return std::none_of (
      _channels.begin (), _channels.end (), [] (const auto &channel) {
          return (channel.server.enabled
                  && !channel.server.bind_endpoints.empty ())
                 || (channel.client.enabled
                     && (channel.client.discovery
                         || !channel.client.connect_endpoints.empty ()));
      });
}

client_server_channel_snapshot_t
client_server_location_runtime_t::build_snapshot_locked (
  const std::string &channel_name) const
{
    client_server_channel_snapshot_t result;
    result.channel_name = channel_name;
    result.observed_at = std::chrono::system_clock::now ();
    const auto configured = std::find_if (
      _channels.begin (), _channels.end (), [&] (const auto &channel) {
          return channel.name == channel_name;
      });
    if (configured == _channels.end ()) {
        result.location.store_healthy = false;
        return result;
    }

    result.local_role = local_role (*configured);
    if (_locations != nullptr) {
        const auto error = _locations->last_error ();
        result.location.store_healthy = !error.has_value ();
        result.location.last_refresh_at =
          _locations->owner_lease_renewed_at ();
        result.location.owner_lease_healthy =
          _locations->owner_lease_healthy ();
        result.location.owner_lease_renewed_at =
          _locations->owner_lease_renewed_at ();
    }

    const auto to_count = [] (std::size_t value) {
        return value > static_cast<std::size_t> (
                          std::numeric_limits<int>::max ())
                 ? std::numeric_limits<int>::max ()
                 : static_cast<int> (value);
    };
    const auto append_server = [&] (
      client_server_server_snapshot_t server,
      bool count_as_client_target) {
        if (count_as_client_target && server.ready && server.weight > 0)
            ++result.ready_server_count;
        result.servers.push_back (std::move (server));
    };

    std::set<std::string> client_target_identities;
    const auto client = _clients.find (channel_name);
    if (client != _clients.end ()) {
        result.connection_intent_count =
          to_count (client->second->connections.size ());
        for (const auto &[key, connection] : client->second->connections) {
            const bool ready =
              connection.owner->ready ()
              && connection.descriptor.state
                   == framework_runtime_state_t::serving
              && connection.descriptor.weight > 0;
            client_server_server_snapshot_t snapshot{
              .server_rid = connection.descriptor.server_rid,
              .lifecycle_generation =
                connection.descriptor.lifecycle_generation,
              .weight = connection.descriptor.weight,
              .ready = ready,
              .state = snapshot_state (
                connection.descriptor.state, ready),
              .descriptor_source =
                key.starts_with ("manual|") ? "manual" : "location_store",
              .last_failure = std::nullopt};
            client_target_identities.insert (
              snapshot.server_rid.to_hex () + "|"
              + std::to_string (snapshot.lifecycle_generation));
            append_server (std::move (snapshot), true);
            const auto pending = connection.owner->pending_request_count ();
            const auto current = static_cast<std::size_t> (
              std::max (0, result.pending_request_count));
            result.pending_request_count = to_count (
              current + pending < current
                ? static_cast<std::size_t> (
                    std::numeric_limits<int>::max ())
                : current + pending);
        }
    }

    const auto local_server = _servers.find (channel_name);
    if (local_server != _servers.end () && local_server->second->owner) {
        const auto admission = local_server->second->owner->descriptor ();
        const auto local_rid = zlink::routing_id_t::from (
          admission.server_routing_id);
        const auto local_identity =
          local_rid.to_hex () + "|"
          + std::to_string (admission.lifecycle_generation);
        if (!client_target_identities.contains (local_identity)) {
            const auto transport_ready =
              admission.state == mesh::service_node_state_t::serving
              && admission.weight > 0;
            append_server (client_server_server_snapshot_t{
              .server_rid = local_rid,
              .lifecycle_generation = admission.lifecycle_generation,
              .weight = static_cast<int> (admission.weight),
              .ready = transport_ready,
              .state = snapshot_state (
                client_server_framework_state (admission.state),
                transport_ready),
              .descriptor_source = "local",
              .last_failure = std::nullopt},
              false);
        }
    }
    result.selectable = configured->client.enabled
                        && result.ready_server_count > 0;
    const auto sequence = _snapshot_sequences.find (channel_name);
    result.sequence = sequence == _snapshot_sequences.end ()
                        ? 0
                        : sequence->second;
    return result;
}

bool client_server_location_runtime_t::snapshot_equivalent (
  const client_server_channel_snapshot_t &left,
  const client_server_channel_snapshot_t &right) noexcept
{
    return left.channel_name == right.channel_name
           && left.local_role == right.local_role
           && left.selectable == right.selectable
           && left.ready_server_count == right.ready_server_count
           && left.connection_intent_count == right.connection_intent_count
           && left.pending_request_count == right.pending_request_count
           && left.servers == right.servers
           && left.location == right.location;
}

client_server_channel_snapshot_t
client_server_location_runtime_t::snapshot (std::string channel_name) const
{
    std::lock_guard lock (_gate);
    return build_snapshot_locked (channel_name);
}

std::unique_ptr<mesh_runtime_observation_t>
client_server_location_runtime_t::observe (
  std::string channel_name,
  std::size_t capacity,
  std::function<void (
    const observed_status_t<client_server_runtime_event_t> &)> observer)
{
    if (channel_name.empty () || capacity == 0 || !observer)
        throw std::invalid_argument (
          "ClientServer observation requires a channel and callback");
    auto value = std::make_shared<observer_t> (
      capacity, std::move (observer));
    {
        std::lock_guard lock (_gate);
        _observers[channel_name].push_back (value);
        const auto current = build_snapshot_locked (channel_name);
        value->enqueue (client_server_runtime_event_t{
          .identifier = "zlink.runtime.client_server.channel_changed",
          .sequence = current.sequence,
          .timestamp = current.observed_at,
          .channel_name = channel_name,
          .reason = std::string ("initial_snapshot")});
    }
    return std::make_unique<client_server_observation_t> (std::move (value));
}

bool client_server_location_runtime_t::is_ready (std::string channel_name) const
{
    return snapshot (std::move (channel_name)).selectable;
}

void client_server_location_runtime_t::publish_snapshot_changes ()
{
    std::vector<std::pair<std::shared_ptr<observer_t>,
                          client_server_runtime_event_t>> notifications;
    {
        std::lock_guard lock (_gate);
        std::set<std::string> channel_names;
        for (const auto &channel : _channels)
            channel_names.insert (channel.name);
        for (const auto &[channel_name, _] : _servers)
            channel_names.insert (channel_name);
        for (const auto &[channel_name, _] : _clients)
            channel_names.insert (channel_name);

        for (const auto &channel_name : channel_names) {
            auto current = build_snapshot_locked (channel_name);
            const auto previous = _last_snapshots.find (channel_name);
            if (previous != _last_snapshots.end ()
                && snapshot_equivalent (previous->second, current))
                continue;
            current.sequence = ++_snapshot_sequences[channel_name];
            current.observed_at = std::chrono::system_clock::now ();
            _last_snapshots.insert_or_assign (channel_name, current);
            client_server_runtime_event_t event{
              .identifier = "zlink.runtime.client_server.channel_changed",
              .sequence = current.sequence,
              .timestamp = current.observed_at,
              .channel_name = channel_name,
              .reason = std::string ("snapshot_changed")};
            auto &registered = _observers[channel_name];
            auto write = registered.begin ();
            for (auto read = registered.begin (); read != registered.end (); ++read) {
                if (auto current_observer = read->lock ()) {
                    notifications.emplace_back (current_observer, event);
                    *write++ = *read;
                }
            }
            registered.erase (write, registered.end ());
        }
    }
    for (auto &notification : notifications)
        notification.first->enqueue (std::move (notification.second));
}

void client_server_location_runtime_t::start ()
{
    if (empty ())
        return;
    const bool publishes_server =
      std::any_of (
        _channels.begin (), _channels.end (), [] (const auto &channel) {
            return channel.server.enabled && channel.server.discovery;
        });
    auto owner = _locations->current_owner_token ();
    if (publishes_server && !owner)
        throw std::runtime_error (
          "ClientServer publication requires an active owner lease");

    _stop.store (false, std::memory_order_release);
    try {
        _transport_poller = std::make_unique<zlink::poller_t> ();
        if (!_wake_pipe.open ())
            throw std::runtime_error ("ClientServer runtime wake pipe creation failed");
        _transport_poller->add_fd (
          _wake_pipe.read_fd (),
          zlink::poll_event_flag_t::pollin,
          next_transport_poller_slot ());
        for (const auto &channel : _channels) {
            if (channel.server.enabled
                && !channel.server.bind_endpoints.empty ())
                start_server (
                  channel,
                  channel.server.discovery ? owner : std::nullopt);
            if (channel.client.enabled
                && (channel.client.discovery
                    || !channel.client.connect_endpoints.empty ()))
                start_client (channel);
        }
        reconcile ();
        _channel_runtime.mark_auto_connect_active ();
        _thread = std::thread ([this] { run (); });
    }
    catch (...) {
        stop ();
        throw;
    }
}

void client_server_location_runtime_t::start_server (
  const channel_snapshot_t &channel,
  const std::optional<location_owner_token_t> &publication_owner)
{
    if (channel.server.bind_endpoints.size () != 1) {
        throw std::invalid_argument (
          "ClientServer server requires one bind endpoint");
    }
    protocol::client_server_server_admission_t admission;
    admission.channel_name = channel.name;
    admission.server_routing_id =
      server_routing_id (channel);
    admission.lifecycle_generation = make_lifecycle_generation ();
    admission.weight = static_cast<std::uint32_t> (
      channel.server.service_weight);
    admission.state = mesh::service_node_state_t::preparing;
    admission.security_identity =
      std::string (default_security_identity);
    admission.effective_max_message_bytes =
      effective_max_message_bytes (channel.server);
    admission.advertised_endpoint =
      channel.server.bind_endpoints.front ();

    const auto advertise =
      _advertise_hosts.find (channel.name);
    raw_client_server_server_options_t options{
      admission,
      advertise == _advertise_hosts.end ()
        ? std::nullopt
        : std::optional<std::string> (advertise->second)};
    options.transport_poller = _transport_poller.get ();
    options.transport_poller_slot = next_transport_poller_slot ();
    auto raw = std::make_unique<raw_client_server_server_t> (
      std::move (options));
    raw->start ();
    auto entry = std::make_unique<server_entry_t> ();
    entry->capability = channel.server;
    entry->owner = std::move (raw);
    if (publication_owner) {
        auto descriptor =
          to_descriptor (
            entry->owner->descriptor (), *publication_owner);
        const auto stored = _store
                              ->update_client_server (
                                descriptor,
                                location_write_intent_t::new_claim)
                              .result ()
                              .value ();
        if (stored.status != location_write_status_t::stored) {
            entry->owner->close ();
            throw std::runtime_error (
              "ClientServer descriptor publication was fenced");
        }
        entry->published_descriptor = std::move (descriptor);
    }
    _servers.emplace (channel.name, std::move (entry));
}

void client_server_location_runtime_t::start_client (
  const channel_snapshot_t &channel)
{
    auto entry = std::make_unique<client_channel_t> ();
    entry->snapshot = channel;
    entry->routing_id = client_routing_id (channel);
    _clients.emplace (channel.name, std::move (entry));
    _channel_runtime.bind_client_server_transport (
      channel.name,
      [this, name = channel.name] (
        std::string packet_name,
        std::string content_type,
        zlink::message_t message,
        std::chrono::milliseconds timeout) {
          return send (name, std::move (packet_name),
                       std::move (content_type),
                       std::move (message), timeout);
      },
      [this, name = channel.name] (
        std::string packet_name,
        std::string content_type,
        zlink::message_t message,
        std::chrono::milliseconds timeout) {
          return request (name, std::move (packet_name),
                          std::move (content_type),
                          std::move (message), timeout);
      });
}

void client_server_location_runtime_t::run ()
{
    auto next_reconcile = std::chrono::steady_clock::now ();
    while (!_stop.load (std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= next_reconcile) {
            try {
                publish_servers ();
                reconcile ();
            }
            catch (...) {
                _locations->record_store_error ();
            }
            next_reconcile =
              now + _locations->options ().polling_interval;
        }
        try {
            pump ();
            publish_snapshot_changes ();
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
            trace_client_server_runtime_failure (
              "runtime-loop-error", "unknown exception");
            try {
                _locations->record_store_error ();
            }
            catch (...) {
            }
        }
        if (_stop.load (std::memory_order_acquire))
            break;

        auto wake_at = next_reconcile;
        for (const auto &[_, server] : _servers) {
            if (const auto activity =
                  server->owner->next_liveness_activity ())
                wake_at = std::min (wake_at, *activity);
        }
        for (const auto &client : _client_pump_snapshot) {
            if (const auto activity = client->next_liveness_activity ())
                wake_at = std::min (wake_at, *activity);
        }

        const auto after_pump = std::chrono::steady_clock::now ();
        if (wake_at <= after_pump || !_transport_poller)
            continue;
        try {
            zlink::poll_event_t readiness;
            const auto count = _transport_poller->wait (
              &readiness,
              1,
              std::chrono::duration_cast<std::chrono::milliseconds> (
                wake_at - after_pump));
            if (count == 1
                && readiness.slot != 0
                && readiness.source_kind == zlink::poll_source_kind_t::fd
                && readiness.fd == _wake_pipe.read_fd ())
                _wake_pipe.drain ();
        }
        catch (...) {
            if (!_stop.load (std::memory_order_acquire))
                continue;
            break;
        }
    }
}

void client_server_location_runtime_t::publish_servers ()
{
    const auto owner = _locations->current_owner_token ();
    if (!owner)
        return;
    for (auto &[channel_name, server] : _servers) {
        if (!server->published_descriptor)
            continue;
        const auto weight_override =
          _channel_runtime.server_peer_weight_override (channel_name);
        const auto weight =
          weight_override.value_or (
            server->capability.service_weight);
        const auto state = current_state (*_locations);
        const bool new_owner =
          server->published_descriptor->owner_id != owner->owner_id
          || server->published_descriptor->lease_generation
               != owner->lease_generation;
        if (!new_owner
            && server->published_descriptor->weight == weight
            && server->published_descriptor->state == state)
            continue;

        auto admission = server->owner->descriptor ();
        if (admission.descriptor_revision
            == std::numeric_limits<std::uint64_t>::max ())
            throw std::overflow_error (
              "ClientServer descriptor revision is exhausted");
        ++admission.descriptor_revision;
        admission.weight = static_cast<std::uint32_t> (weight);
        admission.state = client_server_service_state (state);
        server->owner->update_descriptor (admission);
        auto descriptor = to_descriptor (admission, *owner);
        const auto written =
          _store
            ->update_client_server (
              descriptor,
              new_owner ? location_write_intent_t::new_claim
                        : location_write_intent_t::renew)
            .result ()
            .value ();
        if (written.status == location_write_status_t::stored)
            server->published_descriptor = std::move (descriptor);
    }
}

void client_server_location_runtime_t::reconcile ()
{
    for (auto &[_, channel] : _clients)
        reconcile_channel (*channel);
}

void client_server_location_runtime_t::reconcile_channel (
  client_channel_t &channel)
{
    std::map<std::string, client_server_server_descriptor_t> desired;
    location_page_request_t page;
    do {
        const auto listed =
          _store->list_client_servers (
                   channel.snapshot.name, page)
            .result ()
            .value ();
        for (const auto &descriptor : listed.items) {
            if ((descriptor.state
                   == framework_runtime_state_t::stopped)
                || descriptor.state
                     == framework_runtime_state_t::error
                || !owner_is_live (descriptor))
                continue;
            desired.insert_or_assign (
              connection_key (descriptor), descriptor);
        }
        page.continuation_token = listed.continuation_token;
    } while (page.continuation_token);

    for (const auto &endpoint :
         channel.snapshot.client.connect_endpoints) {
        const auto discovered = std::find_if (
          desired.begin (), desired.end (),
          [&] (const auto &entry) {
              return entry.second.endpoint == endpoint;
          });
        if (discovered == desired.end ()) {
            desired.emplace (
              manual_connection_key (endpoint),
              manual_descriptor (channel.snapshot.name, endpoint));
        }
    }
    std::vector<std::shared_ptr<raw_client_server_client_t>> close;
    for (const auto &[key, descriptor] : desired) {
        bool exists = false;
        {
            std::lock_guard lock (_gate);
            const auto found = channel.connections.find (key);
            if (found != channel.connections.end ()) {
                if (found->second.descriptor.endpoint != descriptor.endpoint
                    || found->second.descriptor.server_rid
                         != descriptor.server_rid
                    || found->second.descriptor.lifecycle_generation
                         != descriptor.lifecycle_generation
                    || found->second.descriptor.weight != descriptor.weight
                    || found->second.descriptor.state != descriptor.state) {
                    channel.selector_dirty = true;
                    found->second.descriptor = descriptor;
                }
                exists = true;
            } else if (!key.starts_with ("manual|")) {
                const auto manual = channel.connections.find (
                  manual_connection_key (descriptor.endpoint));
                if (manual != channel.connections.end ()) {
                    auto connection = std::move (manual->second);
                    channel.selector_dirty = true;
                    channel.connections.erase (manual);
                    connection.descriptor = descriptor;
                    channel.connections.emplace (
                      key, std::move (connection));
                    exists = true;
                }
            }
        }
        if (exists)
            continue;
        protocol::client_server_client_admission_t admission;
        admission.channel_name = channel.snapshot.name;
        admission.security_identity =
          std::string (default_security_identity);
        admission.effective_max_message_bytes =
          effective_max_message_bytes (channel.snapshot.client);
        auto expected = key.starts_with ("manual|")
                          ? protocol::client_server_server_admission_t{
                              .channel_name =
                                channel.snapshot.name,
                              .security_identity =
                                std::string (
                                  default_security_identity),
                              .effective_max_message_bytes =
                                admission.effective_max_message_bytes,
                              .advertised_endpoint =
                                descriptor.endpoint}
                          : to_admission (
                              descriptor,
                              admission.effective_max_message_bytes);
        raw_client_server_client_options_t options{
          channel.routing_id,
          admission,
          std::move (expected)};
        options.transport_poller = _transport_poller.get ();
        options.transport_poller_slot = next_transport_poller_slot ();
        auto raw = std::make_shared<raw_client_server_client_t> (
          std::move (options));
        raw->start ();
        std::lock_guard lock (_gate);
        channel.selector_dirty = true;
        channel.connections.emplace (
          key, client_connection_t{descriptor, std::move (raw)});
    }

    {
        std::lock_guard lock (_gate);
        for (auto it = channel.connections.begin ();
             it != channel.connections.end ();) {
            if (desired.contains (it->first)) {
                ++it;
                continue;
            }
            const auto stable =
              stable_key (it->second.descriptor);
            const auto replacement = std::find_if (
              channel.connections.begin (),
              channel.connections.end (),
              [&] (const auto &candidate) {
                  return desired.contains (candidate.first)
                         && stable_key (
                              candidate.second.descriptor)
                              == stable;
              });
            if (replacement != channel.connections.end ()
                && !replacement->second.owner->ready ()) {
                ++it;
                continue;
            }
            close.push_back (it->second.owner);
            channel.selector_dirty = true;
            it = channel.connections.erase (it);
        }
    }
    for (auto &owner : close)
        owner->close ();
}

void client_server_location_runtime_t::pump ()
{
    /* One stable client snapshot serves both pumping and the liveness wake-up
     * calculation in run(). Rebuilding it here avoids a second shared_ptr
     * copy in the same loop iteration. */
    refresh_client_pump_snapshot ();
    const auto now =
      mesh::service_liveness_registry_t::clock_t::now ();
    _server_pump_snapshot.clear ();
    _server_pump_snapshot.reserve (_servers.size ());
    for (auto &[_, server] : _servers)
        _server_pump_snapshot.push_back (server.get ());
    if (!_server_pump_snapshot.empty ()) {
        const auto start = _server_pump_cursor % _server_pump_snapshot.size ();
        for (std::size_t offset = 0; offset < _server_pump_snapshot.size (); ++offset) {
            auto &server = *_server_pump_snapshot[
              (start + offset) % _server_pump_snapshot.size ()];
            (void) server.owner->drain_monitor_events (now);
            receive_batch_budget_t budget;
            while (budget.can_receive ()) {
                const auto result = server.owner->pump_one (now);
                if (result == client_server_pump_result_t::no_data
                    || result == client_server_pump_result_t::backpressured)
                    break;
                budget.account (server.owner->last_pump_bytes ());
                if (budget.exhausted ())
                    break;
            }
            (void) server.owner->tick_liveness (now);
            dispatch_server (server);
        }
        _server_pump_cursor = (start + 1) % _server_pump_snapshot.size ();
    }
    if (!_client_pump_snapshot.empty ()) {
        const auto start = _client_pump_cursor % _client_pump_snapshot.size ();
        for (std::size_t offset = 0; offset < _client_pump_snapshot.size (); ++offset) {
            auto &client = _client_pump_snapshot[
              (start + offset) % _client_pump_snapshot.size ()];
            (void) client->drain_monitor_events (now);
            receive_batch_budget_t budget;
            while (budget.can_receive ()) {
                const auto result = client->pump_one (now);
                if (result == client_server_pump_result_t::no_data
                    || result == client_server_pump_result_t::backpressured)
                    break;
                budget.account (client->last_pump_bytes ());
                if (budget.exhausted ())
                    break;
            }
            (void) client->tick_liveness (now);
            (void) client->expire_requests (
              foundation::operation_registry_t::clock_t::now ());
        }
        _client_pump_cursor = (start + 1) % _client_pump_snapshot.size ();
    }
    std::lock_guard lock (_gate);
    for (auto &[_, channel] : _clients) {
        for (auto &[__, connection] : channel->connections) {
            const bool ready =
              connection.owner->ready ()
              && connection.descriptor.state
                   == framework_runtime_state_t::serving
              && connection.descriptor.weight > 0;
            if (ready != connection.selector_ready) {
                connection.selector_ready = ready;
                channel->selector_dirty = true;
            }
        }
    }
    _ready.notify_all ();
}

void client_server_location_runtime_t::refresh_client_pump_snapshot ()
{
    _client_pump_snapshot.clear ();
    std::lock_guard lock (_gate);
    for (auto &[_, channel] : _clients) {
        for (auto &[__, connection] : channel->connections)
            _client_pump_snapshot.push_back (connection.owner);
    }
}

void client_server_location_runtime_t::dispatch_server (
  server_entry_t &server)
{
    auto &mailbox = server.owner->mailbox ();
    receive_batch_budget_t budget;
    for (;;) {
        if (!budget.can_receive ())
            return;
        auto claim = mailbox.try_claim (
          mesh::service_mailbox_domain_t::application,
          dispatch_limits::receive_batch_messages,
          dispatch_limits::receive_batch_bytes);
        if (!claim)
            return;
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
            try {
                const auto payload =
                  protocol::decode_application_payload (
                    record.parts[1]);
                const auto message =
                  zlink::message_t::from (payload.payload);
                detail::inbound_message_context_t
                  inbound;
                inbound.message.channel_name = record.owner;
                inbound.message.packet_name =
                  payload.packet_name;
                inbound.message.content_type =
                  payload.content_type;
                if (record.correlation)
                    inbound.message.correlation_id =
                      std::to_string (*record.correlation);
                detail::message_flow_tracer_t flow (
                  _channel_runtime.dispatch_options_ref ());
                auto flow_scope = runtime::flow_context_t::enter (
                  payload.flow_id,
                  payload.flow_origin,
                  flow.capture_enabled (),
                  flow_origin_t::inbound);
                flow.trace (message_flow_outcome_t::received, [&] {
                    return message_flow_event_t{
                      message_flow_outcome_t::received,
                      dispatch_error_surface_t::channel,
                      record.request_sequence
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
                auto scope =
                  zlink::framework::detail::service_scope_t::create (
                    _services,
                    zlink::framework::detail::service_scope_kind_t::
                      handler_invocation);
                if (record.request_sequence && record.correlation) {
                    auto reply = _channel_runtime.dispatch_request (
                      record.owner, {}, payload.packet_name,
                      scope.provider (), *_serializers, *_handlers,
                      message, inbound);
                    if (reply) {
                        (void) server.owner->reply (
                          record,
                          protocol::application_payload_t{
                            payload.packet_name,
                            std::string (
                              runtime::messaging::envelope_codec_t::
                                default_content_type),
                            reply.value ().to_bytes ()});
                    } else {
                        const framework_exception_t error (
                          reply.error_kind (),
                          reply.error () != nullptr
                            ? reply.error ()->what ()
                            : "ClientServer request handler failed");
                        report_client_server_dispatch_error (
                          _channel_runtime.dispatch_options_ref (),
                          record,
                          payload.packet_name,
                          dispatch_message_kind_t::request,
                          dispatch_error_action_t::reply_error,
                          error);
                        const auto failure = wire_failure (error);
                        (void) server.owner->reply (
                          record, failure.terminal_result,
                          failure.failure_code);
                    }
                } else {
                    auto result = _channel_runtime.dispatch_send (
                      record.owner, {}, payload.packet_name,
                      scope.provider (), *_serializers, *_handlers,
                      message, inbound);
                    if (!result) {
                        const framework_exception_t error (
                          result.error_kind (),
                          result.error () != nullptr
                            ? result.error ()->what ()
                            : "ClientServer send handler failed");
                        report_client_server_dispatch_error (
                          _channel_runtime.dispatch_options_ref (),
                          record,
                          payload.packet_name,
                          dispatch_message_kind_t::send,
                          dispatch_error_action_t::drop,
                          error);
                    }
                }
            }
            catch (const framework_exception_t &error) {
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (),
                  record,
                  record.parts.size () > 1 ? "<decoded>" : "<unknown>",
                  record.request_sequence
                    ? dispatch_message_kind_t::request
                    : dispatch_message_kind_t::send,
                  record.request_sequence
                    ? dispatch_error_action_t::reply_error
                    : dispatch_error_action_t::drop,
                  error);
                if (record.request_sequence
                    && record.correlation) {
                    const auto failure = wire_failure (error);
                    (void) server.owner->reply (
                      record, failure.terminal_result,
                      failure.failure_code);
                }
            }
            catch (const std::exception &error) {
                const framework_exception_t failure (
                  framework_error_kind_t::internal_failure,
                  error.what ());
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (),
                  record,
                  record.parts.size () > 1 ? "<decoded>" : "<unknown>",
                  record.request_sequence
                    ? dispatch_message_kind_t::request
                    : dispatch_message_kind_t::send,
                  record.request_sequence
                    ? dispatch_error_action_t::reply_error
                    : dispatch_error_action_t::drop,
                  failure);
                if (record.request_sequence
                    && record.correlation) {
                    const auto wire = wire_failure (failure);
                    (void) server.owner->reply (
                      record, wire.terminal_result,
                      wire.failure_code);
                }
            }
            catch (...) {
                const framework_exception_t failure (
                  framework_error_kind_t::internal_failure,
                  "ClientServer dispatch failed");
                report_client_server_dispatch_error (
                  _channel_runtime.dispatch_options_ref (),
                  record,
                  record.parts.size () > 1 ? "<decoded>" : "<unknown>",
                  record.request_sequence
                    ? dispatch_message_kind_t::request
                    : dispatch_message_kind_t::send,
                  record.request_sequence
                    ? dispatch_error_action_t::reply_error
                    : dispatch_error_action_t::drop,
                  failure);
                if (record.request_sequence
                    && record.correlation) {
                    const auto wire = wire_failure (failure);
                    (void) server.owner->reply (
                      record, wire.terminal_result,
                      wire.failure_code);
                }
            }
        }
        (void) mailbox.release (*claim);
        if (yield_after_claim)
            return;
    }
}

result_t<void> client_server_location_runtime_t::send (
  const std::string &channel_name,
  std::string packet_name,
  std::string content_type,
  zlink::message_t message,
  std::chrono::milliseconds timeout)
{
    const auto effective =
      timeout > std::chrono::milliseconds::zero ()
        ? timeout
        : std::chrono::seconds (1);
    const auto wait = std::min (
      effective,
      std::chrono::duration_cast<std::chrono::milliseconds> (
        maximum_ready_wait));
    auto selected = select_ready (
      channel_name, std::chrono::steady_clock::now () + wait);
    if (!selected)
        return result_t<void>::failure (
          selected.error_kind (),
          selected.error () != nullptr
            ? selected.error ()->what ()
            : "ClientServer has no admitted ready server");
    const auto sent = selected.value ()->send (
      protocol::application_payload_t{
        std::move (packet_name),
        std::move (content_type),
        message.to_bytes ()});
    return sent
             ? result_t<void>::success ()
             : result_t<void>::failure (
                 framework_error_kind_t::unavailable,
                 "ClientServer send lost its admitted server");
}

result_t<zlink::message_t>
client_server_location_runtime_t::request (
  const std::string &channel_name,
  std::string packet_name,
  std::string content_type,
  zlink::message_t message,
  std::chrono::milliseconds timeout)
{
    const auto effective =
      timeout > std::chrono::milliseconds::zero ()
        ? timeout
        : std::chrono::seconds (30);
    const auto wait = std::min (
      effective,
      std::chrono::duration_cast<std::chrono::milliseconds> (
        maximum_ready_wait));
    const auto deadline =
      std::chrono::steady_clock::now () + effective;
    auto selected = select_ready (
      channel_name, std::chrono::steady_clock::now () + wait);
    if (!selected)
        return result_t<zlink::message_t>::failure (
          selected.error_kind (),
          selected.error () != nullptr
            ? selected.error ()->what ()
            : "ClientServer has no admitted ready server");
    auto promise = std::make_shared<
      std::promise<result_t<zlink::message_t>>> ();
    auto future = promise->get_future ();
    const auto submitted = selected.value ()->request (
      protocol::application_payload_t{
        std::move (packet_name),
        std::move (content_type),
        message.to_bytes ()},
      effective,
      [promise] (client_server_request_completion_t completion) {
          if (completion.terminal
              != foundation::operation_terminal_t::completed) {
              promise->set_value (
                zlink::framework::detail::result_access_t::failure<
                  zlink::message_t> (
                  client_server_operation_exception (
                    completion.terminal, "ClientServer request")));
              return;
          }
          if (completion.reply_header.terminal_result != 0) {
              const auto error =
                runtime::messaging::request_failure_mapper_t{}
                  .reply_header_exception (
                    completion.reply_header.terminal_result,
                    completion.reply_header.failure_code,
                    "ClientServer request");
              promise->set_value (
                zlink::framework::detail::result_access_t::failure<
                  zlink::message_t> (error));
              return;
          }
          try {
              const auto decoded =
                protocol::decode_application_payload (
                  completion.payload);
              promise->set_value (
                result_t<zlink::message_t>::success (
                  zlink::message_t::from (decoded.payload)));
          }
          catch (const std::exception &error) {
              promise->set_value (
                result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure,
                  error.what ()));
          }
      });
    if (!submitted)
        return result_t<zlink::message_t>::failure (
          framework_error_kind_t::unavailable,
          "ClientServer request lost its admitted server");
    if (future.wait_until (deadline) != std::future_status::ready)
        return zlink::framework::detail::result_access_t::failure<
          zlink::message_t> (client_server_operation_exception (
          foundation::operation_terminal_t::timed_out,
          "ClientServer request"));
    return future.get ();
}

result_t<std::shared_ptr<raw_client_server_client_t>>
client_server_location_runtime_t::select_ready (
  const std::string &channel_name,
  std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock (_gate);
    const auto available = [&] {
        const auto found = _clients.find (channel_name);
        if (found == _clients.end ())
            return false;
        return std::any_of (
          found->second->connections.begin (),
          found->second->connections.end (),
          [] (const auto &entry) {
              return entry.second.owner->ready ()
                     && entry.second.descriptor.state
                          == framework_runtime_state_t::serving
                     && entry.second.descriptor.weight > 0;
          });
    };
    if (!_ready.wait_until (lock, deadline, available)) {
        if (_stop.load (std::memory_order_acquire)) {
            return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
              framework_error_kind_t::shutting_down,
              "ClientServer runtime is stopping");
        }
        const auto found = _clients.find (channel_name);
        if (found == _clients.end ()) {
            return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
              framework_error_kind_t::not_found,
              "ClientServer channel is not registered");
        }
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          framework_error_kind_t::not_found,
          "ClientServer has no ready target before the bounded wait expired");
    }
    const auto channel_it = _clients.find (channel_name);
    if (channel_it == _clients.end ()) {
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          framework_error_kind_t::not_found,
          "ClientServer channel is not registered");
    }
    auto &channel = *channel_it->second;
    if (channel.selector_dirty) {
        channel.selector_candidates.clear ();
        channel.selector_candidates.reserve (channel.connections.size ());
        for (auto &[key, connection] : channel.connections) {
            if (!connection.owner->ready ()
                || connection.descriptor.state
                     != framework_runtime_state_t::serving
                || connection.descriptor.weight <= 0)
                continue;
            channel.selector_candidates.push_back (
              {key,
               static_cast<std::uint32_t> (
                 connection.descriptor.weight),
               stable_key (connection.descriptor)});
        }
        channel.selector.set_candidates (channel.selector_candidates);
        channel.selector_dirty = false;
    }
    const auto selected = channel.selector.select ();
    if (!selected) {
        return result_t<std::shared_ptr<raw_client_server_client_t>>::failure (
          framework_error_kind_t::not_found,
          "ClientServer has no selectable target snapshot");
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

void client_server_location_runtime_t::stop () noexcept
{
    const bool was_stopped =
      _stop.exchange (true, std::memory_order_acq_rel);
    _ready.notify_all ();
    for (const auto &[channel_name, _] : _clients)
        _channel_runtime.unbind_client_server_transport (
          channel_name);
    _wake_pipe.signal ();
    if (_thread.joinable ())
        _thread.join ();
    if (!was_stopped || !_servers.empty () || !_clients.empty ()) {
        stop_clients ();
        stop_servers ();
    }
    if (_transport_poller && _wake_pipe.read_fd () >= 0) {
        try {
            _transport_poller->remove_fd (_wake_pipe.read_fd ());
        }
        catch (...) {
        }
    }
    _wake_pipe.close ();
    if (_transport_poller) {
        try {
            _transport_poller->close ();
        }
        catch (...) {
        }
    }
    _transport_poller.reset ();
}

void client_server_location_runtime_t::stop_clients () noexcept
{
    std::vector<std::shared_ptr<raw_client_server_client_t>> clients;
    {
        std::lock_guard lock (_gate);
        for (auto &[_, channel] : _clients) {
            for (auto &[__, connection] : channel->connections)
                clients.push_back (connection.owner);
            channel->connections.clear ();
        }
        _clients.clear ();
        _client_pump_snapshot.clear ();
    }
    for (auto &client : clients)
        client->close ();
}

void client_server_location_runtime_t::stop_servers () noexcept
{
    for (auto &[_, server] : _servers) {
        if (!server->published_descriptor) {
            server->owner->close ();
            continue;
        }
        try {
            auto admission = server->owner->descriptor ();
            if (admission.state
                != mesh::service_node_state_t::draining) {
                ++admission.descriptor_revision;
                admission.state =
                  mesh::service_node_state_t::draining;
                admission.weight = 0;
                server->owner->update_descriptor (admission);
                auto draining = *server->published_descriptor;
                draining.descriptor_revision =
                  admission.descriptor_revision;
                draining.state =
                  framework_runtime_state_t::draining;
                draining.weight = 0;
                const auto written =
                  _store
                    ->update_client_server (
                      draining,
                      location_write_intent_t::renew)
                    .result ()
                    .value ();
                if (written.status
                    == location_write_status_t::stored)
                    server->published_descriptor = std::move (draining);
            }
            (void) _store
              ->remove_client_server (
                {server->published_descriptor->channel_name,
                 server->published_descriptor->server_rid},
                {server->published_descriptor->owner_id,
                 server->published_descriptor->lease_generation})
              .result ()
              .value ();
        }
        catch (...) {
        }
        server->owner->close ();
    }
    _servers.clear ();
    _server_pump_snapshot.clear ();
}

std::uint64_t
client_server_location_runtime_t::make_lifecycle_generation ()
{
    static std::atomic_uint64_t counter{1};
    const auto random =
      (static_cast<std::uint64_t> (std::random_device{} ()) << 32u)
      ^ static_cast<std::uint64_t> (std::random_device{} ());
    const auto time = static_cast<std::uint64_t> (
      std::chrono::steady_clock::now ()
        .time_since_epoch ()
        .count ());
    auto value =
      (random ^ time ^ counter.fetch_add (1))
      & static_cast<std::uint64_t> (
          std::numeric_limits<std::int64_t>::max ());
    return value == 0 ? 1 : value;
}

std::uint32_t
client_server_location_runtime_t::effective_max_message_bytes (
  const channel_capability_snapshot_t &capability)
{
    if (!capability.max_message_size
        || capability.max_message_size->bytes () <= 0)
        return default_effective_max_message_bytes;
    return static_cast<std::uint32_t> (
      std::min<std::int64_t> (
        capability.max_message_size->bytes (),
        std::numeric_limits<std::uint32_t>::max ()));
}

std::vector<std::uint8_t>
client_server_location_runtime_t::client_routing_id (
  const channel_snapshot_t &channel)
{
    if (channel.client.routing_id)
        return channel.client.routing_id->to_bytes ();
    return zlink::routing_id_t::from (
             channel.name + ":client:"
             + std::to_string (make_lifecycle_generation ()))
      .to_bytes ();
}

std::vector<std::uint8_t>
client_server_location_runtime_t::server_routing_id (
  const channel_snapshot_t &channel)
{
    if (channel.server.routing_id)
        return channel.server.routing_id->to_bytes ();
    return zlink::routing_id_t::from (
             channel.name + ":server:"
             + std::to_string (make_lifecycle_generation ()))
      .to_bytes ();
}

protocol::client_server_server_admission_t
client_server_location_runtime_t::to_admission (
  const client_server_server_descriptor_t &descriptor,
  std::uint32_t effective_max_message_bytes)
{
    protocol::client_server_server_admission_t admission;
    admission.channel_name = descriptor.channel_name;
    admission.server_routing_id =
      descriptor.server_rid.to_bytes ();
    admission.lifecycle_generation =
      descriptor.lifecycle_generation;
    admission.descriptor_revision =
      descriptor.descriptor_revision;
    admission.weight = static_cast<std::uint32_t> (
      descriptor.weight);
    admission.state =
      client_server_service_state (descriptor.state);
    admission.security_identity =
      descriptor.security_identity;
    admission.effective_max_message_bytes =
      effective_max_message_bytes;
    admission.advertised_endpoint = descriptor.endpoint;
    return admission;
}

client_server_server_descriptor_t
client_server_location_runtime_t::to_descriptor (
  const protocol::client_server_server_admission_t &admission,
  const location_owner_token_t &owner)
{
    return client_server_server_descriptor_t{
      .channel_name = admission.channel_name,
      .server_rid =
        zlink::routing_id_t::from (
          admission.server_routing_id),
      .lifecycle_generation =
        admission.lifecycle_generation,
      .descriptor_revision =
        admission.descriptor_revision,
      .endpoint = admission.advertised_endpoint,
      .weight = static_cast<int> (admission.weight),
      .state = client_server_framework_state (admission.state),
      .security_identity = admission.security_identity,
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
}

bool client_server_location_runtime_t::owner_is_live (
  const client_server_server_descriptor_t &descriptor) const
{
    const auto lease =
      _leases->read_owner_lease (descriptor.owner_id)
        .result ()
        .value ();
    const auto *found =
      std::get_if<owner_lease_found_t> (&lease);
    return found != nullptr
           && found->token.owner_id == descriptor.owner_id
           && found->token.lease_generation
                == descriptor.lease_generation
           && found->lease_expires_at > found->store_now;
}

} // namespace zlink::framework::runtime::client_server
