/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/mesh/route_mesh_connection_policy.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{

bool route_mesh_connection_not_required (
  const service_node_descriptor_t &local,
  const service_node_descriptor_t &remote) noexcept
{
    const auto public_role = [] (service_object_role_t role) {
        return role == service_object_role_t::client
          ? object_role_t::client
          : role == service_object_role_t::server
              ? object_role_t::server
              : object_role_t::none;
    };
    return route_mesh_connection_not_required (
      public_role (local.object_role), !local.channels.empty (),
      public_role (remote.object_role), !remote.channels.empty ());
}

std::uint64_t
sum_service_weights (std::span<const int> weights)
{
    std::uint64_t total = 0;
    for (const auto weight : weights) {
        if (weight < 0 || weight > 10000)
            throw std::invalid_argument (
              "service weight must be in range 0..10000");
        const auto value =
          static_cast<std::uint64_t> (weight);
        if (total
              > std::numeric_limits<std::uint64_t>::max ()
                  - value)
            throw std::overflow_error (
              "service weight sum is exhausted");
        total += value;
    }
    return total;
}

namespace
{

bool valid_channels (const std::vector<service_channel_descriptor_t> &channels)
{
    std::string previous;
    bool first = true;
    for (const auto &channel : channels) {
        if (channel.name.empty ()) {
            return false;
        }
        if (channel.weight < 0 || channel.weight > 10000) {
            return false;
        }
        if (!first && previous >= channel.name) {
            return false;
        }
        previous = channel.name;
        first = false;
    }
    return true;
}

bool immutable_fields_match (
  const service_node_descriptor_t &current,
  const service_node_descriptor_t &incoming,
  bool allow_initial_endpoint_resolution = false)
{
    if (current.mesh_name != incoming.mesh_name
        || current.node_routing_id != incoming.node_routing_id
        || current.lifecycle_generation
             != incoming.lifecycle_generation
        || (!allow_initial_endpoint_resolution
            && current.advertised_endpoint
                 != incoming.advertised_endpoint)
        || current.security_identity != incoming.security_identity
        || current.effective_max_message_bytes
             != incoming.effective_max_message_bytes
        || current.application_version
             != incoming.application_version
        || current.protocol_capabilities
             != incoming.protocol_capabilities
        || current.object_role != incoming.object_role
        || current.active_capacity_limit
             != incoming.active_capacity_limit
        || current.pending_capacity_limit
             != incoming.pending_capacity_limit
        || current.channels.size () != incoming.channels.size ()) {
        return false;
    }
    return std::equal (
      current.channels.begin (), current.channels.end (),
      incoming.channels.begin (),
      [] (const auto &left, const auto &right) {
          return left.name == right.name;
      });
}

bool discovery_expectation_matches (
  const service_node_descriptor_t &expected,
  const service_node_descriptor_t &incoming)
{
    return expected.mesh_name == incoming.mesh_name
           && expected.node_routing_id == incoming.node_routing_id
           && expected.advertised_endpoint
                == incoming.advertised_endpoint
           && expected.security_identity
                == incoming.security_identity
           && expected.lifecycle_generation
                == incoming.lifecycle_generation;
}

} // namespace

service_topology_registry_t::service_topology_registry_t (
  service_node_descriptor_t local) :
    _local (std::move (local))
{
    if (!valid_descriptor (_local)) {
        throw std::invalid_argument ("local service descriptor is invalid");
    }
}

bool service_topology_registry_t::byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left,
  const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (
      left.begin (), left.end (), right.begin (), right.end ());
}

bool service_topology_registry_t::valid_descriptor (
  const service_node_descriptor_t &descriptor)
{
    return !descriptor.mesh_name.empty () && !descriptor.node_routing_id.empty ()
           && descriptor.lifecycle_generation != 0
           && descriptor.descriptor_revision != 0
           && !descriptor.advertised_endpoint.empty ()
           && valid_channels (descriptor.channels)
           && !descriptor.security_identity.empty ()
           && descriptor.effective_max_message_bytes != 0
           && descriptor.application_version >= 0
           && descriptor.placement_weight >= 0
           && descriptor.placement_weight <= 10000
           && descriptor.active_capacity_limit != 0
           && descriptor.active_capacity_limit <= 2147483647u
           && descriptor.pending_capacity_limit <= 2147483647u
           && descriptor.active_capacity_used <= descriptor.active_capacity_limit
           && descriptor.pending_capacity_used <= descriptor.pending_capacity_limit
           && std::is_sorted (descriptor.protocol_capabilities.begin (),
                              descriptor.protocol_capabilities.end ())
           && std::adjacent_find (descriptor.protocol_capabilities.begin (),
                                  descriptor.protocol_capabilities.end ())
                == descriptor.protocol_capabilities.end ()
           && std::find (descriptor.protocol_capabilities.begin (),
                         descriptor.protocol_capabilities.end (),
                         "framework-service-v11")
                != descriptor.protocol_capabilities.end ();
}

bool service_topology_registry_t::selectable (
  const service_node_descriptor_t &descriptor,
  const std::string &channel_name)
{
    if (descriptor.state != service_node_state_t::serving) {
        return false;
    }
    const auto found = std::lower_bound (
      descriptor.channels.begin (), descriptor.channels.end (), channel_name,
      [] (const service_channel_descriptor_t &channel, const std::string &name) {
          return channel.name < name;
      });
    return found != descriptor.channels.end () && found->name == channel_name
           && found->weight != 0;
}

void service_topology_registry_t::publish_local (
  service_node_descriptor_t descriptor)
{
    if (!valid_descriptor (descriptor)) {
        throw std::invalid_argument ("published service descriptor is invalid");
    }
    std::function<void ()> changed;
    {
        std::lock_guard lock (_mutex);
        if (descriptor.mesh_name != _local.mesh_name
            || descriptor.node_routing_id != _local.node_routing_id
            || descriptor.lifecycle_generation != _local.lifecycle_generation) {
            throw std::invalid_argument (
              "published service descriptor changes the local identity");
        }
        if (descriptor.descriptor_revision <= _local.descriptor_revision) {
            throw std::invalid_argument (
              "published service descriptor revision is not increasing");
        }
        const bool resolving_bound_endpoint =
          _local.state == service_node_state_t::preparing
          && descriptor.state == service_node_state_t::serving;
        if (!immutable_fields_match (
              _local, descriptor, resolving_bound_endpoint)) {
            throw std::invalid_argument (
              "published service descriptor changes immutable fields");
        }
        _local = std::move (descriptor);
        for (auto it = _not_required_peers.begin ();
             it != _not_required_peers.end ();) {
            if (!route_mesh_connection_not_required (_local, it->second))
                it = _not_required_peers.erase (it);
            else
                ++it;
        }
        changed = _change_handler;
    }
    if (changed)
        changed ();
}

void service_topology_registry_t::set_change_handler (
  std::function<void ()> handler)
{
    std::lock_guard lock (_mutex);
    _change_handler = std::move (handler);
}

service_node_descriptor_t
service_topology_registry_t::local_descriptor () const
{
    std::lock_guard lock (_mutex);
    return _local;
}

peer_admission_result_t service_topology_registry_t::admit (
  service_node_descriptor_t descriptor,
  std::vector<std::uint8_t> connection_id)
{
    return admit_impl (
      std::move (descriptor), std::move (connection_id), std::nullopt,
      nullptr);
}

peer_admission_result_t service_topology_registry_t::admit (
  service_node_descriptor_t descriptor,
  std::vector<std::uint8_t> connection_id,
  service_connection_direction_t direction)
{
    return admit_impl (
      std::move (descriptor), std::move (connection_id), direction,
      nullptr);
}

peer_admission_result_t service_topology_registry_t::admit (
  service_node_descriptor_t descriptor,
  std::vector<std::uint8_t> connection_id,
  service_connection_direction_t direction,
  const service_node_descriptor_t &expected_descriptor)
{
    return admit_impl (
      std::move (descriptor), std::move (connection_id), direction,
      &expected_descriptor);
}

peer_admission_result_t service_topology_registry_t::admit_impl (
  service_node_descriptor_t descriptor,
  std::vector<std::uint8_t> connection_id,
  std::optional<service_connection_direction_t> direction,
  const service_node_descriptor_t *expected_descriptor)
{
    if (!valid_descriptor (descriptor) || connection_id.empty ()) {
        return peer_admission_result_t::invalid_descriptor;
    }
    std::unique_lock lock (_mutex);
    if (descriptor.mesh_name != _local.mesh_name) {
        return peer_admission_result_t::mesh_mismatch;
    }
    if (descriptor.node_routing_id == _local.node_routing_id) {
        return peer_admission_result_t::invalid_descriptor;
    }
    if (expected_descriptor != nullptr
        && !discovery_expectation_matches (
          *expected_descriptor, descriptor)) {
        return peer_admission_result_t::stale_descriptor;
    }
    const auto admitted = _peers.find (descriptor.node_routing_id);
    const auto not_required =
      _not_required_peers.find (descriptor.node_routing_id);
    const auto *current =
      admitted != _peers.end ()
        ? &admitted->second.descriptor
        : not_required != _not_required_peers.end ()
            ? &not_required->second
            : nullptr;
    if (current != nullptr
        && descriptor.lifecycle_generation
             != current->lifecycle_generation
        && expected_descriptor == nullptr) {
        return peer_admission_result_t::stale_descriptor;
    }
    if (current != nullptr
        && descriptor.lifecycle_generation
             == current->lifecycle_generation
        && (descriptor.descriptor_revision
              < current->descriptor_revision
            || (descriptor.descriptor_revision
                  == current->descriptor_revision
                && *current != descriptor))) {
        return peer_admission_result_t::stale_descriptor;
    }
    if (current != nullptr
        && descriptor.lifecycle_generation
             == current->lifecycle_generation
        && descriptor.descriptor_revision
             > current->descriptor_revision
        && !immutable_fields_match (*current, descriptor)) {
        return peer_admission_result_t::stale_descriptor;
    }
    if (route_mesh_connection_not_required (_local, descriptor)) {
        auto key = descriptor.node_routing_id;
        _peers.erase (key);
        _not_required_peers.insert_or_assign (
          std::move (key), std::move (descriptor));
        ++_topology_version;
        auto changed = _change_handler;
        lock.unlock ();
        if (changed)
            changed ();
        return peer_admission_result_t::not_required;
    }

    if (direction.has_value () && admitted != _peers.end ()
        && admitted->second.descriptor.lifecycle_generation
             == descriptor.lifecycle_generation
        && admitted->second.connection_id != connection_id) {
        const auto preferred_direction =
          byte_vector_less_t{} (_local.node_routing_id,
                                descriptor.node_routing_id)
            ? service_connection_direction_t::outbound
            : service_connection_direction_t::inbound;
        const auto keep_current =
          admitted->second.direction != direction.value ()
            ? admitted->second.direction == preferred_direction
            : !byte_vector_less_t{} (
                connection_id, admitted->second.connection_id);
        if (keep_current)
            return peer_admission_result_t::duplicate_connection;
    }

    _not_required_peers.erase (descriptor.node_routing_id);
    auto key = descriptor.node_routing_id;
    _peers.insert_or_assign (
      std::move (key),
      admitted_peer_t{std::move (descriptor), std::move (connection_id),
                      direction.value_or (
                        service_connection_direction_t::inbound)});
    ++_topology_version;
    auto changed = _change_handler;
    lock.unlock ();
    if (changed)
        changed ();
    return peer_admission_result_t::admitted;
}

bool service_topology_registry_t::disconnect (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id)
{
    std::function<void ()> changed;
    {
        std::lock_guard lock (_mutex);
        const auto found = _peers.find (node_routing_id);
        if (found == _peers.end ()
            || found->second.connection_id != connection_id) {
            return false;
        }
        _peers.erase (found);
        ++_topology_version;
        changed = _change_handler;
    }
    if (changed)
        changed ();
    return true;
}

std::vector<admitted_peer_t> service_topology_registry_t::peers () const
{
    std::lock_guard lock (_mutex);
    std::vector<admitted_peer_t> result;
    result.reserve (_peers.size ());
    for (const auto &[_, peer] : _peers) {
        result.push_back (peer);
    }
    return result;
}

std::vector<service_node_descriptor_t>
service_topology_registry_t::not_required_peers () const
{
    std::lock_guard lock (_mutex);
    std::vector<service_node_descriptor_t> result;
    result.reserve (_not_required_peers.size ());
    for (const auto &[_, descriptor] : _not_required_peers)
        result.push_back (descriptor);
    return result;
}

std::optional<admitted_peer_t> service_topology_registry_t::peer (
  const std::vector<std::uint8_t> &node_routing_id) const
{
    std::lock_guard lock (_mutex);
    const auto found = _peers.find (node_routing_id);
    if (found == _peers.end ()) {
        return std::nullopt;
    }
    return found->second;
}

void service_topology_registry_t::materialize_selection_state (
  selection_state_t &state)
{
    if (!state.precomputed)
        return;
    const auto total_selections = static_cast<std::int64_t> (
      state.precomputed_total_selections);
    for (std::size_t index = 0; index < state.ordered_node_ids.size (); ++index) {
        const auto weight = static_cast<std::int64_t> (
          state.ordered_weights[index]);
        state.cumulative[state.ordered_node_ids[index]] =
          state.precomputed_initial_cumulative[index]
          + total_selections * weight
          - static_cast<std::int64_t> (
              state.precomputed_selected_counts[index])
              * static_cast<std::int64_t> (state.total_weight);
    }
    state.precomputed = false;
    state.precomputed_initial_cumulative.clear ();
    state.precomputed_cycle_start_cumulative.clear ();
    state.precomputed_schedule.clear ();
    state.precomputed_selected_counts.clear ();
    state.precomputed_total_selections = 0;
    state.precomputed_cursor = 0;
    state.precomputed_cycle_start = 0;
    state.precomputed_schedule_end = 0;
}

void service_topology_registry_t::rebuild_selection_schedule (
  selection_state_t &state)
{
    constexpr std::size_t max_precomputed_steps = 4096;
    constexpr auto max_precompute_time = std::chrono::milliseconds (5);
    state.precomputed = false;
    state.precomputed_initial_cumulative.clear ();
    state.precomputed_cycle_start_cumulative.clear ();
    state.precomputed_schedule.clear ();
    state.precomputed_selected_counts.clear ();
    state.precomputed_total_selections = 0;
    state.precomputed_cursor = 0;
    state.precomputed_cycle_start = 0;
    state.precomputed_schedule_end = 0;
    if (state.ordered_node_ids.empty ()
        || state.total_weight == 0
        || state.total_weight
             > static_cast<std::uint64_t> (
                 std::numeric_limits<std::int64_t>::max ()))
        return;

    std::vector<std::int64_t> simulated;
    simulated.reserve (state.ordered_node_ids.size ());
    for (const auto &node_id : state.ordered_node_ids)
        simulated.push_back (state.cumulative[node_id]);
    const auto initial = simulated;
    std::map<std::vector<std::int64_t>, std::size_t> seen;
    std::vector<std::size_t> schedule;
    schedule.reserve (max_precomputed_steps);
    const auto started = std::chrono::steady_clock::now ();

    const auto select_index = [&] (
      const std::vector<std::int64_t> &credits) {
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < credits.size (); ++index) {
            const auto candidate_credit =
              credits[index]
              + static_cast<std::int64_t> (state.ordered_weights[index]);
            if (!selected
                || candidate_credit
                     > credits[*selected]
                         + static_cast<std::int64_t> (
                             state.ordered_weights[*selected])
                || (candidate_credit
                      == credits[*selected]
                           + static_cast<std::int64_t> (
                               state.ordered_weights[*selected])
                    && state.ordered_node_ids[index]
                         < state.ordered_node_ids[*selected])) {
                selected = index;
            }
        }
        return selected;
    };

    const auto apply_selection = [&] (std::vector<std::int64_t> &credits,
                                      std::size_t selected) {
        for (std::size_t index = 0; index < credits.size (); ++index)
            credits[index] += static_cast<std::int64_t> (
              state.ordered_weights[index]);
        credits[selected] -= static_cast<std::int64_t> (state.total_weight);
    };

    for (std::size_t step = 0; step < max_precomputed_steps; ++step) {
        if (std::chrono::steady_clock::now () - started
            >= max_precompute_time)
            return;
        const auto [found, inserted] = seen.emplace (simulated, step);
        if (!inserted) {
            const auto cycle_start = found->second;
            auto cycle_state = initial;
            for (std::size_t index = 0; index < cycle_start; ++index)
                apply_selection (cycle_state, schedule[index]);
            state.precomputed = true;
            state.precomputed_initial_cumulative = initial;
            state.precomputed_cycle_start_cumulative = std::move (cycle_state);
            state.precomputed_selected_counts.assign (
              state.ordered_node_ids.size (), 0);
            state.precomputed_cycle_start = cycle_start;
            state.precomputed_schedule_end = schedule.size ();
            state.precomputed_schedule = std::move (schedule);
            return;
        }
        const auto selected = select_index (simulated);
        if (!selected)
            return;
        schedule.push_back (*selected);
        apply_selection (simulated, *selected);
    }
}

std::optional<admitted_peer_t>
service_topology_registry_t::select (const std::string &channel_name)
{
    if (channel_name.empty ()) {
        return std::nullopt;
    }
    std::lock_guard lock (_mutex);
    auto &state = _selection_state[channel_name];
    if (!state.initialized || state.topology_version != _topology_version) {
        materialize_selection_state (state);
        state.weights.clear ();
        state.total_weight = 0;
        for (const auto &[node_id, peer] : _peers) {
            if (!selectable (peer.descriptor, channel_name))
                continue;
            const auto channel = std::lower_bound (
              peer.descriptor.channels.begin (),
              peer.descriptor.channels.end (), channel_name,
              [] (const service_channel_descriptor_t &entry,
                  const std::string &name) { return entry.name < name; });
            const auto weight = static_cast<std::uint64_t> (channel->weight);
            if (state.total_weight
                > std::numeric_limits<std::uint64_t>::max () - weight)
                throw std::overflow_error (
                  "RouteMesh selection weight total is exhausted");
            state.weights.insert_or_assign (node_id, weight);
            state.total_weight += weight;
        }
        for (auto it = state.cumulative.begin ();
             it != state.cumulative.end ();) {
            if (!state.weights.contains (it->first))
                it = state.cumulative.erase (it);
            else
                ++it;
        }
        state.ordered_node_ids.clear ();
        state.ordered_weights.clear ();
        state.ordered_node_ids.reserve (state.weights.size ());
        state.ordered_weights.reserve (state.weights.size ());
        for (const auto &[node_id, weight] : state.weights) {
            state.ordered_node_ids.push_back (node_id);
            state.ordered_weights.push_back (weight);
        }
        rebuild_selection_schedule (state);
        state.initialized = true;
        state.topology_version = _topology_version;
    }

    if (state.weights.empty () || state.total_weight == 0)
        return std::nullopt;

    if (state.precomputed) {
        const auto selected_index =
          state.precomputed_schedule[state.precomputed_cursor++];
        ++state.precomputed_selected_counts[selected_index];
        ++state.precomputed_total_selections;
        const auto selected_node_id =
          state.ordered_node_ids[selected_index];
        if (state.precomputed_cursor == state.precomputed_schedule_end) {
            state.precomputed_initial_cumulative =
              state.precomputed_cycle_start_cumulative;
            std::fill (state.precomputed_selected_counts.begin (),
                       state.precomputed_selected_counts.end (), 0);
            state.precomputed_total_selections = 0;
            state.precomputed_cursor = state.precomputed_cycle_start;
        }
        const auto selected = _peers.find (selected_node_id);
        if (selected == _peers.end ())
            return std::nullopt;
        return selected->second;
    }

    const admitted_peer_t *selected = nullptr;
    std::int64_t selected_cumulative = std::numeric_limits<std::int64_t>::min ();
    for (const auto &[node_id, weight] : state.weights) {
        const auto peer = _peers.find (node_id);
        if (peer == _peers.end ())
            continue;
        auto &cumulative =
          state.cumulative[node_id];
        if (cumulative > std::numeric_limits<std::int64_t>::max ()
                         - static_cast<std::int64_t> (weight)) {
            throw std::overflow_error (
              "RouteMesh selection cumulative value is exhausted");
        }
        cumulative += static_cast<std::int64_t> (weight);
        if (selected == nullptr || cumulative > selected_cumulative
            || (cumulative == selected_cumulative
                && node_id < selected->descriptor.node_routing_id)) {
            selected = &peer->second;
            selected_cumulative = cumulative;
        }
    }
    if (selected == nullptr)
        return std::nullopt;

    auto &selected_value =
      state.cumulative[selected->descriptor.node_routing_id];
    selected_value -= static_cast<std::int64_t> (state.total_weight);
    return *selected;
}

std::vector<admitted_peer_t>
service_topology_registry_t::multicast_targets (
  const std::string &channel_name) const
{
    if (channel_name.empty ())
        return {};
    std::lock_guard lock (_mutex);
    std::vector<admitted_peer_t> result;
    result.reserve (_peers.size ());
    for (const auto &[_, peer] : _peers) {
        if (selectable (peer.descriptor, channel_name))
            result.push_back (peer);
    }
    return result;
}

} // namespace zlink::framework::runtime::mesh
