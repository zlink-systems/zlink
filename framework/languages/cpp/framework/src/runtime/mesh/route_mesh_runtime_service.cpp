/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/mesh/route_mesh_connection_policy.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <algorithm>
#include <set>
#include <thread>
#include <utility>

namespace zlink::framework::runtime
{

mesh_node_snapshot_t build_snapshot (
  const std::shared_ptr<route_mesh_runtime_service_t::state_t> &state,
  std::string mesh_name);

namespace
{

mesh_node_state_t map_state (host::node_status_t::state_t state)
{
    switch (state) {
        case host::node_status_t::state_t::preparing:
            return mesh_node_state_t::starting;
        case host::node_status_t::state_t::serving:
            return mesh_node_state_t::ready;
        case host::node_status_t::state_t::draining:
            return mesh_node_state_t::stopping;
        case host::node_status_t::state_t::stopped:
            return mesh_node_state_t::stopped;
        case host::node_status_t::state_t::error:
            return mesh_node_state_t::failed;
    }
    return mesh_node_state_t::failed;
}

mesh_node_state_t map_state (framework_runtime_state_t state,
                             mesh_node_state_t transport_state)
{
    switch (state) {
        case framework_runtime_state_t::preparing:
            return mesh_node_state_t::starting;
        case framework_runtime_state_t::serving:
            return transport_state;
        case framework_runtime_state_t::relocating:
        case framework_runtime_state_t::relocated:
        case framework_runtime_state_t::draining:
            return mesh_node_state_t::stopping;
        case framework_runtime_state_t::stopped:
            return mesh_node_state_t::stopped;
        case framework_runtime_state_t::error:
            return mesh_node_state_t::failed;
    }
    return mesh_node_state_t::failed;
}

bool capacity_available (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved
                < static_cast<std::uint64_t> (usage.limit);
}

bool placement_capacity_available (
  const mesh_node_descriptor_t &descriptor)
{
    const bool object_slot_available =
      capacity_available (descriptor.capacity.actors)
      || capacity_available (descriptor.capacity.spots);
    const bool activation_slot_available =
      descriptor.activation_concurrency.limit == 0
      || descriptor.activation_concurrency.active
           < static_cast<std::uint64_t> (
             descriptor.activation_concurrency.limit);
    return object_slot_available && activation_slot_available;
}

framework_exception_t invalid_runtime_call (std::string message)
{
    return framework_exception_t (framework_error_kind_t::protocol_error,
                                  std::move (message));
}

} // namespace

struct route_mesh_runtime_service_t::state_t :
    public std::enable_shared_from_this<route_mesh_runtime_service_t::state_t>
{
    using observer_t =
      observation_detail::runtime_observer_state_t<mesh_node_snapshot_t>;

    struct hub_t
    {
        explicit hub_t (std::shared_ptr<detail::mesh_node_runtime_t> node_) :
            node (std::move (node_))
        {
        }

        std::shared_ptr<detail::mesh_node_runtime_t> node;
        std::mutex mutex;
        std::vector<std::weak_ptr<observer_t>> observers;
        std::optional<mesh_node_snapshot_t> last_snapshot;
        bool application_claim_active = false;
        std::uint64_t pending_application_callbacks = 0;
        std::string location_state = "not_configured";
        std::optional<std::chrono::system_clock::time_point>
          location_last_success;
        std::optional<std::chrono::system_clock::time_point>
          location_last_failure;
        std::chrono::steady_clock::time_point next_location_poll{};
        std::chrono::steady_clock::time_point next_descriptor_poll{};
        bool descriptor_baseline_initialized = false;
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
          descriptor_versions;
        std::atomic_bool stopped{false};
        std::thread pump;
    };

    std::map<std::string, std::shared_ptr<hub_t>> hubs;
    location_runtime_query_t *location_runtime = nullptr;
    location_repository_t *location_store = nullptr;
    mutable std::mutex sequence_mutex;
    mutable std::map<std::string, std::uint64_t> sequences;
    mutable std::mutex drain_mutex;
    std::optional<std::chrono::system_clock::time_point> drain_deadline;
    bool work_sealed = false;
    std::atomic_bool stopped{false};

    std::uint64_t next_sequence (const std::string &mesh_name) const
    {
        std::lock_guard lock (sequence_mutex);
        return ++sequences[mesh_name];
    }

    std::shared_ptr<hub_t> require_hub (const std::string &mesh_name) const
    {
        if (mesh_name.empty ())
            throw invalid_runtime_call ("mesh_name is required");
        const auto found = hubs.find (mesh_name);
        if (found == hubs.end ())
            throw invalid_runtime_call ("RouteMesh is not configured: " + mesh_name);
        return found->second;
    }

    void broadcast (hub_t &hub, const mesh_node_snapshot_t &snapshot)
    {
        std::vector<std::shared_ptr<observer_t>> current;
        {
            std::lock_guard lock (hub.mutex);
            auto write = hub.observers.begin ();
            for (auto read = hub.observers.begin (); read != hub.observers.end (); ++read) {
                if (auto observer = read->lock ()) {
                    current.push_back (observer);
                    *write++ = *read;
                }
            }
            hub.observers.erase (write, hub.observers.end ());
        }
        const bool terminal = snapshot.state == mesh_node_state_t::stopped
                              || snapshot.state == mesh_node_state_t::failed;
        for (const auto &observer : current)
            observer->enqueue (snapshot, terminal);
    }

    std::optional<mesh_node_snapshot_t>
    try_build_snapshot (hub_t &hub) noexcept
    {
        try {
            auto snapshot =
              build_snapshot (shared_from_this (), hub.node->mesh_name ());
            {
                std::lock_guard lock (hub.mutex);
                hub.last_snapshot = snapshot;
            }
            return snapshot;
        }
        catch (...) {
            return std::nullopt;
        }
    }

    void publish_current_snapshot (hub_t &hub) noexcept
    {
        try {
            if (auto snapshot = try_build_snapshot (hub))
                broadcast (hub, *snapshot);
        }
        catch (...) {
            // Monitoring projection and delivery never change transport or
            // host lifecycle results. A later change publishes a fresh full
            // snapshot.
        }
    }

    void publish_snapshot_change (hub_t &hub)
    {
        publish_current_snapshot (hub);
    }

    void publish_application_claim_change (hub_t &hub)
    {
        const auto active_callbacks = hub.node->active_application_callbacks ();
        const auto pending_callbacks = hub.node->pending_application_callbacks ();
        const bool active = active_callbacks != 0;
        bool changed;
        {
            std::lock_guard lock (hub.mutex);
            changed = hub.application_claim_active != active;
            hub.application_claim_active = active;
            hub.pending_application_callbacks = pending_callbacks;
        }
        if (!changed)
            return;
        publish_current_snapshot (hub);
    }

    void poll_location (hub_t &hub)
    {
        if (location_runtime == nullptr)
            return;
        const auto now = std::chrono::steady_clock::now ();
        {
            std::lock_guard lock (hub.mutex);
            if (now < hub.next_location_poll)
                return;
            hub.next_location_poll = now + std::chrono::milliseconds (100);
        }
        std::string state = "degraded";
        std::optional<std::chrono::system_clock::time_point> last_success;
        bool failed = true;
        try {
            auto query = location_runtime->get_status ();
            const auto &result = query.result ();
            if (result) {
                state = result.value ().store_healthy ? "ready" : "degraded";
                last_success = result.value ().last_refresh_at;
                failed = result.value ().last_error.has_value ();
            }
        }
        catch (...) {
        }
        bool changed;
        {
            std::lock_guard lock (hub.mutex);
            changed = hub.location_state != state;
            hub.location_state = state;
            if (last_success)
                hub.location_last_success = last_success;
            if (failed)
                hub.location_last_failure = std::chrono::system_clock::now ();
        }
        if (!changed)
            return;
        publish_current_snapshot (hub);
    }

    void poll_location_descriptors (hub_t &hub)
    {
        if (location_store == nullptr)
            return;
        const auto now = std::chrono::steady_clock::now ();
        {
            std::lock_guard lock (hub.mutex);
            if (now < hub.next_descriptor_poll)
                return;
            hub.next_descriptor_poll =
              now + std::chrono::milliseconds (100);
        }

        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
          versions;
        try {
            location_page_request_t page;
            for (;;) {
                auto listed =
                  location_store->list_mesh_nodes (
                    hub.node->mesh_name (), page);
                const auto &result = listed.result ();
                if (!result)
                    return;
                for (const auto &descriptor : result.value ().items) {
                    versions.emplace (
                      descriptor.rid.to_hex (),
                      std::pair{
                        descriptor.lifecycle_generation,
                        descriptor.descriptor_revision});
                }
                if (!result.value ().continuation_token)
                    break;
                page.continuation_token =
                  result.value ().continuation_token;
            }
        }
        catch (...) {
            return;
        }

        bool changed = false;
        {
            std::lock_guard lock (hub.mutex);
            if (!hub.descriptor_baseline_initialized) {
                hub.descriptor_baseline_initialized = true;
                hub.descriptor_versions = std::move (versions);
                return;
            }
            changed = hub.descriptor_versions != versions;
            if (changed)
                hub.descriptor_versions = std::move (versions);
        }
        if (changed)
            publish_snapshot_change (hub);
    }
};

namespace
{

class observation_t final : public mesh_runtime_observation_t
{
  public:
    explicit observation_t (
      std::shared_ptr<route_mesh_runtime_service_t::state_t::observer_t> observer) :
        _observer (std::move (observer))
    {
    }

    ~observation_t () override { close (); }

    void close () override
    {
        if (_observer) {
            _observer->close ();
            _observer.reset ();
        }
    }

  private:
    std::shared_ptr<route_mesh_runtime_service_t::state_t::observer_t> _observer;
};

} // namespace

route_mesh_runtime_service_t::route_mesh_runtime_service_t (
  std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes,
  location_runtime_query_t *location_runtime,
  location_repository_t *location_store) :
    _state (std::make_shared<state_t> ())
{
    _state->location_runtime = location_runtime;
    _state->location_store = location_store;
    for (auto &node : nodes)
        _state->hubs.emplace (node->mesh_name (),
                              std::make_shared<state_t::hub_t> (node));
}

route_mesh_runtime_service_t::~route_mesh_runtime_service_t ()
{
    stop ();
}

void route_mesh_runtime_service_t::start ()
{
    _state->stopped.store (false, std::memory_order_release);
    for (const auto &[_, hub] : _state->hubs) {
        if (hub->pump.joinable ())
            continue;
        std::weak_ptr<state_t> weak_state = _state;
        std::weak_ptr<state_t::hub_t> weak_hub = hub;
        hub->node->native_node ().transport ().topology ()
          .set_change_handler ([weak_state, weak_hub] {
              const auto state = weak_state.lock ();
              const auto observed_hub = weak_hub.lock ();
              if (state && observed_hub
                  && !state->stopped.load (std::memory_order_acquire)) {
                  state->publish_snapshot_change (*observed_hub);
              }
          });
        hub->stopped.store (false, std::memory_order_release);
        const auto state = _state;
        hub->pump = std::thread ([state, hub] {
            while (!hub->stopped.load (std::memory_order_acquire)) {
                state->publish_application_claim_change (*hub);
                state->poll_location (*hub);
                state->poll_location_descriptors (*hub);
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            }
        });
    }
}

void route_mesh_runtime_service_t::stop () noexcept
{
    if (_state->stopped.exchange (true, std::memory_order_acq_rel))
        return;
    for (const auto &[_, hub] : _state->hubs)
        hub->stopped.store (true, std::memory_order_release);
    for (const auto &[_, hub] : _state->hubs) {
        if (hub->pump.joinable ())
            hub->node->native_node ().transport ().topology ()
              .set_change_handler ({});
    }
    for (const auto &[_, hub] : _state->hubs) {
        if (!hub->pump.joinable ())
            continue;
        hub->pump.join ();
        std::vector<std::shared_ptr<state_t::observer_t>> observers;
        {
            std::lock_guard lock (hub->mutex);
            for (const auto &weak : hub->observers) {
                if (auto observer = weak.lock ())
                    observers.push_back (std::move (observer));
            }
            hub->observers.clear ();
        }
        auto terminal = _state->try_build_snapshot (*hub);
        if (!terminal) {
            std::lock_guard lock (hub->mutex);
            terminal = hub->last_snapshot;
        }
        if (!terminal) {
            for (const auto &observer : observers)
                observer->close ();
            continue;
        }
        terminal->state = mesh_node_state_t::stopped;
        terminal->is_ready = false;
        terminal->ready_peer_count = 0;
        for (auto &channel : terminal->channels) {
            channel.is_ready = false;
            channel.ready_target_count = 0;
        }
        for (auto &peer : terminal->peers) {
            if (peer.state != peer_state_t::not_required) {
                peer.state = peer_state_t::not_connected;
                peer.unavailable_reason =
                  topology_reason_t::runtime_not_ready;
            }
        }
        terminal->placement.is_available = false;
        terminal->placement.unavailable_reason =
          topology_reason_t::runtime_not_ready;
        terminal->sequence =
          _state->next_sequence (terminal->mesh_name);
        terminal->observed_at = std::chrono::system_clock::now ();
        for (const auto &observer : observers)
            observer->enqueue (*terminal, true);
    }
}

mesh_node_snapshot_t
build_snapshot (
  const std::shared_ptr<route_mesh_runtime_service_t::state_t> &state,
  std::string mesh_name)
{
    const auto hub = state->require_hub (mesh_name);
    const auto status = hub->node->status ();
    const auto descriptor =
      hub->node->native_node ().transport ().topology ().local_descriptor ();
    const auto peers = hub->node->native_node ().transport ().topology ().peers ();
    const auto not_required_peers =
      hub->node->native_node ().transport ().topology ().not_required_peers ();
    const auto transport_state = map_state (status.state);

    std::vector<mesh_node_descriptor_t> location_descriptors;
    if (state->location_store != nullptr) {
        try {
            location_page_request_t page;
            for (;;) {
                auto listed =
                  state->location_store->list_mesh_nodes (mesh_name, page);
                const auto &result = listed.result ();
                if (!result)
                    break;
                const auto &value = result.value ();
                location_descriptors.insert (
                  location_descriptors.end (),
                  value.items.begin (), value.items.end ());
                if (!value.continuation_token)
                    break;
                page.continuation_token = value.continuation_token;
            }
        }
        catch (...) {
        }
    }

    std::vector<mesh_peer_snapshot_t> peer_snapshots;
    std::map<std::string, std::uint64_t> ready_remote_members;
    std::set<std::string> classified_peer_ids;
    peer_snapshots.reserve (
      peers.size () + not_required_peers.size ()
      + location_descriptors.size ());
    for (const auto &peer : peers) {
        std::vector<std::string> channel_names;
        for (const auto &channel : peer.descriptor.channels) {
            channel_names.push_back (channel.name);
            if (channel.weight > 0)
                ++ready_remote_members[channel.name];
        }
        std::sort (channel_names.begin (), channel_names.end ());
        classified_peer_ids.insert (
          zlink::routing_id_t::from (
            peer.descriptor.node_routing_id).to_hex ());
        peer_snapshots.push_back (mesh_peer_snapshot_t{
          .node_rid = zlink::routing_id_t::from (
            peer.descriptor.node_routing_id),
          .state = peer.descriptor.state == mesh::service_node_state_t::draining
                     ? peer_state_t::draining
                     : peer_state_t::ready,
          .unavailable_reason =
            peer.descriptor.state == mesh::service_node_state_t::draining
              ? std::optional<topology_reason_t>{
                  topology_reason_t::draining}
              : std::nullopt});
    }
    for (const auto &peer : not_required_peers) {
        std::vector<std::string> channel_names;
        for (const auto &channel : peer.channels)
            channel_names.push_back (channel.name);
        const auto rid =
          zlink::routing_id_t::from (peer.node_routing_id);
        if (!classified_peer_ids.insert (rid.to_hex ()).second)
            continue;
        peer_snapshots.push_back (mesh_peer_snapshot_t{
          .node_rid = rid,
          .state = peer_state_t::not_required,
          .unavailable_reason = std::nullopt});
    }

    const auto local_location = std::find_if (
      location_descriptors.begin (), location_descriptors.end (),
      [&status] (const mesh_node_descriptor_t &candidate) {
          return candidate.rid == status.routing_id ()
                 && candidate.lifecycle_generation
                      == status.lifecycle_generation ();
      });
    const auto mapped_state =
      hub->stopped.load (std::memory_order_acquire)
        ? mesh_node_state_t::stopped
        : local_location != location_descriptors.end ()
            ? map_state (local_location->state, transport_state)
            : transport_state;
    const auto local_role =
      local_location != location_descriptors.end ()
        ? local_location->object_role
        : descriptor.object_role == mesh::service_object_role_t::client
            ? object_role_t::client
            : descriptor.object_role == mesh::service_object_role_t::server
                ? object_role_t::server
                : object_role_t::none;
    const auto local_channels =
      local_location != location_descriptors.end ()
        ? local_location->channel_weights
        : [&descriptor] {
              std::map<std::string, int> weights;
              for (const auto &channel : descriptor.channels)
                  weights.emplace (channel.name, channel.weight);
              return weights;
          } ();
    for (const auto &remote : location_descriptors) {
        if (remote.rid == status.routing_id ()
            || remote.state == framework_runtime_state_t::relocated
            || remote.state == framework_runtime_state_t::stopped
            || remote.state == framework_runtime_state_t::error
            || !classified_peer_ids.insert (
                 remote.rid.to_hex ()).second)
            continue;
        const auto not_required =
          mesh::route_mesh_connection_not_required (
            local_role, !local_channels.empty (),
            remote.object_role, !remote.channel_weights.empty ());
        std::vector<std::string> channel_names;
        channel_names.reserve (remote.channel_weights.size ());
        for (const auto &[channel_name, _] : remote.channel_weights)
            channel_names.push_back (channel_name);
        peer_snapshots.push_back (mesh_peer_snapshot_t{
          .node_rid = remote.rid,
          .state =
            not_required
              ? peer_state_t::not_required
              : remote.state == framework_runtime_state_t::draining
                  ? peer_state_t::draining
                  : peer_state_t::not_connected,
          .unavailable_reason =
            not_required
              ? std::nullopt
              : std::optional<topology_reason_t> (
                  remote.state == framework_runtime_state_t::draining
                    ? topology_reason_t::draining
                    : topology_reason_t::no_ready_peer)});
    }

    std::vector<mesh_channel_snapshot_t> channels;
    for (const auto &name : hub->node->channel_names ()) {
        const auto local_server = local_channels.find (name);
        const auto ready =
          (local_server != local_channels.end () && local_server->second > 0
             ? std::uint64_t{1}
             : std::uint64_t{0})
          + ready_remote_members[name];
        channels.push_back (mesh_channel_snapshot_t{
          .channel_name = name,
          .is_ready = ready != 0,
          .ready_target_count = static_cast<std::uint32_t> (ready)});
    }

    mesh_node_descriptor_t placement;
    placement.mesh_name = mesh_name;
    placement.rid = status.routing_id ();
    placement.lifecycle_generation = status.lifecycle_generation ();
    placement.descriptor_revision = descriptor.descriptor_revision;
    placement.endpoint = status.local_endpoint ();
    placement.object_role =
      descriptor.object_role == mesh::service_object_role_t::client
        ? object_role_t::client
        : descriptor.object_role == mesh::service_object_role_t::server
            ? object_role_t::server
            : object_role_t::none;
    placement.placement_weight = descriptor.placement_weight;
    placement.capacity.actors.limit = hub->node->actor_limit ();
    placement.capacity.spots.limit = hub->node->spot_limit ();
    placement.activation_concurrency.limit =
      hub->node->activation_concurrency_limit ();
    if (local_location != location_descriptors.end ())
        placement = *local_location;
    const bool placement_available =
      placement.object_role == object_role_t::server
      && mapped_state == mesh_node_state_t::ready
      && placement.placement_weight > 0
      && placement_capacity_available (placement);

    const bool required_peer_unavailable =
      std::any_of (
        peer_snapshots.begin (), peer_snapshots.end (),
        [] (const mesh_peer_snapshot_t &peer) {
            return peer.state == peer_state_t::connecting
                   || peer.state == peer_state_t::not_connected;
        });
    const auto public_state =
      mapped_state == mesh_node_state_t::ready
          && required_peer_unavailable
        ? mesh_node_state_t::degraded
        : mapped_state;
    if (public_state != mesh_node_state_t::ready) {
        for (auto &channel : channels)
            channel.is_ready = false;
    }

    return mesh_node_snapshot_t{
      .mesh_name = std::move (mesh_name),
      .state = public_state,
      .is_ready = public_state == mesh_node_state_t::ready,
      .ready_peer_count = static_cast<std::uint32_t> (
        std::count_if (
          peer_snapshots.begin (), peer_snapshots.end (),
          [] (const mesh_peer_snapshot_t &peer) {
              return peer.state == peer_state_t::ready;
          })),
      .channels = std::move (channels),
      .peers = std::move (peer_snapshots),
      .placement =
        mesh_placement_snapshot_t{
          .is_available = placement_available,
          .active_actor_count = static_cast<std::uint32_t> (
            placement.capacity.actors.active),
          .active_spot_count = static_cast<std::uint32_t> (
            placement.capacity.spots.active),
          .unavailable_reason =
            placement_available
              ? std::nullopt
              : std::optional<topology_reason_t>{
                  mapped_state != mesh_node_state_t::ready
                    ? mapped_state == mesh_node_state_t::stopping
                        ? topology_reason_t::draining
                        : topology_reason_t::runtime_not_ready
                    : topology_reason_t::capacity_exceeded}},
      .sequence = state->next_sequence (descriptor.mesh_name),
      .observed_at = std::chrono::system_clock::now ()};
}

mesh_node_snapshot_t
route_mesh_runtime_service_t::snapshot (std::string mesh_name) const
{
    const auto hub = _state->require_hub (mesh_name);
    auto snapshot = build_snapshot (_state, std::move (mesh_name));
    {
        std::lock_guard lock (hub->mutex);
        hub->last_snapshot = snapshot;
    }
    return snapshot;
}

std::unique_ptr<mesh_runtime_observation_t>
route_mesh_runtime_service_t::observe (
  std::string mesh_name,
  std::size_t capacity,
  std::function<void (
    const observed_status_t<mesh_node_snapshot_t> &)> observer)
{
    if (capacity == 0)
        throw invalid_runtime_call ("capacity must be positive");
    if (!observer)
        throw invalid_runtime_call ("observer is required");
    const auto hub = _state->require_hub (mesh_name);
    auto initial = snapshot (mesh_name);
    auto registered = std::make_shared<state_t::observer_t> (
      capacity, std::move (observer));
    {
        std::lock_guard lock (hub->mutex);
        hub->observers.push_back (registered);
    }
    const bool terminal = initial.state == mesh_node_state_t::stopped
                          || initial.state == mesh_node_state_t::failed;
    registered->enqueue (
      std::move (initial), terminal);
    return std::make_unique<observation_t> (std::move (registered));
}

bool route_mesh_runtime_service_t::is_ready (std::string mesh_name) const
{
    return snapshot (std::move (mesh_name)).is_ready;
}

route_mesh_runtime_host_service_t::route_mesh_runtime_host_service_t (
  std::shared_ptr<route_mesh_runtime_service_t> runtime) :
    _runtime (std::move (runtime))
{
}

void route_mesh_runtime_host_service_t::start (service_provider_t &)
{
    _runtime->start ();
}

void route_mesh_runtime_host_service_t::request_stop () noexcept
{
    _runtime->stop ();
}

void route_mesh_runtime_host_service_t::stop () noexcept
{
    _runtime->stop ();
}

} // namespace zlink::framework::runtime
