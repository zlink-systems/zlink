/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/execution/state_lane.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <atomic>
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

    friend bool operator== (const stream_connection_t &, const stream_connection_t &) = default;
};

struct stream_binding_t
{
    stream_connection_t connection;
    std::uint64_t binding_generation = 0;
    object_ref_t actor;
    std::uint64_t target_node_generation = 0;
    std::uint64_t owner_lease_generation = 0;

    friend bool operator== (const stream_binding_t &, const stream_binding_t &) = default;
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

using stream_retained_outbound_t = std::function<void (bool)>;

enum class stream_outbound_admission_kind_t
{
    immediate,
    retained
};

struct stream_outbound_admission_t
{
    stateful_error_t error = stateful_error_t::none;
    stream_outbound_admission_kind_t kind = stream_outbound_admission_kind_t::immediate;
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
    using authority_resolver_t = std::function<std::optional<object_ref_t> (const std::string &)>;

    explicit stream_session_registry_t (authority_resolver_t resolver,
                                        std::function<void ()> activity_handler = {});

    stream_connection_t open (std::string connection_id,
                              std::function<void ()> close_connection = {});
    bool close (const stream_connection_t &connection);
    std::vector<stream_binding_t> bindings (const stream_connection_t &connection) const;
    std::pair<stateful_error_t, stream_binding_t> bind (const stream_connection_t &connection,
                                                        const object_ref_t &actor,
                                                        std::uint64_t target_node_generation = 0,
                                                        std::uint64_t owner_lease_generation = 0);
    std::pair<stateful_error_t, stream_binding_t>
    bind_remote (const stream_connection_t &connection,
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
    admit_inbound (const std::string &connection_id,
                   std::uint64_t binding_generation,
                   const std::string &actor_id,
                   std::uint64_t expected_sequence,
                   std::chrono::milliseconds timeout);
    stateful_error_t complete_inbound (const stream_dispatch_t &dispatch);
    std::pair<stateful_error_t, stream_barrier_t> try_seal_actor (const object_ref_t &actor);
    stateful_error_t abort_barrier (const stream_barrier_t &barrier);
    stateful_error_t commit_barrier (const stream_barrier_t &barrier, const object_ref_t &target);
    stream_route_seal_admission_t seal_remote_route (const std::string &connection_id,
                                                     std::uint64_t binding_generation,
                                                     const std::string &actor_id,
                                                     std::uint64_t object_generation);
    bool remote_route_seal_ready (const stream_barrier_t &barrier) const;
    bool close_remote_route_seal (const stream_barrier_t &barrier);
    bool remote_route_sealed (const std::string &actor_id) const;
    /* Session-owned push admission (Session-Actor binding §3 item 3, §8.1).
     * The current binding of `actor_id` must carry the same binding
     * generation and ObjectGeneration. A sealed or not yet published binding
     * holds the push until the seal or the publication ends. */
    stream_outbound_admission_t admit_outbound (const std::string &actor_id,
                                                std::uint64_t object_generation,
                                                std::uint64_t binding_generation,
                                                stream_retained_outbound_t retained);
    std::vector<stream_retained_outbound_t>
    discard_retained_outbound (const std::string &actor_id, std::uint64_t binding_generation);
    std::vector<stream_retained_outbound_t> take_all_retained_outbound ();
    /* Internal projection hook. commit_remote_route runs it after the
     * aggregate commits but before the state lane publishes that aggregate to
     * readers. It must not re-enter this registry; hook failure cannot veto or
     * roll back the committed route. */
    using route_terminal_commit_t = std::function<bool (const stream_route_admission_t &)>;
    stream_route_admission_t commit_remote_route (const std::string &connection_id,
                                                  std::uint64_t binding_generation,
                                                  const std::string &actor_id,
                                                  std::uint64_t object_generation,
                                                  std::uint64_t previous_authority_owner_generation,
                                                  object_ref_t target,
                                                  std::uint64_t target_node_generation,
                                                  route_terminal_commit_t commit_terminal = {});
    stream_route_admission_t
    acknowledge_remote_abort (const std::string &connection_id,
                              std::uint64_t binding_generation,
                              const std::string &actor_id,
                              std::uint64_t object_generation,
                              std::uint64_t current_authority_owner_generation,
                              route_terminal_commit_t commit_terminal = {});
    bool try_seal_all ();
    void release_all () noexcept;
    void force_close_all () noexcept;
    bool is_current (const stream_binding_t &binding) const;
    bool is_current_for_connection (const stream_connection_t &connection,
                                    const stream_binding_t &binding) const;
    std::optional<stream_binding_t> current_binding (const std::string &actor_id) const;

  private:
    struct session_binding_aggregate_t
    {
        stream_binding_t binding;
        std::uint64_t next_inbound_sequence = 1;
        std::shared_ptr<stream_ingress_drain_t> ingress_drain =
          std::make_shared<stream_ingress_drain_t> ();
        std::optional<std::uint64_t> barrier_token;
        std::deque<stream_retained_outbound_t> retained_outbound;
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

    static bool exact_actor (const object_ref_t &left, const object_ref_t &right);
    std::pair<stateful_error_t, stream_binding_t>
    bind_verified (const stream_connection_t &connection,
                   const object_ref_t &actor,
                   std::uint64_t target_node_generation,
                   std::uint64_t owner_lease_generation,
                   bool route_publish_pending = false,
                   std::uint64_t binding_generation = 0);
    session_binding_aggregate_t *current_aggregate_unlocked (const std::string &actor_id);
    const session_binding_aggregate_t *
    current_aggregate_unlocked (const std::string &actor_id) const;
    static std::vector<stream_retained_outbound_t>
    take_retained_outbound_unlocked (session_binding_aggregate_t &aggregate);
    void notify_changed () noexcept;

    authority_resolver_t _resolver;
    const std::function<void ()> _activity_handler;
    offload_executor_t _lane_executor;
    mutable state_lane_t _lane;
    std::mutex _changed_mutex;
    std::condition_variable _changed;
    std::atomic_uint64_t _changed_generation = 0;
    std::map<std::string, connection_state_t> _connections;
    std::map<std::string, std::uint64_t> _last_connection_generation;
    std::map<std::string, actor_binding_locator_t> _actor_bindings;
    std::map<std::uint64_t, object_ref_t> _barriers;
    std::uint64_t _next_binding_generation = 1;
    std::uint64_t _next_barrier_token = 1;
    bool _all_sealed = false;
};

} // namespace zlink::framework::runtime::stateful
