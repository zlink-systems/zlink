/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "actor_transfer_coordinator.hpp"
#include "runtime/actors/actor_serial_executor.hpp"
#include "runtime/protocol/actor_join_recovery_codec.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/locations/location_lifecycle.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/operations/exactly_once_table.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/locations/resolvers.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::detail
{

namespace runtime = zlink::framework::runtime;

namespace service = zlink::framework::runtime::host;

class actor_dispatch_admission_token_t;

using instance_spot_idle_eviction_callback_t = std::function<bool (
  const spot_id_t &, std::string_view, std::uint64_t, std::uint64_t, std::function<bool ()>)>;
using instance_spot_close_begin_callback_t =
  std::function<std::optional<service::instance_spot_close_completion_t> (
    const spot_id_t &, std::string_view, std::uint64_t, std::uint64_t)>;

class spot_node_builder_state_t
{
  public:
    explicit spot_node_builder_state_t (std::string name) :
        lane_executor (), lane (lane_executor), snapshot{.name = std::move (name)}
    {
    }
    ~spot_node_builder_state_t ();

    // Builder configuration is synchronously admitted through this lane.  The
    // MeshNode start path takes its configuration projection only after those
    // turns have completed, so a caller observes registration before the
    // configuring API returns (discovery 9).
    runtime::offload_executor_t lane_executor;
    runtime::state_lane_t lane;
    // Compatibility gate for callers outside spots/** that still compile
    // against the builder-state test seam. spot_runtime itself never acquires
    // this mutex; all state it owns is serialized by lane.
    mutable std::recursive_mutex mutex;
    spot_node_snapshot_t snapshot;
    std::map<std::string, std::type_index> spot_factories;
    // Configure callbacks receive a builder reference. Keep the sealed builder
    // alive with the registration so an escaped reference reports a typed
    // configuration error instead of becoming dangling.
    std::vector<std::shared_ptr<void>> factory_builder_lifetimes;
    std::map<std::string, spot_lifecycle_callbacks_t> spot_lifecycles;
    std::map<std::string, spot_id_t> spot_ids_by_name;
    std::map<std::string, std::string> spot_names_by_id;
    std::map<std::string, spot_context_t> spot_contexts_by_id;
    struct pending_spot_creation_t
    {
        std::string spot_name;
        std::shared_future<void> future;
        std::uint64_t reservation = 0;
    };
    std::map<std::string, pending_spot_creation_t> pending_spot_creations_by_id;
    std::uint64_t next_pending_spot_creation_reservation = 1;
    std::weak_ptr<service::mesh_node_t> native_node;
    std::function<task_t<spot_create_result_t> (bool,
                                                std::optional<spot_id_t>,
                                                std::string,
                                                std::optional<std::string>,
                                                std::optional<message_t>,
                                                std::chrono::milliseconds)>
      create_user_spot;
    std::function<task_t<std::optional<spot_ref_t>> (spot_id_t)> find_user_spot;
    std::function<task_t<bool> (spot_ref_t)> close_user_spot;
    instance_spot_idle_eviction_callback_t admit_instance_spot_idle_eviction;
    instance_spot_close_begin_callback_t begin_instance_spot_close;
    std::shared_ptr<channel_runtime_state_t> channel_runtime;
    dispatch_options_t dispatch;
    runtime::location_lifecycle_t *location_lifecycle = nullptr;
    runtime::spot_address_resolver_t *spot_location_resolver = nullptr;
    std::optional<service_provider_t> root_services;
    std::shared_ptr<std::atomic_bool> drain_flag;
    std::shared_ptr<monitoring_runtime_state_t> monitoring;
    std::chrono::milliseconds one_way_send_timeout{std::chrono::seconds (1)};
    std::chrono::milliseconds instance_spot_idle_timeout{0};
    std::unique_ptr<zlink::timer_t> instance_spot_idle_timer;
    std::atomic_bool stopping{false};
    std::map<std::string, spot_id_t> actor_spot_ids;
    std::map<std::string, std::uint64_t> actor_generations;
    std::map<std::string, runtime::protocol::actor_route_fence_t> actor_authority_fences;
    // A remote Actor PREPARE owns its target Context before user lifecycle
    // work leaves the node lane.  The reservation contributes to actor_count
    // so close/idle eviction cannot observe the Context as empty between
    // validation and the final actor registry publication.
    std::map<std::string, spot_id_t> pending_actor_contexts;
    std::map<std::string, std::string> actor_types_by_id;
    std::set<std::string> actor_created_keys;
    /* Destruction requested from an active handler is deferred until the
     * borrowed instance is no longer in use. Packets are rejected as soon as
     * that intent is accepted. */
    std::set<std::string> retiring_actor_keys;
    std::set<std::string> destroying_actors;
    std::set<std::string> destroyed_actor_keys;
    // A request id is reserved before dispatch and receives one terminal
    // reply. The table owns both states so replay and retry use the same
    // exactly-once transition.
    runtime::exactly_once_table_t<std::string, zlink::message_t> dispatched_request_replies;
    struct pending_handoff_request_t
    {
        actor_ref_t actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::uint64_t reply_route_id = 0;
        service::reply_token_t reply_token;
        runtime::messaging::envelope_header_t request_header;
        std::chrono::steady_clock::time_point deadline;
    };
    /* Spec 51 scopes an OperationId to the initiating source-owner lifecycle,
     * not process-wide. Keep the initiating source node and route fence in
     * the durable pending identity so identical operation pairs from a
     * restarted/concurrent source cannot settle each other's reply token. */
    struct pending_handoff_request_key_t
    {
        std::string source_node_rid;
        std::uint64_t operation_high = 0;
        std::uint64_t operation_low = 0;
        runtime::protocol::actor_route_fence_t source_fence;

        bool operator< (const pending_handoff_request_key_t &other) const
        {
            return std::tie (source_node_rid, operation_high, operation_low, source_fence.actor_id,
                             source_fence.object_generation, source_fence.target_node_routing_id,
                             source_fence.target_node_generation,
                             source_fence.authority_owner_generation,
                             source_fence.owner_lease_generation)
                   < std::tie (other.source_node_rid, other.operation_high, other.operation_low,
                               other.source_fence.actor_id, other.source_fence.object_generation,
                               other.source_fence.target_node_routing_id,
                               other.source_fence.target_node_generation,
                               other.source_fence.authority_owner_generation,
                               other.source_fence.owner_lease_generation);
        }
    };
    std::map<pending_handoff_request_key_t, pending_handoff_request_t> pending_handoff_requests;
    std::function<task_t<bool> (const zlink::routing_id_t &,
                                const zlink::routing_id_t &,
                                const runtime::protocol::wire_operation_id_t &,
                                std::uint64_t,
                                const runtime::protocol::actor_route_fence_t &,
                                const result_t<zlink::message_t> &)>
      actor_handoff_terminal_sender;
    // The Entry Spot is fixed to node lifecycle (one per node, never
    // relocates) and is intentionally not published into mesh spot routing,
    // so an OnLeave notification back to a source Entry Spot cannot resolve
    // through the normal spot-address send path. Node-level send (this
    // callback, mirroring actor_handoff_terminal_sender) needs only the
    // destination node's routing id, not a resolved spot address.
    std::function<task_t<zlink::submit_result_t> (const zlink::routing_id_t &,
                                                  std::vector<zlink::message_t>)>
      actor_leave_notification_sender;
    // Requests currently dispatched to each actor and not yet replied. Sampled
    // once per transfer right at the moving transition (runtime-metrics §4.3
    // pending_requests). The node state lane owns both dispatch updates and
    // transfer samples, so the token can settle this counter with its lifecycle
    // claim in one terminal turn.
    std::map<std::string, std::size_t> actor_pending_requests;
    struct pending_remote_source_cleanup_t
    {
        actor_ref_t source_actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::string transfer_id;
        spot_id_t source_spot_id;
        std::uint64_t source_spot_generation = 0;
        spot_id_t target_spot_id;
        runtime::protocol::actor_route_fence_t target_fence;
        std::chrono::steady_clock::time_point not_before;
        // True once submit_remote_actor_leave has validated and accepted the
        // OnLeave command for this transfer (idempotency guard against a
        // duplicate/retried command; does NOT mean the callback has run).
        bool leave_submitted = false;
        // OnLeave is a one-way notification the target sends back to the
        // source after commit (spec 15: source membership cleanup must not
        // run ahead of the OnLeave callback). submit_remote_actor_leave only
        // *queues* the callback onto the source Spot's serial executor and
        // returns immediately, so leave_submitted flips true well before the
        // callback actually executes. The sweep must hold the erase --
        // which OnLeave's own dispatch needs the local Actor instance to
        // still be registered for -- until the callback has actually
        // finished (leave_completed), except as a last-resort bound if the
        // notification is genuinely lost (target crash, partition) or there
        // is no OnLeave handler to await: leave_deadline.
        bool leave_completed = false;
        std::chrono::steady_clock::time_point leave_deadline;
    };
    std::vector<pending_remote_source_cleanup_t> pending_remote_source_cleanups;
    // A canonical routed Join can deliver the target's one-way OnLeave
    // command before the source has observed the Join completion and
    // installed its exact source-cleanup/Message-Follow fence.  A command
    // without the source Spot generation cannot safely run against the live
    // Spot until that fence exists, so retain the already authority-validated
    // identity and replay it when complete_remote_actor_transfer publishes the
    // matching cleanup. Its lifetime is owned by the exact source_remote move,
    // not MessageFollowDuration (which may legitimately be zero).
    struct pending_remote_actor_leave_t
    {
        std::string transfer_id;
        actor_ref_t source_actor;
        spot_id_t source_spot_id;
        spot_id_t target_spot_id;
        runtime::protocol::actor_route_fence_t target_fence;
        // The early node-send already owns an application-mailbox
        // reservation. Keep that exact terminal with the deferred command so
        // replay can transfer it into the source Spot lifecycle lane without
        // a second capacity decision.
        std::function<void ()> transfer_owner_reservation;
        std::size_t transferred_owner_byte_cost = 0;
    };
    std::vector<pending_remote_actor_leave_t> pending_remote_actor_leaves;
    struct actor_factory_registration_t
    {
        std::type_index actor_type{typeid (void)};
        factory_relocation_configuration_t relocation;
        std::function<std::shared_ptr<void> (std::string)> create_instance;
        std::function<void (void *, const actor_ref_t &, void *)> configure_instance;
        std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)>
          serialize_instance;
        std::function<void (void *, const zlink::message_t &, serializer_registry_t &)>
          deserialize_instance;
        std::function<std::shared_ptr<void> (actor_context_t)> create_context_instance;
        actor_join_completion_callback_t on_join_completed;
        std::function<task_t<std::vector<std::byte>> (void *, std::stop_token)> capture;
        std::function<task_t<void> (void *, std::vector<std::byte>, std::stop_token)> restore;
    };
    std::function<result_t<void> (const actor_ref_t &)> destroy_actor_registry;
    std::function<result_t<void> (const actor_ref_t &)> update_actor_registry_ref;
    std::function<task_t<std::optional<zlink::message_t>> (
      const actor_ref_t &,
      actor_context_t,
      stream_message_kind_t,
      std::string_view,
      const zlink::message_t &,
      service_provider_t &,
      serializer_registry_t &,
      spot_inbound_message_t,
      const runtime::protocol::actor_route_fence_t *)>
      actor_packet_relay;
    std::function<task_t<std::optional<zlink::message_t>> (
      const actor_ref_t &,
      const runtime::messaging::envelope_header_t &,
      const zlink::message_t &,
      std::chrono::milliseconds,
      const zlink::routing_id_t &,
      const runtime::protocol::actor_route_fence_t &,
      std::uint8_t,
      const runtime::protocol::wire_operation_id_t &,
      std::uint64_t)>
      actor_message_follow_relay;
    /* Network Actor messages are admitted against the complete route fence
     * before their typed body is decoded. The host supplies the cached
     * authority/owner check; the local transfer coordinator may allow a
     * committed source route to enter Message Follow first. */
    std::function<bool (const runtime::protocol::actor_route_fence_t &)> actor_route_admission;
    std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                                node_rid_t,
                                                const zlink::message_t &,
                                                const std::optional<zlink::message_t> &)>
      actor_entry_spot_join;
    std::map<std::string, actor_factory_registration_t> actor_factories;
    std::map<std::string, factory_relocation_configuration_t> spot_factory_relocations;
    std::map<std::string, std::int32_t> spot_stable_type_limits;
    std::map<std::string, spot_relocation_coordination_mode_t> spot_relocation_coordination_modes;
    actor_transfer_coordinator_t actor_transfer_coordinator;
    struct actor_join_relocation_recovery_t
    {
        std::string handoff_id;
        spot_id_t source_spot_id;
        spot_id_t target_spot_id;
        std::uint64_t target_node_generation = 0;
        std::uint64_t target_spot_generation = 0;
        actor_ref_t source_actor;
        std::uint64_t completion_operation_id_high = 0;
        std::uint64_t completion_operation_id_low = 0;
        std::vector<std::uint8_t> admission_reply;
    };
    std::map<std::string, actor_join_relocation_recovery_t> actor_join_relocation_recoveries;
    // Message Follow relays messages that reach the committed source route
    // after relocation. The common contract bounds its default duration to 30s.
    std::chrono::milliseconds message_follow_duration{30000};
    std::map<std::string, std::shared_ptr<void>> actor_instances;
    std::set<std::pair<std::uint64_t, std::uint64_t>> committed_join_locations;
    std::set<std::pair<std::uint64_t, std::uint64_t>> delivered_join_completions;
    std::set<std::pair<std::uint64_t, std::uint64_t>> delivering_join_completions;
    std::shared_ptr<runtime::stateful::relocation_store_port_t> relocation_store;
    std::shared_ptr<runtime::stateful::authority_relocation_port_t> relocation_authority;
    /* Address → (type, id) lookup for instance-identity public surfaces
     * (destroy_actor). Never dereferenced — resolution only compares
     * addresses — and maintained alongside every registration/erasure, so
     * a freed instance can at worst leave an entry that no longer resolves
     * to a live registration. */
    std::map<const void *, std::pair<std::string, std::string>> actor_instance_index;
    std::map<std::string, spot_route_t> actor_routes;
    std::map<std::string, std::unique_ptr<service::actor_t>> native_actors;
    std::unordered_set<std::string> mesh_runtime_owned_native_actor_ids;
    std::map<std::string, std::uint64_t> core_actor_membership_epochs;
    std::map<std::string, std::shared_ptr<service::spot_t>> native_spots_by_id;
    std::shared_ptr<service::spot_t> routed_control_spot;
    runtime::offload_executor_t route_client_lane_executor;
    runtime::state_lane_t route_client_lane{route_client_lane_executor};
    std::optional<route_client_t> route_client;
    struct queued_actor_packet_t
    {
        service::receive_record_t record;
        std::vector<zlink::message_t> parts;
    };
    std::vector<queued_actor_packet_t> queued_actor_packets;
    std::map<std::string, std::function<std::optional<spot_route_t> (spot_id_t)>> resolvers;
    std::shared_ptr<runtime::offload_executor_t> worker_executor;
    /* Deadline callbacks leave the native timer scheduler before they settle
     * an Actor transfer. Keeping that handoff separate from the Actor worker
     * prevents a single occupied Actor worker from delaying cancellation. */
    std::shared_ptr<runtime::offload_executor_t> deadline_executor;
    worker_options_t worker_options;
    std::stop_source worker_cancellation;
    std::uint64_t next_spot_id = 1;
};

struct spot_create_call_state_t
{
    std::shared_ptr<spot_node_builder_state_t> node;
    std::weak_ptr<spot_context_state_t> source;
    bool exclusive = false;
    std::optional<spot_id_t> spot_id;
    std::string stable_type;
    std::optional<std::string> mesh_name;
    std::optional<message_t> request;
    std::optional<std::chrono::milliseconds> timeout;
    bool submitted = false;
};

void drain_spot_node_executors (spot_node_builder_state_t &node);

void report_logical_multicast_failure (const std::shared_ptr<spot_node_builder_state_t> &state,
                                       std::string_view channel_name,
                                       std::string_view topic,
                                       std::string_view packet_name,
                                       const framework_exception_t &error) noexcept;

/* actor_instance_index maintenance (caller runs on the node state lane). A record
 * replaces any prior address for the same actor, so a re-registered actor
 * never leaves an older address that would resolve to the live actor. */
inline void erase_actor_instance_index_unlocked (spot_node_builder_state_t &node,
                                                 std::string_view actor_type,
                                                 std::string_view actor_id)
{
    for (auto it = node.actor_instance_index.begin (); it != node.actor_instance_index.end ();) {
        if (it->second.first == actor_type && it->second.second == actor_id) {
            it = node.actor_instance_index.erase (it);
        } else {
            ++it;
        }
    }
}

inline void record_actor_instance_index_unlocked (spot_node_builder_state_t &node,
                                                  const actor_ref_t &actor_ref,
                                                  const void *instance)
{
    if (instance == nullptr) {
        return;
    }
    erase_actor_instance_index_unlocked (
      node, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref),
      actor_ref.actor_id ().value ());
    const auto actor_type =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    node.actor_instance_index[instance] = {actor_type, actor_id};
    // Internal Actor packets contain the logical Actor id but not its stable
    // type. Keep the type index when an instance is installed so a recreated
    // Actor can accept its first bind packet before the new authority snapshot
    // becomes visible through the Location Store reader.
    node.actor_types_by_id[actor_id] = actor_type;
}

struct entry_spot_domain_t final
{
};

struct user_spot_domain_t final
{
};

struct instance_spot_domain_t final
{
};

// The Spot kind tag is consumed while a factory is registered or an
// activation is constructed. A materialized Spot retains one of these domain
// alternatives, so contradictory kind flags and unsupported lifecycle
// combinations cannot be represented in the runtime state.
class spot_lifecycle_domain_t final
{
  public:
    static spot_lifecycle_domain_t entry ()
    {
        return spot_lifecycle_domain_t (entry_spot_domain_t{});
    }

    static spot_lifecycle_domain_t user ()
    {
        return spot_lifecycle_domain_t (user_spot_domain_t{});
    }

    static spot_lifecycle_domain_t instance ()
    {
        return spot_lifecycle_domain_t (instance_spot_domain_t{});
    }

    bool is_entry () const noexcept { return std::holds_alternative<entry_spot_domain_t> (_value); }

    bool is_instance () const noexcept
    {
        return std::holds_alternative<instance_spot_domain_t> (_value);
    }

    bool allows_relocation () const noexcept { return !is_entry (); }

    bool allows_idle_eviction () const noexcept { return is_instance (); }

  private:
    using value_t = std::variant<entry_spot_domain_t, user_spot_domain_t, instance_spot_domain_t>;

    explicit spot_lifecycle_domain_t (entry_spot_domain_t value) : _value (value) {}

    explicit spot_lifecycle_domain_t (user_spot_domain_t value) : _value (value) {}

    explicit spot_lifecycle_domain_t (instance_spot_domain_t value) : _value (value) {}

    value_t _value;
};

/* Owns the application queues for one Spot. The containing node state lane
 * owns mutations of the name maps; readers use the published Actor snapshot
 * so dispatch need not re-enter that lane. */
class spot_serial_executor_t
{
  public:
    using queue_t = runtime::serial_execution_queue_t;
    using queue_ptr_t = std::shared_ptr<queue_t>;
    using actor_executor_t = runtime::actor_serial_executor_t;
    using actor_executor_ptr_t = std::shared_ptr<actor_executor_t>;
    using actor_executor_map_t = std::map<std::string, actor_executor_ptr_t>;
    struct actor_queue_submission_t
    {
        queue_ptr_t queue;
        runtime::serial_submission_id_t id = 0;
    };

    spot_serial_executor_t (std::shared_ptr<runtime::offload_executor_t> worker_executor,
                            runtime::state_lane_t &state_lane,
                            runtime::serial_lane_policy_t spot_policy,
                            queue_ptr_t spot_queue = {}) :
        _worker_executor (std::move (worker_executor)), _state_lane (&state_lane),
        _spot_policy (std::move (spot_policy))
    {
        _spot_queue = std::move (spot_queue);
        if (!_spot_queue && _worker_executor) {
            _spot_queue = std::make_shared<queue_t> (
              *_worker_executor, runtime::serial_execution_queue_options_t{},
              queue_t::error_handler_t{}, _spot_policy);
        }
    }

    const queue_ptr_t &spot_queue () const noexcept { return _spot_queue; }

    bool execute_spot (std::string name,
                       queue_t::async_work_t work,
                       runtime::serial_work_options_t options = {}) const
    {
        return _spot_queue
               && _spot_queue->try_post_async (std::move (name), std::move (work),
                                                std::move (options));
    }

    bool execute_spot (std::string name,
                       std::function<void ()> work,
                       runtime::serial_work_options_t options = {}) const
    {
        return _spot_queue
               && _spot_queue->try_post (std::move (name), std::move (work),
                                         std::move (options));
    }

    bool execute_lifecycle (std::string name, std::function<void ()> work) const
    {
        return _spot_queue
               && _spot_queue->try_post (
                 std::move (name), std::move (work),
                 runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
    }

    actor_executor_ptr_t actor_executor (const std::string &actor_id)
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return actor_executor_on_lane (actor_id);
        return _state_lane->run ([this, &actor_id] {
            return actor_executor_on_lane (actor_id);
        }).get ();
    }

    actor_executor_ptr_t find_actor_executor (const std::string &actor_id) const
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return find_actor_executor_on_lane (actor_id);
        return _state_lane->run ([this, &actor_id] {
            return find_actor_executor_on_lane (actor_id);
        }).get ();
    }

    void erase_actor_queue (const std::string &actor_id)
    {
        if (!_state_lane || _state_lane->is_on_lane ()) {
            erase_actor_queue_on_lane (actor_id);
            return;
        }
        _state_lane->run ([this, &actor_id] {
            erase_actor_queue_on_lane (actor_id);
        }).get ();
    }

    void replace_actor_queue (std::string actor_id, queue_ptr_t queue)
    {
        if (!_state_lane || _state_lane->is_on_lane ()) {
            replace_actor_queue_on_lane (std::move (actor_id), std::move (queue));
            return;
        }
        _state_lane->run ([this, actor_id = std::move (actor_id), queue = std::move (queue)] () mutable {
            replace_actor_queue_on_lane (std::move (actor_id), std::move (queue));
        }).get ();
    }

    std::shared_ptr<const actor_executor_map_t> actor_executor_snapshot () const noexcept
    {
        return std::atomic_load_explicit (&_actor_executor_snapshot, std::memory_order_acquire);
    }

    queue_ptr_t timer_queue (const std::string &timer_name)
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return timer_queue_on_lane (timer_name);
        return _state_lane->run ([this, &timer_name] {
            return timer_queue_on_lane (timer_name);
        }).get ();
    }

    std::vector<queue_ptr_t> timer_queues () const
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return timer_queues_on_lane ();
        return _state_lane->run ([this] { return timer_queues_on_lane (); }).get ();
    }

    void cancel_timer (const std::string &timer_name)
    {
        if (!_state_lane || _state_lane->is_on_lane ()) {
            cancel_timer_on_lane (timer_name);
            return;
        }
        _state_lane->run ([this, &timer_name] {
            cancel_timer_on_lane (timer_name);
        }).get ();
    }

    bool uses_spot_execution_gate () const noexcept
    {
        return _spot_policy.allows_turn_yield ();
    }

    bool execute_actor (const std::string &actor_id,
                        std::string name,
                        queue_t::async_work_t work,
                        runtime::serial_work_options_t options = {},
                        bool current_turn = false,
                        std::function<void ()> rejected = {})
    {
        if (current_turn && _spot_queue && uses_spot_execution_gate ()) {
            auto spot_options = options;
            spot_options.byte_cost = queue_t::fixed_work_byte_cost;
            spot_options.transfer_owner_reservation = {};
            // The Actor handoff fence belongs to the per-Actor queue boundary
            // only; the Spot execution-gate hop never carries it.
            spot_options.refuse_when_actor_handoff_fenced = false;
            spot_options.actor_handoff_fence_refused = nullptr;
            return _spot_queue->try_post_async (
              std::move (name), std::move (work), std::move (spot_options));
        }

        auto executor = find_actor_executor_snapshot (actor_id);
        if (!executor)
            executor = actor_executor (actor_id);
        if (!executor)
            return false;
        if (!uses_spot_execution_gate ())
            return executor->execute_actor (std::move (name), std::move (work),
                                            std::move (options));

        auto spot_options = options;
        spot_options.byte_cost = queue_t::fixed_work_byte_cost;
        spot_options.transfer_owner_reservation = {};
        spot_options.refuse_when_actor_handoff_fenced = false;
        spot_options.actor_handoff_fence_refused = nullptr;
        return executor->execute_actor (
          std::move (name),
          [this, work = std::move (work), spot_options,
           rejected = std::move (rejected)] (auto actor_complete) mutable {
              const auto posted = _spot_queue
                                    && _spot_queue->try_post_async (
                                      "spot-handler",
                                      [work = std::move (work), actor_complete] (auto spot_complete) mutable {
                                          const auto spot_turn = detail::capture_current_serial_turn ();
                                          auto actor_terminal = std::make_shared<std::atomic_bool> (false);
                                          work ([spot_complete = std::move (spot_complete),
                                                 actor_complete, spot_turn,
                                                 actor_terminal] (std::function<void ()> finish) mutable {
                                              auto complete_actor =
                                                [finish = std::move (finish), actor_complete,
                                                 actor_terminal] () mutable {
                                                    if (actor_terminal->exchange (
                                                          true, std::memory_order_acq_rel))
                                                        return;
                                                    std::exception_ptr error;
                                                    try {
                                                        if (finish)
                                                            finish ();
                                                    }
                                                    catch (...) {
                                                        error = std::current_exception ();
                                                    }
                                                    actor_complete ([error] {
                                                        if (error)
                                                            std::rethrow_exception (error);
                                                    });
                                                };
                                              if (spot_turn && spot_turn->released ()) {
                                                  complete_actor ();
                                                  return;
                                              }
                                              spot_complete (std::move (complete_actor));
                                          });
                                      },
                                      spot_options);
              if (!posted) {
                  if (rejected)
                      rejected ();
                  actor_complete ([] {});
              }
          },
          std::move (options));
    }

    bool actor_queue_closed (const std::string &actor_id) const noexcept
    {
        const auto executor = find_actor_executor_snapshot (actor_id);
        return !executor || executor->closed ();
    }

    bool ensure_actor_queue (const std::string &actor_id)
    {
        return static_cast<bool> (actor_executor (actor_id));
    }

    const void *actor_queue_identity (const std::string &actor_id) const noexcept
    {
        const auto executor = find_actor_executor_snapshot (actor_id);
        return executor ? executor->queue_identity () : nullptr;
    }

    bool actor_queue_matches (const std::string &actor_id, const void *identity) const noexcept
    {
        return actor_queue_identity (actor_id) == identity;
    }

    result_t<std::shared_ptr<detail::deferred_barrier_t>> reserve_actor_handoff_barrier (
      const std::string &actor_id, std::string name)
    {
        const auto executor = actor_executor (actor_id);
        if (!executor)
            return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
              framework_error_kind_t::shutting_down,
              "Actor handoff queue is unavailable");
        return executor->execute_lifecycle (std::move (name));
    }

    result_t<actor_queue_submission_t> execute_actor_cancellable (
      const std::string &actor_id,
      std::string name,
      queue_t::async_work_t work,
      std::function<void ()> cancel,
      runtime::serial_work_options_t options = {})
    {
        auto executor = find_actor_executor_snapshot (actor_id);
        if (!executor)
            executor = actor_executor (actor_id);
        if (!executor)
            return result_t<actor_queue_submission_t>::failure (
              framework_error_kind_t::shutting_down,
              "Actor handoff queue is unavailable");
        const auto submitted = executor->execute_actor (
          std::move (name), std::move (work), std::move (cancel), std::move (options));
        if (!submitted)
            return result_t<actor_queue_submission_t>::failure (
              submitted.error_kind (),
              submitted.error () != nullptr ? submitted.error ()->what ()
                                            : "Actor handoff queue is full");
        return result_t<actor_queue_submission_t>::success (
          actor_queue_submission_t{executor->queue (), submitted.value ()});
    }

    bool execute_timer (const std::string &timer_name,
                        std::string name,
                        queue_t::async_work_t work,
                        runtime::serial_work_options_t options = {})
    {
        if (uses_spot_execution_gate ()) {
            options.byte_cost = queue_t::fixed_work_byte_cost;
            options.transfer_owner_reservation = {};
            return _spot_queue
                   && _spot_queue->try_post_async (
                     std::move (name), std::move (work), std::move (options));
        }
        const auto queue = timer_queue (timer_name);
        return queue && queue->try_post_async (std::move (name), std::move (work),
                                               std::move (options));
    }

    void close ()
    {
        std::vector<queue_ptr_t> queues;
        if (!_state_lane || _state_lane->is_on_lane ())
            queues = close_on_lane ();
        else
            queues = _state_lane->run ([this] { return close_on_lane (); }).get ();
        for (const auto &queue : queues)
            if (queue)
                queue->close ();
    }

  private:
    void assert_on_lane () const noexcept
    {
        if (_state_lane)
            assert (_state_lane->is_on_lane ());
    }

    actor_executor_ptr_t actor_executor_on_lane (const std::string &actor_id)
    {
        assert_on_lane ();
        auto &executor = _actor_executors[actor_id];
        if (!executor && _worker_executor) {
            executor = std::make_shared<actor_executor_t> (_worker_executor);
            publish_actor_executor_snapshot ();
        }
        return executor;
    }

    actor_executor_ptr_t
    find_actor_executor_snapshot (const std::string &actor_id) const noexcept
    {
        const auto snapshot = actor_executor_snapshot ();
        if (!snapshot)
            return {};
        const auto found = snapshot->find (actor_id);
        return found == snapshot->end () ? actor_executor_ptr_t{} : found->second;
    }

    actor_executor_ptr_t find_actor_executor_on_lane (const std::string &actor_id) const
    {
        assert_on_lane ();
        const auto found = _actor_executors.find (actor_id);
        return found == _actor_executors.end () ? actor_executor_ptr_t{} : found->second;
    }

    void erase_actor_queue_on_lane (const std::string &actor_id)
    {
        assert_on_lane ();
        _actor_executors.erase (actor_id);
        publish_actor_executor_snapshot ();
    }

    void replace_actor_queue_on_lane (std::string actor_id, queue_ptr_t queue)
    {
        assert_on_lane ();
        _actor_executors.insert_or_assign (
          std::move (actor_id),
          std::make_shared<actor_executor_t> (_worker_executor, std::move (queue)));
        publish_actor_executor_snapshot ();
    }

    queue_ptr_t timer_queue_on_lane (const std::string &timer_name)
    {
        assert_on_lane ();
        auto &queue = _timer_queues[timer_name];
        if (!queue && _worker_executor) {
            queue = std::make_shared<queue_t> (
              *_worker_executor, runtime::serial_execution_queue_options_t{},
              queue_t::error_handler_t{}, runtime::serial_lane_policy_t::actor_delivery ());
        }
        return queue;
    }

    std::vector<queue_ptr_t> timer_queues_on_lane () const
    {
        assert_on_lane ();
        std::vector<queue_ptr_t> result;
        result.reserve (_timer_queues.size ());
        for (const auto &[_, queue] : _timer_queues)
            result.push_back (queue);
        return result;
    }

    void cancel_timer_on_lane (const std::string &timer_name)
    {
        assert_on_lane ();
        const auto found = _timer_queues.find (timer_name);
        if (found == _timer_queues.end ())
            return;
        if (found->second)
            found->second->cancel_pending ();
        _timer_queues.erase (found);
    }

    std::vector<queue_ptr_t> close_on_lane ()
    {
        assert_on_lane ();
        std::vector<queue_ptr_t> queues;
        queues.reserve (1 + _actor_executors.size () + _timer_queues.size ());
        if (_spot_queue)
            queues.push_back (_spot_queue);
        for (const auto &[_, executor] : _actor_executors)
            if (executor)
                queues.push_back (executor->queue ());
        for (const auto &[_, queue] : _timer_queues)
            queues.push_back (queue);
        _actor_executors.clear ();
        _timer_queues.clear ();
        publish_actor_executor_snapshot ();
        return queues;
    }

  private:
    void publish_actor_executor_snapshot ()
    {
        auto snapshot = std::make_shared<actor_executor_map_t> (_actor_executors);
        std::shared_ptr<const actor_executor_map_t> published = std::move (snapshot);
        std::atomic_store_explicit (&_actor_executor_snapshot, std::move (published),
                                    std::memory_order_release);
    }

    std::shared_ptr<runtime::offload_executor_t> _worker_executor;
    runtime::state_lane_t *_state_lane = nullptr;
    runtime::serial_lane_policy_t _spot_policy;
    queue_ptr_t _spot_queue;
    actor_executor_map_t _actor_executors;
    std::map<std::string, queue_ptr_t> _timer_queues;
    std::shared_ptr<const actor_executor_map_t> _actor_executor_snapshot =
      std::make_shared<actor_executor_map_t> ();
};

class spot_context_state_t : public std::enable_shared_from_this<spot_context_state_t>
{
  private:
    template<typename Work>
    decltype(auto) state_sync (Work &&work) const
    {
        auto owner = state_lane_owner ();
        if (!owner || owner->lane.is_on_lane ())
            return std::invoke (work);
        return owner->lane
          .run ([work = std::forward<Work> (work)] () mutable -> decltype(auto) {
              return std::invoke (work);
          })
          .get ();
    }

  public:
    void detach_application_instance (
      bool notify_closing,
      spot_close_reason_t close_reason = spot_close_reason_t::explicit_close,
      std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::time_point::max (),
      std::stop_token cleanup_cancellation = {})
    {
        auto lifetime_guard = shared_from_this ();
        state_sync ([this] { callback_admission_closed = true; });

        auto owner = state_lane_owner ();
        if (!owner) {
            // Compatibility for an already-detached test/context. Production
            // contexts retain lane_owner from activation until destruction.
            auto instance = std::move (spot_instance);
            std::exception_ptr callback_error;
            if (notify_closing && lifecycle.on_closing && instance) {
                try {
                    lifecycle.on_closing (instance.get (),
                                          spot_closing_context_t{close_reason, deadline},
                                          cleanup_cancellation);
                }
                catch (...) {
                    callback_error = std::current_exception ();
                }
            }
            cancel_timers ();
            timer_handler_instances.clear ();
            instance.reset ();
            if (activation_scope) {
                activation_scope->close ();
                activation_scope.reset ();
            }
            node.reset ();
            if (callback_error)
                std::rethrow_exception (callback_error);
            return;
        }

        const auto reservation = owner->lane.run ([this, &owner] {
            if (close_reservation != 0)
                return std::make_pair (close_reservation, false);
            return std::make_pair (reserve_close_core (*owner, false), true);
        }).get ();
        std::exception_ptr detach_error;
        try {
            detach_application_instance_reserved (owner, reservation.first, notify_closing,
                                                   close_reason, deadline,
                                                   cleanup_cancellation);
        }
        catch (...) {
            detach_error = std::current_exception ();
        }
        if (reservation.second) {
            owner->lane.run ([this, token = reservation.first] {
                clear_close_reservation_core (token);
            }).get ();
        }
        if (detach_error)
            std::rethrow_exception (detach_error);
    }

    bool close_now ()
    {
        auto owner = state_lane_owner ();
        if (!owner)
            return false;

        const auto start = owner->lane.run ([this, &owner] {
            if (node.get () != owner.get () || closed || actor_count != 0) {
                return close_start_t{};
            }
            close_start_t result;
            if (close_reservation != 0) {
                if (close_reservation_is_idle)
                    return result;
                result.token = close_reservation;
                result.existing = true;
                return result;
            }
            result.token = reserve_close_core (*owner, false);
            result.spot_id = spot_id;
            result.spot_name = spot_name;
            result.object_generation = object_generation;
            result.authority_owner_generation = authority_owner_generation;
            result.begin_instance_close = owner->begin_instance_spot_close;
            result.requires_instance_close = is_instance_spot () && bool (result.begin_instance_close);
            return result;
        }).get ();
        if (start.token == 0)
            return false;
        if (start.existing) {
            return state_sync ([this] { return close_requested; });
        }

        std::optional<service::instance_spot_close_completion_t> completion;
        try {
            if (start.requires_instance_close) {
                completion = start.begin_instance_close (
                  start.spot_id, start.spot_name, start.object_generation,
                  start.authority_owner_generation);
            }
        }
        catch (...) {
            owner->lane.run ([this, token = start.token] {
                clear_close_reservation_core (token);
            }).get ();
            throw;
        }
        if (start.requires_instance_close && !completion) {
            owner->lane.run ([this, token = start.token] {
                clear_close_reservation_core (token);
            }).get ();
            return false;
        }

        const auto decision = owner->lane.run ([this, &owner, token = start.token,
                                                &completion] {
            if (close_reservation != token || close_reservation_is_idle
                || node.get () != owner.get () || closed || actor_count != 0) {
                if (callback_depth == 0 && !close_requested)
                    callback_admission_closed = false;
                clear_close_reservation_core (token);
                return close_decision_t{};
            }
            callback_admission_closed = true;
            if (callback_depth != 0) {
                close_requested = true;
                if (completion)
                    pending_instance_spot_close_completion = std::move (*completion);
                return close_decision_t{true, false};
            }
            close_requested = false;
            closed = true;
            return close_decision_t{false, true};
        }).get ();
        if (decision.deferred)
            return true;
        if (!decision.committed) {
            if (completion) {
                (void) (*completion) (false);
            }
            return false;
        }

        try {
            close_application_then_release_location (
              owner, spot_close_reason_t::explicit_close, start.token);
        }
        catch (...) {
            if (completion)
                (void) (*completion) (true);
            throw;
        }
        return completion ? (*completion) (true) : true;
    }

    struct timer_fire_state_snapshot_t
    {
        std::shared_ptr<void> spot_instance;
        std::shared_ptr<channel_runtime_state_t> channel_runtime;
        bool configured = false;
        bool admitted = false;
    };

    bool enter_callback ();
    timer_fire_state_snapshot_t enter_timer_callback ();
    void leave_callback (std::function<void ()> settle_owner_state = {}) noexcept;
    bool is_current_callback_thread () const;
    bool admission_blocked () const noexcept
    {
        return state_sync ([this] {
            return callback_admission_closed || idle_eviction_in_progress
                   || close_reservation != 0;
        });
    }

    bool idle_quiescent () const;

    bool try_post_serial (std::string name,
                          std::function<void ()> work,
                          runtime::serial_work_options_t options = {});
    bool try_post_serial_after_current_turn (std::string name,
                                             std::function<void ()> work,
                                             runtime::serial_work_options_t options = {});
    bool try_post_serial_async (std::string name,
                                runtime::serial_execution_queue_t::async_work_t work,
                                runtime::serial_work_options_t options = {});
    void run_serial_task_async (
      std::string name,
      std::function<task_t<void> ()> work,
      std::function<void (result_t<void>)> completion,
      std::function<void (const std::shared_ptr<runtime::serial_execution_queue_t> &,
                          runtime::serial_submission_id_t)> submitted = {},
      std::function<void ()> activated = {},
      std::function<void (bool)> cancellation_observed = {},
      std::function<void ()> transfer_owner_reservation = {},
      std::size_t transferred_owner_byte_cost = 0);
    result_t<void> run_serial_task (std::string name, std::function<task_t<void> ()> work);
    bool run_serial_sync (std::string name, std::function<void ()> work);
    bool owns_current_serial_turn () const;
    void defer_relocation_ready ();
    void ensure_relocation_turn_open () const;
    void complete_relocation_ready (spot_relocation_ready_outcome_t outcome);
    void drain_serial ();
    void cancel_timers () noexcept;

    // Assigned once with node when the activation is created and never reset.
    // Teardown can therefore select its owner lane after node becomes null.
    std::weak_ptr<spot_node_builder_state_t> lane_owner;
    std::shared_ptr<spot_node_builder_state_t> node;
    std::shared_ptr<channel_runtime_state_t> channel_runtime;
    node_rid_t node_rid;
    std::string mesh_name;
    spot_id_t spot_id;
    std::uint64_t object_generation = 1;
    std::uint64_t authority_owner_generation = 1;
    std::string spot_name;
    user_spot_execution_mode_t execution_mode = user_spot_execution_mode_t::spot_wide;
    spot_relocation_coordination_mode_t relocation_coordination_mode =
      spot_relocation_coordination_mode_t::framework_managed;
    spot_lifecycle_domain_t lifecycle_domain = spot_lifecycle_domain_t::user ();
    bool relocation_boundary_active = false;
    bool relocation_ready_deferred = false;
    std::vector<spot_packet_descriptor_t> packets;
    std::vector<spot_handler_descriptor_t> handlers;
    std::vector<spot_handler_registry_t::invoker_t> handler_invokers;
    std::map<std::type_index, spot_actor_admission_callbacks_t> actor_admissions;
    std::weak_ptr<service::spot_t> native_spot;
    std::vector<std::shared_ptr<timer_state_t>> timers;
    std::shared_ptr<service_scope_t> activation_scope;
    std::map<std::type_index, std::shared_ptr<void>> timer_handler_instances;
    std::shared_ptr<void> spot_instance;
    std::shared_ptr<runtime::offload_executor_t> serial_executor;
    std::shared_ptr<runtime::serial_execution_queue_t> serial_queue;
    std::shared_ptr<spot_serial_executor_t> spot_serial_executor;
    std::shared_ptr<worker_scheduler_t> worker_scheduler;
    std::vector<zlink::received_t> queued_routed_packets;
    spot_lifecycle_callbacks_t lifecycle;
    std::map<std::type_index, std::function<task_t<void> (void *, void *)>>
      on_actor_joined_callbacks;
    std::map<
      std::type_index,
      std::function<void (void *, void *, const zlink::message_t &, serializer_registry_t &)>>
      on_create_actor_callbacks;
    std::map<std::type_index, std::function<task_t<void> (void *, void *)>>
      on_leave_actor_callbacks;
    std::map<std::type_index, std::function<task_t<void> (void *, void *)>>
      on_disconnect_actor_callbacks;
    bool close_requested = false;
    bool idle_eviction_in_progress = false;
    bool callback_admission_closed = false;
    bool closed = false;
    std::size_t actor_count = 0;
    // Node-lane ownership claim spanning an external close/store callback.
    // Actor membership publication must reject/retry while this is non-zero.
    std::uint64_t close_reservation = 0;
    std::uint64_t next_close_reservation = 1;
    bool close_reservation_is_idle = false;
    const spot_context_t *close_registered_context = nullptr;
    std::atomic<std::int64_t> last_application_work_completed_ns{0};
    // The node state lane owns lifecycle admission and close/eviction state.
    // A token acquired in a node turn can therefore retain this depth through
    // queue wait, Yield, and handler terminal without a second owner claim.
    std::size_t callback_depth = 0;
    service::instance_spot_close_completion_t pending_instance_spot_close_completion;

    bool has_active_callback () const
    {
        return state_sync ([this] { return callback_depth > 0; });
    }

    std::shared_ptr<spot_serial_executor_t> ensure_spot_serial_executor ()
    {
        if (spot_serial_executor)
            return spot_serial_executor;
        auto owner = lane_owner.lock ();
        if (!owner)
            owner = node;
        const auto worker_executor = serial_executor ? serial_executor : owner
                                                           ? owner->worker_executor
                                                           : nullptr;
        if (!owner || !worker_executor)
            return spot_serial_executor;
        if (!owner->lane.is_on_lane ())
            return owner->lane.run ([this, owner] {
                return ensure_spot_serial_executor_on_lane (owner->lane);
            }).get ();
        return ensure_spot_serial_executor_on_lane (owner->lane);
    }

  private:
    std::shared_ptr<spot_serial_executor_t>
    ensure_spot_serial_executor_on_lane (runtime::state_lane_t &state_lane)
    {
        assert (state_lane.is_on_lane ());
        if (spot_serial_executor)
            return spot_serial_executor;
        const auto lane_owner_state = lane_owner.lock ();
        const auto owner = lane_owner_state ? lane_owner_state : node;
        const auto worker_executor = serial_executor ? serial_executor
                                                     : owner ? owner->worker_executor : nullptr;
        if (!worker_executor)
            return {};
        const auto policy = is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
                            : execution_mode == user_spot_execution_mode_t::spot_wide
                              ? runtime::serial_lane_policy_t::spot_wide ()
                              : runtime::serial_lane_policy_t::per_actor_spot ();
        spot_serial_executor = std::make_shared<spot_serial_executor_t> (
          worker_executor, state_lane, policy, serial_queue);
        return spot_serial_executor;
    }

  public:
    bool is_entry_spot () const noexcept { return lifecycle_domain.is_entry (); }

    bool is_instance_spot () const noexcept { return lifecycle_domain.is_instance (); }

    bool allows_relocation () const noexcept { return lifecycle_domain.allows_relocation (); }

    bool
    accepts_route_fence (const runtime::protocol::spot_route_fence_t &target,
                         const std::optional<location_owner_token_t> &owner_token) const noexcept
    {
        return target.spot_id == spot_id
               && target.authority_owner_generation == authority_owner_generation && owner_token
               && owner_token->lease_generation == target.owner_lease_generation;
    }

    std::uint64_t begin_idle_close_reservation ()
    {
        auto owner = state_lane_owner ();
        if (!owner)
            return 0;
        return state_sync ([this, &owner] {
            if (node.get () != owner.get () || closed || actor_count != 0
                || !lifecycle_domain.allows_idle_eviction ()) {
                return std::uint64_t{0};
            }
            if (close_reservation != 0)
                return close_reservation_is_idle ? close_reservation : std::uint64_t{0};
            return reserve_close_core (*owner, true);
        });
    }

    void cancel_idle_close_reservation (std::uint64_t token) noexcept
    {
        try {
            if (auto owner = state_lane_owner ()) {
                state_sync ([this, token] {
                    if (!closed && close_reservation == token && close_reservation_is_idle) {
                        clear_close_reservation_core (token);
                        idle_eviction_in_progress = false;
                        if (!close_requested)
                            callback_admission_closed = false;
                    }
                });
            }
        }
        catch (...) {
        }
    }

    bool try_close_idle (std::uint64_t expected_reservation = 0)
    {
        auto owner = state_lane_owner ();
        if (!owner)
            return false;

        const auto start = owner->lane.run ([this, &owner, expected_reservation] {
            auto token = expected_reservation;
            if (token == 0) {
                if (close_reservation != 0) {
                    if (!close_reservation_is_idle)
                        return idle_close_start_t{};
                    token = close_reservation;
                }
                else {
                    token = reserve_close_core (*owner, true);
                }
            }
            if (token == 0 || close_reservation != token || !close_reservation_is_idle
                || node.get () != owner.get () || closed || actor_count != 0
                || !lifecycle_domain.allows_idle_eviction ()) {
                return idle_close_start_t{};
            }
            if (callback_depth != 0 || close_requested
                || (callback_admission_closed && !idle_eviction_in_progress)) {
                return idle_close_start_t{token, false};
            }
            callback_admission_closed = true;
            idle_eviction_in_progress = true;
            return idle_close_start_t{token, idle_age_allows_close_core (*owner)};
        }).get ();
        if (start.token == 0 || !start.age_allows_close) {
            if (start.token != 0)
                cancel_idle_close_reservation (start.token);
            return false;
        }

        if (!idle_quiescent ()) {
            cancel_idle_close_reservation (start.token);
            return false;
        }

        const auto committed = owner->lane.run ([this, &owner, token = start.token] {
            if (close_reservation != token || !close_reservation_is_idle
                || node.get () != owner.get () || closed || actor_count != 0
                || !lifecycle_domain.allows_idle_eviction ()
                || !idle_age_allows_close_core (*owner) || callback_depth != 0
                || !callback_admission_closed || !idle_eviction_in_progress) {
                return false;
            }
            closed = true;
            return true;
        }).get ();
        if (!committed) {
            cancel_idle_close_reservation (start.token);
            return false;
        }

        close_application_then_release_location (
          owner, spot_close_reason_t::idle_evicted, start.token);
        return true;
    }

  private:
    struct close_decision_t
    {
        bool deferred = false;
        bool committed = false;
    };

    struct close_start_t
    {
        std::uint64_t token = 0;
        spot_id_t spot_id;
        std::string spot_name;
        std::uint64_t object_generation = 0;
        std::uint64_t authority_owner_generation = 0;
        instance_spot_close_begin_callback_t begin_instance_close;
        bool requires_instance_close = false;
        bool existing = false;
    };

    struct idle_close_start_t
    {
        std::uint64_t token = 0;
        bool age_allows_close = false;
    };

    struct application_detach_work_t
    {
        std::shared_ptr<void> instance;
        std::function<void (void *, const spot_closing_context_t &, std::stop_token)> on_closing;
    };

    std::shared_ptr<spot_node_builder_state_t> state_lane_owner () const
    {
        if (auto owner = lane_owner.lock ())
            return owner;
        // Tests and contexts created before lane_owner wiring set node directly.
        return node;
    }

    std::uint64_t reserve_close_core (spot_node_builder_state_t &owner, bool idle)
    {
        auto token = next_close_reservation++;
        if (token == 0) {
            token = next_close_reservation++;
        }
        if (next_close_reservation == 0)
            next_close_reservation = 1;
        close_reservation = token;
        close_reservation_is_idle = idle;
        const auto found = owner.spot_contexts_by_id.find (std::string (spot_id));
        close_registered_context = found == owner.spot_contexts_by_id.end ()
                                     ? nullptr
                                     : std::addressof (found->second);
        return token;
    }

    void clear_close_reservation_core (std::uint64_t token) noexcept
    {
        if (token == 0 || close_reservation != token)
            return;
        close_reservation = 0;
        close_reservation_is_idle = false;
        close_registered_context = nullptr;
    }

    bool idle_age_allows_close_core (const spot_node_builder_state_t &owner) const
    {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
                              std::chrono::steady_clock::now ().time_since_epoch ())
                              .count ();
        const auto timeout_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds> (owner.instance_spot_idle_timeout)
            .count ();
        const auto last_ns = last_application_work_completed_ns.load (std::memory_order_relaxed);
        return last_ns > 0 && now_ns >= last_ns && now_ns - last_ns >= timeout_ns;
    }

    void detach_application_instance_reserved (
      const std::shared_ptr<spot_node_builder_state_t> &owner,
      std::uint64_t token,
      bool notify_closing,
      spot_close_reason_t close_reason,
      std::chrono::system_clock::time_point deadline,
      std::stop_token cleanup_cancellation)
    {
        auto work = owner->lane.run ([this, &owner, token] {
            if (close_reservation != token || node.get () != owner.get ())
                return application_detach_work_t{};
            return application_detach_work_t{std::move (spot_instance), lifecycle.on_closing};
        }).get ();

        std::exception_ptr callback_error;
        if (notify_closing && work.on_closing && work.instance) {
            try {
                work.on_closing (work.instance.get (),
                                 spot_closing_context_t{close_reason, deadline},
                                 cleanup_cancellation);
            }
            catch (...) {
                callback_error = std::current_exception ();
            }
        }

        // The reservation transfers the closed activation's teardown resources
        // to this coordinator. Timer/native callbacks remain outside state turns.
        cancel_timers ();
        auto scope = owner->lane.run ([this, token] {
            if (close_reservation != token)
                return std::shared_ptr<service_scope_t>{};
            timer_handler_instances.clear ();
            return activation_scope;
        }).get ();
        work.instance.reset ();
        if (scope)
            scope->close ();
        owner->lane.run ([this, &owner, token, &scope] {
            if (close_reservation != token)
                return;
            if (activation_scope == scope)
                activation_scope.reset ();
            if (node.get () == owner.get ())
                node.reset ();
        }).get ();

        if (callback_error)
            std::rethrow_exception (callback_error);
    }

    void close_application_then_release_location (
      const std::shared_ptr<spot_node_builder_state_t> &owner,
      spot_close_reason_t reason,
      std::uint64_t token)
    {
        std::exception_ptr closing_error;
        try {
            // The location remains published until OnClosing has completed, so
            // another node cannot create a competing incarnation while the
            // application is still preserving its final state.
            detach_application_instance_reserved (
              owner, token, true, reason, std::chrono::system_clock::time_point::max (), {});
        }
        catch (...) {
            closing_error = std::current_exception ();
        }

        const auto rid = std::string (spot_id);
        auto *location_lifecycle = owner->lane.run ([this, &owner, token] {
            return close_reservation == token ? owner->location_lifecycle : nullptr;
        }).get ();
        try {
            if (location_lifecycle) {
                (void) location_lifecycle->release_spot (spot_location_key_t{rid});
            }
        }
        catch (...) {
            owner->lane.run ([this, token] { clear_close_reservation_core (token); }).get ();
            throw;
        }

        owner->lane.run ([this, &owner, &rid, token] {
            if (close_reservation != token)
                return;
            const auto context = owner->spot_contexts_by_id.find (rid);
            const auto exact_context =
              context != owner->spot_contexts_by_id.end ()
              && std::addressof (context->second) == close_registered_context;
            if (exact_context) {
                owner->spot_contexts_by_id.erase (context);
                owner->spot_names_by_id.erase (rid);
                owner->native_spots_by_id.erase (rid);
                for (auto iterator = owner->spot_ids_by_name.begin ();
                     iterator != owner->spot_ids_by_name.end (); ++iterator) {
                    if (iterator->second == rid) {
                        owner->spot_ids_by_name.erase (iterator);
                        break;
                    }
                }
            }
            clear_close_reservation_core (token);
        }).get ();
        if (closing_error)
            std::rethrow_exception (closing_error);
    }
};

class spot_context_access_t final
{
  public:
    static spot_context_t create () { return spot_context_t (); }

    static spot_context_t create (std::shared_ptr<spot_context_state_t> state)
    {
        return spot_context_t (std::move (state));
    }
};

inline void record_actor_route_unlocked (spot_node_builder_state_t &state,
                                         const std::string &key,
                                         spot_route_t route,
                                         std::uint64_t generation)
{
    state.actor_spot_ids[key] = route.spot_id;
    state.actor_routes[key] = std::move (route);
    state.actor_generations[key] = generation;
}

inline void record_actor_spot_location_unlocked (spot_node_builder_state_t &state,
                                                 const std::string &key,
                                                 spot_id_t spot_id,
                                                 std::uint64_t generation)
{
    state.actor_spot_ids[key] = std::move (spot_id);
    state.actor_routes.erase (key);
    state.actor_generations[key] = generation;
}

inline void record_actor_context_route_unlocked (spot_node_builder_state_t &state,
                                                 const std::string &key,
                                                 const std::string &node_rid,
                                                 spot_context_state_t &context_state,
                                                 std::uint64_t generation)
{
    record_actor_route_unlocked (state, key,
                                 spot_route_t{node_rid_t::from_string (node_rid),
                                              context_state.spot_id, context_state.spot_name},
                                 generation);
    context_state.actor_count++;
}

inline std::string effective_spot_node_rid (const spot_node_snapshot_t &snapshot)
{
    if (snapshot.routing_id) {
        return snapshot.routing_id->to_string ();
    }
    return snapshot.name;
}

class spot_node_runtime_t
{
  public:
    struct application_relocation_unit_t
    {
        spot_id_t spot_id;
        std::string spot_type;
        bool ready = false;
        std::vector<actor_ref_t> actors;
    };
    struct remote_actor_transfer_t
    {
        spot_id_t source_spot_id;
        zlink::message_t state;
        std::string relocation_content_type;
    };
    explicit spot_node_runtime_t (std::shared_ptr<spot_node_builder_state_t> state);

    std::weak_ptr<spot_node_builder_state_t> weak_state () const noexcept { return _state; }

    static spot_node_runtime_t from (const spot_node_builder_t &builder);
    static std::optional<spot_node_runtime_t> from (const zlink_builder_t &builder,
                                                    const std::string &spot_node_name);
    static std::vector<spot_node_snapshot_t> snapshots (const zlink_builder_t &builder);

    local_spot_create_result_t create_spot (std::string spot_name);
    local_spot_create_result_t create_spot (std::string spot_name, zlink::message_t request);
    local_spot_create_result_t get_or_create_spot (std::string spot_name, spot_id_t spot_id);
    local_spot_create_result_t get_or_create_spot (std::string spot_name,
                                                   spot_id_t spot_id,
                                                   zlink::message_t request,
                                                   std::uint64_t object_generation = 1,
                                                   std::string mesh_name = {},
                                                   std::uint64_t authority_owner_generation = 1);
    task_t<zlink::message_t>
    dispatch_instance_activation (const spot_id_t &spot_id,
                                  std::string packet_name,
                                  std::string content_type,
                                  std::vector<std::uint8_t> payload,
                                  std::map<std::string, std::string> metadata,
                                  bool request,
                                  std::string correlation_id,
                                  service_provider_t &services,
                                  serializer_registry_t &serializers,
                                  std::optional<std::string> flow_id = std::nullopt,
                                  std::optional<flow_origin_t> flow_origin = std::nullopt);
    std::optional<spot_info_t> find_spot (spot_id_t spot_id) const;
    std::vector<spot_info_t> list_spots () const;
    task_t<bool> close_spot (spot_id_t spot_id);
    bool close_all_user_spots ();
    node_rid_t node_rid () const;
    std::optional<std::string> spot_name_for (spot_id_t spot_id) const;
    std::optional<spot_route_t> resolve_spot (spot_id_t spot_id) const;
    std::optional<spot_id_t> actor_spot (const actor_ref_t &actor_ref) const;
    result_t<bool> destroy_actor (const actor_ref_t &actor_ref);
    void record_actor_spot (const actor_ref_t &actor_ref, spot_id_t spot_id);
    std::optional<spot_route_t> actor_route (const actor_ref_t &actor_ref) const;
    bool matches_actor_message_follow_source (
      const actor_ref_t &actor_ref,
      const runtime::protocol::actor_route_fence_t &source_fence) const;
    result_t<std::optional<actor_message_follow_target_t>>
    try_acquire_actor_message_follow (const actor_ref_t &actor_ref,
                                      std::size_t payload_bytes,
                                      std::size_t hop_count,
                                      const runtime::protocol::actor_route_fence_t &source_fence);
    void release_actor_message_follow (const actor_ref_t &actor_ref,
                                       const runtime::protocol::actor_route_fence_t &source_fence,
                                       std::size_t payload_bytes) noexcept;
    bool try_begin_actor_message_follow_notification (
      const actor_ref_t &actor_ref,
      const runtime::protocol::actor_route_fence_t &source_fence,
      const runtime::protocol::actor_route_fence_t &target_fence);
    bool complete_actor_message_follow_notification (
      const actor_ref_t &actor_ref,
      const runtime::protocol::actor_route_fence_t &source_fence,
      const runtime::protocol::actor_route_fence_t &target_fence,
      bool transport_accepted);
    void record_actor_route (const actor_ref_t &actor_ref, spot_route_t route);
    std::optional<std::string> actor_route_transport_name () const;
    void request_stop () noexcept;
    bool stopping () const noexcept;
    void cancel_timers () noexcept;
    void cancel_pending_dispatch () noexcept;
    void cancel_pending_work () noexcept;

    /* Releases the native SPOT/actor handles this node created on its own
     * zlink context. The host service must call this before terminating the
     * context: a live socket keeps `zlink_ctx_term()` waiting forever. */
    void release_native_handles () noexcept;
    std::optional<actor_ref_t> current_actor_ref (const actor_ref_t &actor_ref) const;
    void attach_native_node (std::shared_ptr<service::mesh_node_t> node);
    void detach_native_node ();
    void evict_idle_spots () noexcept;
    void record_core_actor_transfer_activation (std::string actor_id,
                                                std::uint64_t membership_epoch);
    void bind_location_lifecycle (runtime::location_lifecycle_t &lifecycle);
    void bind_spot_location_resolver (runtime::spot_address_resolver_t &resolver);
    void bind_service_provider (service_provider_t &services);
    void bind_drain_flag (std::shared_ptr<std::atomic_bool> flag);
    /* Entry spots are host infrastructure and are excluded. */
    std::size_t active_user_spot_count () const;
    /* In-flight probe for the drain worker: true while any spot callback of
     * this node is still executing (graceful-drain-handoff §4-4). */
    bool has_active_callbacks () const;
    /* Actors still joined to this node's spots, for the drain handoff pass
     * (graceful-drain-handoff §5.2). */
    std::vector<actor_ref_t> local_actor_refs () const;
    std::vector<spot_id_t> deferred_relocation_ready_spots () const;
    std::vector<application_relocation_unit_t> application_relocation_units () const;
    void begin_relocation_readiness ();
    void end_relocation_readiness (const std::vector<spot_id_t> &relocated_spots);
    bool complete_relocation_ready (const spot_id_t &spot_id,
                                    spot_relocation_ready_outcome_t outcome);
    /* Domain snapshot for the drain handoff join — the same shape the erased
     * cross-node leave path sends alongside the entry-spot join. */
    std::optional<zlink::message_t> serialize_actor_snapshot (const actor_ref_t &actor_ref) const;
    std::shared_ptr<service::mesh_node_t> native_node () const;
    task_t<void> send_spot_mesh_parts (const zlink::routing_id_t &target_node_rid,
                                       const spot_id_t &target_spot_id,
                                       runtime::messaging::message_parts_t parts) const;
    task_t<void> send_spot_mesh_parts_exact (const spot_id_t &source_spot_id,
                                             const zlink::routing_id_t &target_node_rid,
                                             const spot_id_t &target_spot_id,
                                             std::uint64_t target_spot_generation,
                                             runtime::messaging::message_parts_t parts) const;
    // Node-level send for the OnLeave notification -- see
    // actor_leave_notification_sender's comment for why this cannot go
    // through send_spot_mesh_parts_exact.
    task_t<zlink::submit_result_t>
    send_actor_leave_notification (const zlink::routing_id_t &target_node_rid,
                                   runtime::messaging::message_parts_t parts) const;
    std::optional<std::uint64_t>
    resolve_spot_generation (const zlink::routing_id_t &target_node_rid,
                             const spot_id_t &target_spot_id) const;
    // Canonical actorJoin(28) reaches this boundary without a stable type.
    // Read the target's Store Authority row here (not a cacheable routing
    // projection), alongside the Actor Authority fence/type resolution in
    // admit_remote_actor_to_spot. The transport deliberately does not own
    // these authority decisions.
    result_t<std::uint64_t>
    resolve_wire_actor_join_target (const runtime::protocol::spot_route_fence_t &fence) const;
    std::vector<spot_context_t> active_contexts () const;
    result_t<void> dispatch_subscription (const spot_context_t &context,
                                          std::string topic,
                                          const zlink::message_t &message,
                                          service_provider_t &services,
                                          serializer_registry_t &serializers) const;
    result_t<void>
    dispatch_subscription (const spot_context_t &context,
                           std::string topic,
                           const std::vector<zlink::message_t> &parts,
                           service_provider_t &services,
                           serializer_registry_t &serializers,
                           std::function<void ()> before_application_handler = {},
                           std::function<void ()> transfer_owner_reservation = {},
                           std::size_t transferred_owner_byte_cost = 0) const;
    result_t<std::size_t> dispatch_multicast (std::string topic,
                                              const std::vector<zlink::message_t> &parts,
                                              service_provider_t &services,
                                              serializer_registry_t &serializers) const;
    bool dispatch_mesh_record (const service::ready_record_t &owner,
                               const service::receive_record_t &record,
                               std::vector<zlink::message_t> &parts,
                               service_provider_t &services,
                               serializer_registry_t &serializers,
                               std::function<void ()> deferred_terminal = {},
                               bool *terminal_deferred = nullptr,
                               std::function<void ()> before_application_handler = {});
    void set_route_client (route_client_t route_client);
    void on_destroy_actor (std::function<result_t<void> (const actor_ref_t &)> destroy_actor);
    void on_actor_ref_updated (std::function<result_t<void> (const actor_ref_t &)> update_actor);
    void on_actor_entry_spot_join (
      std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                                  node_rid_t,
                                                  const zlink::message_t &,
                                                  const std::optional<zlink::message_t> &)> join);
    void on_actor_packet_relay (std::function<task_t<std::optional<zlink::message_t>> (
                                  const actor_ref_t &,
                                  actor_context_t,
                                  stream_message_kind_t,
                                  std::string_view,
                                  const zlink::message_t &,
                                  service_provider_t &,
                                  serializer_registry_t &,
                                  spot_inbound_message_t,
                                  const runtime::protocol::actor_route_fence_t *)> relay);
    void on_actor_message_follow (std::function<task_t<std::optional<zlink::message_t>> (
                                    const actor_ref_t &,
                                    const runtime::messaging::envelope_header_t &,
                                    const zlink::message_t &,
                                    std::chrono::milliseconds,
                                    const zlink::routing_id_t &,
                                    const runtime::protocol::actor_route_fence_t &,
                                    std::uint8_t,
                                    const runtime::protocol::wire_operation_id_t &,
                                    std::uint64_t)> relay);
    void on_actor_handoff_terminal (
      std::function<task_t<bool> (const zlink::routing_id_t &,
                                  const zlink::routing_id_t &,
                                  const runtime::protocol::wire_operation_id_t &,
                                  std::uint64_t,
                                  const runtime::protocol::actor_route_fence_t &,
                                  const result_t<zlink::message_t> &)> sender);
    void on_actor_leave_notification (
      std::function<task_t<zlink::submit_result_t> (const zlink::routing_id_t &,
                                                    std::vector<zlink::message_t>)> sender);
    void invalidate_message_follow_route (const runtime::protocol::message_follow_notice_t &notice);
    spot_manager_t manager () const;
    result_t<actor_join_reply_t>
    join_actor_to_spot_erased (const actor_ref_t &actor_ref,
                               spot_id_t spot_id,
                               const zlink::message_t &request,
                               const std::optional<zlink::message_t> &actor_snapshot = std::nullopt,
                               actor_context_t actor_context = {},
                               std::uint64_t completion_operation_id_high = 0,
                               std::uint64_t completion_operation_id_low = 0);
    result_t<actor_join_reply_t>
    join_remote_actor_to_spot_erased (const actor_ref_t &actor_ref,
                                      spot_id_t spot_id,
                                      const zlink::message_t &request,
                                      actor_context_t actor_context = {});
    result_t<spot_actor_join_result_t>
    admit_remote_actor_to_spot (std::string transfer_id,
                                const actor_ref_t &actor_ref,
                                spot_id_t source_spot_id,
                                spot_id_t target_spot_id,
                                const zlink::message_t &request,
                                std::uint64_t completion_operation_id_high = 0,
                                std::uint64_t completion_operation_id_low = 0,
                                std::uint64_t actor_authority_owner_generation = 0,
                                std::uint64_t actor_node_generation = 0,
                                std::uint64_t expected_owner_lease_generation = 0,
                                bool actor_type_from_authority_only = false,
                                std::uint64_t target_spot_generation = 0,
                                std::uint64_t target_spot_authority_owner_generation = 0);
    std::optional<bool> validate_actor_join_relocation_prepare (
      const runtime::protocol::relocation_prepare_t &prepare) const;
    bool consume_actor_join_recovery (runtime::stateful::frozen_object_state_t &frozen,
                                      const runtime::stateful::object_ref_t &target,
                                      const runtime::protocol::relocation_prepare_t &prepare);
    /* Undo a consume_actor_join_recovery that a later staging step rejected.
     * Keyed on the wire stable type rather than the local actor-type index:
     * an Actor that has never been created on this node has no index entry
     * yet, which is exactly the case that leaks (15 §4.2). */
    void discard_actor_join_recovery (const std::string &stable_type,
                                      const runtime::stateful::object_ref_t &target) noexcept;
    std::optional<std::tuple<std::string, std::string, std::uint64_t>>
    actor_join_relocation_authority_spot (const runtime::stateful::object_ref_t &target) const;
    // handoff_backlog holds the in-flight packets the source preserved while the
    // actor was moving (§10.2-2). They are enqueued on the target actor's
    // dispatch queue before the committed location is published (§10.2-3), and
    // services may be null only when the backlog is empty.
    result_t<actor_join_reply_t> prepare_remote_actor_to_spot (std::string transfer_id,
                                                               const actor_ref_t &actor_ref,
                                                               spot_id_t target_spot_id,
                                                               zlink::message_t transfer_state,
                                                               actor_context_t actor_context = {},
                                                               bool defer_joined_callback = false);
    result_t<void> commit_remote_actor_authority (
      const std::string &transfer_id,
      const actor_ref_t &actor_ref,
      const spot_id_t &target_spot_id,
      std::uint64_t target_spot_generation,
      std::uint64_t source_authority_owner_generation,
      std::string source_mesh_name,
      std::string target_mesh_name,
      std::uint64_t target_node_lifecycle_generation,
      location_owner_token_t target_owner,
      std::uint64_t *committed_previous_authority_owner_generation = nullptr,
      std::uint64_t *committed_target_authority_owner_generation = nullptr);
    result_t<void>
    stage_remote_actor_commit_backlog (const std::string &transfer_id,
                                       std::vector<handoff_packet_t> handoff_backlog);
    std::optional<actor_join_reply_t>
    completed_remote_actor_commit (const std::string &transfer_id,
                                   const actor_ref_t &source_actor,
                                   const spot_id_t &target_spot_id) const;
    result_t<actor_join_reply_t>
    commit_remote_actor_to_spot (std::string transfer_id,
                                 const actor_ref_t &actor_ref,
                                 spot_id_t target_spot_id,
                                 zlink::message_t transfer_state,
                                 actor_context_t actor_context = {},
                                 std::vector<handoff_packet_t> handoff_backlog = {},
                                 service_provider_t *services = nullptr);
    result_t<actor_join_reply_t> finalize_remote_actor_to_spot (
      std::string transfer_id,
      const actor_ref_t &actor_ref,
      spot_id_t target_spot_id,
      service_provider_t &services,
      actor_gateway_runtime_t *actor_gateway = nullptr,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
    void finalize_remote_actor_to_spot_async (
      std::string transfer_id,
      const actor_ref_t &actor_ref,
      spot_id_t target_spot_id,
      service_provider_t &services,
      actor_gateway_runtime_t *actor_gateway,
      std::optional<std::chrono::steady_clock::time_point> deadline,
      std::function<void (result_t<actor_join_reply_t>)> completion,
      std::function<task_t<void> ()> submit_source_leave = {});
    std::size_t cleanup_expired_actor_admissions ();
    std::size_t cleanup_expired_actor_admissions_at (std::chrono::steady_clock::time_point now);
    bool stage_session_relocation_route (const std::string &transfer_id,
                                         std::vector<std::uint8_t> route,
                                         std::string actor_type,
                                         std::uint64_t target_owner_lease_generation);
    bool
    commit_session_relocation_route_authority (const std::string &transfer_id,
                                               std::uint64_t previous_authority_owner_generation,
                                               std::uint64_t target_authority_owner_generation);
    bool adopt_committed_actor_relocation_authority (const runtime::stateful::object_ref_t &target,
                                                     std::uint64_t target_node_generation,
                                                     std::uint64_t target_owner_lease_generation);
    task_t<bool> activate_session_relocation_route (const std::string &transfer_id);
    bool remove_actor_message_follow (const actor_ref_t &actor_ref,
                                      const runtime::protocol::actor_route_fence_t &source_fence,
                                      const runtime::protocol::actor_route_fence_t &target_fence);
    std::string next_actor_transfer_id ();
    // Transfer ID stamped on a still-reserved deferred join, so the remote
    // join reuses the correlation already carried by preserved handoff
    // markers instead of allocating a second ID.
    std::optional<std::string> reserved_actor_transfer_id (const actor_ref_t &actor_ref) const;
    result_t<std::shared_ptr<deferred_barrier_t>>
    reserve_actor_join_barrier (const actor_ref_t &actor_ref);
    std::pair<std::uint64_t, std::uint64_t>
    actor_join_operation_id (std::string_view transfer_id) const;
    result_t<void>
    deliver_actor_join_completion (const actor_ref_t &actor_ref,
                                   const actor_join_completion_t &completion,
                                   std::optional<spot_id_t> source_spot_id = std::nullopt);
    // In-flight handoff (spot-actor §10): drains the packets preserved while the
    // actor was moving, in arrival order. The commit path calls this once to fill
    // the commit request and once more after the ack for packets that raced it.
    std::vector<handoff_packet_t> take_actor_handoff_backlog (const actor_ref_t &actor_ref);
    bool actor_transfer_in_progress (const actor_ref_t &actor_ref) const;
    bool actor_transfer_in_progress (std::string_view actor_id) const;
    // Node-local knowledge required to admit a wire actorJoin(28): the
    // stable type string (never carried on that wire body — reconstructing
    // an actor_ref_t requires it) and the actor's current membership epoch
    // (seeded only at actor creation; a wire actorJoin(28) for an actor this
    // node has never created/known is reported std::nullopt rather than
    // guessed, so the caller can reject cleanly).
    std::optional<std::string> resolve_actor_type (std::string_view actor_id) const;
    std::optional<std::uint64_t> resolve_actor_membership_epoch (std::string_view actor_id) const;
    // Same resolution join_actor_to_entry_spot_erased performs internally,
    // exposed so a caller building an actorJoin(28) accepted reply can name
    // the entry Spot it actually joined (entry=true requests carry an
    // advisory targetSpot the entry path does not use to select the Spot).
    std::optional<spot_id_t> resolve_entry_spot_id () const;
    // actor_context_t's default constructor is private (friended to
    // spot_node_runtime_t among a few others) -- this exposes it to callers
    // outside spots/** that need a default/unbound context, e.g. the
    // actorJoin(28) receiver path, which does not run bind_actor_route.
    static actor_context_t default_actor_context ();
    void set_message_follow_duration (std::chrono::milliseconds duration);
    void bind_relocation_store (std::shared_ptr<runtime::stateful::relocation_store_port_t> store);
    void bind_relocation_authority (
      std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority);
    std::vector<std::uint8_t>
    capture_spot_relocation_state (const runtime::stateful::object_ref_t &spot,
                                   const std::string &stable_type,
                                   std::stop_token cancellation = {}) const;
    bool restore_spot_relocation_state (const runtime::stateful::frozen_object_state_t &frozen,
                                        const runtime::stateful::object_ref_t &target,
                                        std::stop_token cancellation = {});
    bool
    materialize_relocation_state (const runtime::stateful::frozen_object_state_t &frozen,
                                  const runtime::stateful::object_ref_t &target,
                                  const std::optional<runtime::stateful::object_ref_t> &target_spot,
                                  std::stop_token cancellation = {});
    bool
    commit_relocation_materialization (const std::vector<runtime::stateful::object_ref_t> &targets);
    void abort_relocation_materialization (
      const std::vector<runtime::stateful::object_ref_t> &targets) noexcept;
    result_t<remote_actor_transfer_t> transfer_actor_out (const actor_ref_t &actor_ref,
                                                          std::string transfer_id = {},
                                                          bool capture_state = true);
    result_t<void> leave_actor_for_remote_transfer (const actor_ref_t &actor_ref);
    result_t<void>
    submit_remote_actor_leave (const std::string &transfer_id,
                               const actor_ref_t &source_actor,
                               const spot_id_t &source_spot_id,
                               std::uint64_t source_spot_generation,
                               const spot_id_t &target_spot_id,
                               const runtime::protocol::actor_route_fence_t &target_fence,
                               std::function<void ()> transfer_owner_reservation = {},
                               std::size_t transferred_owner_byte_cost = 0);
    void fail_remote_actor_transfer (
      const actor_ref_t &actor_ref,
      bool reconcile,
      std::optional<reconcile_target_context_t> reconcile_context = std::nullopt);
    // Fast-fails whatever backlog is currently parked for actor_key without
    // closing the move (the actor stays blocked from local dispatch) --
    // spec 28: an indeterminate reconcile deadline must remain unavailable
    // with an explicit failure, never silently parked and never reopened
    // for blind local replay.
    task_t<void> fast_fail_reconcile_backlog (actor_ref_t actor_ref, std::string actor_key);
    task_t<void>
    complete_remote_actor_transfer (const actor_ref_t &source_actor,
                                    const actor_ref_t &target_actor,
                                    spot_route_t target_route,
                                    runtime::protocol::actor_route_fence_t source_fence,
                                    runtime::protocol::actor_route_fence_t target_fence,
                                    std::string transfer_id = {});
    // Emits an internal transfer lifecycle boundary through the configured
    // public message-flow observer. The transfer id is both the correlation
    // key and lifecycle flow key so role-server evidence can join source and
    // target events without parsing runtime logs. Hot-path callers that build
    // marker/transfer-id strings must gate on actor_transfer_marker_enabled()
    // so an off/errors mode pays no allocation (spec 26 §4).
    bool actor_transfer_marker_enabled () const noexcept;
    void
    emit_actor_transfer_marker (std::string marker,
                                const actor_ref_t &actor_ref,
                                std::string transfer_id,
                                std::optional<spot_id_t> spot_id = std::nullopt,
                                std::optional<node_rid_t> target_node_rid = std::nullopt) const;
    result_t<actor_join_reply_t>
    join_actor_to_entry_spot_erased (const actor_ref_t &actor_ref,
                                     node_rid_t spot_node_rid,
                                     const zlink::message_t &request,
                                     const std::optional<zlink::message_t> &actor_snapshot,
                                     actor_context_t actor_context);
    task_t<std::optional<zlink::message_t>>
    relay_actor_packet (const actor_ref_t &actor_ref,
                        actor_context_t actor_context,
                        std::string_view packet_name,
                        const zlink::message_t &message,
                        service_provider_t &services,
                        serializer_registry_t &serializers,
                        spot_inbound_message_t metadata = {});
    task_t<std::optional<zlink::message_t>> relay_actor_packet (
      const actor_ref_t &actor_ref,
      actor_context_t actor_context,
      stream_message_kind_t message_kind,
      std::string_view packet_name,
      const zlink::message_t &message,
      service_provider_t &services,
      serializer_registry_t &serializers,
      spot_inbound_message_t metadata = {},
      const runtime::protocol::actor_route_fence_t *admitted_message_follow_target = nullptr,
      std::function<void ()> before_application_handler = {},
      std::function<void ()> after_application_admission = {},
      // The inbound caller's own identity (spec 28.en:
      // 584-591): a synthesized-fence relay (a cold probe
      // with no incoming follow fence) must still forward
      // these to the Message Follow relay instead of the
      // zero/empty placeholders that skip its stale-cache
      // notice back to the true original caller.
      zlink::routing_id_t inbound_source_node_rid = zlink::routing_id_t::from (std::uint32_t{0}),
      runtime::protocol::wire_operation_id_t inbound_operation = {},
      std::uint64_t inbound_reply_route_id = 0,
      std::optional<std::string> inbound_deadline = std::nullopt,
      std::function<void ()> transfer_owner_reservation = {},
      std::size_t transferred_owner_byte_cost = 0,
      std::shared_ptr<actor_dispatch_admission_token_t> admission_token = {});
    result_t<void> notify_actor_disconnected_erased (const actor_ref_t &actor_ref) const;

    template <typename TActor>
    std::optional<std::reference_wrapper<TActor>> actor_instance (const actor_ref_t &actor_ref)
    {
        const auto key = actor_key (actor_ref);
        auto instance = _state->lane.run ([&] {
            const auto found = _state->actor_instances.find (key);
            return found == _state->actor_instances.end () ? std::shared_ptr<void>{}
                                                           : found->second;
        }).get ();
        if (!instance) {
            return std::nullopt;
        }
        return *static_cast<TActor *> (instance.get ());
    }

    template <typename TSpot, typename TActor>
    result_t<actor_join_reply_t> join_actor_to_spot (const actor_ref_t &actor_ref,
                                                     spot_id_t spot_id,
                                                     TActor &actor,
                                                     const zlink::message_t &request)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor ref is empty");
        }
        std::shared_ptr<void> spot_instance;
        serializer_registry_t *callback_serializers = nullptr;
        serializer_registry_t *serializers = nullptr;
        auto context = _state->lane.run ([&] () -> std::optional<spot_context_t> {
            auto selected = find_context_core (spot_id);
            if (!selected)
                return std::nullopt;
            const auto &state = selected->_state;
            if (state->node.get () != _state.get () || state->closed
                || state->close_reservation != 0 || !state->spot_instance) {
                return std::nullopt;
            }
            spot_instance = state->spot_instance;
            callback_serializers =
              _state->channel_runtime ? _state->channel_runtime->serializers : nullptr;
            serializers = state->channel_runtime ? state->channel_runtime->serializers : nullptr;
            return selected;
        }).get ();
        if (!context) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "target spot is not registered");
        }
        auto &spot = *static_cast<TSpot *> (spot_instance.get ());
        if constexpr (!has_actor_join_callback<TSpot>) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "spot actor join callback is not registered");
        } else {
            const auto response = invoke_actor_join_callback (
              spot, actor_ref.actor_id ().value (), request, callback_serializers);
            if (!response.accepted) {
                return result_t<actor_join_reply_t>::success (
                  actor_join_reply_t{1, actor_ref, actor_join_reply (response, *serializers)});
            }

            const auto committed =
              commit_actor_to_context<TSpot, TActor> (actor_ref, actor, *context, request);
            return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{0, committed, actor_join_reply (response, *serializers)});
        }
    }

    template <typename TEntrySpot, typename TActor>
    result_t<actor_join_reply_t> join_actor_to_entry_spot (const actor_ref_t &actor_ref,
                                                           node_rid_t spot_node_rid,
                                                           TActor &actor,
                                                           const zlink::message_t &request)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor ref is empty");
        }
        enum class entry_selection_t
        {
            selected,
            node_mismatch,
            not_registered,
            not_created,
            context_missing
        };
        entry_selection_t selection = entry_selection_t::context_missing;
        std::shared_ptr<void> spot_instance;
        serializer_registry_t *callback_serializers = nullptr;
        serializer_registry_t *serializers = nullptr;
        auto context = _state->lane.run ([&] () -> std::optional<spot_context_t> {
            if (spot_node_rid.empty ()
                || spot_node_rid.value () != detail::effective_spot_node_rid (_state->snapshot)) {
                selection = entry_selection_t::node_mismatch;
                return std::nullopt;
            }
            if (!_state->snapshot.entry_spot_name) {
                selection = entry_selection_t::not_registered;
                return std::nullopt;
            }
            const auto entry_id =
              _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            if (entry_id == _state->spot_ids_by_name.end ()) {
                selection = entry_selection_t::not_created;
                return std::nullopt;
            }
            auto selected = find_context_core (entry_id->second);
            if (!selected) {
                selection = entry_selection_t::context_missing;
                return std::nullopt;
            }
            const auto &state = selected->_state;
            if (state->node.get () != _state.get () || state->closed
                || state->close_reservation != 0 || !state->spot_instance) {
                selection = entry_selection_t::context_missing;
                return std::nullopt;
            }
            spot_instance = state->spot_instance;
            callback_serializers =
              _state->channel_runtime ? _state->channel_runtime->serializers : nullptr;
            serializers = state->channel_runtime ? state->channel_runtime->serializers : nullptr;
            selection = entry_selection_t::selected;
            return selected;
        }).get ();
        if (selection == entry_selection_t::node_mismatch) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "spot node rid does not match this node");
        }
        if (selection == entry_selection_t::not_registered) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "entry spot is not registered");
        }
        if (selection == entry_selection_t::not_created) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "entry spot is not created");
        }
        if (!context) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "entry spot context is not registered");
        }

        auto &spot = *static_cast<TEntrySpot *> (spot_instance.get ());
        if constexpr (has_actor_join_callback<TEntrySpot>) {
            const auto response = invoke_actor_join_callback (
              spot, actor_ref.actor_id ().value (), request, callback_serializers);
            if (!response.accepted) {
                return result_t<actor_join_reply_t>::success (
                  actor_join_reply_t{1, actor_ref, actor_join_reply (response, *serializers)});
            }

            const auto committed =
              commit_actor_to_context<TEntrySpot, TActor> (actor_ref, actor, *context, request);
            return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{0, committed, actor_join_reply (response, *serializers)});
        }

        const auto committed =
          commit_actor_to_context<TEntrySpot, TActor> (actor_ref, actor, *context, request);
        return result_t<actor_join_reply_t>::success (actor_join_reply_t{0, committed, {}});
    }

    template <typename TActor>
    result_t<void> leave_actor (const actor_ref_t &actor_ref, TActor &actor)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            "actor ref is empty");
        }
        commit_actor_left<TActor> (actor_ref, actor);
        return result_t<void>::success ();
    }

    template <typename TActor>
    result_t<void> notify_on_disconnect_actor (const actor_ref_t &actor_ref, TActor &actor)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            "actor ref is empty");
        }
        actor_task_callback_projection_t callback;
        const auto key = actor_key (actor_ref);
        const auto stale = _state->lane.run ([&] {
            const auto found_location = _state->actor_spot_ids.find (key);
            if (found_location == _state->actor_spot_ids.end ())
                return false;
            const auto found_generation = _state->actor_generations.find (key);
            if (found_generation != _state->actor_generations.end ()
                && found_generation->second != actor_ref.object_generation ()) {
                return true;
            }
            auto context = find_context_core (found_location->second);
            if (!context)
                return false;
            const auto &state = context->_state;
            const auto found = state->on_disconnect_actor_callbacks.find (
              std::type_index (typeid (TActor)));
            if (found != state->on_disconnect_actor_callbacks.end () && state->spot_instance) {
                callback.context = state;
                callback.spot_instance = state->spot_instance;
                callback.callback = found->second;
            }
            return false;
        }).get ();
        if (stale) {
            return detail::boundary_failure<void> (detail::boundary_error_t::stale_generation,
                                                   "actor generation is stale");
        }
        try {
            run_actor_task_callback ("spot-lifecycle-disconnect",
                                     "spot actor disconnect callback failed", callback,
                                     std::addressof (actor));
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "spot actor disconnected callback failed");
        }
    }

  private:
    std::optional<std::string> spot_name_for_unlocked (const spot_id_t &spot_id) const;

    std::shared_ptr<service::spot_t>
    attach_native_spot (const std::shared_ptr<spot_context_state_t> &state,
                        bool relocation_restore = false,
                        bool publish = true);

    template <typename TSpot>
    static constexpr bool has_framework_actor_join_callback =
      requires (TSpot & spot, std::string_view actor_id, const message_t &request)
    {
        {
            spot.on_actor_join (actor_id, request)
        } -> std::same_as<spot_actor_join_result_t>;
    };

    template <typename TSpot>
    static constexpr bool has_raw_actor_join_callback =
      requires (TSpot & spot, std::string_view actor_id, const zlink::message_t &request)
    {
        {
            spot.on_actor_join (actor_id, request)
        } -> std::same_as<spot_actor_join_result_t>;
    };

    template <typename TSpot>
    static constexpr bool has_actor_join_callback =
      has_framework_actor_join_callback<TSpot> || has_raw_actor_join_callback<TSpot>;

    template <typename TSpot>
    spot_actor_join_result_t invoke_actor_join_callback (TSpot &spot,
                                                         std::string_view actor_id,
                                                         const zlink::message_t &request,
                                                         serializer_registry_t *serializers)
    {
        if constexpr (has_framework_actor_join_callback<TSpot>) {
            return spot.on_actor_join (actor_id, message_t::from_raw (request, serializers));
        } else {
            return spot.on_actor_join (actor_id, request);
        }
    }

    static zlink::message_t actor_join_reply (const spot_actor_join_result_t &response,
                                              serializer_registry_t &serializers)
    {
        return response.reply ? response.reply->to_raw (serializers) : zlink::message_t{};
    }

    template <typename TSpot, typename TActor>
    static constexpr bool has_on_actor_joined_callback = requires (TSpot & spot, TActor &actor)
    {
        spot.on_actor_joined (actor);
    };

    template <typename TSpot, typename TActor>
    static constexpr bool has_on_create_actor_callback = requires (TSpot & spot, TActor &actor)
    {
        spot.on_create_actor (actor);
    };

    template <typename TSpot, typename TActor>
    static constexpr bool has_framework_payload_on_create_actor_callback =
      requires (TSpot & spot, TActor &actor, const message_t &request)
    {
        spot.on_create_actor (actor, request);
    };

    template <typename TSpot, typename TActor>
    static constexpr bool has_raw_payload_on_create_actor_callback =
      requires (TSpot & spot, TActor &actor, const zlink::message_t &request)
    {
        spot.on_create_actor (actor, request);
    };

    template <typename TSpot, typename TActor>
    static constexpr bool has_on_leave_actor_callback = requires (TSpot & spot, TActor &actor)
    {
        spot.on_leave_actor (actor);
    };

    template <typename TSpot, typename TActor>
    static constexpr bool has_on_disconnect_actor_callback = requires (TSpot & spot, TActor &actor)
    {
        spot.on_disconnect_actor (actor);
    };

    static std::string actor_key (const actor_ref_t &actor_ref)
    {
        return std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref))
               + ":" + std::string (actor_ref.actor_id ().value ());
    }

    /* `refuse_destroyed` re-checks the destroy tombstone in the same lane turn
     * that installs, so a destroy landing inside the factory window cannot be
     * resurrected by a late install; the caller then reports the actor as
     * destroyed. Join paths pass false: a join legitimately recreates a
     * destroyed actor in a new generation and clears the tombstone on commit. */
    std::shared_ptr<void> install_actor_instance (const actor_ref_t &actor_ref,
                                                  const std::string &key,
                                                  std::shared_ptr<void> instance,
                                                  bool refuse_destroyed = false) const
    {
        return _state->lane.run ([&] {
            auto &slot = _state->actor_instances[key];
            if (!slot) {
                if (refuse_destroyed && _state->destroyed_actor_keys.contains (key)) {
                    _state->actor_instances.erase (key);
                    return std::shared_ptr<void>{};
                }
                /* A concurrent creator that won the race keeps its instance; the
                 * loser's copy is released when this frame ends. */
                slot = std::move (instance);
            }
            detail::record_actor_instance_index_unlocked (*_state, actor_ref, slot.get ());
            return slot;
        }).get ();
    }

    struct actor_task_callback_projection_t
    {
        std::shared_ptr<spot_context_state_t> context;
        std::shared_ptr<void> spot_instance;
        std::function<task_t<void> (void *, void *)> callback;
    };

    static void run_actor_task_callback (const char *turn_name,
                                         const char *failure_message,
                                         const actor_task_callback_projection_t &projection,
                                         void *actor)
    {
        if (!projection.context || !projection.spot_instance || !projection.callback)
            return;
        const auto completed = projection.context->run_serial_task (turn_name, [&] {
            return projection.callback (projection.spot_instance.get (), actor);
        });
        if (!completed) {
            throw framework_exception_t (completed.error_kind (), completed.error () != nullptr
                                                                    ? completed.error ()->what ()
                                                                    : failure_message);
        }
    }

    std::optional<spot_context_t> find_context_core (const spot_id_t &spot_id) const
    {
        const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
        if (found == _state->spot_contexts_by_id.end ())
            return std::nullopt;
        return spot_context_t (found->second._state);
    }

    std::optional<spot_context_t> find_context (const spot_id_t &spot_id) const
    {
        return _state->lane.run ([&] { return find_context_core (spot_id); }).get ();
    }

    template <typename TSpot, typename TActor>
    actor_ref_t commit_actor_to_context (const actor_ref_t &actor_ref,
                                         TActor &actor,
                                         spot_context_t &context,
                                         const zlink::message_t &create_request)
    {
        commit_actor_left<TActor> (actor_ref, actor);
        const auto key = actor_key (actor_ref);
        const auto target_state = context._state;
        std::shared_ptr<void> spot_instance;
        serializer_registry_t *serializers = nullptr;
        std::string node_rid;
        std::optional<actor_ref_t> committed;
        bool create_entry_actor = false;

        std::function<task_t<void> (void *, void *)> joined_callback = [] (void *spot,
                                                                           void *actor) {
            if constexpr (has_on_actor_joined_callback<TSpot, TActor>) {
                if constexpr (std::same_as<decltype (static_cast<TSpot *> (spot)->on_actor_joined (
                                             *static_cast<TActor *> (actor))),
                                           task_t<void>>) {
                    return static_cast<TSpot *> (spot)->on_actor_joined (
                      *static_cast<TActor *> (actor));
                } else {
                    static_cast<TSpot *> (spot)->on_actor_joined (*static_cast<TActor *> (actor));
                }
            }
            return task_t<void> (result_t<void>::success ());
        };
        std::function<void (void *, void *, const zlink::message_t &, serializer_registry_t &)>
          create_callback = [] (void *spot, void *actor, const zlink::message_t &request,
                                serializer_registry_t &registry) {
              if constexpr (detail::entry_spot_type<TSpot>
                            && has_framework_payload_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (
                    *static_cast<TActor *> (actor), message_t::from_raw (request, &registry));
              } else if constexpr (detail::entry_spot_type<TSpot>
                                   && has_raw_payload_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (*static_cast<TActor *> (actor),
                                                                request);
              } else if constexpr (detail::entry_spot_type<TSpot>
                                   && has_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (*static_cast<TActor *> (actor));
              }
          };
        std::function<task_t<void> (void *, void *)> leave_callback = [] (void *spot,
                                                                          void *actor) {
            if constexpr (has_on_leave_actor_callback<TSpot, TActor>) {
                if constexpr (std::same_as<decltype (static_cast<TSpot *> (spot)->on_leave_actor (
                                             *static_cast<TActor *> (actor))),
                                           task_t<void>>) {
                    return static_cast<TSpot *> (spot)->on_leave_actor (
                      *static_cast<TActor *> (actor));
                } else {
                    static_cast<TSpot *> (spot)->on_leave_actor (*static_cast<TActor *> (actor));
                }
            }
            return task_t<void> (result_t<void>::success ());
        };
        std::function<task_t<void> (void *, void *)> disconnect_callback = [] (void *spot,
                                                                               void *actor) {
            if constexpr (has_on_disconnect_actor_callback<TSpot, TActor>) {
                if constexpr (std::same_as<decltype (static_cast<TSpot *> (spot)->on_disconnect_actor (
                                             *static_cast<TActor *> (actor))),
                                           task_t<void>>) {
                    return static_cast<TSpot *> (spot)->on_disconnect_actor (
                      *static_cast<TActor *> (actor));
                } else {
                    static_cast<TSpot *> (spot)->on_disconnect_actor (
                      *static_cast<TActor *> (actor));
                }
            }
            return task_t<void> (result_t<void>::success ());
        };

        const auto prepared = _state->lane.run ([&] {
            const auto found = _state->spot_contexts_by_id.find (std::string (target_state->spot_id));
            if (found == _state->spot_contexts_by_id.end ()
                || found->second._state.get () != target_state.get ()
                || target_state->node.get () != _state.get () || target_state->closed
                || target_state->close_reservation != 0 || !target_state->spot_instance) {
                return false;
            }
            /* Typed joins hold externally-owned actors: they are indexed
             * for instance-identity surfaces (destroy_actor) but never
             * stored in actor_instances, whose consumers dereference. */
            record_actor_instance_index_unlocked (*_state, actor_ref, std::addressof (actor));
            const auto actor_type = std::type_index (typeid (TActor));
            target_state->on_actor_joined_callbacks[actor_type] = joined_callback;
            target_state->on_create_actor_callbacks[actor_type] = create_callback;
            target_state->on_leave_actor_callbacks[actor_type] = leave_callback;
            target_state->on_disconnect_actor_callbacks[actor_type] = disconnect_callback;
            spot_instance = target_state->spot_instance;
            serializers = target_state->channel_runtime ? target_state->channel_runtime->serializers
                                                        : nullptr;
            node_rid = effective_spot_node_rid (_state->snapshot);
            committed.emplace (::zlink::framework::detail::actor_ref_access_t::make (
              node_rid_t::from_string (node_rid),
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
              std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation () + 1));
            if constexpr (detail::entry_spot_type<TSpot>) {
                create_entry_actor = _state->actor_created_keys.insert (key).second;
            }
            return true;
        }).get ();
        if (!prepared) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "target spot is not registered");
        }

        if constexpr (detail::entry_spot_type<TSpot>) {
            if (create_entry_actor) {
                if (!serializers) {
                    throw framework_exception_t (
                      framework_error_kind_t::protocol_error,
                      "spot create actor requires a serializer registry");
                }
                if (!target_state->run_serial_sync ("spot-lifecycle-create", [&] {
                        create_callback (spot_instance.get (), std::addressof (actor),
                                         create_request, *serializers);
                    })) {
                    throw framework_exception_t (framework_error_kind_t::rejected,
                                                 "spot serial queue is full");
                }
            }
        }
        run_actor_task_callback (
          "spot-lifecycle-join", "spot actor joined callback failed",
          actor_task_callback_projection_t{target_state, spot_instance, joined_callback},
          std::addressof (actor));

        const auto route_committed = _state->lane.run ([&] {
            const auto found = _state->spot_contexts_by_id.find (std::string (target_state->spot_id));
            if (found == _state->spot_contexts_by_id.end ()
                || found->second._state.get () != target_state.get ()
                || target_state->node.get () != _state.get () || target_state->closed
                || target_state->close_reservation != 0
                || target_state->spot_instance.get () != spot_instance.get ()) {
                return false;
            }
            record_actor_context_route_unlocked (*_state, key, node_rid, *target_state,
                                                 actor_ref.object_generation () + 1);
            return true;
        }).get ();
        if (!route_committed) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "target spot is not registered");
        }
        return *committed;
    }

    template <typename TActor> void commit_actor_left (const actor_ref_t &actor_ref, TActor &actor)
    {
        actor_task_callback_projection_t callback;
        const auto key = actor_key (actor_ref);
        _state->lane.run ([&] {
            const auto found_location = _state->actor_spot_ids.find (key);
            if (found_location == _state->actor_spot_ids.end ())
                return;
            auto previous_context = find_context_core (found_location->second);
            _state->actor_spot_ids.erase (found_location);
            _state->actor_routes.erase (key);
            _state->actor_generations.erase (key);
            if (!previous_context)
                return;
            const auto &state = previous_context->_state;
            if (state->actor_count > 0)
                state->actor_count--;
            const auto found =
              state->on_leave_actor_callbacks.find (std::type_index (typeid (TActor)));
            if (found != state->on_leave_actor_callbacks.end () && state->spot_instance) {
                callback.context = state;
                callback.spot_instance = state->spot_instance;
                callback.callback = found->second;
            }
        }).get ();
        run_actor_task_callback ("spot-lifecycle-leave", "spot actor leave callback failed",
                                 callback, std::addressof (actor));
    }

    local_spot_create_result_t
    create_spot_context (std::string spot_name,
                         spot_id_t spot_id,
                         zlink::message_t request,
                         std::uint64_t object_generation = 1,
                         std::string mesh_name = {},
                         std::function<task_t<void> (void *)> staged_restore = {},
                         std::uint64_t authority_owner_generation = 1);
    struct actor_join_state_snapshot_t
    {
        std::optional<spot_context_t> context;
        std::optional<spot_node_builder_state_t::actor_factory_registration_t> registration;
        std::optional<spot_actor_admission_callbacks_t> admission;
        std::shared_ptr<void> actor_instance;
        std::shared_ptr<void> spot_instance;
        serializer_registry_t *serializers = nullptr;
        std::string mesh_name;
        std::string node_rid;
        spot_id_t source_spot_id;
        std::chrono::milliseconds message_follow_duration{0};
        bool has_root_services = false;
    };

    actor_join_state_snapshot_t actor_join_state_snapshot (
      const actor_ref_t &actor_ref,
      spot_id_t spot_id,
      const zlink::message_t &request);
    result_t<spot_actor_admission_callbacks_t>
    actor_admission (spot_context_t &context,
                     std::type_index actor_type,
                     spot_id_t spot_id,
                     const actor_ref_t &actor_ref);
    void commit_accepted_actor_join (const std::string &key,
                                     spot_context_t &context,
                                     const actor_ref_t &committed,
                                     std::type_index actor_type,
                                     void *actor,
                                     const spot_actor_admission_callbacks_t &admission,
                                     bool create_entry_actor,
                                     const zlink::message_t &create_request,
                                     std::string operation_id,
                                     bool &authority_committed);
    task_t<void> replay_actor_handoff_batch (actor_ref_t actor_ref,
                                             std::vector<handoff_packet_t> backlog,
                                             service_provider_t services,
                                             std::string transfer_id,
                                             bool reuse_active_actor_queue,
                                             std::function<bool ()> stop_requested);
    void deliver_actor_join_completion_async (
      actor_ref_t actor_ref,
      actor_join_completion_t completion,
      std::optional<spot_id_t> source_spot_id,
      std::function<void (result_t<void>)> completed,
      std::function<void (result_t<void>, std::function<void (result_t<void>)>)> settle_delivery =
        {});
    void enqueue_actor_handoff_replay (const actor_ref_t &actor_ref,
                                       std::vector<handoff_packet_t> backlog,
                                       service_provider_t &services,
                                       std::string transfer_id);
    // Drains a source-side handoff after a pre-commit failure while the move
    // gate remains closed. Replay is posted before the gate is removed, so a
    // new dispatch cannot overtake the preserved packets.
    void replay_actor_handoff_until_move_closed (const actor_ref_t &actor_ref,
                                                 std::string transfer_id);

    std::shared_ptr<spot_node_builder_state_t> _state;
};

} // namespace zlink::framework::detail
