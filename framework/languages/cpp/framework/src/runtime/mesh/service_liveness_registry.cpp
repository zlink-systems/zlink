/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/service_liveness_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::mesh
{

service_liveness_registry_t::service_liveness_registry_t (
  std::chrono::milliseconds probe_interval,
  std::chrono::milliseconds peer_timeout) :
    _probe_interval (probe_interval), _peer_timeout (peer_timeout)
{
    if (_probe_interval <= std::chrono::milliseconds::zero ()
        || _peer_timeout <= _probe_interval) {
        throw std::invalid_argument (
          "service liveness requires a positive probe interval and larger timeout");
    }
}

bool service_liveness_registry_t::byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left,
  const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (
      left.begin (), left.end (), right.begin (), right.end ());
}

void service_liveness_registry_t::admit (
  std::vector<std::uint8_t> node_routing_id,
  std::vector<std::uint8_t> connection_id,
  clock_t::time_point now)
{
    if (node_routing_id.empty () || connection_id.empty ()) {
        throw std::invalid_argument (
          "service liveness admission requires node and connection identities");
    }
    std::lock_guard lock (_mutex);
    auto key = node_routing_id;
    _peers.insert_or_assign (
      std::move (key),
      peer_t{std::move (connection_id), now + _peer_timeout,
             now + _probe_interval, std::nullopt});
}

bool service_liveness_registry_t::disconnect (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id)
{
    std::lock_guard lock (_mutex);
    const auto found = _peers.find (node_routing_id);
    if (found == _peers.end () || found->second.connection_id != connection_id) {
        return false;
    }
    _peers.erase (found);
    return true;
}

bool service_liveness_registry_t::acknowledge (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id,
  std::uint64_t probe_id,
  clock_t::time_point now)
{
    std::lock_guard lock (_mutex);
    const auto found = _peers.find (node_routing_id);
    if (found == _peers.end () || found->second.connection_id != connection_id
        || !found->second.outstanding_probe
        || *found->second.outstanding_probe != probe_id) {
        return false;
    }
    found->second.outstanding_probe.reset ();
    found->second.deadline = now + _peer_timeout;
    return true;
}

std::optional<service_probe_t>
service_liveness_registry_t::acknowledge_probe (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id,
  std::uint64_t probe_id) const
{
    if (probe_id == 0) {
        return std::nullopt;
    }
    std::lock_guard lock (_mutex);
    const auto found = _peers.find (node_routing_id);
    if (found == _peers.end () || found->second.connection_id != connection_id) {
        return std::nullopt;
    }
    return service_probe_t{node_routing_id, connection_id, probe_id};
}

service_liveness_tick_t
service_liveness_registry_t::tick (clock_t::time_point now)
{
    service_liveness_tick_t result;
    std::lock_guard lock (_mutex);
    for (auto entry = _peers.begin (); entry != _peers.end ();) {
        if (entry->second.deadline <= now) {
            result.timed_out_nodes.push_back (entry->first);
            entry = _peers.erase (entry);
            continue;
        }
        if (entry->second.next_probe <= now) {
            if (!entry->second.outstanding_probe) {
                if (_next_probe_id == 0) {
                    _next_probe_id = 1;
                }
                entry->second.outstanding_probe = _next_probe_id++;
            }
            result.probes.push_back (
              service_probe_t{entry->first, entry->second.connection_id,
                              *entry->second.outstanding_probe});
            do {
                entry->second.next_probe += _probe_interval;
            } while (entry->second.next_probe <= now);
        }
        ++entry;
    }
    return result;
}

std::optional<service_liveness_registry_t::clock_t::time_point>
service_liveness_registry_t::next_activity () const
{
    std::lock_guard lock (_mutex);
    std::optional<clock_t::time_point> result;
    for (const auto &[_, peer] : _peers) {
        const auto candidate = std::min (peer.deadline, peer.next_probe);
        if (!result || candidate < *result)
            result = candidate;
    }
    return result;
}

std::size_t service_liveness_registry_t::size () const
{
    std::lock_guard lock (_mutex);
    return _peers.size ();
}

} // namespace zlink::framework::runtime::mesh
