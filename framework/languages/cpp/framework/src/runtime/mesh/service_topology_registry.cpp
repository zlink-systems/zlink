/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/client_server/weighted_selector.hpp"
#include <opentelemetry/metrics/provider.h>
#include "runtime/mesh/route_mesh_connection_policy.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{

bool route_mesh_connection_not_required (const service_node_descriptor_t &local,
                                         const service_node_descriptor_t &remote) noexcept
{
    const auto public_role = [] (service_object_role_t role) {
        return role == service_object_role_t::client   ? object_role_t::client
               : role == service_object_role_t::server ? object_role_t::server
                                                       : object_role_t::none;
    };
    return route_mesh_connection_not_required (
      public_role (local.object_role), !local.channels.empty (), public_role (remote.object_role),
      !remote.channels.empty ());
}

std::uint64_t sum_service_weights (std::span<const int> weights)
{
    std::uint64_t total = 0;
    for (const auto weight : weights) {
        if (weight < 0 || weight > 10000)
            throw std::invalid_argument ("service weight must be in range 0..10000");
        const auto value = static_cast<std::uint64_t> (weight);
        if (total > std::numeric_limits<std::uint64_t>::max () - value)
            throw std::overflow_error ("service weight sum is exhausted");
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

bool immutable_fields_match (const service_node_descriptor_t &current,
                             const service_node_descriptor_t &incoming,
                             bool allow_initial_endpoint_resolution = false)
{
    if (current.mesh_name != incoming.mesh_name
        || current.node_routing_id != incoming.node_routing_id
        || current.lifecycle_generation != incoming.lifecycle_generation
        || (!allow_initial_endpoint_resolution
            && current.advertised_endpoint != incoming.advertised_endpoint)
        || current.security_identity != incoming.security_identity
        || current.application_version != incoming.application_version
        || current.protocol_capabilities != incoming.protocol_capabilities
        || current.object_role != incoming.object_role
        || current.active_capacity_limit != incoming.active_capacity_limit
        || current.pending_capacity_limit != incoming.pending_capacity_limit
        || current.channels.size () != incoming.channels.size ()) {
        return false;
    }
    return std::equal (
      current.channels.begin (), current.channels.end (), incoming.channels.begin (),
      [] (const auto &left, const auto &right) { return left.name == right.name; });
}

bool discovery_expectation_matches (const service_node_descriptor_t &expected,
                                    const service_node_descriptor_t &incoming)
{
    return expected.mesh_name == incoming.mesh_name
           && expected.node_routing_id == incoming.node_routing_id
           && expected.advertised_endpoint == incoming.advertised_endpoint
           && expected.security_identity == incoming.security_identity
           && expected.lifecycle_generation == incoming.lifecycle_generation;
}

} // namespace

service_topology_registry_t::service_topology_registry_t (
  service_node_descriptor_t local,
  std::vector<std::string> metric_channel_names,
  service_liveness_registry_t *liveness_owner) :
    _local (std::move (local)),
    _liveness_owner (liveness_owner),
    _metric_channel_names (std::move (metric_channel_names))
{
    if (!valid_descriptor (_local)) {
        throw std::invalid_argument ("local service descriptor is invalid");
    }
    for (const auto &channel : _local.channels)
        _metric_channel_names.push_back (channel.name);
    std::sort (_metric_channel_names.begin (), _metric_channel_names.end ());
    _metric_channel_names.erase (
      std::unique (_metric_channel_names.begin (), _metric_channel_names.end ()),
      _metric_channel_names.end ());
    _selection_failures =
      opentelemetry::metrics::Provider::GetMeterProvider ()
        ->GetMeter ("zlink.framework")
        ->CreateDoubleCounter ("zlink.mesh_node.channel.selection_failures", "", "{failure}");
}

bool service_topology_registry_t::byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left, const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (left.begin (), left.end (), right.begin (), right.end ());
}

bool service_topology_registry_t::valid_descriptor (const service_node_descriptor_t &descriptor)
{
    return !descriptor.mesh_name.empty () && !descriptor.node_routing_id.empty ()
           && descriptor.lifecycle_generation != 0 && descriptor.descriptor_revision != 0
           && !descriptor.advertised_endpoint.empty () && valid_channels (descriptor.channels)
           && !descriptor.security_identity.empty () && descriptor.application_version >= 0
           && descriptor.placement_weight >= 0 && descriptor.placement_weight <= 10000
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
                         descriptor.protocol_capabilities.end (), protocol::required_capability)
                != descriptor.protocol_capabilities.end ();
}

bool service_topology_registry_t::selectable (const service_node_descriptor_t &descriptor,
                                              const std::string &channel_name)
{
    if (descriptor.state != service_node_state_t::serving) {
        return false;
    }
    const auto found =
      std::lower_bound (descriptor.channels.begin (), descriptor.channels.end (), channel_name,
                        [] (const service_channel_descriptor_t &channel, const std::string &name) {
                            return channel.name < name;
                        });
    return found != descriptor.channels.end () && found->name == channel_name && found->weight != 0;
}

std::function<void ()>
service_topology_registry_t::publish_local_on_lane (service_node_descriptor_t descriptor)
{
    if (!valid_descriptor (descriptor))
        throw std::invalid_argument ("published service descriptor is invalid");
    std::function<void ()> changed;
    if (descriptor.mesh_name != _local.mesh_name
        || descriptor.node_routing_id != _local.node_routing_id
        || descriptor.lifecycle_generation != _local.lifecycle_generation) {
        throw std::invalid_argument ("published service descriptor changes the local identity");
    }
    if (descriptor.descriptor_revision <= _local.descriptor_revision) {
        throw std::invalid_argument ("published service descriptor revision is not increasing");
    }
    const bool resolving_bound_endpoint = _local.state == service_node_state_t::preparing
                                          && descriptor.state == service_node_state_t::serving;
    if (!immutable_fields_match (_local, descriptor, resolving_bound_endpoint)) {
        throw std::invalid_argument ("published service descriptor changes immutable fields");
    }
    _local = std::move (descriptor);
    for (auto it = _not_required_peers.begin (); it != _not_required_peers.end ();) {
        if (!route_mesh_connection_not_required (_local, it->second))
            it = _not_required_peers.erase (it);
        else
            ++it;
    }
    changed = _change_handler;
    return changed;
}

void service_topology_registry_t::publish_local (service_node_descriptor_t descriptor)
{
    auto changed = _lane
                     .run ([this, descriptor = std::move (descriptor)] () mutable {
                         return publish_local_on_lane (std::move (descriptor));
                     })
                     .get ();
    if (changed)
        changed ();
}

std::vector<admitted_peer_t>
service_topology_registry_t::publish_local_snapshot (service_node_descriptor_t descriptor)
{
    auto publication = _lane
                         .run ([this, descriptor = std::move (descriptor)] () mutable {
                             auto changed = publish_local_on_lane (std::move (descriptor));
                             return std::pair{peers_on_lane (), std::move (changed)};
                         })
                         .get ();
    if (publication.second)
        publication.second ();
    return std::move (publication.first);
}

std::pair<service_node_descriptor_t, std::vector<admitted_peer_t>>
service_topology_registry_t::publish_draining_snapshot ()
{
    auto publication =
      _lane
        .run ([this] {
            auto descriptor = _local;
            std::function<void ()> changed;
            if (descriptor.state != service_node_state_t::draining) {
                if (descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
                    throw std::overflow_error ("service descriptor revision is exhausted");
                descriptor.state = service_node_state_t::draining;
                ++descriptor.descriptor_revision;
                changed = publish_local_on_lane (descriptor);
            }
            return std::tuple{std::move (descriptor), peers_on_lane (), std::move (changed)};
        })
        .get ();
    if (std::get<2> (publication))
        std::get<2> (publication) ();
    return {std::move (std::get<0> (publication)), std::move (std::get<1> (publication))};
}
void service_topology_registry_t::set_change_handler (std::function<void ()> handler)
{
    _lane.run ([&, this] { _change_handler = std::move (handler); }).get ();
}

service_node_descriptor_t service_topology_registry_t::local_descriptor () const
{
    return _lane.run ([this] { return _local; }).get ();
}

task_t<std::tuple<service_node_descriptor_t,
                  std::vector<admitted_peer_t>,
                  std::vector<service_node_descriptor_t>>>
service_topology_registry_t::monitoring_snapshot_async () const
{
    return _lane.run_task (
      [this] { return std::tuple{_local, peers_on_lane (), not_required_peers_on_lane ()}; });
}

peer_admission_result_t
service_topology_registry_t::admit (service_node_descriptor_t descriptor,
                                    std::vector<std::uint8_t> connection_id,
                                    service_liveness_registry_t::clock_t::time_point now)
{
    return admit_impl (std::move (descriptor), std::move (connection_id), nullptr, now);
}

peer_admission_result_t
service_topology_registry_t::admit (service_node_descriptor_t descriptor,
                                    std::vector<std::uint8_t> connection_id,
                                    const service_node_descriptor_t &expected_descriptor,
                                    service_liveness_registry_t::clock_t::time_point now)
{
    return admit_impl (std::move (descriptor), std::move (connection_id), &expected_descriptor,
                       now);
}

peer_admission_result_t
service_topology_registry_t::admit_impl (service_node_descriptor_t descriptor,
                                         std::vector<std::uint8_t> connection_id,
                                         const service_node_descriptor_t *expected_descriptor,
                                         service_liveness_registry_t::clock_t::time_point now)
{
    if (!valid_descriptor (descriptor) || connection_id.empty ()) {
        return peer_admission_result_t::invalid_descriptor;
    }
    const auto result =
      _lane
        .run ([&, this] {
            if (descriptor.mesh_name != _local.mesh_name) {
                return std::pair{peer_admission_result_t::mesh_mismatch, std::function<void ()>{}};
            }
            if (descriptor.node_routing_id == _local.node_routing_id) {
                return std::pair{peer_admission_result_t::invalid_descriptor,
                                 std::function<void ()>{}};
            }
            if (expected_descriptor != nullptr
                && !discovery_expectation_matches (*expected_descriptor, descriptor)) {
                return std::pair{peer_admission_result_t::stale_descriptor,
                                 std::function<void ()>{}};
            }
            const auto admitted = _peers.find (descriptor.node_routing_id);
            const auto not_required = _not_required_peers.find (descriptor.node_routing_id);
            const auto *current = admitted != _peers.end () ? &admitted->second.descriptor
                                  : not_required != _not_required_peers.end ()
                                    ? &not_required->second
                                    : nullptr;
            if (current != nullptr
                && descriptor.lifecycle_generation != current->lifecycle_generation
                && expected_descriptor == nullptr) {
                return std::pair{peer_admission_result_t::stale_descriptor,
                                 std::function<void ()>{}};
            }
            if (current != nullptr
                && descriptor.lifecycle_generation == current->lifecycle_generation
                && (descriptor.descriptor_revision < current->descriptor_revision
                    || (descriptor.descriptor_revision == current->descriptor_revision
                        && *current != descriptor))) {
                return std::pair{peer_admission_result_t::stale_descriptor,
                                 std::function<void ()>{}};
            }
            if (current != nullptr
                && descriptor.lifecycle_generation == current->lifecycle_generation
                && descriptor.descriptor_revision > current->descriptor_revision
                && !immutable_fields_match (*current, descriptor)) {
                return std::pair{peer_admission_result_t::stale_descriptor,
                                 std::function<void ()>{}};
            }
            if (route_mesh_connection_not_required (_local, descriptor)) {
                auto key = descriptor.node_routing_id;
                _peers.erase (key);
                _not_required_peers.insert_or_assign (std::move (key), std::move (descriptor));
                ++_topology_version;
                rebuild_channel_selections ();
                return std::pair{peer_admission_result_t::not_required, _change_handler};
            }

            service_liveness_registry_t::connection_t connection =
              admitted != _peers.end () && admitted->second.connection_id == connection_id
                ? admitted->second.liveness
                : nullptr;
            if (_liveness_owner) {
                if (!connection)
                    connection =
                      _liveness_owner->admit (descriptor.node_routing_id, connection_id, now);
                connection->record_received (now);
            }
            if (admitted != _peers.end () && admitted->second.connection_id == connection_id
                && admitted->second.descriptor == descriptor) {
                return std::pair{peer_admission_result_t::duplicate_connection,
                                 std::function<void ()>{}};
            }

            _not_required_peers.erase (descriptor.node_routing_id);
            auto key = descriptor.node_routing_id;
            const auto admission_epoch = ++_topology_version;
            _peers.insert_or_assign (
              std::move (key), admitted_peer_t{std::move (descriptor), std::move (connection_id),
                                               admission_epoch, std::move (connection)});
            rebuild_channel_selections ();
            return std::pair{peer_admission_result_t::admitted, _change_handler};
        })
        .get ();
    if (result.second)
        result.second ();
    return result.first;
}

bool service_topology_registry_t::disconnect (const std::vector<std::uint8_t> &node_routing_id,
                                              const std::vector<std::uint8_t> &connection_id)
{
    auto changed =
      _lane
        .run ([&, this] {
            std::function<void ()> changed;
            const auto found = _peers.find (node_routing_id);
            if (found == _peers.end () || found->second.connection_id != connection_id) {
                return std::pair{false, std::move (changed)};
            }
            _peers.erase (found);
            ++_topology_version;
            rebuild_channel_selections ();
            changed = _change_handler;
            return std::pair{true, std::move (changed)};
        })
        .get ();
    if (changed.second)
        changed.second ();
    return changed.first;
}

std::vector<admitted_peer_t> service_topology_registry_t::peers () const
{
    return _lane.run_checked ([this] { return peers_on_lane (); }).get ();
}

std::vector<service_node_descriptor_t> service_topology_registry_t::not_required_peers () const
{
    return _lane.run_checked ([this] { return not_required_peers_on_lane (); }).get ();
}

std::vector<admitted_peer_t> service_topology_registry_t::peers_on_lane () const
{
    std::vector<admitted_peer_t> result;
    result.reserve (_peers.size ());
    for (const auto &[_, peer] : _peers)
        result.push_back (peer);
    return result;
}

std::vector<service_node_descriptor_t>
service_topology_registry_t::not_required_peers_on_lane () const
{
    std::vector<service_node_descriptor_t> result;
    result.reserve (_not_required_peers.size ());
    for (const auto &[_, descriptor] : _not_required_peers)
        result.push_back (descriptor);
    return result;
}

std::optional<admitted_peer_t>
service_topology_registry_t::peer (const std::vector<std::uint8_t> &node_routing_id) const
{
    return _lane
      .run ([&, this] () -> std::optional<admitted_peer_t> {
          const auto found = _peers.find (node_routing_id);
          if (found == _peers.end ()) {
              return std::nullopt;
          }
          return found->second;
      })
      .get ();
}

void service_topology_registry_t::materialize_selection_state (selection_state_t &state)
{
    if (!state.precomputed)
        return;
    std::vector<std::size_t> selected_counts (state.ordered_node_ids.size (), 0);
    for (std::size_t step = 0; step < state.precomputed_cursor; ++step)
        ++selected_counts[state.precomputed_schedule[step]];
    const auto total_selections = static_cast<std::int64_t> (state.precomputed_cursor);
    for (std::size_t index = 0; index < state.ordered_node_ids.size (); ++index) {
        const auto weight = static_cast<std::int64_t> (state.ordered_weights[index]);
        state.cumulative[state.ordered_node_ids[index].first] =
          state.precomputed_initial_cumulative[index] + total_selections * weight
          - static_cast<std::int64_t> (selected_counts[index])
              * static_cast<std::int64_t> (state.total_weight);
    }
    state.precomputed = false;
    state.precomputed_initial_cumulative.clear ();
    state.precomputed_schedule.clear ();
    state.precomputed_cursor = 0;
    state.precomputed_cycle_start = 0;
}

void service_topology_registry_t::rebuild_selection_schedule (selection_state_t &state)
{
    state.precomputed = false;
    state.precomputed_initial_cumulative.clear ();
    state.precomputed_schedule.clear ();
    state.precomputed_cursor = 0;
    state.precomputed_cycle_start = 0;
    if (state.ordered_node_ids.empty () || state.total_weight == 0
        || state.total_weight
             > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ()))
        return;

    std::vector<std::int64_t> initial;
    initial.reserve (state.ordered_node_ids.size ());
    for (const auto &node_id : state.ordered_node_ids)
        initial.push_back (state.cumulative[node_id.first]);
    const auto select_index = [&] (const std::vector<std::int64_t> &credits) {
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < credits.size (); ++index) {
            const auto candidate_credit =
              credits[index] + static_cast<std::int64_t> (state.ordered_weights[index]);
            if (!selected
                || candidate_credit
                     > credits[*selected]
                         + static_cast<std::int64_t> (state.ordered_weights[*selected])
                || (candidate_credit
                      == credits[*selected]
                           + static_cast<std::int64_t> (state.ordered_weights[*selected])
                    && state.ordered_node_ids[index].first
                         < state.ordered_node_ids[*selected].first)) {
                selected = index;
            }
        }
        return selected;
    };

    const auto apply_selection = [&] (std::vector<std::int64_t> &credits, std::size_t selected) {
        for (std::size_t index = 0; index < credits.size (); ++index)
            credits[index] += static_cast<std::int64_t> (state.ordered_weights[index]);
        credits[selected] -= static_cast<std::int64_t> (state.total_weight);
    };

    std::vector<std::size_t> schedule;
    std::size_t cycle_start = 0;
    if (!client_server::precompute_weighted_schedule (initial, schedule, cycle_start, select_index,
                                                      apply_selection))
        return;
    state.precomputed = true;
    state.precomputed_initial_cumulative = initial;
    state.precomputed_cycle_start = cycle_start;
    state.precomputed_schedule = std::move (schedule);
}

void service_topology_registry_t::rebuild_channel_selections ()
{
    std::set<std::string> channel_names;
    for (const auto &[_, peer] : _peers) {
        for (const auto &channel : peer.descriptor.channels)
            channel_names.insert (channel.name);
    }

    for (auto state = _selection_state.begin (); state != _selection_state.end ();) {
        if (!channel_names.contains (state->first))
            state = _selection_state.erase (state);
        else
            ++state;
    }

    for (const auto &channel_name : channel_names) {
        auto &state = _selection_state[channel_name];
        materialize_selection_state (state);
        state.weights.clear ();
        state.total_weight = 0;
        bool all_draining = true;
        for (const auto &[node_id, peer] : _peers) {
            const auto channel = std::lower_bound (
              peer.descriptor.channels.begin (), peer.descriptor.channels.end (), channel_name,
              [] (const service_channel_descriptor_t &entry, const std::string &name) {
                  return entry.name < name;
              });
            if (channel == peer.descriptor.channels.end () || channel->name != channel_name)
                continue;
            all_draining &= peer.descriptor.state == service_node_state_t::draining;
            if (!selectable (peer.descriptor, channel_name))
                continue;
            const auto weight = static_cast<std::uint64_t> (channel->weight);
            if (state.total_weight > std::numeric_limits<std::uint64_t>::max () - weight)
                throw std::overflow_error ("RouteMesh selection weight total is exhausted");
            state.weights.insert_or_assign (node_id, weight);
            state.total_weight += weight;
        }
        state.unavailable_reason = all_draining ? "draining" : "not_ready";
        for (auto current = state.cumulative.begin (); current != state.cumulative.end ();) {
            if (!state.weights.contains (current->first))
                current = state.cumulative.erase (current);
            else
                ++current;
        }
        state.ordered_node_ids.clear ();
        state.ordered_weights.clear ();
        state.ordered_node_ids.reserve (state.weights.size ());
        state.ordered_weights.reserve (state.weights.size ());
        for (const auto &[node_id, weight] : state.weights) {
            state.ordered_node_ids.emplace_back (node_id, _peers.at (node_id).liveness);
            state.ordered_weights.push_back (weight);
        }
        rebuild_selection_schedule (state);
    }
}

result_t<std::vector<std::uint8_t>>
service_topology_registry_t::select (const std::string &channel_name,
                                     service_liveness_registry_t::connection_t *admission)
{
    if (channel_name.empty ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          framework_error_kind_t::not_found, "RouteMesh has no ready channel target snapshot");
    }
    return _lane
      .run ([&, this] () -> result_t<std::vector<std::uint8_t>> {
          const auto found = _selection_state.find (channel_name);
          if (found == _selection_state.end ()) {
              record_selection_failure (channel_name);
              return result_t<std::vector<std::uint8_t>>::failure (
                framework_error_kind_t::not_found,
                "RouteMesh has no ready channel target snapshot");
          }
          auto &state = found->second;

          if (state.precomputed) {
              const auto selected_index = state.precomputed_schedule[state.precomputed_cursor++];
              if (state.precomputed_cursor == state.precomputed_schedule.size ())
                  state.precomputed_cursor = state.precomputed_cycle_start;
              const auto &target = state.ordered_node_ids[selected_index];
              if (admission)
                  *admission = target.second;
              return result_t<std::vector<std::uint8_t>>::success (target.first);
          }

          const admitted_peer_t *selected = nullptr;
          std::int64_t selected_cumulative = std::numeric_limits<std::int64_t>::min ();
          for (const auto &[node_id, weight] : state.weights) {
              const auto peer = _peers.find (node_id);
              if (peer == _peers.end ())
                  continue;
              auto &cumulative = state.cumulative[node_id];
              if (cumulative > std::numeric_limits<std::int64_t>::max ()
                                 - static_cast<std::int64_t> (weight)) {
                  throw std::overflow_error ("RouteMesh selection cumulative value is exhausted");
              }
              cumulative += static_cast<std::int64_t> (weight);
              if (selected == nullptr || cumulative > selected_cumulative
                  || (cumulative == selected_cumulative
                      && node_id < selected->descriptor.node_routing_id)) {
                  selected = &peer->second;
                  selected_cumulative = cumulative;
              }
          }
          if (selected == nullptr) {
              record_selection_failure (channel_name);
              const bool has_ready_target =
                std::any_of (_peers.begin (), _peers.end (), [&channel_name] (const auto &entry) {
                    const auto &descriptor = entry.second.descriptor;
                    if (descriptor.state != service_node_state_t::serving
                        && descriptor.state != service_node_state_t::draining
                        && descriptor.state != service_node_state_t::retiring)
                        return false;
                    return std::any_of (descriptor.channels.begin (), descriptor.channels.end (),
                                        [&channel_name] (const auto &channel) {
                                            return channel.name == channel_name;
                                        });
                });
              return result_t<std::vector<std::uint8_t>>::failure (
                has_ready_target ? framework_error_kind_t::unavailable
                                 : framework_error_kind_t::not_found,
                "RouteMesh has no selectable channel member");
          }

          auto &selected_value = state.cumulative[selected->descriptor.node_routing_id];
          selected_value -= static_cast<std::int64_t> (state.total_weight);
          if (admission)
              *admission = selected->liveness;
          return result_t<std::vector<std::uint8_t>>::success (
            selected->descriptor.node_routing_id);
      })
      .get ();
}

void service_topology_registry_t::record_selection_failure (
  const std::string &channel_name) const noexcept
{
    // Labels come only from startup registrations; arbitrary requested names
    // and per-peer identities cannot create metric series.
    if (!std::binary_search (_metric_channel_names.begin (), _metric_channel_names.end (),
                             channel_name))
        return;
    const auto found = _selection_state.find (channel_name);
    const char *reason = _local.state == service_node_state_t::draining ? "draining"
                         : found == _selection_state.end ()             ? "no_member"
                                                            : found->second.unavailable_reason;
    _selection_failures->Add (
      1, {{"mesh_name",
           opentelemetry::nostd::string_view (_local.mesh_name.data (), _local.mesh_name.size ())},
          {"channel_name",
           opentelemetry::nostd::string_view (channel_name.data (), channel_name.size ())},
          {"reason", reason}});
}

void service_topology_registry_t::observe_channel_metrics (
  opentelemetry::metrics::ObserverResult result, bool closed) const
{
    auto observer = opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (result);
    _lane
      .run ([&] {
          // Selection already maintains the bounded member aggregates. No
          // application object, mailbox or Location Store traversal is needed.
          for (const auto &channel : _metric_channel_names) {
              const auto found = _selection_state.find (channel);
              const auto count =
                closed || found == _selection_state.end () ? 0 : found->second.weights.size ();
              observer->Observe (static_cast<double> (count),
                                 {{"mesh_name", _local.mesh_name}, {"channel_name", channel}});
          }
      })
      .get ();
}

std::vector<admitted_peer_t>
service_topology_registry_t::multicast_targets (const std::string &channel_name) const
{
    if (channel_name.empty ())
        return {};
    return _lane
      .run ([&, this] {
          std::vector<admitted_peer_t> result;
          result.reserve (_peers.size ());
          for (const auto &[_, peer] : _peers) {
              if (selectable (peer.descriptor, channel_name))
                  result.push_back (peer);
          }
          return result;
      })
      .get ();
}

} // namespace zlink::framework::runtime::mesh
