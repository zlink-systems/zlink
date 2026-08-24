/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/stateful/stateful_object_runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

struct stream_ingress_drain_t
{
    std::set<std::pair<std::uint64_t, std::uint64_t>> active;
    bool accepts_completion = true;
};

struct stream_dispatch_t
{
    stream_binding_t binding;
    std::uint64_t inbound_sequence = 0;
    std::shared_ptr<stream_ingress_drain_t> drain;
};

struct stream_barrier_t
{
    std::uint64_t token = 0;
    object_ref_t actor;
};

struct stream_remote_tenure_t
{
    std::string actor_id;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string target_node_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t owner_lease_generation = 0;
    std::uint64_t binding_generation = 0;

    friend bool operator== (const stream_remote_tenure_t &,
                            const stream_remote_tenure_t &) = default;
};

struct stream_remote_tenure_proof_t
{
    stream_remote_tenure_t tenure;
    std::string owner_id;

    friend bool operator== (const stream_remote_tenure_proof_t &,
                            const stream_remote_tenure_proof_t &) = default;
};

using stream_retained_outbound_t = std::function<void (bool)>;

enum class stream_outbound_admission_kind_t
{
    immediate,
    retained
};

struct stream_outbound_admission_t
{
    stateful_error_t error = stateful_error_t::none;
    stream_outbound_admission_kind_t kind =
      stream_outbound_admission_kind_t::immediate;
    std::uint64_t token = 0;
};

struct stream_route_admission_t
{
    stateful_error_t error = stateful_error_t::none;
    std::optional<stream_binding_t> binding;
    std::uint64_t last_accepted_sequence = 0;
    std::vector<stream_retained_outbound_t> retained_outbound;
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

    stream_connection_t open (
      std::string connection_id,
      std::function<void ()> close_connection = {});
    bool close (const stream_connection_t &connection);
    std::vector<stream_binding_t> bindings (
      const stream_connection_t &connection) const;
    std::pair<stateful_error_t, stream_binding_t> bind (
      const stream_connection_t &connection,
      const object_ref_t &actor,
      std::uint64_t target_node_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    std::pair<stateful_error_t, stream_binding_t> bind_remote (
      const stream_connection_t &connection,
      const object_ref_t &verified_actor,
      std::uint64_t target_node_generation,
      std::uint64_t owner_lease_generation,
      bool route_publish_pending = false,
      std::uint64_t binding_generation = 0);
    std::optional<std::vector<stream_retained_outbound_t>>
    complete_route_publish (const stream_binding_t &binding);
    stateful_error_t unbind (const stream_binding_t &binding);
    /* Restores a binding that was displaced by a later bind operation. This
     * is an internal transaction-compensation step for a failed owner-layer
     * route update; it does not create a new binding generation. */
    stateful_error_t restore (const stream_binding_t &binding);
    std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
    admit_inbound (const stream_binding_t &binding);
    std::pair<stateful_error_t, std::optional<stream_dispatch_t>>
    admit_inbound (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const std::string &actor_id,
      std::uint64_t expected_sequence,
      std::chrono::milliseconds timeout);
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
    bool remote_route_seal_ready (
      const stream_barrier_t &barrier) const;
    bool close_remote_route_seal (
      const stream_barrier_t &barrier);
    bool remote_route_sealed (const std::string &actor_id) const;
    std::optional<stream_remote_tenure_proof_t>
    remote_tenure_proof (
      const std::string &actor_id,
      std::uint64_t binding_generation,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation,
      const std::string &target_node_id,
      std::uint64_t target_node_generation) const;
    bool confirm_remote_tenure (
      const stream_remote_tenure_t &tenure);
    bool memoize_remote_tenure (
      stream_remote_tenure_proof_t proof,
      std::uint64_t previous_authority_owner_generation);
    stream_outbound_admission_t admit_outbound (
      const stream_remote_tenure_t &tenure,
      std::optional<stream_remote_tenure_proof_t> first_proof,
      stream_retained_outbound_t retained);
    std::vector<stream_retained_outbound_t>
    discard_retained_outbound (
      const std::string &actor_id,
      std::uint64_t binding_generation);
    std::vector<stream_retained_outbound_t>
    take_all_retained_outbound ();
    /* Internal projection hook. The aggregate commits before this hook runs,
     * and hook failure cannot veto or roll back the committed route. */
    using route_terminal_commit_t =
      std::function<bool (const stream_route_admission_t &)>;
    stream_route_admission_t commit_remote_route (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const std::string &actor_id,
      std::uint64_t object_generation,
      std::uint64_t previous_authority_owner_generation,
      object_ref_t target,
      std::uint64_t target_node_generation,
      std::uint64_t target_owner_lease_generation,
      route_terminal_commit_t commit_terminal = {});
    stream_route_admission_t acknowledge_remote_abort (
      const std::string &connection_id,
      std::uint64_t binding_generation,
      const std::string &actor_id,
      std::uint64_t object_generation,
      std::uint64_t current_authority_owner_generation,
      route_terminal_commit_t commit_terminal = {});
    bool try_seal_all ();
    void release_all () noexcept;
    void force_close_all () noexcept;
    bool is_current (const stream_binding_t &binding) const;
    bool is_current_for_connection (
      const stream_connection_t &connection,
      const stream_binding_t &binding) const;
    std::optional<stream_binding_t> current_binding (
      const std::string &actor_id) const;

  private:
    struct retained_outbound_state_t
    {
        std::uint64_t token = 0;
        stream_remote_tenure_t tenure;
        stream_retained_outbound_t completion;
    };

    struct session_binding_aggregate_t
    {
        stream_binding_t binding;
        std::uint64_t next_inbound_sequence = 1;
        std::shared_ptr<stream_ingress_drain_t> ingress_drain =
          std::make_shared<stream_ingress_drain_t> ();
        std::optional<std::uint64_t> barrier_token;
        std::optional<stream_remote_tenure_proof_t> pending_remote_tenure;
        std::deque<retained_outbound_state_t> retained_outbound;
        std::uint64_t next_outbound_token = 1;
        bool route_publish_pending = false;
    };

    struct connection_state_t
    {
        stream_connection_t connection;
        std::map<std::string, session_binding_aggregate_t> bindings;
        std::function<void ()> close_connection;
    };

    struct actor_binding_locator_t
    {
        stream_connection_t connection;
        std::uint64_t binding_generation = 0;
    };

    static bool exact_actor (const object_ref_t &left,
                             const object_ref_t &right);
    std::pair<stateful_error_t, stream_binding_t> bind_verified (
      const stream_connection_t &connection,
      const object_ref_t &actor,
      std::uint64_t target_node_generation,
      std::uint64_t owner_lease_generation,
      bool route_publish_pending = false,
      std::uint64_t binding_generation = 0);
    session_binding_aggregate_t *current_aggregate_unlocked (
      const std::string &actor_id);
    const session_binding_aggregate_t *current_aggregate_unlocked (
      const std::string &actor_id) const;
    static bool exact_tenure_target (
      const stream_remote_tenure_t &tenure,
      const stream_binding_t &binding);
    bool memoize_remote_tenure_unlocked (
      session_binding_aggregate_t &aggregate,
      stream_remote_tenure_proof_t proof,
      std::uint64_t previous_authority_owner_generation);

    authority_resolver_t _resolver;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::map<std::string, connection_state_t> _connections;
    std::map<std::string, std::uint64_t> _last_connection_generation;
    std::map<std::string, actor_binding_locator_t> _actor_bindings;
    std::map<std::uint64_t, object_ref_t> _barriers;
    std::uint64_t _next_binding_generation = 1;
    std::uint64_t _next_barrier_token = 1;
    bool _all_sealed = false;
};

} // namespace zlink::framework::runtime::stateful
