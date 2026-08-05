/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/stateful/stateful_object_runtime.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::runtime::stateful
{

struct stream_connection_t
{
    std::string connection_id;
    std::uint64_t connection_generation = 0;

    friend bool operator== (const stream_connection_t &,
                            const stream_connection_t &) = default;
};

struct stream_binding_t
{
    stream_connection_t connection;
    std::uint64_t binding_generation = 0;
    object_ref_t actor;
    std::uint64_t target_node_generation = 0;
    std::uint64_t owner_lease_generation = 0;

    friend bool operator== (const stream_binding_t &,
                            const stream_binding_t &) = default;
};

struct stream_dispatch_t
{
    stream_binding_t binding;
    std::uint64_t inbound_sequence = 0;
};

struct stream_barrier_t
{
    std::uint64_t token = 0;
    object_ref_t actor;
};

struct stream_route_admission_t
{
    stateful_error_t error = stateful_error_t::none;
    std::optional<stream_binding_t> binding;
    std::uint64_t last_accepted_sequence = 0;
};

struct stream_route_seal_admission_t
{
    stateful_error_t error = stateful_error_t::none;
    std::optional<stream_binding_t> binding;
    stream_barrier_t barrier;
    std::uint64_t last_accepted_sequence = 0;
};

class stream_session_registry_t
{
  public:
    using authority_resolver_t =
      std::function<std::optional<object_ref_t> (const std::string &)>;

    explicit stream_session_registry_t (authority_resolver_t resolver);

    stream_connection_t open (std::string connection_id);
    bool close (const stream_connection_t &connection);
    std::pair<stateful_error_t, stream_binding_t> bind (
      const stream_connection_t &connection,
      const object_ref_t &actor,
      std::uint64_t target_node_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    stateful_error_t unbind (const stream_binding_t &binding);
    /* Restores a binding that was displaced by a later bind operation. This
     * is an internal transaction-compensation step for a failed owner-layer
     * route update; it does not create a new binding generation. */
    stateful_error_t restore (const stream_binding_t &binding);
    std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
    admit_inbound (const stream_binding_t &binding);
    stateful_error_t complete_inbound (const stream_dispatch_t &dispatch);
    std::pair<stateful_error_t, stream_barrier_t>
    try_seal_actor (const object_ref_t &actor);
    stateful_error_t abort_barrier (const stream_barrier_t &barrier);
    stateful_error_t commit_barrier (
      const stream_barrier_t &barrier, const object_ref_t &target);
    stream_route_seal_admission_t seal_remote_route (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const object_ref_t &actor,
      std::uint64_t target_node_generation,
      std::uint64_t owner_lease_generation);
    stream_route_admission_t commit_remote_route (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const std::string &actor_id,
      std::uint64_t object_generation,
      std::uint64_t previous_authority_owner_generation,
      object_ref_t target,
      std::uint64_t target_node_generation,
      std::uint64_t replayed_high_water);
    stream_route_admission_t acknowledge_remote_abort (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const std::string &actor_id,
      std::uint64_t object_generation,
      std::uint64_t current_authority_owner_generation);
    bool try_seal_all ();
    void release_all () noexcept;
    void force_close_all () noexcept;
    bool is_current (const stream_binding_t &binding) const;
    std::optional<stream_binding_t> current_binding (
      const std::string &actor_id) const;

  private:
    struct connection_state_t
    {
        stream_connection_t connection;
        std::uint64_t next_binding_generation = 1;
        std::uint64_t next_inbound_sequence = 1;
        std::map<std::string, stream_binding_t> bindings;
        std::map<std::uint64_t, std::string> active_inbound;
        std::map<std::string, std::uint64_t> barrier_tokens;
    };

    static bool exact_actor (const object_ref_t &left,
                             const object_ref_t &right);

    authority_resolver_t _resolver;
    mutable std::mutex _mutex;
    std::map<std::string, connection_state_t> _connections;
    std::map<std::string, std::uint64_t> _last_connection_generation;
    std::map<std::string, stream_binding_t> _actor_bindings;
    std::map<std::uint64_t, object_ref_t> _barriers;
    std::uint64_t _next_barrier_token = 1;
    bool _all_sealed = false;
};

} // namespace zlink::framework::runtime::stateful
