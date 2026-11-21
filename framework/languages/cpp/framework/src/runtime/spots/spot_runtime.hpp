/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "actor_transfer_coordinator.hpp"
#include "activation_admission.hpp"
#include "runtime/actors/actor_serial_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/diagnostics/dispatch_events.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/operations/exactly_once_table.hpp"
#include "runtime/dispatch/application_record.hpp"
#include "runtime/locations/location_repository_values.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_close_operation.hpp"
#include "runtime/stateful/object_ref.hpp"

#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/locations/resolvers.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>

#include <atomic>
#include <limits>
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

namespace zlink::framework
{
struct authority_key_t;
}

namespace zlink::framework::runtime
{
class application_dispatch_terminal_owner_t;
class location_lifecycle_t;
class runtime_failure_collector_t;
class spot_address_resolver_t;
struct actor_authority_payload_t;
struct serial_application_target_t;

namespace host
{
class actor_handle_t;
class public_host_runtime_t;
class spot_handle_t;
}

namespace stateful
{
class authority_relocation_port_t;
class relocation_store_port_t;
struct frozen_object_state_t;
struct object_inventory_t;
}

namespace protocol
{
struct instance_activation_recovery_t;
}
}

namespace zlink::framework::detail
{

namespace runtime = zlink::framework::runtime;

namespace service = zlink::framework::runtime::host;

class actor_dispatch_admission_token_t;
class actor_gateway_runtime_t;
class channel_runtime_state_t;
class core_timer_drain_loop_t;
class service_scope_t;
class spot_context_state_t;
class spot_node_builder_state_t;

extern thread_local constinit const spot_context_state_t *current_callback_context;

// Records a Close outcome that has no caller, such as an OnClosing failure.
void report_spot_close_diagnostic (const std::shared_ptr<spot_node_builder_state_t> &owner,
                                   std::string_view spot_id,
                                   std::string_view result,
                                   std::exception_ptr error = {}) noexcept;

using instance_spot_idle_eviction_callback_t = std::function<bool (
  const spot_id_t &, std::string_view, std::uint64_t, std::uint64_t, std::function<void ()>)>;
using instance_spot_close_begin_callback_t = std::function<task_t<service::spot_close_commit_t> (
  const spot_id_t &, std::string_view, std::uint64_t, std::uint64_t)>;
using user_spot_close_begin_callback_t = std::function<task_t<service::spot_close_commit_t> (
  const spot_id_t &, std::uint64_t, std::uint64_t)>;
struct instance_spot_retained_message_t
{
    std::shared_ptr<const runtime::protocol::instance_activation_recovery_t> original_activation;
    std::function<task_t<zlink::message_t> ()> resume_missing;
    mutable std::shared_ptr<task_completion_source_t<zlink::message_t>> completion;
};

struct host_lifecycle_snapshot_t
{
    framework_runtime_state_t phase = framework_runtime_state_t::preparing;
    std::optional<std::chrono::system_clock::time_point> shutdown_deadline;
    std::stop_token cleanup_cancellation;
};

// Retain the context until the callback itself completes, even if its caller stops waiting.
task_t<void> invoke_spot_closing (
  std::function<task_t<void> (void *, const spot_closing_context_t &, std::stop_token)> callback,
  void *instance,
  spot_closing_context_t context,
  std::stop_token cleanup_cancellation);

enum class handler_invocation_phase_t
{
    before_admission,
    after_queue_submission
};
using handler_invocation_callback_t = std::function<void (
  handler_invocation_phase_t, std::string_view, std::string_view, std::string_view)>;

class spot_node_builder_state_t
{
  public:
    explicit spot_node_builder_state_t (std::string name);
    ~spot_node_builder_state_t ();
    std::shared_ptr<runtime::runtime_failure_collector_t> runtime_failures () const;

    // Builder configuration is synchronously admitted through this lane.  The
    // MeshNode start path takes its configuration projection only after those
    // turns have completed, so a caller observes registration before the
    // configuring API returns (discovery 9).
    runtime::state_lane_t lane;
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
        std::shared_ptr<task_completion_source_t<void>> completion;
        std::uint64_t reservation = 0;
        // Queue resource only; the existing preparation operation owns application admission.
        std::weak_ptr<spot_context_state_t> context;
    };
    std::map<std::string, pending_spot_creation_t> pending_spot_creations_by_id;
    std::uint64_t next_pending_spot_creation_reservation = 1;
    // MeshNode §5.1: this MeshNode's one activation admission record.
    std::shared_ptr<activation_admission_t> activation_admission =
      std::make_shared<activation_admission_t> ();
    std::weak_ptr<service::public_host_runtime_t> native_node;
    // This node's own RID. The builder fixes the snapshot routing id before start and
    // nothing writes it afterwards, so attach_native_node publishes it once and every
    // later reader (message-flow markers) reads the immutable value without a lane turn.
    node_rid_t node_rid;
    std::function<task_t<spot_create_result_t> (bool,
                                                std::optional<spot_id_t>,
                                                std::string,
                                                std::optional<std::string>,
                                                std::optional<message_t>,
                                                std::chrono::milliseconds)>
      create_user_spot;
    std::function<task_t<std::optional<spot_ref_t>> (spot_id_t)> find_user_spot;
    std::function<task_t<bool> (spot_ref_t, std::chrono::steady_clock::time_point, std::uint64_t)>
      close_user_spot;
    instance_spot_idle_eviction_callback_t admit_instance_spot_idle_eviction;
    instance_spot_close_begin_callback_t begin_instance_spot_close;
    std::function<task_t<result_t<runtime::protocol::instance_spot_activation_header_t>> (
      runtime::protocol::instance_spot_activation_header_t)>
      select_instance_spot_target;
    user_spot_close_begin_callback_t begin_user_spot_close;
    std::shared_ptr<channel_runtime_state_t> channel_runtime;
    dispatch_options_t dispatch;
    // Install before node start; clear after node stop. Only tests wait in this callback.
    handler_invocation_callback_t handler_invocation_for_tests;
    runtime::location_lifecycle_t *location_lifecycle = nullptr;
    std::function<task_t<void> (authority_key_t, authority_snapshot_t, task_t<void>)>
      discarded_authority_cleanup;
    std::shared_ptr<runtime::spot_address_resolver_t> spot_location_resolver;
    std::optional<service_provider_t> root_services;
    std::shared_ptr<std::atomic_bool> drain_flag;
    std::function<host_lifecycle_snapshot_t ()> host_lifecycle;
    /* The host admission gate (Spot address messaging §9): a draining host
     * ends new Spot creation and Actor admission with ShuttingDown before any
     * Spot seal is consulted. The host owns the flag (bind_drain_flag). */
    bool host_draining () const noexcept
    {
        return drain_flag && drain_flag->load (std::memory_order_acquire);
    }
    std::shared_ptr<monitoring_runtime_state_t> monitoring;
    std::chrono::milliseconds instance_spot_idle_timeout{0};
    std::unique_ptr<core_timer_drain_loop_t> instance_spot_idle_timer;
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
        static constexpr std::chrono::seconds default_timeout{30};
        actor_ref_t actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::uint64_t reply_route_id = 0;
        std::optional<service::reply_token_t> reply_token;
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
    struct pending_remote_source_cleanup_t
    {
        actor_ref_t source_actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::string transfer_id;
        spot_id_t source_spot_id;
        std::uint64_t source_spot_generation = 0;
        spot_id_t target_spot_id;
        runtime::protocol::relocation_target_fence_t target_identity;
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
        std::function<actor_t *(void *)> as_actor;
        actor_join_completion_callback_t on_join_completed;
        std::function<task_t<std::vector<std::byte>> (void *, std::stop_token)> capture;
        std::function<task_t<void> (void *, std::vector<std::byte>, std::stop_token)> restore;
        bool duplicate_bind_is_default = false;
        bool session_bound_is_default = false;
    };
    std::function<task_t<result_t<void>> (actor_ref_t)> destroy_actor_registry;
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
    std::map<std::string, actor_factory_registration_t> actor_factories;
    std::map<std::string, factory_relocation_configuration_t> spot_factory_relocations;
    std::map<std::string, std::int32_t> spot_stable_type_limits;
    std::map<std::string, spot_relocation_coordination_mode_t> spot_relocation_coordination_modes;
    std::shared_ptr<actor_transfer_coordinator_t> actor_transfer_coordinator =
      std::make_shared<actor_transfer_coordinator_t> ();
    // Earliest instant the node's own Actor management (handoff request
    // expiry, remote leave and source cleanup) can be due, written on lane.
    // An insertion resets it to management_due_now; pending leaves or
    // cleanups keep it there because they follow transfer events.
    static constexpr std::chrono::steady_clock::rep management_due_now =
      std::numeric_limits<std::chrono::steady_clock::rep>::min ();
    static constexpr std::chrono::steady_clock::rep management_never_due =
      std::numeric_limits<std::chrono::steady_clock::rep>::max ();
    std::atomic<std::chrono::steady_clock::rep> management_next_due{management_due_now};
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
    std::map<std::string, std::unique_ptr<service::actor_handle_t>> native_actors;
    std::unordered_set<std::string> mesh_runtime_owned_native_actor_ids;
    std::map<std::string, std::uint64_t> core_actor_membership_epochs;
    std::map<std::string, std::shared_ptr<service::spot_handle_t>> native_spots_by_id;
    std::shared_ptr<service::spot_handle_t> routed_control_spot;
    std::shared_ptr<const route_client_t> route_client;
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

void drain_spot_node_executors (
  spot_node_builder_state_t &node,
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now ());

void report_logical_multicast_failure (const std::shared_ptr<spot_node_builder_state_t> &state,
                                       std::string_view channel_name,
                                       std::string_view mesh_name,
                                       std::string_view topic,
                                       std::string_view target_rid,
                                       dispatch_error_reason_t reason) noexcept;

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
class spot_serial_executor_t : public std::enable_shared_from_this<spot_serial_executor_t>
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
                            queue_ptr_t spot_queue = {},
                            queue_t::execution_admission_t execution_admission = {});

    struct preparation_t
    {
        struct closed_t
        {
        };
        struct first_record_t
        {
            std::shared_ptr<const void> message;
        };
        using admission_t = std::variant<closed_t, first_record_t>;
        std::shared_ptr<detail::deferred_barrier_t> lifecycle;
        task_completion_source_t<void> terminal;
        std::function<result_t<void> (result_t<void>)> release_aliases;
        // Null means the preparation no longer restricts application starts.
        std::shared_ptr<const admission_t> admission;
    };

    std::shared_ptr<preparation_t> preparation_operation () const
    {
        return std::atomic_load (&_preparation);
    }

    void attach_preparation (const std::shared_ptr<preparation_t> &preparation);

    std::shared_ptr<preparation_t>
    begin_preparation (std::shared_ptr<detail::deferred_barrier_t> lifecycle = {});

    std::shared_ptr<detail::deferred_barrier_t> preparation_lifecycle () const
    {
        auto preparation = std::atomic_load (&_preparation);
        return preparation ? preparation->lifecycle : nullptr;
    }

    void admit_preparation_message (std::shared_ptr<const void> message);

    void close_preparation_admission ();

    void open_preparation_admission ();

    task_t<void> finish_preparation (detail::deferred_barrier_t::async_work_t cleanup = {});

    void execution_operation_changed ()
    {
        assert_on_lane ();
        if (_spot_queue)
            _spot_queue->execution_operation_changed ();
        for (const auto &[_, executor] : _actor_executors)
            executor->queue ()->execution_operation_changed ();
        for (const auto &[_, queue] : _timer_queues)
            queue->execution_operation_changed ();
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
               && _spot_queue->try_post (std::move (name), std::move (work), std::move (options));
    }

    bool execute_lifecycle (std::string name, std::function<void ()> work) const;

    actor_executor_ptr_t actor_executor (const std::string &actor_id);

    actor_executor_ptr_t find_actor_executor (const std::string &actor_id) const
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return find_actor_executor_on_lane (actor_id);
        return _state_lane
          ->run ([this, &actor_id] { return find_actor_executor_on_lane (actor_id); })
          .get ();
    }

    void erase_actor_queue (const std::string &actor_id);

    void replace_actor_queue (std::string actor_id, queue_ptr_t queue);

    std::shared_ptr<const actor_executor_map_t> actor_executor_snapshot () const noexcept
    {
        return std::atomic_load_explicit (&_actor_executor_snapshot, std::memory_order_acquire);
    }

    queue_ptr_t timer_queue (const std::string &timer_name);

    std::vector<queue_ptr_t> timer_queues () const
    {
        if (!_state_lane || _state_lane->is_on_lane ())
            return timer_queues_on_lane ();
        return _state_lane->run ([this] { return timer_queues_on_lane (); }).get ();
    }

    void cancel_timer (const std::string &timer_name);

    bool uses_spot_execution_gate () const noexcept { return _spot_policy.allows_turn_yield (); }

    actor_executor_ptr_t prepare_actor_executor (const std::string &actor_id)
    {
        auto executor = find_actor_executor_snapshot (actor_id);
        return executor ? executor : actor_executor (actor_id);
    }

    bool execute_actor (const std::string &actor_id,
                        std::string name,
                        queue_t::async_work_t work,
                        runtime::serial_work_options_t options = {},
                        bool current_turn = false,
                        std::function<void ()> rejected = {})
    {
        const auto executor =
          current_turn ? actor_executor_ptr_t{} : prepare_actor_executor (actor_id);
        return execute_actor (executor, std::move (name), std::move (work), std::move (options),
                              current_turn, std::move (rejected));
    }

    bool execute_actor (const actor_executor_ptr_t &executor,
                        std::string name,
                        queue_t::async_work_t work,
                        runtime::serial_work_options_t options = {},
                        bool current_turn = false,
                        std::function<void ()> rejected = {})
    {
        if (current_turn && _spot_queue) {
            options.transfer_owner_reservation = {};
            work ([] (std::function<void ()> finish) {
                if (finish)
                    finish ();
            });
            return true;
        }
        if (!executor)
            return false;
        if (!uses_spot_execution_gate ())
            return executor->execute_actor (std::move (name), std::move (work),
                                            std::move (options));
        return executor->execute_actor (
          std::move (name),
          [gate = _spot_queue, work = std::move (work),
           rejected = std::move (rejected)] (auto complete) mutable {
              if (!gate || gate->closed ()) {
                  if (rejected)
                      rejected ();
                  complete ([] {});
                  return;
              }
              work (std::move (complete));
          },
          std::move (options));
    }

    bool execute_actor_lifecycle (const std::string &actor_id,
                                  std::string name,
                                  queue_t::async_work_t work,
                                  std::function<void ()> rejected);

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

    result_t<actor_queue_submission_t>
    execute_actor_cancellable (const std::string &actor_id,
                               std::string name,
                               queue_t::async_work_t work,
                               std::function<void ()> cancel,
                               runtime::serial_work_options_t options = {})
    {
        auto executor = prepare_actor_executor (actor_id);
        if (!executor)
            return result_t<actor_queue_submission_t>::failure (
              framework_error_kind_t::shutting_down, "Actor handoff queue is unavailable");
        if (uses_spot_execution_gate ()) {
            work = [gate = _spot_queue, work = std::move (work)] (auto complete) mutable {
                // Replay is infrastructure that can deliver new Spot records. Its Actor
                // claim stays active, while those records need the shared Spot gate.
                if (auto turn = detail::capture_current_serial_turn ();
                    turn && turn->belongs_to (gate.get ()))
                    (void) turn->release ();
                work (std::move (complete));
            };
        }
        const auto submitted = executor->execute_actor (std::move (name), std::move (work),
                                                        std::move (cancel), std::move (options));
        if (!submitted)
            return result_t<actor_queue_submission_t>::failure (
              submitted.error_kind (), submitted.error () != nullptr
                                         ? submitted.error ()->what ()
                                         : "Actor handoff queue is closed");
        return result_t<actor_queue_submission_t>::success (
          actor_queue_submission_t{executor->queue (), submitted.value ()});
    }

    bool execute_timer (const std::string &timer_name,
                        std::string name,
                        queue_t::async_work_t work,
                        runtime::serial_work_options_t options = {})
    {
        if (uses_spot_execution_gate ()) {
            options.transfer_owner_reservation = {};
            return _spot_queue
                   && _spot_queue->try_post_async (std::move (name), std::move (work),
                                                   std::move (options));
        }
        const auto queue = timer_queue (timer_name);
        return queue
               && queue->try_post_async (std::move (name), std::move (work), std::move (options));
    }

    void close (std::vector<task_t<void>> *completions = nullptr,
                queue_t::execution_status_t status = queue_t::execution_status_t::stopped);

  private:
    static queue_t::application_start_policy_t
    preparation_policy (const std::shared_ptr<preparation_t> &preparation);

    void update_preparation_policy (std::shared_ptr<preparation_t> preparation);

    void assert_on_lane () const noexcept
    {
        if (_state_lane)
            assert (_state_lane->is_on_lane ());
    }

    actor_executor_ptr_t actor_executor_on_lane (const std::string &actor_id);

    actor_executor_ptr_t find_actor_executor_snapshot (const std::string &actor_id) const noexcept
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

    void replace_actor_queue_on_lane (std::string actor_id, queue_ptr_t queue);

    queue_ptr_t timer_queue_on_lane (const std::string &timer_name);

    std::vector<queue_ptr_t> timer_queues_on_lane () const;

    void cancel_timer_on_lane (const std::string &timer_name);

    std::vector<queue_ptr_t> close_on_lane ();

  private:
    void publish_actor_executor_snapshot ();

    std::shared_ptr<runtime::offload_executor_t> _worker_executor;
    runtime::state_lane_t *_state_lane = nullptr;
    runtime::serial_lane_policy_t _spot_policy;
    queue_ptr_t _spot_queue;
    queue_t::execution_admission_t _execution_admission;
    std::shared_ptr<preparation_t> _preparation;
    actor_executor_map_t _actor_executors;
    std::map<std::string, queue_ptr_t> _timer_queues;
    std::shared_ptr<const actor_executor_map_t> _actor_executor_snapshot =
      std::make_shared<actor_executor_map_t> ();
};

class spot_context_state_t : public std::enable_shared_from_this<spot_context_state_t>
{
    friend class ::zlink::framework::spot_context_t;
    friend class ::zlink::framework::spot_handler_registry_t;

  private:
    void settle_admitted_callbacks_core ();

    template <typename Work> decltype (auto) state_sync (Work &&work) const
    {
        auto owner = state_lane_owner ();
        if (!owner || owner->lane.is_on_lane ())
            return std::invoke (work);
        return owner->lane
          .run ([work = std::forward<Work> (work)] () mutable -> decltype (auto) {
              return std::invoke (work);
          })
          .get ();
    }

  public:
    task_t<void> admitted_callbacks_terminal ();

    task_t<void> complete_preparation (bool ready);
    task_t<void> discard_preparation (std::shared_ptr<service::spot_handle_t> native,
                                      std::function<void ()> release_admitted_work = {});
    void detach_application_instance (
      bool notify_closing,
      spot_close_reason_t close_reason = spot_close_reason_t::explicit_close,
      std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::time_point::max (),
      std::stop_token cleanup_cancellation = {});

    /* Close steps 1-4 (Spot address messaging §7) for this activation. The
     * caller runs it on the Spot lifecycle lane. `begin` is step 1 of the
     * Spot's authority kind; without it the Spot's own authority step is used
     * and a node-local Spot has none. While a Location Store step runs the
     * Close item returns its turn and `resume` returns the continuation to it
     * through the gate (gate §7). `done` receives the Close result once. */
    void close_now (service::spot_close_begin_t begin,
                    service::spot_close_done_t done,
                    detail::task_scheduler_t resume,
                    spot_close_reason_t reason,
                    std::shared_ptr<spot_serial_executor_t::preparation_t> operation = {},
                    std::chrono::system_clock::time_point deadline =
                      std::chrono::system_clock::time_point::max (),
                    std::stop_token cleanup_cancellation = {});

    /* Runs a Close on this Spot's lifecycle lane after the lifecycle work
     * accepted before it (Spot address messaging §7). */
    void request_close (service::spot_close_begin_t begin,
                        service::spot_close_done_t done,
                        spot_close_reason_t reason = spot_close_reason_t::explicit_close,
                        std::chrono::system_clock::time_point deadline =
                          std::chrono::system_clock::time_point::max (),
                        std::stop_token cleanup_cancellation = {});

    /* Step 1 of a Close that only releases the local activation: a node-local
     * Spot, or operational cleanup whose authority another owner holds. */
    static service::spot_close_begin_t local_close_step ();

    /* 요청 수락은 owner lane에서 한 번 판정한다. Explicit Close는 intent
     * record를 application FIFO에 보존하며, 닫힌 incarnation은 나머지
     * 메시지를 거부한다. Idle과 운영 seal의 수락 계약은 그대로 유지한다.
     * claim은 수락한 작업의 terminal에서 leave_callback이 해제한다. */
    bool admit (bool claim = false,
                const instance_spot_retained_message_t *retained_record = nullptr);
    framework_exception_t admission_error () const;
    static framework_exception_t target_not_found (std::string message);
    // Admits a timer callback claim; payload is selected in its execution turn.
    bool admit_timer_fire (const std::shared_ptr<runtime::serial_execution_queue_t> &queue = {});
    // Counts a lifecycle callback that its lane admitted earlier.
    void enter_callback ();
    void leave_callback (std::function<void ()> settle_owner_state = {}) noexcept;
    bool is_current_callback_thread () const;
    /* Actor membership changes only on the owner lane of a current, unclosed
     * activation that no Close or idle cleanup has reserved: Close step 1
     * decides on the membership it observed (Spot address messaging §7). */
    bool accepts_membership_core (const spot_node_builder_state_t &owner) const noexcept
    {
        return node.get () == &owner && !closed && close_reservation == 0 && spot_instance;
    }

    bool idle_quiescent () const;

    bool try_post_serial (std::string name,
                          std::function<void ()> work,
                          runtime::serial_work_options_t options = {});
    bool try_post_serial_after_current_turn (std::string name,
                                             std::function<void ()> work,
                                             runtime::serial_work_options_t options = {});
    bool try_post_serial_task_after_current_turn (std::string name,
                                                  std::function<task_t<void> ()> work,
                                                  runtime::serial_work_options_t options = {});
    // Queues work its caller already admitted (admit with a claim).
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
      std::size_t transferred_owner_byte_cost = 0,
      std::shared_ptr<serial_turn_t> accepted_turn = {});
    /* The lifecycle item completes the task. A caller that holds this Spot's turn
     * runs the work inline, so its task is already complete. */
    task_t<void> run_serial_task (std::string name, std::function<task_t<void> ()> work);
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
    std::shared_ptr<deferred_barrier_t> relocation_ready_barrier;
    // The serial turn that registered the deferral; only that turn rejects
    // later Framework operations (Spot model §5.1).
    std::weak_ptr<serial_turn_t> relocation_ready_turn;
    std::vector<spot_packet_descriptor_t> packets;
    std::vector<spot_handler_descriptor_t> handlers;
    std::vector<spot_handler_registry_t::invoker_t> handler_invokers;
    std::map<std::type_index, spot_actor_admission_callbacks_t> actor_admissions;
    std::weak_ptr<service::spot_handle_t> native_spot;
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
    // Idle 정리와 운영 teardown이 이 admission seal을 소유한다.
    // Explicit Close는 lifecycle FIFO 작업으로 application 실행 순서를 정한다.
    bool admission_sealed = false;
    // 확정된 local Close의 사유가 기존 closed 상태를 함께 나타냅니다.
    std::optional<spot_close_reason_t> closed;
    std::size_t actor_count = 0;
    // Node-lane ownership claim spanning an external close/store callback.
    // Actor membership publication is refused while it is non-zero
    // (accepts_membership_core); message admission is not (admit).
    std::uint64_t close_reservation = 0;
    std::uint64_t next_close_reservation = 1;
    // What holds close_reservation: idle cleanup, a Spot Close (§7), or
    // operational teardown that detaches the application instance.
    enum class close_reservation_kind_t
    {
        none,
        idle,
        explicit_close,
        teardown
    };
    close_reservation_kind_t close_reservation_kind = close_reservation_kind_t::none;
    const spot_context_t *close_registered_context = nullptr;
    std::atomic<std::int64_t> last_application_work_completed_ns{0};
    // The node state lane owns lifecycle admission and close/eviction state.
    // A token acquired in a node turn can therefore retain this depth through
    // queue wait, Yield, and handler terminal without a second owner claim.
    std::size_t callback_depth = 0;
    // Preparation cleanup and node drain observe the same admitted-callback count.
    // Only the incarnation state lane registers and consumes this notification.
    std::shared_ptr<task_completion_source_t<void>> admitted_callback_terminal;
    // Close requests that merged into the running Close receive its result.
    std::vector<service::spot_close_done_t> merged_close_results;

    bool has_active_callback () const;

    std::shared_ptr<spot_serial_executor_t> ensure_spot_serial_executor ();

  private:
    // The seal check behind admit and admit_timer_fire; runs on the owner lane.
    bool admit_core (bool claim,
                     const instance_spot_retained_message_t *retained_record = nullptr) noexcept;

    std::shared_ptr<spot_serial_executor_t>
    ensure_spot_serial_executor_on_lane (runtime::state_lane_t &state_lane);

  public:
    // A Close request made while a Close or teardown holds the reservation
    // receives that Close's result. Runs on the node state lane.
    bool merge_into_running_close_core (const service::spot_close_done_t &done);

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

    std::uint64_t begin_idle_close_reservation ();

    void cancel_idle_close_reservation (std::uint64_t token) noexcept;

    /* Idle cleanup re-checks the sealed activation's serial quiescence and age
     * before any authority write (Object lifecycle §5). After this returns
     * true nothing can enter the activation, so committing the idle Close
     * cannot be refused once the authority is Closing. */
    bool idle_close_ready (std::uint64_t reservation);

    /* Local steps of an idle Close whose authority is Closing. */
    void commit_idle_close (std::uint64_t reservation);

  private:
    enum class close_path_t
    {
        not_executed, // not the current activation or has Actor membership: `false`
        start,        // steps 1-4
        failed        // a prior Close sealed this activation but failed
    };

    struct close_start_t
    {
        close_path_t path = close_path_t::not_executed;
        std::uint64_t token = 0;
        const char *kind = nullptr;
        service::spot_close_begin_t authority_begin;
    };

    /* Steps 2-4 after step 1 committed `Closing`. */
    void run_local_close_steps (
      const std::shared_ptr<spot_node_builder_state_t> &owner,
      std::uint64_t token,
      std::function<task_t<bool> ()> release,
      std::function<task_t<authority_snapshot_t> ()> reincarnate,
      std::function<task_t<bool> (const authority_snapshot_t &)> discard_reincarnation,
      service::spot_close_done_t done,
      detail::task_scheduler_t resume,
      spot_close_reason_t reason,
      std::shared_ptr<spot_serial_executor_t::preparation_t> operation,
      std::chrono::system_clock::time_point deadline,
      std::stop_token cleanup_cancellation);

    struct application_detach_work_t
    {
        std::shared_ptr<void> instance;
        std::function<task_t<void> (void *, const spot_closing_context_t &, std::stop_token)>
          on_closing;
    };

    std::shared_ptr<spot_node_builder_state_t> state_lane_owner () const
    {
        if (auto owner = lane_owner.lock ())
            return owner;
        // Tests and contexts created before lane_owner wiring set node directly.
        return node;
    }

    std::uint64_t reserve_close_core (spot_node_builder_state_t &owner,
                                      close_reservation_kind_t kind);

    void clear_close_reservation_core (std::uint64_t token) noexcept;

    bool idle_age_allows_close_core (const spot_node_builder_state_t &owner) const;

    void
    detach_application_instance_reserved (const std::shared_ptr<spot_node_builder_state_t> &owner,
                                          std::uint64_t token,
                                          bool notify_closing,
                                          spot_close_reason_t close_reason,
                                          std::chrono::system_clock::time_point deadline,
                                          std::stop_token cleanup_cancellation);

    void close_application_then_release_location (
      const std::shared_ptr<spot_node_builder_state_t> &owner,
      spot_close_reason_t reason,
      std::uint64_t token,
      std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::time_point::max (),
      std::stop_token cleanup_cancellation = {});
};

class spot_context_access_t final
{
  public:
    static std::shared_ptr<spot_context_state_t> state (const spot_context_t &context)
    {
        return context._state;
    }
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
    friend struct spot_context_state_t;

  public:
    struct spot_relocation_unit_t
    {
        spot_id_t spot_id;
        std::string spot_type;
        std::vector<actor_ref_t> actors;
    };
    struct relocation_units_t
    {
        std::vector<spot_relocation_unit_t> spots;
        std::vector<actor_ref_t> standalone_actors;
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
    local_spot_create_result_t
    get_or_create_spot (std::string spot_name,
                        spot_id_t spot_id,
                        zlink::message_t request,
                        std::uint64_t object_generation = 1,
                        std::string mesh_name = {},
                        std::uint64_t authority_owner_generation = 1,
                        std::function<task_t<void> (bool)> *preparation_terminal = nullptr);
    task_t<std::shared_ptr<local_spot_create_result_t>>
    get_or_create_spot_async (std::string spot_name,
                              spot_id_t spot_id,
                              zlink::message_t request,
                              std::uint64_t object_generation = 1,
                              std::string mesh_name = {},
                              std::uint64_t authority_owner_generation = 1,
                              std::function<task_t<void> (bool)> *preparation_terminal = nullptr);
    task_t<zlink::message_t> dispatch_instance_activation (
      const spot_id_t &spot_id,
      std::string packet_name,
      std::string content_type,
      std::vector<std::uint8_t> payload,
      std::map<std::string, std::string> metadata,
      bool request,
      std::string correlation_id,
      service_provider_t &services,
      serializer_registry_t &serializers,
      std::function<void ()> accept_activation,
      std::optional<std::string> flow_id = std::nullopt,
      std::optional<flow_origin_t> flow_origin = std::nullopt,
      std::function<void ()> *accepted_turn_terminal = nullptr,
      std::shared_ptr<const instance_spot_retained_message_t> retained_message = {});
    std::optional<spot_info_t> find_spot (spot_id_t spot_id) const;
    bool matches_instance_activation (const std::string &spot_id,
                                      const std::string &stable_type,
                                      const std::string &mesh_name,
                                      std::uint64_t object_generation,
                                      std::uint64_t authority_owner_generation) const;
    std::vector<spot_info_t> list_spots () const;
    task_t<bool> close_spot (spot_id_t spot_id,
                             spot_close_reason_t reason = spot_close_reason_t::explicit_close,
                             std::chrono::system_clock::time_point deadline =
                               std::chrono::system_clock::time_point::max (),
                             std::stop_token cleanup_cancellation = {});
    // Runs a remote User Spot Close (command 48) for the local owner activation.
    void close_user_spot_owner (const std::string &spot_id,
                                std::uint64_t object_generation,
                                std::chrono::system_clock::time_point deadline,
                                service::spot_close_begin_t begin,
                                service::spot_close_done_t done);
    bool close_all_spots (std::chrono::steady_clock::time_point deadline_at,
                          std::chrono::system_clock::time_point closing_deadline,
                          std::stop_source &cleanup_cancellation);
    node_rid_t node_rid () const;
    std::optional<std::string> spot_name_for (spot_id_t spot_id) const;
    std::optional<spot_route_t> resolve_spot (spot_id_t spot_id) const;
    std::optional<spot_id_t> actor_spot (const actor_ref_t &actor_ref) const;
    task_t<result_t<bool>>
    run_actor_turn (actor_ref_t actor_ref,
                    std::function<task_t<result_t<bool>> (actor_t &, std::stop_token)> work,
                    std::stop_token cancellation = {});
    task_t<result_t<bool>> destroy_actor (actor_ref_t actor_ref);
    task_t<result_t<bool>> release_local_actor_async (actor_ref_t actor);
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
    void cancel_dispatch_waits () noexcept;
    void cancel_pending_dispatch () noexcept;
    void cancel_pending_work (
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now ()) noexcept;

    /* Releases the native SPOT/actor handles this node created on its own
     * zlink context. The host service must call this before terminating the
     * context: a live socket keeps `zlink_ctx_term()` waiting forever. */
    void release_native_handles () noexcept;
    std::optional<actor_ref_t> current_actor_ref (const actor_ref_t &actor_ref) const;
    void attach_native_node (std::shared_ptr<service::public_host_runtime_t> node);
    void detach_native_node ();
    void evict_idle_spots () noexcept;
    void record_core_actor_transfer_activation (std::string actor_id,
                                                std::uint64_t membership_epoch);
    void bind_location_lifecycle (runtime::location_lifecycle_t &lifecycle);
    void resume_location_execution ();
    task_t<void> request_location_execution_discard (
      runtime::serial_execution_queue_t::execution_admission_t allowed);
    void observe_location_authority (authority_key_t key,
                                     authority_snapshot_t snapshot,
                                     std::string expected_owner);
    task_t<result_t<bool>>
    discard_local_actor_async (actor_ref_t actor,
                               runtime::serial_execution_queue_t::execution_status_t status =
                                 runtime::serial_execution_queue_t::execution_status_t::stopped);
    task_t<runtime::serial_execution_queue_t::ready_confirmation_t>
    prepare_store_ready (runtime::stateful::object_ref_t object);
    void bind_spot_location_resolver (std::shared_ptr<runtime::spot_address_resolver_t> resolver);
    void bind_service_provider (service_provider_t &services);
    void bind_drain_flag (std::shared_ptr<std::atomic_bool> flag);
    void bind_host_lifecycle (std::function<host_lifecycle_snapshot_t ()> read_lifecycle);
    /* Entry spots are host infrastructure and are excluded. */
    std::size_t active_user_spot_count () const;
    task_t<std::size_t> active_user_spot_count_async () const;
    task_t<std::pair<std::size_t, std::size_t>> monitoring_counts_async () const;
    /* Actors still joined to this node's spots, for the drain handoff pass
     * (graceful-drain-handoff §5.2). */
    std::vector<actor_ref_t> local_actor_refs () const;
    task_t<bool> relocation_ready_async () const;
    task_t<relocation_units_t> relocation_units () const;
    void begin_relocation_readiness ();
    task_t<void> begin_relocation_readiness_async ();
    void end_relocation_readiness (const std::vector<spot_id_t> &relocated_spots);
    /* Domain snapshot for the drain handoff join — the same shape the erased
     * cross-node leave path sends alongside the entry-spot join. */
    std::optional<zlink::message_t> serialize_actor_snapshot (const actor_ref_t &actor_ref) const;
    std::shared_ptr<service::public_host_runtime_t> native_node () const;
    task_t<std::shared_ptr<service::public_host_runtime_t>> native_node_async () const;
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
    task_t<std::optional<std::uint64_t>>
    resolve_spot_generation (const zlink::routing_id_t &target_node_rid,
                             const spot_id_t &target_spot_id) const;
    // Canonical actorJoin(28) reaches this boundary without a stable type.
    // Read the target's Store Authority row here (not a cacheable routing
    // projection), alongside the Actor Authority fence/type resolution in
    // admit_remote_actor_to_spot. The transport deliberately does not own
    // these authority decisions.
    result_t<std::uint64_t>
    resolve_wire_actor_join_target (const runtime::protocol::spot_route_fence_t &fence,
                                    bool entry = false) const;
    std::vector<spot_context_t> active_contexts () const;
    // Empty success means no handler matched; an admitted handler owns the returned task.
    result_t<std::optional<task_t<zlink::message_t>>>
    dispatch_subscription (const spot_context_t &context,
                           std::string topic,
                           const std::vector<zlink::message_t> &parts,
                           service_provider_t &services,
                           serializer_registry_t &serializers,
                           runtime::handler_entry_t before_application_handler = {},
                           std::function<void ()> transfer_owner_reservation = {},
                           std::size_t transferred_owner_byte_cost = 0,
                           const service::receive_record_t *multicast = nullptr) const;
    bool dispatch_mesh_record (
      const service::ready_record_t &owner,
      const service::receive_record_t &record,
      std::vector<zlink::message_t> &parts,
      const std::optional<runtime::messaging::envelope_header_t> &decoded_header,
      service_provider_t &services,
      serializer_registry_t &serializers,
      std::function<void ()> deferred_terminal = {},
      bool *terminal_deferred = nullptr,
      runtime::handler_entry_t before_application_handler = {},
      runtime::serial_execution_queue_t::async_completion_t application_completed = {},
      runtime::application_dispatch_terminal_owner_t *local_terminal = nullptr);
    void set_route_client (route_client_t route_client);
    void on_destroy_actor (std::function<task_t<result_t<void>> (actor_ref_t)> destroy_actor);
    void on_actor_ref_updated (std::function<result_t<void> (const actor_ref_t &)> update_actor);
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
    task_t<actor_join_reply_t> join_actor_to_spot_erased (
      const actor_ref_t &actor_ref,
      spot_id_t spot_id,
      const zlink::message_t &request,
      const std::optional<zlink::message_t> &actor_snapshot = std::nullopt,
      actor_context_t actor_context = {},
      std::uint64_t completion_operation_id_high = 0,
      std::uint64_t completion_operation_id_low = 0,
      std::function<void (std::function<void (result_t<void>)>)> *source_leave = nullptr,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
    task_t<actor_join_reply_t> join_remote_actor_to_spot_erased (
      const actor_ref_t &actor_ref,
      spot_id_t spot_id,
      const zlink::message_t &request,
      actor_context_t actor_context = {},
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
    // Completes when the target Spot's admission lifecycle item completes.
    task_t<spot_actor_join_result_t>
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
    void dispatch_wire_actor_join_admission (const spot_id_t &target_spot_id,
                                             std::string_view actor_id,
                                             std::function<task_t<void> ()> admission,
                                             std::function<void (result_t<void>)> rejected,
                                             std::function<void ()> completed = {});
    std::optional<bool> validate_actor_join_relocation_prepare (
      const runtime::protocol::relocation_prepare_t &prepare) const;
    task_t<bool>
    consume_actor_join_recovery_async (runtime::stateful::frozen_object_state_t &frozen,
                                       runtime::stateful::object_ref_t target,
                                       runtime::protocol::relocation_prepare_t prepare);
    /* Undo a consume_actor_join_recovery_async that a later staging step rejected.
     * Keyed on the wire stable type rather than the local actor-type index:
     * an Actor that has never been created on this node has no index entry
     * yet, which is exactly the case that leaks (15 §4.2). */
    task_t<void> discard_actor_join_recovery_async (std::string stable_type,
                                                    runtime::stateful::object_ref_t target);
    task_t<void>
    abort_relocation_materialization_async (std::vector<runtime::stateful::object_ref_t> targets);
    std::optional<runtime::actor_authority_payload_t>
    actor_relocation_authority (const runtime::stateful::object_ref_t &target) const;
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
    task_t<result_t<void>> commit_remote_actor_authority (
      std::string transfer_id,
      actor_ref_t actor_ref,
      spot_id_t target_spot_id,
      std::uint64_t target_spot_generation,
      std::uint64_t source_authority_owner_generation,
      std::string source_mesh_name,
      std::string target_mesh_name,
      std::uint64_t target_node_lifecycle_generation,
      location_owner_token_t target_owner,
      std::string expected_source_store_version,
      std::uint64_t *committed_previous_authority_owner_generation = nullptr,
      std::uint64_t *committed_target_authority_owner_generation = nullptr);
    result_t<void>
    stage_remote_actor_commit_backlog (const std::string &transfer_id,
                                       std::vector<handoff_packet_t> handoff_backlog);
    std::optional<actor_join_reply_t>
    completed_remote_actor_commit (const std::string &transfer_id,
                                   const actor_ref_t &source_actor,
                                   const spot_id_t &target_spot_id) const;
    task_t<actor_join_reply_t>
    commit_remote_actor_to_spot (std::string transfer_id,
                                 const actor_ref_t &actor_ref,
                                 spot_id_t target_spot_id,
                                 zlink::message_t transfer_state,
                                 actor_context_t actor_context = {},
                                 std::vector<handoff_packet_t> handoff_backlog = {},
                                 service_provider_t *services = nullptr);
    task_t<actor_join_reply_t> finalize_remote_actor_to_spot (
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
    task_t<std::optional<std::chrono::steady_clock::time_point>>
    next_management_activity_async () const;
    task_t<std::optional<std::chrono::steady_clock::time_point>>
    advance_management_async (bool include_next_activity = true);
    std::size_t cleanup_expired_actor_admissions ();
    std::size_t
    cleanup_expired_actor_admissions_at (std::chrono::steady_clock::time_point now,
                                         std::function<void ()> after_coordinator_snapshot = {});
    task_t<std::size_t> cleanup_expired_actor_admissions_at_async (
      std::chrono::steady_clock::time_point now,
      std::function<void ()> after_coordinator_snapshot = {});
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
    std::string next_actor_transfer_id ();
    // Transfer ID stamped on a still-reserved deferred join, so the remote
    // join reuses the correlation already carried by preserved handoff
    // markers instead of allocating a second ID.
    std::optional<std::string> reserved_actor_transfer_id (const actor_ref_t &actor_ref) const;
    result_t<std::shared_ptr<deferred_barrier_t>>
    reserve_actor_join_barrier (const actor_ref_t &actor_ref);
    std::pair<std::uint64_t, std::uint64_t>
    actor_join_operation_id (std::string_view transfer_id) const;
    task_t<result_t<void>>
    deliver_actor_join_completion (const actor_ref_t &actor_ref,
                                   const actor_join_completion_t &completion,
                                   std::optional<spot_id_t> source_spot_id = std::nullopt);
    task_t<std::shared_ptr<runtime::serial_execution_queue_t>>
    actor_application_queue (std::string_view actor_id) const;
    task_t<bool> notify_actor_application_ready (
      std::string actor_id, std::function<std::optional<runtime::serial_prefix_work_t> ()> take);
    task_t<void> accept_actor_application_ready (
      std::string actor_id,
      std::shared_ptr<runtime::serial_execution_queue_t> queue,
      std::function<std::optional<runtime::serial_prefix_work_t> ()> take);
    task_t<void> seal_actor_application_suffix (const actor_ref_t &actor_ref,
                                                std::function<void ()> seal);
    task_t<std::vector<std::shared_ptr<runtime::serial_application_record_t>>>
    take_actor_application_suffix (const actor_ref_t &actor_ref,
                                   std::function<void ()> seal = {}) const;
    task_t<result_t<void>> open_local_actor_handoff (actor_ref_t actor_ref,
                                                     std::string expected_source_transfer = {});
    void cancel_local_actor_handoff (const actor_ref_t &actor_ref);
    // In-flight handoff (spot-actor §10): drains the packets preserved while the
    // actor was moving, in arrival order. The commit path calls this once to fill
    // the commit request and once more after the ack for packets that raced it.
    std::vector<handoff_packet_t> take_actor_handoff_backlog (const actor_ref_t &actor_ref);
    bool actor_transfer_in_progress (const actor_ref_t &actor_ref) const;
    task_t<bool> actor_transfer_in_progress_async (actor_ref_t actor) const;
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
    // Actor model §6.3: whether this node's Entry Spot accepts the Actor registered under
    // `stable_type`, which decides whether the node creates new Actors of that type.
    task_t<bool> entry_accepts_actor (std::string stable_type) const;
    // actor_context_t's default constructor is private (friended to
    // spot_node_runtime_t among a few others) -- this exposes it to callers
    // outside spots/** that need a default/unbound context, e.g. the
    // actorJoin(28) receiver path, which does not run bind_actor_route.
    static actor_context_t default_actor_context ();
    void set_message_follow_duration (std::chrono::milliseconds duration);
    void bind_relocation_store (std::shared_ptr<runtime::stateful::relocation_store_port_t> store);
    void bind_relocation_authority (
      std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority);
    task_t<runtime::stateful::frozen_object_state_t>
    capture_relocation_state (const runtime::stateful::object_ref_t &object,
                              const std::string &stable_type,
                              std::stop_token cancellation) const;
    // The execution queue that owns the Spot's application turns; null if not materialized.
    task_t<std::shared_ptr<runtime::serial_execution_queue_t>>
    spot_execution_queue (std::string spot_key) const;
    task_t<std::shared_ptr<runtime::serial_application_target_t>>
    prepare_spot_dispatch (std::string spot_key,
                           std::optional<runtime::protocol::spot_route_fence_t> route) const;
    task_t<bool> restore_spot_relocation_state (runtime::stateful::frozen_object_state_t frozen,
                                                runtime::stateful::object_ref_t target,
                                                std::stop_token cancellation = {});
    // Completes when the application lifecycle steps of the materialization do.
    task_t<bool>
    materialize_relocation_state (const runtime::stateful::frozen_object_state_t &frozen,
                                  const runtime::stateful::object_ref_t &target,
                                  const std::optional<runtime::stateful::object_ref_t> &target_spot,
                                  std::stop_token cancellation = {});
    bool
    commit_relocation_materialization (const std::vector<runtime::stateful::object_ref_t> &targets);
    task_t<void>
    retire_relocation_source (const std::vector<runtime::stateful::object_ref_t> &sources);
    task_t<result_t<remote_actor_transfer_t>> transfer_actor_out (const actor_ref_t &actor_ref,
                                                                  std::string transfer_id = {},
                                                                  bool capture_state = true);
    task_t<result_t<void>>
    submit_remote_actor_leave_async (std::string transfer_id,
                                     actor_ref_t source_actor,
                                     spot_id_t source_spot_id,
                                     std::uint64_t source_spot_generation,
                                     spot_id_t target_spot_id,
                                     runtime::protocol::actor_route_fence_t target_fence,
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
    task_t<void> fast_fail_reconcile_backlog (
      actor_ref_t actor_ref,
      std::string actor_key,
      std::optional<runtime::protocol::actor_route_fence_t> source_fence = std::nullopt);
    task_t<void>
    complete_remote_actor_transfer (const actor_ref_t &source_actor,
                                    const actor_ref_t &target_actor,
                                    spot_route_t target_route,
                                    runtime::protocol::actor_route_fence_t source_fence,
                                    runtime::protocol::relocation_target_fence_t target_identity,
                                    std::optional<std::uint64_t> follow_authority_generation,
                                    std::string transfer_id = {});
    // Emits an internal transfer lifecycle boundary through the configured
    // public message-flow observer. The transfer id is both the correlation
    // key and lifecycle flow key so role-server evidence can join source and
    // target events without parsing runtime logs. The source RID is the
    // node's own immutable RID, so the marker hands one event to the observer
    // and never waits for a lane turn (spec 26 §4). Hot-path callers
    // that build marker/transfer-id strings must gate on
    // actor_transfer_marker_enabled() so an off/errors mode pays no allocation.
    bool actor_transfer_marker_enabled (
      message_flow_result_t result = message_flow_result_t::succeeded) const noexcept;
    void emit_actor_transfer_marker (std::string marker,
                                     const actor_ref_t &actor_ref,
                                     std::string transfer_id,
                                     std::optional<spot_id_t> spot_id = std::nullopt,
                                     std::optional<node_rid_t> target_node_rid = std::nullopt,
                                     std::exception_ptr exception = nullptr) const;
    task_t<actor_join_reply_t> join_actor_to_entry_spot_erased (
      const actor_ref_t &actor_ref,
      node_rid_t spot_node_rid,
      const zlink::message_t &request,
      const std::optional<zlink::message_t> &actor_snapshot,
      actor_context_t actor_context,
      std::function<void (std::function<void (result_t<void>)>)> *source_leave = nullptr,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
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
      runtime::handler_entry_t before_application_handler = {},
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
      std::shared_ptr<actor_dispatch_admission_token_t> admission_token = {},
      std::shared_ptr<runtime::serial_application_record_t> application_record = {},
      spot_handler_registry_t::actor_queue_dispatch_t actor_queue_dispatch =
        spot_handler_registry_t::actor_queue_dispatch_t::acquire);
    task_t<zlink::message_t> notify_actor_disconnected_erased (void *instance,
                                                               std::type_index actor_type,
                                                               actor_gateway_runtime_t gateway,
                                                               zlink::message_t body) const;

    template <typename TActor>
    std::optional<std::reference_wrapper<TActor>> actor_instance (const actor_ref_t &actor_ref)
    {
        const auto key = actor_key (actor_ref);
        auto instance = _state->lane
                          .run ([&] {
                              const auto found = _state->actor_instances.find (key);
                              return found == _state->actor_instances.end ()
                                       ? std::shared_ptr<void>{}
                                       : found->second;
                          })
                          .get ();
        if (!instance) {
            return std::nullopt;
        }
        return *static_cast<TActor *> (instance.get ());
    }

  private:
    static task_t<std::optional<std::chrono::steady_clock::time_point>>
    next_management_activity_on_lane_async (
      std::shared_ptr<spot_node_builder_state_t> state,
      std::optional<std::chrono::steady_clock::time_point> next);
    friend struct spot_node_runtime_test_access_t;
    std::shared_ptr<runtime::serial_execution_queue_t>
    execution_queue_on_lane (const runtime::stateful::object_ref_t &object);
    result_t<bool> validate_local_actor_release_on_lane (const actor_ref_t &actor_ref) const;
    result_t<bool> release_local_actor_on_lane (const actor_ref_t &actor_ref);
    static task_t<void>
    retire_relocation_source_in (spot_node_runtime_t self,
                                 std::vector<runtime::stateful::object_ref_t> sources);
    static task_t<void> fast_fail_reconcile_backlog_core (
      std::shared_ptr<spot_node_builder_state_t> state,
      actor_ref_t actor_ref,
      std::string actor_key,
      std::optional<runtime::protocol::actor_route_fence_t> source_fence);
    task_t<std::vector<runtime::stateful::object_inventory_t>> local_actor_inventory_async () const;
    static void filter_local_actor_inventory_on_lane (
      const spot_node_builder_state_t &state,
      std::vector<runtime::stateful::object_inventory_t> &inventory);
    std::size_t active_user_spot_count_on_lane () const;
    std::optional<std::string> spot_name_for_unlocked (const spot_id_t &spot_id) const;

    static std::shared_ptr<service::spot_handle_t>
    attach_native_spot (const std::shared_ptr<spot_context_state_t> &state,
                        bool relocation_restore = false,
                        bool publish = true);

    task_t<std::size_t> settle_abandoned_actor_leave_reservations_async (
      std::function<void ()> after_coordinator_snapshot = {});

    static std::string actor_key (const actor_ref_t &actor_ref)
    {
        return actor_key (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref),
                          actor_ref.actor_id ().value ());
    }

    static std::string actor_key (std::string_view actor_type, std::string_view actor_id)
    {
        return std::string (actor_type) + ":" + std::string (actor_id);
    }

    /* `refuse_destroyed` re-checks the destroy tombstone in the same lane turn
     * that installs, so a destroy landing inside the factory window cannot be
     * resurrected by a late install; the caller then reports the actor as
     * destroyed. Join paths pass false: a join legitimately recreates a
     * destroyed actor in a new generation and clears the tombstone on commit. */
    std::shared_ptr<void> install_actor_instance (const actor_ref_t &actor_ref,
                                                  const std::string &key,
                                                  std::shared_ptr<void> instance,
                                                  bool refuse_destroyed = false) const;

    std::optional<spot_context_t> find_context_core (const spot_id_t &spot_id) const;

    std::optional<spot_context_t> find_context (const spot_id_t &spot_id) const;

    result_t<std::shared_ptr<spot_context_state_t>>
    find_active_remote_actor_join_target (const spot_id_t &target_spot_id) const;
    template <typename TJoinResult>
    static task_t<void>
    run_actor_join_admission_task (spot_node_runtime_t runtime,
                                   std::shared_ptr<TJoinResult> join_result,
                                   actor_ref_t actor,
                                   service::actor_control_t control,
                                   std::string join_spot_id,
                                   std::vector<zlink::message_t> parts,
                                   bool remote_transfer,
                                   zlink::message_t request,
                                   bool targets_entry_spot,
                                   std::string local_node_rid,
                                   service_provider_t *services,
                                   serializer_registry_t *serializers,
                                   std::optional<std::chrono::steady_clock::time_point> deadline);
    task_t<actor_join_reply_t> run_actor_join_control (
      const actor_ref_t &actor,
      const service::actor_control_t &control,
      const std::string &join_spot_id,
      const std::vector<zlink::message_t> &parts,
      const zlink::message_t &request,
      bool remote_transfer,
      bool targets_entry_spot,
      const std::string &local_node_rid,
      service_provider_t &services,
      serializer_registry_t &serializers,
      std::function<void (std::function<void (result_t<void>)>)> *source_leave,
      std::optional<std::chrono::steady_clock::time_point> deadline);
    // The runtime value lives in the coroutine frame; the call copies its inputs.
    static task_t<bool>
    materialize_relocation_state_in (spot_node_runtime_t self,
                                     runtime::stateful::frozen_object_state_t frozen,
                                     runtime::stateful::object_ref_t target,
                                     std::optional<runtime::stateful::object_ref_t> target_spot,
                                     std::stop_token cancellation);
    task_t<bool>
    materialize_actor_relocation_state (runtime::stateful::frozen_object_state_t frozen,
                                        runtime::stateful::object_ref_t target,
                                        std::optional<runtime::stateful::object_ref_t> target_spot,
                                        std::stop_token cancellation);
    void end_relocation_activation_admissions (
      const std::vector<runtime::stateful::object_ref_t> &targets) noexcept;
    task_t<std::shared_ptr<local_spot_create_result_t>> create_spot_context (
      std::string spot_name,
      spot_id_t spot_id,
      zlink::message_t request,
      std::uint64_t object_generation = 1,
      std::string mesh_name = {},
      std::function<task_t<void> (void *)> staged_restore = {},
      std::uint64_t authority_owner_generation = 1,
      std::shared_ptr<runtime::serial_execution_queue_t> retained_queue = {},
      std::function<task_t<void> (bool)> *preparation_terminal = nullptr,
      std::shared_ptr<spot_serial_executor_t::preparation_t> inherited_operation = {});
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
    };

    task_t<std::shared_ptr<actor_join_state_snapshot_t>> actor_join_state_snapshot (
      const actor_ref_t &actor_ref, spot_id_t spot_id, const zlink::message_t &request);
    result_t<spot_actor_admission_callbacks_t> actor_admission (spot_context_t &context,
                                                                std::type_index actor_type,
                                                                spot_id_t spot_id,
                                                                const actor_ref_t &actor_ref);
    task_t<std::function<void (std::function<void (result_t<void>)>)>> commit_accepted_actor_join (
      const std::string &key,
      spot_context_t &context,
      const actor_ref_t &committed,
      std::type_index actor_type,
      void *actor,
      const spot_actor_admission_callbacks_t &admission,
      bool create_entry_actor,
      const zlink::message_t &create_request,
      std::string operation_id,
      bool &authority_committed,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
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
    void replay_actor_handoff_until_move_closed (const actor_ref_t &actor_ref);
    task_t<void> replay_actor_handoff_until_move_closed_async (actor_ref_t actor_ref);
    task_t<void> replay_actor_handoff_core (const actor_ref_t &actor_ref);

    std::shared_ptr<spot_node_builder_state_t> _state;
};

task_t<result_t<void>>
cleanup_local_actor_resources (std::shared_ptr<service::public_host_runtime_t> native,
                               std::optional<service_provider_t> services,
                               actor_ref_t actor);

} // namespace zlink::framework::detail
