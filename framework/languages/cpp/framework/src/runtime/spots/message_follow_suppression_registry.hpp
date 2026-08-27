/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/protocol/service_wire_codec.hpp"

#include <map>
#include <tuple>

namespace zlink::framework::detail
{

struct message_follow_suppression_key_t
{
    runtime::protocol::actor_route_fence_t source;
    runtime::protocol::actor_route_fence_t target;

    friend bool operator== (const message_follow_suppression_key_t &,
                            const message_follow_suppression_key_t &) = default;

    bool operator< (const message_follow_suppression_key_t &other) const noexcept
    {
        return std::tie (source.actor_id,
                         source.object_generation,
                         source.target_node_routing_id,
                         source.target_node_generation,
                         source.authority_owner_generation,
                         source.owner_lease_generation,
                         target.actor_id,
                         target.object_generation,
                         target.target_node_routing_id,
                         target.target_node_generation,
                         target.authority_owner_generation,
                         target.owner_lease_generation)
               < std::tie (other.source.actor_id,
                           other.source.object_generation,
                           other.source.target_node_routing_id,
                           other.source.target_node_generation,
                           other.source.authority_owner_generation,
                           other.source.owner_lease_generation,
                           other.target.actor_id,
                           other.target.object_generation,
                           other.target.target_node_routing_id,
                           other.target.target_node_generation,
                           other.target.authority_owner_generation,
                           other.target.owner_lease_generation);
    }
};

// Owns only Message Follow notification suppression. Route lookup, payload
// lifetime, reply routing, and completion remain with their existing owners.
class message_follow_suppression_registry_t
{
  public:
    void retain (message_follow_suppression_key_t key)
    {
        _states.try_emplace (std::move (key), state_t::idle);
    }

    bool try_begin (const message_follow_suppression_key_t &key)
    {
        const auto found = _states.find (key);
        if (found == _states.end () || found->second != state_t::idle)
            return false;
        found->second = state_t::in_flight;
        return true;
    }

    bool mark_sent (const message_follow_suppression_key_t &key)
    {
        const auto found = _states.find (key);
        if (found == _states.end () || found->second != state_t::in_flight)
            return false;
        found->second = state_t::sent_until_expiry;
        return true;
    }

    bool abort (const message_follow_suppression_key_t &key)
    {
        const auto found = _states.find (key);
        if (found == _states.end () || found->second != state_t::in_flight)
            return false;
        found->second = state_t::idle;
        return true;
    }

    void erase (const message_follow_suppression_key_t &key) noexcept
    {
        _states.erase (key);
    }

    std::size_t size () const noexcept
    {
        return _states.size ();
    }

  private:
    enum class state_t
    {
        idle,
        in_flight,
        sent_until_expiry
    };

    std::map<message_follow_suppression_key_t, state_t> _states;
};

} // namespace zlink::framework::detail
