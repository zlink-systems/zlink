/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/stream_session_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::stateful
{

stream_session_registry_t::stream_session_registry_t (
  authority_resolver_t resolver) :
    _resolver (std::move (resolver))
{
    if (!_resolver) {
        throw std::invalid_argument (
          "stream session authority resolver is empty");
    }
}

stream_connection_t stream_session_registry_t::open (
  std::string connection_id)
{
    if (connection_id.empty ()) {
        throw std::invalid_argument ("stream connection id is empty");
    }
    std::lock_guard lock (_mutex);
    const auto generation = ++_last_connection_generation[connection_id];
    stream_connection_t connection{std::move (connection_id), generation};
    const auto previous = _connections.find (connection.connection_id);
    if (previous != _connections.end ()) {
        for (const auto &[actor_id, binding] : previous->second.bindings) {
            const auto indexed = _actor_bindings.find (actor_id);
            if (indexed != _actor_bindings.end ()
                && indexed->second == binding)
                _actor_bindings.erase (indexed);
        }
    }
    _connections[connection.connection_id] =
      connection_state_t{connection};
    return connection;
}

bool stream_session_registry_t::close (
  const stream_connection_t &connection)
{
    std::lock_guard lock (_mutex);
    const auto current = _connections.find (connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != connection) {
        return false;
    }
    for (const auto &[actor_id, binding] : current->second.bindings) {
        const auto indexed = _actor_bindings.find (actor_id);
        if (indexed != _actor_bindings.end () && indexed->second == binding)
            _actor_bindings.erase (indexed);
    }
    _connections.erase (current);
    return true;
}

std::pair<stateful_error_t, stream_binding_t>
stream_session_registry_t::bind (
  const stream_connection_t &connection,
  const object_ref_t &actor,
  std::uint64_t target_node_generation,
  std::uint64_t owner_lease_generation)
{
    const auto authority = _resolver (actor.key);
    if (!authority || !exact_actor (*authority, actor)) {
        return {authority ? stateful_error_t::generation_stale
                          : stateful_error_t::not_found,
                {}};
    }

    std::lock_guard lock (_mutex);
    if (_all_sealed)
        return {stateful_error_t::moving, {}};
    const auto current = _connections.find (connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != connection) {
        return {stateful_error_t::conflict, {}};
    }
    auto &state = current->second;
    const auto actor_id = actor.key;
    if (state.barrier_tokens.contains (actor_id))
        return {stateful_error_t::moving, {}};
    stream_binding_t binding{
      connection, state.next_binding_generation++, actor,
      target_node_generation, owner_lease_generation};
    const auto previous = _actor_bindings.find (actor_id);
    if (previous != _actor_bindings.end ()) {
        const auto previous_connection =
          _connections.find (previous->second.connection.connection_id);
        if (previous_connection != _connections.end ()
            && previous_connection->second.connection
                 == previous->second.connection) {
            previous_connection->second.bindings.erase (actor_id);
            previous_connection->second.barrier_tokens.erase (actor_id);
        }
    }
    state.bindings[actor_id] = binding;
    _actor_bindings[actor_id] = binding;
    return {stateful_error_t::none, binding};
}

stateful_error_t stream_session_registry_t::unbind (
  const stream_binding_t &binding)
{
    std::lock_guard lock (_mutex);
    const auto current =
      _connections.find (binding.connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != binding.connection
        || !current->second.bindings.contains (binding.actor.key)
        || current->second.bindings.at (binding.actor.key) != binding) {
        return stateful_error_t::conflict;
    }
    current->second.bindings.erase (binding.actor.key);
    current->second.barrier_tokens.erase (binding.actor.key);
    const auto indexed = _actor_bindings.find (binding.actor.key);
    if (indexed != _actor_bindings.end () && indexed->second == binding)
        _actor_bindings.erase (indexed);
    return stateful_error_t::none;
}

stateful_error_t stream_session_registry_t::restore (
  const stream_binding_t &binding)
{
    std::lock_guard lock (_mutex);
    if (_all_sealed) {
        return stateful_error_t::moving;
    }
    const auto current =
      _connections.find (binding.connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != binding.connection
        || current->second.barrier_tokens.contains (binding.actor.key)) {
        return stateful_error_t::conflict;
    }

    const auto indexed = _actor_bindings.find (binding.actor.key);
    if (indexed != _actor_bindings.end () && indexed->second != binding) {
        return stateful_error_t::conflict;
    }
    const auto local = current->second.bindings.find (binding.actor.key);
    if (local != current->second.bindings.end () && local->second != binding) {
        return stateful_error_t::conflict;
    }

    current->second.bindings[binding.actor.key] = binding;
    _actor_bindings[binding.actor.key] = binding;
    if (current->second.next_binding_generation
        <= binding.binding_generation
        && binding.binding_generation != std::numeric_limits<std::uint64_t>::max ()) {
        current->second.next_binding_generation = binding.binding_generation + 1;
    }
    return stateful_error_t::none;
}

std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
stream_session_registry_t::admit_inbound (
  const stream_binding_t &binding)
{
    std::lock_guard lock (_mutex);
    if (_all_sealed)
        return {stateful_error_t::moving, std::nullopt};
    const auto current =
      _connections.find (binding.connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != binding.connection
        || !current->second.bindings.contains (binding.actor.key)
        || current->second.bindings.at (binding.actor.key) != binding) {
        return {stateful_error_t::conflict, std::nullopt};
    }
    if (current->second.barrier_tokens.contains (binding.actor.key))
        return {stateful_error_t::moving, std::nullopt};
    const auto sequence = current->second.next_inbound_sequence++;
    current->second.active_inbound.emplace (sequence, binding.actor.key);
    return {stateful_error_t::none,
            stream_dispatch_t{
              binding, sequence}};
}

stateful_error_t stream_session_registry_t::complete_inbound (
  const stream_dispatch_t &dispatch)
{
    std::lock_guard lock (_mutex);
    const auto current =
      _connections.find (dispatch.binding.connection.connection_id);
    if (current == _connections.end ()
        || current->second.connection != dispatch.binding.connection)
        return stateful_error_t::conflict;
    const auto active =
      current->second.active_inbound.find (dispatch.inbound_sequence);
    if (active == current->second.active_inbound.end ()
        || active->second != dispatch.binding.actor.key)
        return stateful_error_t::not_found;
    current->second.active_inbound.erase (active);
    return stateful_error_t::none;
}

std::pair<stateful_error_t, stream_barrier_t>
stream_session_registry_t::try_seal_actor (const object_ref_t &actor)
{
    if (actor.kind != object_kind_t::actor)
        return {stateful_error_t::invalid, {}};
    std::lock_guard lock (_mutex);
    connection_state_t *affected = nullptr;
    const auto indexed = _actor_bindings.find (actor.key);
    if (indexed != _actor_bindings.end ()
        && exact_actor (indexed->second.actor, actor)) {
        const auto connection =
          _connections.find (indexed->second.connection.connection_id);
        if (connection != _connections.end ()
            && connection->second.connection
                 == indexed->second.connection) {
            affected = &connection->second;
            if (affected->barrier_tokens.contains (actor.key))
                return {stateful_error_t::moving, {}};
            for (const auto &[_, active_actor] : affected->active_inbound) {
                if (active_actor == actor.key)
                    return {stateful_error_t::backpressured, {}};
            }
        }
    }
    if (_next_barrier_token == 0)
        return {stateful_error_t::conflict, {}};
    const auto token = _next_barrier_token++;
    if (affected != nullptr)
        affected->barrier_tokens[actor.key] = token;
    _barriers.emplace (token, actor);
    return {
      stateful_error_t::none, stream_barrier_t{token, actor}};
}

stateful_error_t stream_session_registry_t::abort_barrier (
  const stream_barrier_t &barrier)
{
    std::lock_guard lock (_mutex);
    const auto found = _barriers.find (barrier.token);
    if (found == _barriers.end () || !exact_actor (found->second, barrier.actor))
        return stateful_error_t::not_found;
    for (auto &[_, state] : _connections) {
        const auto token = state.barrier_tokens.find (barrier.actor.key);
        if (token != state.barrier_tokens.end ()
            && token->second == barrier.token)
            state.barrier_tokens.erase (token);
    }
    _barriers.erase (found);
    return stateful_error_t::none;
}

stateful_error_t stream_session_registry_t::commit_barrier (
  const stream_barrier_t &barrier,
  const object_ref_t &target)
{
    std::lock_guard lock (_mutex);
    const auto found = _barriers.find (barrier.token);
    if (found == _barriers.end () || !exact_actor (found->second, barrier.actor)
        || target.kind != object_kind_t::actor
        || target.key != barrier.actor.key
        || target.object_generation != barrier.actor.object_generation
        || target.authority_owner_generation
             <= barrier.actor.authority_owner_generation)
        return stateful_error_t::conflict;
    const auto indexed = _actor_bindings.find (barrier.actor.key);
    if (indexed != _actor_bindings.end ()) {
        const auto connection =
          _connections.find (indexed->second.connection.connection_id);
        if (connection == _connections.end ()
            || connection->second.connection
                 != indexed->second.connection)
            return stateful_error_t::conflict;
        auto &state = connection->second;
        const auto token = state.barrier_tokens.find (barrier.actor.key);
        const auto binding = state.bindings.find (barrier.actor.key);
        if (token == state.barrier_tokens.end ()
            || token->second != barrier.token
            || binding == state.bindings.end ()
            || !exact_actor (binding->second.actor, barrier.actor))
            return stateful_error_t::conflict;
        auto next = binding->second;
        next.actor = target;
        binding->second = next;
        indexed->second = next;
        state.barrier_tokens.erase (token);
    }
    _barriers.erase (found);
    return stateful_error_t::none;
}

stream_route_seal_admission_t
stream_session_registry_t::seal_remote_route (
  const std::string &connection_id,
  std::uint64_t binding_generation,
  const object_ref_t &actor,
  std::uint64_t target_node_generation,
  std::uint64_t owner_lease_generation)
{
    std::lock_guard lock (_mutex);
    const auto connection = _connections.find (connection_id);
    const auto indexed = _actor_bindings.find (actor.key);
    if (connection == _connections.end ()
        || indexed == _actor_bindings.end ()
        || indexed->second.connection != connection->second.connection
        || indexed->second.binding_generation != binding_generation) {
        return {stateful_error_t::not_found, std::nullopt, {}, 0};
    }
    auto &state = connection->second;
    const auto binding = state.bindings.find (actor.key);
    const auto last_sequence = state.next_inbound_sequence - 1;
    const auto active = std::any_of (
      state.active_inbound.begin (), state.active_inbound.end (),
      [&actor] (const auto &entry) {
          return entry.second == actor.key;
      });
    if (binding == state.bindings.end ()
        || binding->second != indexed->second
        || binding->second.actor.kind != object_kind_t::actor
        || actor.kind != object_kind_t::actor
        || binding->second.actor.key != actor.key
        || binding->second.actor.object_generation
             != actor.object_generation
        || binding->second.actor.authority_owner_generation
             != actor.authority_owner_generation
        || binding->second.actor.node_id != actor.node_id
        || target_node_generation == 0
        || owner_lease_generation == 0
        || binding->second.target_node_generation
             != target_node_generation
        || binding->second.owner_lease_generation
             != owner_lease_generation
        || active
        || state.barrier_tokens.contains (actor.key)
        || _next_barrier_token == 0) {
        return {active ? stateful_error_t::backpressured
                       : stateful_error_t::conflict,
                binding == state.bindings.end ()
                  ? std::optional<stream_binding_t>{}
                  : std::make_optional (binding->second),
                {}, last_sequence};
    }
    const auto token = _next_barrier_token++;
    const stream_barrier_t barrier{token, binding->second.actor};
    state.barrier_tokens.emplace (actor.key, token);
    _barriers.emplace (token, binding->second.actor);
    return {stateful_error_t::none, binding->second,
            barrier, last_sequence};
}

stream_route_admission_t stream_session_registry_t::commit_remote_route (
  const std::string &connection_id,
  std::uint64_t binding_generation,
  const std::string &actor_id,
  std::uint64_t object_generation,
  std::uint64_t previous_authority_owner_generation,
  object_ref_t target,
  std::uint64_t target_node_generation,
  std::uint64_t replayed_high_water)
{
    std::lock_guard lock (_mutex);
    const auto connection = _connections.find (connection_id);
    const auto indexed = _actor_bindings.find (actor_id);
    if (connection == _connections.end ()
        || indexed == _actor_bindings.end ()
        || indexed->second.connection != connection->second.connection
        || indexed->second.binding_generation != binding_generation) {
        return {stateful_error_t::not_found, std::nullopt, 0};
    }
    auto &state = connection->second;
    const auto binding = state.bindings.find (actor_id);
    const auto barrier = state.barrier_tokens.find (actor_id);
    const auto last_sequence = state.next_inbound_sequence - 1;
    const auto active = std::any_of (
      state.active_inbound.begin (), state.active_inbound.end (),
      [&actor_id] (const auto &entry) {
          return entry.second == actor_id;
      });
    if (binding == state.bindings.end ()
        || binding->second != indexed->second
        || binding->second.actor.kind != object_kind_t::actor
        || binding->second.actor.object_generation != object_generation
        || binding->second.actor.authority_owner_generation
             != previous_authority_owner_generation
        || barrier == state.barrier_tokens.end ()
        || !_barriers.contains (barrier->second)
        || !exact_actor (_barriers.at (barrier->second),
                         binding->second.actor)
        || target.kind != object_kind_t::actor
        || target.key != actor_id
        || target.object_generation != object_generation
        || target.authority_owner_generation
             <= previous_authority_owner_generation
        || target_node_generation == 0
        || active
        || replayed_high_water != last_sequence) {
        return {stateful_error_t::conflict,
                binding == state.bindings.end ()
                  ? std::optional<stream_binding_t>{}
                  : std::make_optional (binding->second),
                last_sequence};
    }
    auto next = binding->second;
    next.actor = std::move (target);
    next.target_node_generation = target_node_generation;
    binding->second = next;
    indexed->second = next;
    _barriers.erase (barrier->second);
    state.barrier_tokens.erase (barrier);
    return {stateful_error_t::none, next, last_sequence};
}

stream_route_admission_t
stream_session_registry_t::acknowledge_remote_abort (
  const std::string &connection_id,
  std::uint64_t binding_generation,
  const std::string &actor_id,
  std::uint64_t object_generation,
  std::uint64_t current_authority_owner_generation)
{
    std::lock_guard lock (_mutex);
    const auto connection = _connections.find (connection_id);
    const auto indexed = _actor_bindings.find (actor_id);
    if (connection == _connections.end ()
        || indexed == _actor_bindings.end ()
        || indexed->second.connection != connection->second.connection
        || indexed->second.binding_generation != binding_generation) {
        return {stateful_error_t::not_found, std::nullopt, 0};
    }
    const auto last_sequence =
      connection->second.next_inbound_sequence - 1;
    const auto barrier =
      connection->second.barrier_tokens.find (actor_id);
    if (indexed->second.actor.kind != object_kind_t::actor
        || indexed->second.actor.object_generation != object_generation
        || indexed->second.actor.authority_owner_generation
             != current_authority_owner_generation
        || barrier == connection->second.barrier_tokens.end ()
        || !_barriers.contains (barrier->second)
        || !exact_actor (_barriers.at (barrier->second),
                         indexed->second.actor)) {
        return {stateful_error_t::conflict, indexed->second,
                last_sequence};
    }
    _barriers.erase (barrier->second);
    connection->second.barrier_tokens.erase (barrier);
    return {stateful_error_t::none, indexed->second, last_sequence};
}

std::optional<stream_binding_t> stream_session_registry_t::current_binding (
  const std::string &actor_id) const
{
    std::lock_guard lock (_mutex);
    const auto found = _actor_bindings.find (actor_id);
    if (found == _actor_bindings.end ())
        return std::nullopt;
    return found->second;
}

bool stream_session_registry_t::try_seal_all ()
{
    std::lock_guard lock (_mutex);
    if (_all_sealed)
        return true;
    for (const auto &[_, state] : _connections) {
        if (!state.active_inbound.empty ())
            return false;
    }
    _all_sealed = true;
    return true;
}

void stream_session_registry_t::release_all () noexcept
{
    std::lock_guard lock (_mutex);
    _all_sealed = false;
}

void stream_session_registry_t::force_close_all () noexcept
{
    std::lock_guard lock (_mutex);
    _all_sealed = true;
    _barriers.clear ();
    _actor_bindings.clear ();
    _connections.clear ();
}

bool stream_session_registry_t::is_current (
  const stream_binding_t &binding) const
{
    std::lock_guard lock (_mutex);
    const auto current =
      _connections.find (binding.connection.connection_id);
    return current != _connections.end ()
           && current->second.connection == binding.connection
           && current->second.bindings.contains (binding.actor.key)
           && current->second.bindings.at (binding.actor.key) == binding;
}

bool stream_session_registry_t::exact_actor (
  const object_ref_t &left,
  const object_ref_t &right)
{
    return left.kind == object_kind_t::actor
           && right.kind == object_kind_t::actor
           && left == right;
}

} // namespace zlink::framework::runtime::stateful
