/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/stream_session_registry.hpp"

#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime::stateful
{

namespace
{

void settle_retained_outbound (std::vector<stream_retained_outbound_t> retained,
                               bool deliver) noexcept
{
    for (auto &settle : retained) {
        if (!settle)
            continue;
        try {
            settle (deliver);
        }
        catch (...) {
        }
    }
}

} // namespace

stream_session_registry_t::stream_session_registry_t (authority_resolver_t resolver) :
    _resolver (std::move (resolver)), _lane_executor (), _lane (_lane_executor)
{
    if (!_resolver) {
        throw std::invalid_argument ("stream session authority resolver is empty");
    }
}

stream_connection_t stream_session_registry_t::open (std::string connection_id,
                                                     std::function<void ()> close_connection)
{
    if (connection_id.empty ()) {
        throw std::invalid_argument ("stream connection id is empty");
    }
    auto [connection, displaced] =
      _lane
        .run ([this, connection_id = std::move (connection_id),
               close_connection = std::move (close_connection)] () mutable {
            std::vector<stream_retained_outbound_t> displaced;
            const auto generation = ++_last_connection_generation[connection_id];
            stream_connection_t connection{std::move (connection_id), generation};
            const auto previous = _connections.find (connection.connection_id);
            if (previous != _connections.end ()) {
                for (auto &[actor_id, aggregate] : previous->second.bindings) {
                    aggregate.ingress_drain->accepts_completion = false;
                    const auto indexed = _actor_bindings.find (actor_id);
                    if (indexed != _actor_bindings.end ()
                        && indexed->second.connection == aggregate.binding.connection
                        && indexed->second.binding_generation
                             == aggregate.binding.binding_generation)
                        _actor_bindings.erase (indexed);
                    while (!aggregate.retained_outbound.empty ()) {
                        displaced.push_back (
                          std::move (aggregate.retained_outbound.front ().completion));
                        aggregate.retained_outbound.pop_front ();
                    }
                }
            }
            _connections[connection.connection_id] =
              connection_state_t{connection, {}, std::move (close_connection)};
            notify_changed ();
            return std::make_pair (std::move (connection), std::move (displaced));
        })
        .get ();
    settle_retained_outbound (std::move (displaced), false);
    return connection;
}

bool stream_session_registry_t::close (const stream_connection_t &connection)
{
    auto [closed, discarded] =
      _lane
        .run ([this, &connection] {
            std::vector<stream_retained_outbound_t> discarded;
            const auto current = _connections.find (connection.connection_id);
            if (current == _connections.end () || current->second.connection != connection) {
                return std::make_pair (false, std::move (discarded));
            }
            for (auto &[actor_id, aggregate] : current->second.bindings) {
                aggregate.ingress_drain->accepts_completion = false;
                const auto indexed = _actor_bindings.find (actor_id);
                if (indexed != _actor_bindings.end ()
                    && indexed->second.connection == aggregate.binding.connection
                    && indexed->second.binding_generation == aggregate.binding.binding_generation)
                    _actor_bindings.erase (indexed);
                while (!aggregate.retained_outbound.empty ()) {
                    discarded.push_back (
                      std::move (aggregate.retained_outbound.front ().completion));
                    aggregate.retained_outbound.pop_front ();
                }
            }
            _connections.erase (current);
            notify_changed ();
            return std::make_pair (true, std::move (discarded));
        })
        .get ();
    settle_retained_outbound (std::move (discarded), false);
    return closed;
}

std::vector<stream_binding_t>
stream_session_registry_t::bindings (const stream_connection_t &connection) const
{
    return _lane
      .run ([this, &connection] {
          const auto current = _connections.find (connection.connection_id);
          if (current == _connections.end () || current->second.connection != connection) {
              return std::vector<stream_binding_t>{};
          }
          std::vector<stream_binding_t> result;
          result.reserve (current->second.bindings.size ());
          for (const auto &[_, aggregate] : current->second.bindings)
              result.push_back (aggregate.binding);
          return result;
      })
      .get ();
}

std::pair<stateful_error_t, stream_binding_t>
stream_session_registry_t::bind (const stream_connection_t &connection,
                                 const object_ref_t &actor,
                                 std::uint64_t target_node_generation,
                                 std::uint64_t owner_lease_generation)
{
    const auto authority = _resolver (actor.key);
    if (!authority || !exact_actor (*authority, actor)) {
        return {authority ? stateful_error_t::generation_stale : stateful_error_t::not_found, {}};
    }

    return bind_verified (connection, actor, target_node_generation, owner_lease_generation, false,
                          0);
}

std::pair<stateful_error_t, stream_binding_t>
stream_session_registry_t::bind_remote (const stream_connection_t &connection,
                                        const object_ref_t &verified_actor,
                                        std::uint64_t target_node_generation,
                                        std::uint64_t owner_lease_generation,
                                        bool route_publish_pending,
                                        std::uint64_t binding_generation)
{
    if (verified_actor.kind != object_kind_t::actor || verified_actor.key.empty ()
        || verified_actor.object_generation == 0 || verified_actor.authority_owner_generation == 0
        || verified_actor.node_id.empty () || target_node_generation == 0
        || owner_lease_generation == 0) {
        return {stateful_error_t::invalid, {}};
    }
    return bind_verified (connection, verified_actor, target_node_generation,
                          owner_lease_generation, route_publish_pending, binding_generation);
}

std::pair<stateful_error_t, stream_binding_t>
stream_session_registry_t::bind_verified (const stream_connection_t &connection,
                                          const object_ref_t &actor,
                                          std::uint64_t target_node_generation,
                                          std::uint64_t owner_lease_generation,
                                          bool route_publish_pending,
                                          std::uint64_t binding_generation)
{
    auto [result, displaced] =
      _lane
        .run ([this, &connection, &actor, target_node_generation, owner_lease_generation,
               route_publish_pending, binding_generation] {
            std::vector<stream_retained_outbound_t> displaced;
            if (_all_sealed)
                return std::make_pair (std::pair{stateful_error_t::moving, stream_binding_t{}},
                                       std::move (displaced));
            const auto current = _connections.find (connection.connection_id);
            if (current == _connections.end () || current->second.connection != connection) {
                return std::make_pair (std::pair{stateful_error_t::conflict, stream_binding_t{}},
                                       std::move (displaced));
            }
            auto &state = current->second;
            const auto actor_id = actor.key;
            if (const auto existing = state.bindings.find (actor_id);
                existing != state.bindings.end () && existing->second.barrier_token)
                return std::make_pair (std::pair{stateful_error_t::moving, stream_binding_t{}},
                                       std::move (displaced));
            if (binding_generation != 0) {
                const auto *current_binding = current_aggregate_unlocked (actor_id);
                if (current_binding != nullptr
                    && binding_generation < current_binding->binding.binding_generation) {
                    return std::make_pair (
                      std::pair{stateful_error_t::conflict, stream_binding_t{}},
                      std::move (displaced));
                }
            }
            if (binding_generation == std::numeric_limits<std::uint64_t>::max ()
                || (binding_generation == 0
                    && (_next_binding_generation == 0
                        || _next_binding_generation
                             == std::numeric_limits<std::uint64_t>::max ()))) {
                return std::make_pair (std::pair{stateful_error_t::conflict, stream_binding_t{}},
                                       std::move (displaced));
            }
            // Binding generation is owner-lifecycle local, not connection local.
            // A reconnect therefore cannot alias the previous physical Session just
            // because both connections would otherwise start at generation one.
            const auto issued_generation =
              binding_generation != 0 ? binding_generation : _next_binding_generation++;
            if (_next_binding_generation <= issued_generation)
                _next_binding_generation = issued_generation + 1;
            stream_binding_t binding{connection, issued_generation, actor, target_node_generation,
                                     owner_lease_generation};
            // Spec 20 section 7 makes route replacement, held submission, and release
            // one serial span. The drain therefore belongs to that span, not to one
            // binding generation: an admitted frame retains its exact completion fence
            // while a reconnect installs the successor binding.
            std::shared_ptr<stream_ingress_drain_t> inherited_ingress_drain;
            const auto discard_binding = [&] (connection_state_t &owner) {
                const auto found = owner.bindings.find (actor_id);
                if (found == owner.bindings.end ())
                    return;
                if (!inherited_ingress_drain)
                    inherited_ingress_drain = found->second.ingress_drain;
                while (!found->second.retained_outbound.empty ()) {
                    displaced.push_back (
                      std::move (found->second.retained_outbound.front ().completion));
                    found->second.retained_outbound.pop_front ();
                }
                owner.bindings.erase (found);
            };
            const auto previous = _actor_bindings.find (actor_id);
            if (previous != _actor_bindings.end ()) {
                const auto previous_connection =
                  _connections.find (previous->second.connection.connection_id);
                if (previous_connection != _connections.end ()
                    && previous_connection->second.connection == previous->second.connection) {
                    discard_binding (previous_connection->second);
                }
            }
            discard_binding (state);
            session_binding_aggregate_t aggregate;
            aggregate.binding = binding;
            aggregate.route_publish_pending = route_publish_pending;
            if (inherited_ingress_drain)
                aggregate.ingress_drain = std::move (inherited_ingress_drain);
            state.bindings[actor_id] = std::move (aggregate);
            _actor_bindings[actor_id] =
              actor_binding_locator_t{binding.connection, binding.binding_generation};
            notify_changed ();
            return std::make_pair (std::pair{stateful_error_t::none, std::move (binding)},
                                   std::move (displaced));
        })
        .get ();
    settle_retained_outbound (std::move (displaced), false);
    return result;
}

std::optional<std::vector<stream_retained_outbound_t>>
stream_session_registry_t::complete_route_publish (const stream_binding_t &binding)
{
    return _lane
      .run ([this, &binding] () -> std::optional<std::vector<stream_retained_outbound_t>> {
          auto *aggregate = current_aggregate_unlocked (binding.actor.key);
          if (aggregate == nullptr || aggregate->binding != binding
              || !aggregate->route_publish_pending) {
              return std::nullopt;
          }
          aggregate->route_publish_pending = false;
          std::vector<stream_retained_outbound_t> retained;
          std::deque<retained_outbound_state_t> still_held;
          while (!aggregate->retained_outbound.empty ()) {
              auto pending = std::move (aggregate->retained_outbound.front ());
              aggregate->retained_outbound.pop_front ();
              if (exact_tenure_target (pending.tenure, binding)) {
                  retained.push_back (std::move (pending.completion));
              } else {
                  still_held.push_back (std::move (pending));
              }
          }
          aggregate->retained_outbound = std::move (still_held);
          notify_changed ();
          return retained;
      })
      .get ();
}

stateful_error_t stream_session_registry_t::unbind (const stream_binding_t &binding)
{
    auto [result, discarded] =
      _lane
        .run ([this, &binding] {
            std::vector<stream_retained_outbound_t> discarded;
            const auto current = _connections.find (binding.connection.connection_id);
            if (current == _connections.end () || current->second.connection != binding.connection
                || !current->second.bindings.contains (binding.actor.key)
                || current->second.bindings.at (binding.actor.key).binding != binding) {
                return std::make_pair (stateful_error_t::conflict, std::move (discarded));
            }
            auto &aggregate = current->second.bindings.at (binding.actor.key);
            aggregate.ingress_drain->accepts_completion = false;
            while (!aggregate.retained_outbound.empty ()) {
                discarded.push_back (std::move (aggregate.retained_outbound.front ().completion));
                aggregate.retained_outbound.pop_front ();
            }
            current->second.bindings.erase (binding.actor.key);
            const auto indexed = _actor_bindings.find (binding.actor.key);
            if (indexed != _actor_bindings.end ()
                && indexed->second.connection == binding.connection
                && indexed->second.binding_generation == binding.binding_generation)
                _actor_bindings.erase (indexed);
            notify_changed ();
            return std::make_pair (stateful_error_t::none, std::move (discarded));
        })
        .get ();
    settle_retained_outbound (std::move (discarded), false);
    return result;
}

stateful_error_t stream_session_registry_t::restore (const stream_binding_t &binding)
{
    return _lane
      .run ([this, &binding] {
          if (_all_sealed) {
              return stateful_error_t::moving;
          }
          const auto current = _connections.find (binding.connection.connection_id);
          if (current == _connections.end () || current->second.connection != binding.connection) {
              return stateful_error_t::conflict;
          }

          const auto indexed = _actor_bindings.find (binding.actor.key);
          if (indexed != _actor_bindings.end ()
              && (indexed->second.connection != binding.connection
                  || indexed->second.binding_generation != binding.binding_generation)) {
              return stateful_error_t::conflict;
          }
          const auto local = current->second.bindings.find (binding.actor.key);
          if (local != current->second.bindings.end ()
              && (local->second.binding != binding || local->second.barrier_token)) {
              return stateful_error_t::conflict;
          }

          session_binding_aggregate_t aggregate;
          aggregate.binding = binding;
          current->second.bindings[binding.actor.key] = std::move (aggregate);
          _actor_bindings[binding.actor.key] =
            actor_binding_locator_t{binding.connection, binding.binding_generation};
          if (_next_binding_generation <= binding.binding_generation
              && binding.binding_generation != std::numeric_limits<std::uint64_t>::max ()) {
              _next_binding_generation = binding.binding_generation + 1;
          }
          return stateful_error_t::none;
      })
      .get ();
}

std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
stream_session_registry_t::admit_inbound (const stream_binding_t &binding)
{
    return _lane
      .run ([this, &binding] () -> std::pair<stateful_error_t, std::optional<stream_dispatch_t>> {
          if (_all_sealed)
              return {stateful_error_t::moving, std::nullopt};
          const auto current = _connections.find (binding.connection.connection_id);
          if (current == _connections.end () || current->second.connection != binding.connection
              || !current->second.bindings.contains (binding.actor.key)
              || current->second.bindings.at (binding.actor.key).binding != binding) {
              return {stateful_error_t::conflict, std::nullopt};
          }
          auto &aggregate = current->second.bindings.at (binding.actor.key);
          if (aggregate.barrier_token)
              return {stateful_error_t::moving, std::nullopt};
          if (aggregate.next_inbound_sequence == std::numeric_limits<std::uint64_t>::max ()) {
              return {stateful_error_t::conflict, std::nullopt};
          }
          const auto sequence = aggregate.next_inbound_sequence++;
          aggregate.ingress_drain->active.emplace (binding.binding_generation, sequence);
          return {stateful_error_t::none,
                  stream_dispatch_t{binding, sequence, aggregate.ingress_drain}};
      })
      .get ();
}

std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
stream_session_registry_t::admit_inbound (const std::string &connection_id,
                                          std::uint64_t binding_generation,
                                          const std::string &actor_id,
                                          std::uint64_t expected_sequence,
                                          std::chrono::milliseconds timeout)
{
    if (connection_id.empty () || binding_generation == 0 || actor_id.empty ()
        || expected_sequence == 0 || timeout < std::chrono::milliseconds::zero ()) {
        return {stateful_error_t::invalid, std::nullopt};
    }
    using result_t = std::pair<stateful_error_t, std::optional<stream_dispatch_t>>;
    const auto attempt = [this, &connection_id, binding_generation, &actor_id, expected_sequence] {
        return _lane
          .run ([this, &connection_id, binding_generation, &actor_id, expected_sequence] {
              std::optional<result_t> result;
              const auto connection = _connections.find (connection_id);
              const auto aggregate = current_aggregate_unlocked (actor_id);
              if (connection == _connections.end () || aggregate == nullptr
                  || aggregate->binding.connection != connection->second.connection
                  || aggregate->binding.binding_generation != binding_generation) {
                  result.emplace (stateful_error_t::conflict, std::nullopt);
              } else if (!_all_sealed && !aggregate->barrier_token) {
                  if (aggregate->next_inbound_sequence > expected_sequence
                      || aggregate->next_inbound_sequence
                           == std::numeric_limits<std::uint64_t>::max ()) {
                      result.emplace (stateful_error_t::conflict, std::nullopt);
                  } else if (aggregate->next_inbound_sequence == expected_sequence) {
                      const auto sequence = aggregate->next_inbound_sequence++;
                      aggregate->ingress_drain->active.emplace (binding_generation, sequence);
                      notify_changed ();
                      result.emplace (
                        stateful_error_t::none,
                        stream_dispatch_t{aggregate->binding, sequence, aggregate->ingress_drain});
                  }
              }
              return std::make_pair (std::move (result),
                                     _changed_generation.load (std::memory_order_acquire));
          })
          .get ();
    };

    auto [result, observed_generation] = attempt ();
    if (result)
        return std::move (*result);
    if (timeout == std::chrono::milliseconds::zero ())
        return {stateful_error_t::moving, std::nullopt};
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    for (;;) {
        std::unique_lock lock (_changed_mutex);
        if (!_changed.wait_until (lock, deadline, [&] {
                return _changed_generation.load (std::memory_order_acquire) != observed_generation;
            })) {
            return {stateful_error_t::moving, std::nullopt};
        }
        lock.unlock ();
        std::tie (result, observed_generation) = attempt ();
        if (result)
            return std::move (*result);
    }
}

stateful_error_t stream_session_registry_t::complete_inbound (const stream_dispatch_t &dispatch)
{
    return _lane
      .run ([this, &dispatch] {
          if (!dispatch.drain || !dispatch.drain->accepts_completion)
              return stateful_error_t::conflict;
          const auto active = dispatch.drain->active.find (
            {dispatch.binding.binding_generation, dispatch.inbound_sequence});
          if (active == dispatch.drain->active.end ())
              return stateful_error_t::not_found;
          dispatch.drain->active.erase (active);
          notify_changed ();
          return stateful_error_t::none;
      })
      .get ();
}

std::pair<stateful_error_t, stream_barrier_t>
stream_session_registry_t::try_seal_actor (const object_ref_t &actor)
{
    if (actor.kind != object_kind_t::actor)
        return {stateful_error_t::invalid, {}};
    return _lane
      .run ([this, &actor] () -> std::pair<stateful_error_t, stream_barrier_t> {
          auto *affected = current_aggregate_unlocked (actor.key);
          if (affected != nullptr && exact_actor (affected->binding.actor, actor)) {
              if (affected->barrier_token)
                  return {stateful_error_t::moving, {}};
              if (!affected->ingress_drain->active.empty ())
                  return {stateful_error_t::backpressured, {}};
          } else {
              affected = nullptr;
          }
          if (_next_barrier_token == 0)
              return {stateful_error_t::conflict, {}};
          const auto token = _next_barrier_token++;
          if (affected != nullptr)
              affected->barrier_token = token;
          _barriers.emplace (token, actor);
          return {stateful_error_t::none, stream_barrier_t{token, actor}};
      })
      .get ();
}

stateful_error_t stream_session_registry_t::abort_barrier (const stream_barrier_t &barrier)
{
    return _lane
      .run ([this, &barrier] {
          const auto found = _barriers.find (barrier.token);
          if (found == _barriers.end () || !exact_actor (found->second, barrier.actor))
              return stateful_error_t::not_found;
          if (auto *aggregate = current_aggregate_unlocked (barrier.actor.key);
              aggregate != nullptr && aggregate->barrier_token
              && *aggregate->barrier_token == barrier.token)
              aggregate->barrier_token.reset ();
          _barriers.erase (found);
          notify_changed ();
          return stateful_error_t::none;
      })
      .get ();
}

stateful_error_t stream_session_registry_t::commit_barrier (const stream_barrier_t &barrier,
                                                            const object_ref_t &target)
{
    return _lane
      .run ([this, &barrier, &target] {
          const auto found = _barriers.find (barrier.token);
          if (found == _barriers.end () || !exact_actor (found->second, barrier.actor)
              || target.kind != object_kind_t::actor || target.key != barrier.actor.key
              || target.object_generation != barrier.actor.object_generation
              || target.authority_owner_generation <= barrier.actor.authority_owner_generation)
              return stateful_error_t::conflict;
          if (auto *aggregate = current_aggregate_unlocked (barrier.actor.key);
              aggregate != nullptr) {
              if (!aggregate->barrier_token || *aggregate->barrier_token != barrier.token
                  || !exact_actor (aggregate->binding.actor, barrier.actor))
                  return stateful_error_t::conflict;
              auto next = aggregate->binding;
              next.actor = target;
              aggregate->binding = next;
              aggregate->barrier_token.reset ();
          }
          _barriers.erase (found);
          notify_changed ();
          return stateful_error_t::none;
      })
      .get ();
}

stream_route_seal_admission_t
stream_session_registry_t::seal_remote_route (const std::string &connection_id,
                                              std::uint64_t binding_generation,
                                              const object_ref_t &actor,
                                              std::uint64_t target_node_generation,
                                              std::uint64_t owner_lease_generation)
{
    return _lane
      .run ([this, &connection_id, binding_generation, &actor, target_node_generation,
             owner_lease_generation] () -> stream_route_seal_admission_t {
          const auto connection = _connections.find (connection_id);
          auto *aggregate = current_aggregate_unlocked (actor.key);
          if (connection == _connections.end () || aggregate == nullptr
              || aggregate->binding.connection != connection->second.connection
              || aggregate->binding.binding_generation != binding_generation) {
              return {stateful_error_t::not_found, std::nullopt, {}, 0};
          }
          const auto last_sequence = aggregate->next_inbound_sequence - 1;
          const auto active = !aggregate->ingress_drain->active.empty ();
          if (aggregate->binding.actor.kind != object_kind_t::actor
              || actor.kind != object_kind_t::actor || aggregate->binding.actor.key != actor.key
              || aggregate->binding.actor.object_generation != actor.object_generation
              || aggregate->binding.actor.authority_owner_generation
                   != actor.authority_owner_generation
              || aggregate->binding.actor.node_id != actor.node_id || target_node_generation == 0
              || owner_lease_generation == 0
              || aggregate->binding.target_node_generation != target_node_generation
              || aggregate->binding.owner_lease_generation != owner_lease_generation
              || aggregate->barrier_token || _next_barrier_token == 0) {
              return {stateful_error_t::conflict, aggregate->binding, {}, last_sequence};
          }
          const auto token = _next_barrier_token++;
          const stream_barrier_t barrier{token, aggregate->binding.actor};
          aggregate->barrier_token = token;
          _barriers.emplace (token, aggregate->binding.actor);
          return {active ? stateful_error_t::backpressured : stateful_error_t::none,
                  aggregate->binding, barrier, last_sequence};
      })
      .get ();
}

bool stream_session_registry_t::remote_route_seal_ready (const stream_barrier_t &barrier) const
{
    return _lane
      .run ([this, &barrier] {
          const auto found = _barriers.find (barrier.token);
          if (found == _barriers.end () || !exact_actor (found->second, barrier.actor))
              return false;
          const auto *aggregate = current_aggregate_unlocked (barrier.actor.key);
          if (aggregate == nullptr)
              return true;
          return aggregate->ingress_drain->active.empty ();
      })
      .get ();
}

bool stream_session_registry_t::close_remote_route_seal (const stream_barrier_t &barrier)
{
    auto [closed, discarded, close_connection] =
      _lane
        .run ([this, &barrier] () -> std::tuple<bool, std::vector<stream_retained_outbound_t>,
                                                std::function<void ()>> {
            const auto found = _barriers.find (barrier.token);
            if (found == _barriers.end () || !exact_actor (found->second, barrier.actor))
                return {false, {}, {}};
            auto *aggregate = current_aggregate_unlocked (barrier.actor.key);
            if (aggregate == nullptr || !aggregate->barrier_token
                || *aggregate->barrier_token != barrier.token)
                return {false, {}, {}};
            const auto connection = aggregate->binding.connection;
            const auto connection_found = _connections.find (connection.connection_id);
            if (connection_found == _connections.end ()
                || connection_found->second.connection != connection)
                return {false, {}, {}};
            auto close_connection = std::move (connection_found->second.close_connection);
            std::vector<stream_retained_outbound_t> discarded;
            for (auto &[actor_id, binding] : connection_found->second.bindings) {
                binding.ingress_drain->accepts_completion = false;
                _actor_bindings.erase (actor_id);
                while (!binding.retained_outbound.empty ()) {
                    discarded.push_back (std::move (binding.retained_outbound.front ().completion));
                    binding.retained_outbound.pop_front ();
                }
            }
            for (auto current = _barriers.begin (); current != _barriers.end ();) {
                if (connection_found->second.bindings.contains (current->second.key))
                    current = _barriers.erase (current);
                else
                    ++current;
            }
            _connections.erase (connection_found);
            notify_changed ();
            return {true, std::move (discarded), std::move (close_connection)};
        })
        .get ();
    if (!closed)
        return false;
    settle_retained_outbound (std::move (discarded), false);
    if (close_connection) {
        try {
            close_connection ();
        }
        catch (...) {
        }
    }
    return true;
}

bool stream_session_registry_t::remote_route_sealed (const std::string &actor_id) const
{
    return _lane
      .run ([this, &actor_id] {
          const auto *aggregate = current_aggregate_unlocked (actor_id);
          return aggregate != nullptr && aggregate->barrier_token.has_value ();
      })
      .get ();
}

std::optional<stream_remote_tenure_proof_t>
stream_session_registry_t::remote_tenure_proof (const std::string &actor_id,
                                                std::uint64_t binding_generation,
                                                std::uint64_t object_generation,
                                                std::uint64_t authority_owner_generation,
                                                const std::string &target_node_id,
                                                std::uint64_t target_node_generation) const
{
    return _lane
      .run ([this, &actor_id, binding_generation, object_generation, authority_owner_generation,
             &target_node_id,
             target_node_generation] () -> std::optional<stream_remote_tenure_proof_t> {
          const auto *aggregate = current_aggregate_unlocked (actor_id);
          if (aggregate == nullptr || aggregate->binding.binding_generation != binding_generation
              || !aggregate->pending_remote_tenure)
              return std::nullopt;
          const auto &tenure = aggregate->pending_remote_tenure->tenure;
          if (tenure.actor_id != actor_id || tenure.object_generation != object_generation
              || tenure.authority_owner_generation != authority_owner_generation
              || tenure.target_node_id != target_node_id
              || tenure.target_node_generation != target_node_generation)
              return std::nullopt;
          return aggregate->pending_remote_tenure;
      })
      .get ();
}

bool stream_session_registry_t::confirm_remote_tenure (const stream_remote_tenure_t &tenure)
{
    return _lane
      .run ([this, &tenure] {
          auto *aggregate = current_aggregate_unlocked (tenure.actor_id);
          if (aggregate == nullptr || aggregate->binding.actor.kind != object_kind_t::actor
              || aggregate->binding.actor.key != tenure.actor_id
              || aggregate->binding.actor.object_generation != tenure.object_generation
              || aggregate->binding.actor.authority_owner_generation
                   != tenure.authority_owner_generation
              || aggregate->binding.actor.node_id != tenure.target_node_id
              || aggregate->binding.target_node_generation != tenure.target_node_generation
              || aggregate->binding.binding_generation != tenure.binding_generation
              || tenure.owner_lease_generation == 0
              || (aggregate->binding.owner_lease_generation != 0
                  && aggregate->binding.owner_lease_generation != tenure.owner_lease_generation)) {
              return false;
          }
          aggregate->binding.owner_lease_generation = tenure.owner_lease_generation;
          return true;
      })
      .get ();
}

bool stream_session_registry_t::memoize_remote_tenure (
  stream_remote_tenure_proof_t proof, std::uint64_t previous_authority_owner_generation)
{
    return _lane
      .run ([this, proof = std::move (proof), previous_authority_owner_generation] () mutable {
          auto *aggregate = current_aggregate_unlocked (proof.tenure.actor_id);
          return aggregate != nullptr
                 && memoize_remote_tenure_unlocked (*aggregate, std::move (proof),
                                                    previous_authority_owner_generation);
      })
      .get ();
}

stream_outbound_admission_t
stream_session_registry_t::admit_outbound (const stream_remote_tenure_t &tenure,
                                           std::optional<stream_remote_tenure_proof_t> first_proof,
                                           stream_retained_outbound_t retained)
{
    return _lane
      .run ([this, &tenure, first_proof = std::move (first_proof),
             retained = std::move (retained)] () mutable -> stream_outbound_admission_t {
          auto *aggregate = current_aggregate_unlocked (tenure.actor_id);
          if (aggregate == nullptr
              || aggregate->binding.binding_generation != tenure.binding_generation
              || aggregate->binding.actor.object_generation != tenure.object_generation) {
              return {stateful_error_t::conflict};
          }
          if (exact_tenure_target (tenure, aggregate->binding)
              && aggregate->route_publish_pending) {
              if (!retained || aggregate->next_outbound_token == 0)
                  return {stateful_error_t::conflict};
              const auto token = aggregate->next_outbound_token++;
              aggregate->retained_outbound.push_back (
                retained_outbound_state_t{token, tenure, std::move (retained)});
              return {stateful_error_t::none, stream_outbound_admission_kind_t::retained, token};
          }
          if (exact_tenure_target (tenure, aggregate->binding)) {
              return {stateful_error_t::none, stream_outbound_admission_kind_t::immediate, 0};
          }
          if (!retained
              || tenure.authority_owner_generation
                   <= aggregate->binding.actor.authority_owner_generation) {
              return {stateful_error_t::conflict};
          }
          if (!aggregate->pending_remote_tenure) {
              if (!first_proof
                  || !memoize_remote_tenure_unlocked (
                    *aggregate, std::move (*first_proof),
                    aggregate->binding.actor.authority_owner_generation)) {
                  return {stateful_error_t::conflict};
              }
          }
          if (!aggregate->pending_remote_tenure
              || aggregate->pending_remote_tenure->tenure != tenure
              || aggregate->next_outbound_token == 0) {
              return {stateful_error_t::conflict};
          }
          const auto token = aggregate->next_outbound_token++;
          aggregate->retained_outbound.push_back (
            retained_outbound_state_t{token, tenure, std::move (retained)});
          return {stateful_error_t::none, stream_outbound_admission_kind_t::retained, token};
      })
      .get ();
}

std::vector<stream_retained_outbound_t>
stream_session_registry_t::discard_retained_outbound (const std::string &actor_id,
                                                      std::uint64_t binding_generation)
{
    return _lane
      .run ([this, &actor_id, binding_generation] {
          auto *aggregate = current_aggregate_unlocked (actor_id);
          if (aggregate == nullptr || aggregate->binding.binding_generation != binding_generation)
              return std::vector<stream_retained_outbound_t>{};
          std::vector<stream_retained_outbound_t> discarded;
          discarded.reserve (aggregate->retained_outbound.size ());
          while (!aggregate->retained_outbound.empty ()) {
              discarded.push_back (std::move (aggregate->retained_outbound.front ().completion));
              aggregate->retained_outbound.pop_front ();
          }
          aggregate->pending_remote_tenure.reset ();
          return discarded;
      })
      .get ();
}

std::vector<stream_retained_outbound_t> stream_session_registry_t::take_all_retained_outbound ()
{
    return _lane
      .run ([this] {
          std::vector<stream_retained_outbound_t> retained;
          for (auto &[_, connection] : _connections) {
              for (auto &[__, aggregate] : connection.bindings) {
                  while (!aggregate.retained_outbound.empty ()) {
                      retained.push_back (
                        std::move (aggregate.retained_outbound.front ().completion));
                      aggregate.retained_outbound.pop_front ();
                  }
                  aggregate.pending_remote_tenure.reset ();
              }
          }
          return retained;
      })
      .get ();
}

stream_route_admission_t
stream_session_registry_t::commit_remote_route (const std::string &connection_id,
                                                std::uint64_t binding_generation,
                                                const std::string &actor_id,
                                                std::uint64_t object_generation,
                                                std::uint64_t previous_authority_owner_generation,
                                                object_ref_t target,
                                                std::uint64_t target_node_generation,
                                                std::uint64_t target_owner_lease_generation,
                                                route_terminal_commit_t commit_terminal)
{
    auto admission =
      _lane
        .run ([this, &connection_id, binding_generation, &actor_id, object_generation,
               previous_authority_owner_generation, target = std::move (target),
               target_node_generation,
               target_owner_lease_generation] () mutable -> stream_route_admission_t {
            const auto connection = _connections.find (connection_id);
            auto *aggregate = current_aggregate_unlocked (actor_id);
            if (connection == _connections.end () || aggregate == nullptr
                || aggregate->binding.connection != connection->second.connection
                || aggregate->binding.binding_generation != binding_generation) {
                return {stateful_error_t::not_found, std::nullopt, 0, {}};
            }
            const auto last_sequence = aggregate->next_inbound_sequence - 1;
            if (aggregate->binding.actor.kind != object_kind_t::actor
                || aggregate->binding.actor.object_generation != object_generation
                || aggregate->binding.actor.authority_owner_generation
                     != previous_authority_owner_generation
                || !aggregate->barrier_token || !_barriers.contains (*aggregate->barrier_token)
                || !exact_actor (_barriers.at (*aggregate->barrier_token), aggregate->binding.actor)
                || target.kind != object_kind_t::actor || target.key != actor_id
                || target.object_generation != object_generation
                || target.authority_owner_generation <= previous_authority_owner_generation
                || target_node_generation == 0 || !aggregate->ingress_drain->active.empty ()) {
                return {stateful_error_t::conflict, aggregate->binding, last_sequence, {}};
            }
            auto next = aggregate->binding;
            next.actor = std::move (target);
            next.target_node_generation = target_node_generation;
            next.owner_lease_generation = target_owner_lease_generation;
            const stream_remote_tenure_t target_tenure{
              actor_id,           object_generation,      next.actor.authority_owner_generation,
              next.actor.node_id, target_node_generation, target_owner_lease_generation,
              binding_generation};
            std::vector<stream_retained_outbound_t> retained;
            if (aggregate->pending_remote_tenure
                && aggregate->pending_remote_tenure->tenure == target_tenure) {
                retained.reserve (aggregate->retained_outbound.size ());
                while (!aggregate->retained_outbound.empty ()) {
                    retained.push_back (
                      std::move (aggregate->retained_outbound.front ().completion));
                    aggregate->retained_outbound.pop_front ();
                }
            }
            aggregate->pending_remote_tenure.reset ();
            aggregate->binding = next;
            _barriers.erase (*aggregate->barrier_token);
            aggregate->barrier_token.reset ();
            stream_route_admission_t admission{stateful_error_t::none, next, last_sequence,
                                               std::move (retained)};
            notify_changed ();
            return admission;
        })
        .get ();
    if (admission.error != stateful_error_t::none)
        return admission;
    try {
        if (commit_terminal)
            (void) commit_terminal (admission);
    }
    catch (...) {
    }
    return admission;
}

stream_route_admission_t stream_session_registry_t::acknowledge_remote_abort (
  const std::string &connection_id,
  std::uint64_t binding_generation,
  const std::string &actor_id,
  std::uint64_t object_generation,
  std::uint64_t current_authority_owner_generation,
  route_terminal_commit_t commit_terminal)
{
    auto admission =
      _lane
        .run ([this, &connection_id, binding_generation, &actor_id, object_generation,
               current_authority_owner_generation] () -> stream_route_admission_t {
            const auto connection = _connections.find (connection_id);
            auto *aggregate = current_aggregate_unlocked (actor_id);
            if (connection == _connections.end () || aggregate == nullptr
                || aggregate->binding.connection != connection->second.connection
                || aggregate->binding.binding_generation != binding_generation) {
                return {stateful_error_t::not_found, std::nullopt, 0, {}};
            }
            const auto last_sequence = aggregate->next_inbound_sequence - 1;
            if (aggregate->binding.actor.kind != object_kind_t::actor
                || aggregate->binding.actor.object_generation != object_generation
                || aggregate->binding.actor.authority_owner_generation
                     != current_authority_owner_generation
                || !aggregate->barrier_token || !_barriers.contains (*aggregate->barrier_token)
                || !exact_actor (_barriers.at (*aggregate->barrier_token),
                                 aggregate->binding.actor)) {
                return {stateful_error_t::conflict, aggregate->binding, last_sequence, {}};
            }
            std::vector<stream_retained_outbound_t> retained;
            retained.reserve (aggregate->retained_outbound.size ());
            while (!aggregate->retained_outbound.empty ()) {
                retained.push_back (std::move (aggregate->retained_outbound.front ().completion));
                aggregate->retained_outbound.pop_front ();
            }
            aggregate->pending_remote_tenure.reset ();
            _barriers.erase (*aggregate->barrier_token);
            aggregate->barrier_token.reset ();
            stream_route_admission_t admission{stateful_error_t::none, aggregate->binding,
                                               last_sequence, std::move (retained)};
            notify_changed ();
            return admission;
        })
        .get ();
    if (admission.error != stateful_error_t::none)
        return admission;
    try {
        if (commit_terminal)
            (void) commit_terminal (admission);
    }
    catch (...) {
    }
    return admission;
}

std::optional<stream_binding_t>
stream_session_registry_t::current_binding (const std::string &actor_id) const
{
    return _lane
      .run ([this, &actor_id] {
          const auto *aggregate = current_aggregate_unlocked (actor_id);
          return aggregate == nullptr ? std::nullopt : std::make_optional (aggregate->binding);
      })
      .get ();
}

bool stream_session_registry_t::try_seal_all ()
{
    return _lane
      .run ([this] {
          if (_all_sealed)
              return true;
          for (const auto &[_, state] : _connections) {
              for (const auto &[__, aggregate] : state.bindings)
                  if (!aggregate.ingress_drain->active.empty ())
                      return false;
          }
          _all_sealed = true;
          return true;
      })
      .get ();
}

void stream_session_registry_t::release_all () noexcept
{
    _lane
      .run ([this] {
          _all_sealed = false;
          notify_changed ();
      })
      .get ();
}

void stream_session_registry_t::force_close_all () noexcept
{
    auto closed = _lane
                    .run ([this] {
                        decltype (_connections) closed;
                        _all_sealed = true;
                        _barriers.clear ();
                        _actor_bindings.clear ();
                        for (auto &[_, connection] : _connections) {
                            for (auto &[__, aggregate] : connection.bindings)
                                aggregate.ingress_drain->accepts_completion = false;
                        }
                        closed.swap (_connections);
                        notify_changed ();
                        return closed;
                    })
                    .get ();
    for (auto &[_, connection] : closed) {
        for (auto &[__, aggregate] : connection.bindings) {
            while (!aggregate.retained_outbound.empty ()) {
                auto settle = std::move (aggregate.retained_outbound.front ().completion);
                aggregate.retained_outbound.pop_front ();
                if (!settle)
                    continue;
                try {
                    settle (false);
                }
                catch (...) {
                }
            }
        }
    }
}

bool stream_session_registry_t::is_current (const stream_binding_t &binding) const
{
    return _lane
      .run ([this, &binding] {
          const auto current = _connections.find (binding.connection.connection_id);
          return current != _connections.end () && current->second.connection == binding.connection
                 && current->second.bindings.contains (binding.actor.key)
                 && current->second.bindings.at (binding.actor.key).binding == binding;
      })
      .get ();
}

bool stream_session_registry_t::is_current_for_connection (const stream_connection_t &connection,
                                                           const stream_binding_t &binding) const
{
    return _lane
      .run ([this, &connection, &binding] {
          if (binding.connection != connection)
              return false;
          const auto owner = _connections.find (connection.connection_id);
          if (owner == _connections.end () || owner->second.connection != connection) {
              return false;
          }
          const auto owned = owner->second.bindings.find (binding.actor.key);
          if (owned == owner->second.bindings.end () || owned->second.binding != binding) {
              return false;
          }
          const auto indexed = _actor_bindings.find (binding.actor.key);
          return indexed != _actor_bindings.end () && indexed->second.connection == connection
                 && indexed->second.binding_generation == binding.binding_generation;
      })
      .get ();
}

stream_session_registry_t::session_binding_aggregate_t *
stream_session_registry_t::current_aggregate_unlocked (const std::string &actor_id)
{
    const auto indexed = _actor_bindings.find (actor_id);
    if (indexed == _actor_bindings.end ())
        return nullptr;
    const auto connection = _connections.find (indexed->second.connection.connection_id);
    if (connection == _connections.end ()
        || connection->second.connection != indexed->second.connection)
        return nullptr;
    const auto aggregate = connection->second.bindings.find (actor_id);
    if (aggregate == connection->second.bindings.end ()
        || aggregate->second.binding.binding_generation != indexed->second.binding_generation)
        return nullptr;
    return &aggregate->second;
}

const stream_session_registry_t::session_binding_aggregate_t *
stream_session_registry_t::current_aggregate_unlocked (const std::string &actor_id) const
{
    const auto indexed = _actor_bindings.find (actor_id);
    if (indexed == _actor_bindings.end ())
        return nullptr;
    const auto connection = _connections.find (indexed->second.connection.connection_id);
    if (connection == _connections.end ()
        || connection->second.connection != indexed->second.connection)
        return nullptr;
    const auto aggregate = connection->second.bindings.find (actor_id);
    if (aggregate == connection->second.bindings.end ()
        || aggregate->second.binding.binding_generation != indexed->second.binding_generation)
        return nullptr;
    return &aggregate->second;
}

void stream_session_registry_t::notify_changed () noexcept
{
    {
        std::lock_guard lock (_changed_mutex);
        _changed_generation.fetch_add (1, std::memory_order_release);
    }
    _changed.notify_all ();
}

bool stream_session_registry_t::exact_tenure_target (const stream_remote_tenure_t &tenure,
                                                     const stream_binding_t &binding)
{
    return tenure.actor_id == binding.actor.key
           && tenure.object_generation == binding.actor.object_generation
           && tenure.authority_owner_generation == binding.actor.authority_owner_generation
           && tenure.target_node_id == binding.actor.node_id
           && tenure.target_node_generation == binding.target_node_generation
           && tenure.owner_lease_generation == binding.owner_lease_generation
           && tenure.binding_generation == binding.binding_generation;
}

bool stream_session_registry_t::memoize_remote_tenure_unlocked (
  session_binding_aggregate_t &aggregate,
  stream_remote_tenure_proof_t proof,
  std::uint64_t previous_authority_owner_generation)
{
    const auto &tenure = proof.tenure;
    if (proof.owner_id.empty () || tenure.actor_id.empty () || tenure.target_node_id.empty ()
        || tenure.object_generation == 0 || tenure.authority_owner_generation == 0
        || tenure.target_node_generation == 0 || tenure.owner_lease_generation == 0
        || tenure.binding_generation == 0 || aggregate.binding.actor.kind != object_kind_t::actor
        || tenure.actor_id != aggregate.binding.actor.key
        || tenure.object_generation != aggregate.binding.actor.object_generation
        || tenure.binding_generation != aggregate.binding.binding_generation
        || previous_authority_owner_generation != aggregate.binding.actor.authority_owner_generation
        || tenure.authority_owner_generation <= previous_authority_owner_generation) {
        return false;
    }
    if (aggregate.pending_remote_tenure)
        return aggregate.pending_remote_tenure == proof;
    aggregate.pending_remote_tenure = std::move (proof);
    return true;
}

bool stream_session_registry_t::exact_actor (const object_ref_t &left, const object_ref_t &right)
{
    return left.kind == object_kind_t::actor && right.kind == object_kind_t::actor && left == right;
}

} // namespace zlink::framework::runtime::stateful
