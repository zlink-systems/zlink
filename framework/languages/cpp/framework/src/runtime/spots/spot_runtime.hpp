/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "actor_transfer_coordinator.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
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
#include <vector>

namespace zlink::framework::detail
{

namespace runtime = zlink::framework::runtime;

namespace service = zlink::framework::runtime::host;

using instance_spot_idle_eviction_callback_t = std::function<bool (
  const spot_id_t &,
  std::string_view,
  std::uint64_t,
  std::uint64_t,
  std::function<bool ()>)>;

class spot_node_builder_state_t
{
  public:
    explicit spot_node_builder_state_t (std::string name) : snapshot{.name = std::move (name)} {}
    ~spot_node_builder_state_t ();

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
    std::function<task_t<spot_create_result_t> (
      bool,
      std::optional<spot_id_t>,
      std::string,
      std::optional<std::string>,
      std::optional<message_t>,
      std::chrono::milliseconds)> create_user_spot;
    std::function<task_t<std::optional<spot_ref_t>> (spot_id_t)>
      find_user_spot;
    std::function<task_t<bool> (spot_ref_t)> close_user_spot;
    instance_spot_idle_eviction_callback_t admit_instance_spot_idle_eviction;
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
    std::map<std::string, std::string> actor_types_by_id;
    std::set<std::string> actor_created_keys;
    std::set<std::string> destroying_actors;
    std::set<std::string> destroyed_actor_keys;
    // A request id is reserved before dispatch and receives one terminal
    // reply. The table owns both states so replay and retry use the same
    // exactly-once transition.
    runtime::exactly_once_table_t<std::string, zlink::message_t>
      dispatched_request_replies;
    // Requests currently dispatched to each actor and not yet replied. Sampled
    // once per transfer right at the moving transition (runtime-metrics §4.3
    // pending_requests). Guarded by its own mutex: dispatch runs on the
    // packet-drain thread while the sample is taken on the transfer path.
    std::map<std::string, std::size_t> actor_pending_requests;
    std::mutex actor_pending_requests_mutex;
    struct pending_remote_source_cleanup_t
    {
        actor_ref_t source_actor;
        std::string transfer_id;
        spot_id_t target_spot_id;
        std::chrono::steady_clock::time_point not_before;
    };
    std::vector<pending_remote_source_cleanup_t> pending_remote_source_cleanups;
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
        std::function<std::shared_ptr<void> (actor_context_t)>
          create_context_instance;
        actor_join_completion_callback_t on_join_completed;
        std::function<task_t<std::vector<std::byte>> (
          void *, std::stop_token)> capture;
        std::function<task_t<void> (
          void *, std::vector<std::byte>, std::stop_token)> restore;
    };
    std::function<result_t<void> (const actor_ref_t &)> destroy_actor_registry;
    std::function<result_t<void> (const actor_ref_t &)> update_actor_registry_ref;
    std::function<result_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                                             actor_context_t,
                                                             stream_message_kind_t,
                                                             std::string_view,
                                                             const zlink::message_t &,
                                                             service_provider_t &,
                                                             serializer_registry_t &,
                                                             spot_inbound_message_t)>
      actor_packet_relay;
    std::function<result_t<std::optional<zlink::message_t>> (
        const actor_ref_t &, const runtime::messaging::envelope_header_t &,
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
    std::function<bool (const runtime::protocol::actor_route_fence_t &)>
      actor_route_admission;
    std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                                node_rid_t,
                                                const zlink::message_t &,
                                                const std::optional<zlink::message_t> &)>
      actor_entry_spot_join;
    std::map<std::string, actor_factory_registration_t> actor_factories;
    std::map<std::string, factory_relocation_configuration_t>
      spot_factory_relocations;
    std::map<std::string, std::int32_t> spot_stable_type_limits;
    std::map<std::string, spot_relocation_readiness_mode_t>
      spot_relocation_readiness;
    actor_transfer_coordinator_t actor_transfer_coordinator;
    // Message Follow relays messages that reach the committed source route
    // after relocation. The common contract bounds its default duration to 30s.
    std::chrono::milliseconds message_follow_duration{30000};
    std::map<std::string, std::shared_ptr<void>> actor_instances;
    std::set<std::pair<std::uint64_t, std::uint64_t>>
      committed_join_locations;
    std::set<std::pair<std::uint64_t, std::uint64_t>>
      delivered_join_completions;
    std::set<std::pair<std::uint64_t, std::uint64_t>>
      delivering_join_completions;
        std::shared_ptr<runtime::stateful::relocation_store_port_t>
          relocation_store;
        std::shared_ptr<runtime::stateful::authority_relocation_port_t>
          relocation_authority;
    /* Address → (type, id) lookup for instance-identity public surfaces
     * (destroy_actor). Never dereferenced — resolution only compares
     * addresses — and maintained alongside every registration/erasure, so
     * a freed instance can at worst leave an entry that no longer resolves
     * to a live registration. */
    std::map<const void *, std::pair<std::string, std::string>> actor_instance_index;
    std::map<std::string, std::shared_ptr<std::mutex>> actor_mailboxes;
    std::map<std::string,
             std::shared_ptr<runtime::serial_execution_queue_t>>
      actor_execution_queues;
    std::map<std::string, spot_route_t> actor_routes;
    std::map<std::string, std::unique_ptr<service::actor_t>> native_actors;
    std::unordered_set<std::string> mesh_runtime_owned_native_actor_ids;
    std::map<std::string, std::uint64_t> core_actor_membership_epochs;
    std::map<std::string, std::shared_ptr<service::spot_t>> native_spots_by_id;
    std::shared_ptr<service::spot_t> routed_control_spot;
    std::optional<route_client_t> route_client;
    struct queued_actor_packet_t
    {
        service::receive_record_t record;
        std::vector<zlink::message_t> parts;
    };
    std::vector<queued_actor_packet_t> queued_actor_packets;
    std::map<std::string, std::function<std::optional<spot_route_t> (spot_id_t)>> resolvers;
    std::shared_ptr<runtime::offload_executor_t> worker_executor;
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

/* actor_instance_index maintenance (caller holds the node mutex). A record
 * replaces any prior address for the same actor, so a re-registered actor
 * never leaves an older address that would resolve to the live actor. */
inline void erase_actor_instance_index_unlocked (spot_node_builder_state_t &node,
                                                 std::string_view actor_type,
                                                 std::string_view actor_id)
{
    for (auto it = node.actor_instance_index.begin ();
         it != node.actor_instance_index.end ();) {
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
    const auto actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    node.actor_instance_index[instance] = {actor_type, actor_id};
    // Internal Actor packets contain the logical Actor id but not its stable
    // type. Keep the type index when an instance is installed so a recreated
    // Actor can accept its first bind packet before the new authority snapshot
    // becomes visible through the Location Store reader.
    node.actor_types_by_id[actor_id] = actor_type;
}

class spot_context_state_t : public std::enable_shared_from_this<spot_context_state_t>
{
  public:
    void detach_application_instance (
      bool notify_closing,
      spot_close_reason_t close_reason = spot_close_reason_t::explicit_close,
      std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::time_point::max (),
      std::stop_token cleanup_cancellation = {})
    {
        auto lifetime_guard = shared_from_this ();
        {
            std::lock_guard<std::mutex> callback_lock (callback_mutex);
            callback_admission_closed = true;
        }
        auto instance = std::move (spot_instance);
        std::exception_ptr callback_error;
        if (notify_closing && lifecycle.on_closing && instance) {
            try {
                lifecycle.on_closing (
                  instance.get (),
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
        if (callback_error) {
            std::rethrow_exception (callback_error);
        }
    }

    bool close_now ()
    {
        auto owner = node;
        if (!owner) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> node_lock (owner->mutex);
        if (closed || actor_count != 0) {
            return false;
        }
        {
            std::lock_guard<std::mutex> callback_lock (callback_mutex);
            if (callback_depth != 0) {
                close_requested = true;
                return true;
            }
            callback_admission_closed = true;
            close_requested = false;
            closed = true;
        }
        const auto rid = std::string (spot_id);
        if (owner->location_lifecycle) {
            (void) owner->location_lifecycle->release_spot (
              spot_location_key_t{rid});
        }
        owner->spot_contexts_by_id.erase (rid);
        owner->spot_names_by_id.erase (rid);
        owner->native_spots_by_id.erase (rid);
        for (auto iterator = owner->spot_ids_by_name.begin ();
             iterator != owner->spot_ids_by_name.end (); ++iterator) {
            if (iterator->second == rid) {
                owner->spot_ids_by_name.erase (iterator);
                break;
            }
        }
        detach_application_instance (true);
        return true;
    }

    bool enter_callback ();
    void leave_callback () noexcept;
    bool is_current_callback_thread () const;
    bool admission_blocked () const noexcept
    {
        std::lock_guard<std::mutex> lock (callback_mutex);
        return callback_admission_closed || idle_eviction_in_progress;
    }

    bool idle_quiescent () const;

    bool try_post_serial (
      std::string name,
      std::function<void ()> work,
      runtime::serial_work_options_t options = {});
    bool try_post_serial_after_current_turn (std::string name,
                                             std::function<void ()> work,
                                             runtime::serial_work_options_t options = {});
    bool try_post_serial_async (std::string name,
                                runtime::serial_execution_queue_t::async_work_t work,
                                runtime::serial_work_options_t options = {});
    result_t<void> run_serial_task (std::string name,
                                    std::function<task_t<void> ()> work);
    bool run_serial_sync (std::string name, std::function<void ()> work);
    bool owns_current_serial_turn () const;
    void defer_relocation_ready ();
    void ensure_relocation_turn_open () const;
    void complete_relocation_ready (
      spot_relocation_ready_outcome_t outcome);
    void drain_serial ();
    void cancel_timers () noexcept;

    std::shared_ptr<spot_node_builder_state_t> node;
    std::shared_ptr<channel_runtime_state_t> channel_runtime;
    node_rid_t node_rid;
    std::string mesh_name;
    spot_id_t spot_id;
    std::uint64_t object_generation = 1;
    std::uint64_t authority_owner_generation = 1;
    std::string spot_name;
    user_spot_execution_mode_t execution_mode =
      user_spot_execution_mode_t::spot_wide;
    spot_relocation_readiness_mode_t relocation_readiness =
      spot_relocation_readiness_mode_t::any_turn_boundary;
    detail::spot_runtime_kind_t kind = detail::spot_runtime_kind_t::user;
    bool relocation_boundary_active = false;
    bool relocation_ready_deferred = false;
    std::vector<spot_packet_descriptor_t> packets;
    std::vector<spot_handler_descriptor_t> handlers;
    std::vector<spot_handler_registry_t::invoker_t> handler_invokers;
    std::map<std::type_index, spot_actor_admission_callbacks_t> actor_admissions;
    std::vector<std::string> ordering_log;
    std::weak_ptr<service::spot_t> native_spot;
    std::vector<std::shared_ptr<timer_state_t>> timers;
    std::shared_ptr<service_scope_t> activation_scope;
    std::map<std::type_index, std::shared_ptr<void>> timer_handler_instances;
    std::shared_ptr<void> spot_instance;
    std::shared_ptr<runtime::offload_executor_t> serial_executor;
    std::shared_ptr<runtime::serial_execution_queue_t> serial_queue;
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
    std::atomic<std::int64_t> last_application_work_completed_ns{0};
    mutable std::mutex callback_mutex;
    std::thread::id callback_thread;
    std::size_t callback_depth = 0;

    bool has_active_callback () const
    {
        std::lock_guard<std::mutex> lock (callback_mutex);
        return callback_depth > 0;
    }

    bool is_entry_spot () const noexcept
    {
        return kind == detail::spot_runtime_kind_t::entry;
    }

    bool is_instance_spot () const noexcept
    {
        return kind == detail::spot_runtime_kind_t::instance;
    }

    bool accepts_route_fence (
      const runtime::protocol::spot_route_fence_t &target,
      const std::optional<location_owner_token_t> &owner_token) const noexcept
    {
        return target.spot_id == spot_id
               && target.object_generation == object_generation
               && target.authority_owner_generation
                    == authority_owner_generation
               && owner_token
               && owner_token->lease_generation
                    == target.owner_lease_generation;
    }

    bool try_close_idle ()
    {
        auto owner = node;
        if (!owner) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> node_lock (owner->mutex);
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
          std::chrono::steady_clock::now ().time_since_epoch ())
                              .count ();
        const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
          owner->instance_spot_idle_timeout)
                                  .count ();
        const auto last_ns = last_application_work_completed_ns.load (
          std::memory_order_relaxed);
        if (closed || !is_instance_spot () || !idle_quiescent () || last_ns <= 0
            || now_ns < last_ns || now_ns - last_ns < timeout_ns) {
            return false;
        }
        {
            std::lock_guard<std::mutex> callback_lock (callback_mutex);
            if (!idle_eviction_in_progress || callback_depth != 0
                || close_requested || callback_admission_closed) {
                return false;
            }
            callback_admission_closed = true;
            closed = true;
        }
        const auto rid = std::string (spot_id);
        if (owner->location_lifecycle) {
            (void) owner->location_lifecycle->release_spot (
              spot_location_key_t{rid});
        }
        owner->spot_contexts_by_id.erase (rid);
        owner->spot_names_by_id.erase (rid);
        owner->native_spots_by_id.erase (rid);
        for (auto iterator = owner->spot_ids_by_name.begin ();
             iterator != owner->spot_ids_by_name.end (); ++iterator) {
            if (iterator->second == rid) {
                owner->spot_ids_by_name.erase (iterator);
                break;
            }
        }
        detach_application_instance (true, spot_close_reason_t::idle_evicted);
        return true;
    }
};

class spot_context_access_t final
{
  public:
    static spot_context_t create ()
    {
        return spot_context_t ();
    }

    static spot_context_t create (
      std::shared_ptr<spot_context_state_t> state)
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
    };
    explicit spot_node_runtime_t (std::shared_ptr<spot_node_builder_state_t> state);

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
                        std::uint64_t authority_owner_generation = 1);
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
    std::optional<actor_message_follow_target_t>
    actor_message_follow_target (const actor_ref_t &actor_ref) const;
    std::optional<actor_message_follow_target_t>
    try_acquire_actor_message_follow (
      const actor_ref_t &actor_ref,
      std::size_t payload_bytes,
      std::size_t hop_count);
    void release_actor_message_follow (
      const actor_ref_t &actor_ref,
      std::size_t payload_bytes) noexcept;
    bool mark_actor_message_follow_notified (
      const actor_ref_t &actor_ref,
      const zlink::routing_id_t &source_node);
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
    const std::vector<std::string> &ordering_log (const spot_context_t &context) const;
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
    std::vector<spot_id_t>
    deferred_relocation_ready_spots () const;
    std::vector<application_relocation_unit_t>
    application_relocation_units () const;
    void begin_relocation_readiness ();
    void end_relocation_readiness (
      const std::vector<spot_id_t> &relocated_spots);
    bool complete_relocation_ready (
      const spot_id_t &spot_id,
      spot_relocation_ready_outcome_t outcome);
    /* Domain snapshot for the drain handoff join — the same shape the erased
     * cross-node leave path sends alongside the entry-spot join. */
    std::optional<zlink::message_t> serialize_actor_snapshot (const actor_ref_t &actor_ref) const;
    std::shared_ptr<service::mesh_node_t> native_node () const;
    result_t<void> send_spot_mesh_parts (
      const zlink::routing_id_t &target_node_rid,
      const spot_id_t &target_spot_id,
      runtime::messaging::message_parts_t parts) const;
    std::optional<std::uint64_t> resolve_spot_generation (
      const zlink::routing_id_t &target_node_rid,
      const spot_id_t &target_spot_id) const;
    std::vector<spot_context_t> active_contexts () const;
    result_t<void> dispatch_subscription (const spot_context_t &context,
                                          std::string topic,
                                          const zlink::message_t &message,
                                          service_provider_t &services,
                                          serializer_registry_t &serializers) const;
    result_t<void> dispatch_subscription (const spot_context_t &context,
                                          std::string topic,
                                          const std::vector<zlink::message_t> &parts,
                                          service_provider_t &services,
                                          serializer_registry_t &serializers) const;
    result_t<std::size_t> dispatch_multicast (
      std::string topic,
      const std::vector<zlink::message_t> &parts,
      service_provider_t &services,
      serializer_registry_t &serializers) const;
    bool dispatch_mesh_record (const service::ready_record_t &owner,
                               const service::receive_record_t &record,
                               std::vector<zlink::message_t> &parts,
                               service_provider_t &services,
                               serializer_registry_t &serializers);
    void set_route_client (route_client_t route_client);
    void on_destroy_actor (std::function<result_t<void> (const actor_ref_t &)> destroy_actor);
    void on_actor_ref_updated (std::function<result_t<void> (const actor_ref_t &)> update_actor);
    void on_actor_entry_spot_join (
      std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                                  node_rid_t,
                                                  const zlink::message_t &,
                                                  const std::optional<zlink::message_t> &)> join);
    void on_actor_packet_relay (
      std::function<result_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                                               actor_context_t,
                                                               stream_message_kind_t,
                                                               std::string_view,
                                                               const zlink::message_t &,
                                                               service_provider_t &,
                                                               serializer_registry_t &,
                                                               spot_inbound_message_t)>
        relay);
    void on_actor_message_follow (
      std::function<result_t<std::optional<zlink::message_t>> (
        const actor_ref_t &, const runtime::messaging::envelope_header_t &,
        const zlink::message_t &,
        std::chrono::milliseconds,
        const zlink::routing_id_t &,
        const runtime::protocol::actor_route_fence_t &,
        std::uint8_t,
        const runtime::protocol::wire_operation_id_t &,
        std::uint64_t)> relay);
    void invalidate_message_follow_route (
      const runtime::protocol::message_follow_notice_t &notice);
    spot_manager_t manager () const;
    result_t<actor_join_reply_t> join_actor_to_spot_erased (
      const actor_ref_t &actor_ref,
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
                                std::uint64_t completion_operation_id_low = 0);
    // handoff_backlog holds the in-flight packets the source preserved while the
    // actor was moving (§10.2-2). They are enqueued on the target actor's
    // dispatch queue before the committed location is published (§10.2-3), and
    // services may be null only when the backlog is empty.
    result_t<actor_join_reply_t>
    prepare_remote_actor_to_spot (std::string transfer_id,
                                  const actor_ref_t &actor_ref,
                                  spot_id_t target_spot_id,
                                  zlink::message_t transfer_state,
                                  actor_context_t actor_context = {},
                                  bool defer_joined_callback = false);
    result_t<actor_join_reply_t>
    commit_remote_actor_to_spot (std::string transfer_id,
                                 const actor_ref_t &actor_ref,
                                 spot_id_t target_spot_id,
                                 zlink::message_t transfer_state,
                                 actor_context_t actor_context = {},
                                 std::vector<handoff_packet_t> handoff_backlog = {},
                                 service_provider_t *services = nullptr);
    result_t<actor_join_reply_t>
    finalize_remote_actor_to_spot (std::string transfer_id,
                                   const actor_ref_t &actor_ref,
                                   spot_id_t target_spot_id,
                                   std::vector<handoff_packet_t> handoff_backlog,
                                   service_provider_t &services,
                                   actor_gateway_runtime_t *actor_gateway = nullptr);
    std::size_t cleanup_expired_actor_admissions ();
    std::size_t cleanup_expired_actor_admissions_at (
      std::chrono::steady_clock::time_point now);
    std::string next_actor_transfer_id ();
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
    void set_message_follow_duration (std::chrono::milliseconds duration);
    void bind_relocation_store (
      std::shared_ptr<runtime::stateful::relocation_store_port_t> store);
    void bind_relocation_authority (
      std::shared_ptr<runtime::stateful::authority_relocation_port_t>
        authority);
    std::vector<std::uint8_t> capture_spot_relocation_state (
      const runtime::stateful::object_ref_t &spot,
      const std::string &stable_type,
      std::stop_token cancellation = {}) const;
    bool restore_spot_relocation_state (
      const runtime::stateful::frozen_object_state_t &frozen,
      const runtime::stateful::object_ref_t &target,
      std::stop_token cancellation = {});
    std::optional<runtime::stateful::durable_join_completion_root_t>
    pending_join_completion_root (const std::string &transfer_id) const;
    result_t<void> restore_pending_join_completion (
      const std::string &transfer_id,
      const actor_ref_t &actor,
      const spot_id_t &target_spot_id,
      runtime::stateful::durable_join_completion_root_t root);
    result_t<remote_actor_transfer_t> transfer_actor_out (const actor_ref_t &actor_ref,
                                                          std::string transfer_id = {});
    result_t<void> leave_actor_for_remote_transfer (const actor_ref_t &actor_ref);
    void fail_remote_actor_transfer (const actor_ref_t &actor_ref, bool reconcile);
    void complete_remote_actor_transfer (const actor_ref_t &source_actor,
                                         const actor_ref_t &target_actor,
                                         spot_route_t target_route,
                                         std::string transfer_id = {});
    // Emits an internal transfer lifecycle boundary through the configured
    // public message-flow observer. The transfer id is both the correlation
    // key and lifecycle flow key so role-server evidence can join source and
    // target events without parsing runtime logs.
    void emit_actor_transfer_marker (std::string marker,
                                     const actor_ref_t &actor_ref,
                                     std::string transfer_id,
                                     std::optional<spot_id_t> spot_id = std::nullopt,
                                     std::optional<node_rid_t> target_node_rid = std::nullopt) const;
    result_t<actor_join_reply_t> join_actor_to_entry_spot_erased (
      const actor_ref_t &actor_ref,
      node_rid_t spot_node_rid,
      const zlink::message_t &request,
      const std::optional<zlink::message_t> &actor_snapshot,
      actor_context_t actor_context);
    result_t<std::optional<zlink::message_t>>
    relay_actor_packet (const actor_ref_t &actor_ref,
                        actor_context_t actor_context,
                        std::string_view packet_name,
                        const zlink::message_t &message,
                        service_provider_t &services,
                        serializer_registry_t &serializers,
                        spot_inbound_message_t metadata = {});
    result_t<std::optional<zlink::message_t>>
    relay_actor_packet (const actor_ref_t &actor_ref,
                        actor_context_t actor_context,
                        stream_message_kind_t message_kind,
                        std::string_view packet_name,
                        const zlink::message_t &message,
                        service_provider_t &services,
                        serializer_registry_t &serializers,
                        spot_inbound_message_t metadata = {});
    result_t<void> notify_actor_disconnected_erased (const actor_ref_t &actor_ref) const;

    template <typename TActor>
    std::optional<std::reference_wrapper<TActor>> actor_instance (const actor_ref_t &actor_ref)
    {
        const auto found = _state->actor_instances.find (actor_key (actor_ref));
        if (found == _state->actor_instances.end () || !found->second) {
            return std::nullopt;
        }
        return *static_cast<TActor *> (found->second.get ());
    }

    template <typename TSpot, typename TActor>
    result_t<actor_join_reply_t> join_actor_to_spot (const actor_ref_t &actor_ref,
                                                     spot_id_t spot_id,
                                                     TActor &actor,
                                                     const zlink::message_t &request)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor ref is empty");
        }
        auto context = find_context (spot_id);
        if (!context || !context->_state->spot_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "target spot is not registered");
        }
        auto &spot = *static_cast<TSpot *> (context->_state->spot_instance.get ());
        if constexpr (!has_actor_join_callback<TSpot>) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found,
              "spot actor join callback is not registered");
        } else {
            const auto response = invoke_actor_join_callback (
              spot, actor_ref.actor_id (), request);
            auto &serializers = *context->_state->channel_runtime->serializers;
            if (!response.accepted) {
                return result_t<actor_join_reply_t>::success (
                  actor_join_reply_t{1, actor_ref, actor_join_reply (response, serializers)});
            }

            const auto committed =
              commit_actor_to_context<TSpot, TActor> (actor_ref, actor, *context, request);
            return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{0, committed, actor_join_reply (response, serializers)});
        }
    }

    template <typename TEntrySpot, typename TActor>
    result_t<actor_join_reply_t> join_actor_to_entry_spot (const actor_ref_t &actor_ref,
                                                           node_rid_t spot_node_rid,
                                                           TActor &actor,
                                                           const zlink::message_t &request)
    {
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor ref is empty");
        }
        if (spot_node_rid.empty ()
            || spot_node_rid.value () != detail::effective_spot_node_rid (_state->snapshot)) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found,
              "spot node rid does not match this node");
        }
        if (!_state->snapshot.entry_spot_name) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "entry spot is not registered");
        }
        const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
        if (entry_id == _state->spot_ids_by_name.end ()) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "entry spot is not created");
        }
        auto context = find_context (entry_id->second);
        if (!context || !context->_state->spot_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "entry spot context is not registered");
        }

        auto &spot = *static_cast<TEntrySpot *> (context->_state->spot_instance.get ());
        if constexpr (has_actor_join_callback<TEntrySpot>) {
            const auto response = invoke_actor_join_callback (
              spot, actor_ref.actor_id (), request);
            auto &serializers = *context->_state->channel_runtime->serializers;
            if (!response.accepted) {
                return result_t<actor_join_reply_t>::success (
                  actor_join_reply_t{1, actor_ref, actor_join_reply (response, serializers)});
            }

            const auto committed =
              commit_actor_to_context<TEntrySpot, TActor> (actor_ref, actor, *context, request);
            return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{0, committed, actor_join_reply (response, serializers)});
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
        const auto key = actor_key (actor_ref);
        const auto found_location = _state->actor_spot_ids.find (key);
        if (found_location == _state->actor_spot_ids.end ()) {
            return result_t<void>::success ();
        }
        const auto found_generation = _state->actor_generations.find (key);
        if (found_generation != _state->actor_generations.end ()
            && found_generation->second != actor_ref.object_generation ()) {
            return detail::boundary_failure<void> (detail::boundary_error_t::stale_generation,
                                            "actor generation is stale");
        }
        auto context = find_context (found_location->second);
        if (!context) {
            return result_t<void>::success ();
        }
        try {
            notify_on_disconnect_actor<TActor> (*context->_state, actor);
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "spot actor disconnected callback failed");
        }
    }

  private:
    template <typename TSpot>
    static constexpr bool has_framework_actor_join_callback =
      requires (TSpot & spot, std::string_view actor_id, const message_t &request)
    {
        {
            spot.on_actor_join (actor_id, request)
        } -> std::same_as<spot_actor_join_result_t>;
    };

    template <typename TSpot>
    static constexpr bool has_raw_actor_join_callback = requires (
      TSpot & spot, std::string_view actor_id, const zlink::message_t &request)
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
                                                           const zlink::message_t &request)
    {
        if constexpr (has_framework_actor_join_callback<TSpot>) {
            return spot.on_actor_join (
              actor_id, message_t::from_raw (request, _state->channel_runtime->serializers));
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
        return std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)) + ":" + std::string (actor_ref.actor_id ().value ());
    }

    /* Registration of a factory-owned actor instance. The lookup and the
     * install both run under the node mutex and keep the registry and its
     * identity index in step; the factory itself stays outside the mutex
     * (user code must not be able to invert lock order), so the caller
     * constructs only after the lookup misses. */
    std::shared_ptr<void> registered_actor_instance (const actor_ref_t &actor_ref,
                                                     const std::string &key) const
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        const auto found = _state->actor_instances.find (key);
        if (found == _state->actor_instances.end () || !found->second) {
            return {};
        }
        detail::record_actor_instance_index_unlocked (*_state, actor_ref, found->second.get ());
        return found->second;
    }

    /* `refuse_destroyed` re-checks the destroy tombstone under the same lock
     * that installs, so a destroy landing inside the factory window cannot be
     * resurrected by a late install; the caller then reports the actor as
     * destroyed. Join paths pass false: a join legitimately recreates a
     * destroyed actor in a new generation and clears the tombstone on commit. */
    std::shared_ptr<void> install_actor_instance (const actor_ref_t &actor_ref,
                                                  const std::string &key,
                                                  std::shared_ptr<void> instance,
                                                  bool refuse_destroyed = false) const
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        auto &slot = _state->actor_instances[key];
        if (!slot) {
            if (refuse_destroyed && _state->destroyed_actor_keys.contains (key)) {
                _state->actor_instances.erase (key);
                return {};
            }
            /* A concurrent creator that won the race keeps its instance; the
             * loser's copy is released when this frame ends. */
            slot = std::move (instance);
        }
        detail::record_actor_instance_index_unlocked (*_state, actor_ref, slot.get ());
        return slot;
    }

    std::optional<spot_context_t> find_context (const spot_id_t &spot_id) const
    {
        const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
        if (found == _state->spot_contexts_by_id.end ()) {
            return std::nullopt;
        }
        return spot_context_t (found->second._state);
    }

    template <typename TSpot, typename TActor>
    actor_ref_t commit_actor_to_context (const actor_ref_t &actor_ref,
                                         TActor &actor,
                                         spot_context_t &context,
                                         const zlink::message_t &create_request)
    {
        commit_actor_left<TActor> (actor_ref, actor);
        auto &context_state = *context._state;
        const auto key = actor_key (actor_ref);
        {
            /* Typed joins hold externally-owned actors: they are indexed
             * for instance-identity surfaces (destroy_actor) but never
             * stored in actor_instances, whose consumers dereference. */
            std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
            record_actor_instance_index_unlocked (*_state, actor_ref, std::addressof (actor));
        }
        context_state.on_actor_joined_callbacks[std::type_index (typeid (TActor))] =
          [] (void *spot, void *actor) {
              if constexpr (has_on_actor_joined_callback<TSpot, TActor>) {
                  if constexpr (std::same_as<
                                  decltype (static_cast<TSpot *> (spot)->on_actor_joined (
                                    *static_cast<TActor *> (actor))),
                                  task_t<void>>) {
                      return static_cast<TSpot *> (spot)->on_actor_joined (
                        *static_cast<TActor *> (actor));
                  } else {
                      static_cast<TSpot *> (spot)->on_actor_joined (
                        *static_cast<TActor *> (actor));
                  }
              }
              return task_t<void> (result_t<void>::success ());
          };
        context_state.on_create_actor_callbacks[std::type_index (typeid (TActor))] =
          [] (void *spot, void *actor, const zlink::message_t &request,
              serializer_registry_t &serializers) {
              if constexpr (detail::entry_spot_type<TSpot>
                            && has_framework_payload_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (
                    *static_cast<TActor *> (actor), message_t::from_raw (request, &serializers));
              } else if constexpr (detail::entry_spot_type<TSpot>
                                   && has_raw_payload_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (*static_cast<TActor *> (actor),
                                                              request);
              } else if constexpr (detail::entry_spot_type<TSpot>
                                   && has_on_create_actor_callback<TSpot, TActor>) {
                  static_cast<TSpot *> (spot)->on_create_actor (*static_cast<TActor *> (actor));
              }
          };
        context_state.on_leave_actor_callbacks[std::type_index (typeid (TActor))] = [] (void *spot,
                                                                                      void *actor) {
            if constexpr (has_on_leave_actor_callback<TSpot, TActor>) {
                if constexpr (std::same_as<
                                decltype (static_cast<TSpot *> (spot)->on_leave_actor (
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
        context_state.on_disconnect_actor_callbacks[std::type_index (typeid (TActor))] =
          [] (void *spot, void *actor) {
              if constexpr (has_on_disconnect_actor_callback<TSpot, TActor>) {
                  if constexpr (std::same_as<
                                  decltype (static_cast<TSpot *> (spot)->on_disconnect_actor (
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
        auto committed =
          ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (effective_spot_node_rid (_state->snapshot)),
                       std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                       actor_ref.object_generation () + 1);
        if constexpr (detail::entry_spot_type<TSpot>) {
            if (_state->actor_created_keys.insert (key).second) {
                notify_on_create_actor<TActor> (context_state, actor, create_request);
            }
        }
        notify_on_actor_joined<TActor> (context_state, actor);
        record_actor_context_route_unlocked (*_state, key,
                                             effective_spot_node_rid (_state->snapshot),
                                             context_state, actor_ref.object_generation () + 1);
        return committed;
    }

    template <typename TActor> void commit_actor_left (const actor_ref_t &actor_ref, TActor &actor)
    {
        const auto key = actor_key (actor_ref);
        const auto found_location = _state->actor_spot_ids.find (key);
        if (found_location == _state->actor_spot_ids.end ()) {
            return;
        }
        auto previous_context = find_context (found_location->second);
        _state->actor_spot_ids.erase (found_location);
        _state->actor_routes.erase (key);
        _state->actor_generations.erase (key);
        if (!previous_context) {
            return;
        }
        auto &state = *previous_context->_state;
        if (state.actor_count > 0) {
            state.actor_count--;
        }
        notify_on_leave_actor<TActor> (state, actor);
    }

    template <typename TActor>
    void notify_on_create_actor (spot_context_state_t &state,
                               TActor &actor,
                               const zlink::message_t &request)
    {
        const auto found = state.on_create_actor_callbacks.find (std::type_index (typeid (TActor)));
        if (found != state.on_create_actor_callbacks.end () && state.spot_instance) {
            if (!state.channel_runtime || !state.channel_runtime->serializers) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "spot create actor requires a serializer registry");
            }
            if (!state.run_serial_sync ("spot-lifecycle-create", [&] {
                    found->second (state.spot_instance.get (), &actor, request,
                                   *state.channel_runtime->serializers);
                })) {
                throw framework_exception_t (framework_error_kind_t::rejected,
                                             "spot serial queue is full");
            }
        }
    }

    template <typename TActor>
    void notify_on_actor_joined (spot_context_state_t &state, TActor &actor)
    {
        const auto found = state.on_actor_joined_callbacks.find (std::type_index (typeid (TActor)));
        if (found != state.on_actor_joined_callbacks.end () && state.spot_instance) {
            const auto completed = state.run_serial_task (
              "spot-lifecycle-join",
              [&] { return found->second (state.spot_instance.get (), &actor); });
            if (!completed) {
                throw framework_exception_t (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor joined callback failed");
            }
        }
    }

    template <typename TActor> void notify_on_leave_actor (spot_context_state_t &state, TActor &actor)
    {
        const auto found = state.on_leave_actor_callbacks.find (std::type_index (typeid (TActor)));
        if (found != state.on_leave_actor_callbacks.end () && state.spot_instance) {
            const auto completed = state.run_serial_task (
              "spot-lifecycle-leave",
              [&] { return found->second (state.spot_instance.get (), &actor); });
            if (!completed) {
                throw framework_exception_t (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor leave callback failed");
            }
        }
    }

    template <typename TActor>
    void notify_on_disconnect_actor (spot_context_state_t &state, TActor &actor)
    {
        const auto found =
          state.on_disconnect_actor_callbacks.find (std::type_index (typeid (TActor)));
        if (found != state.on_disconnect_actor_callbacks.end () && state.spot_instance) {
            const auto completed = state.run_serial_task (
              "spot-lifecycle-disconnect",
              [&] { return found->second (state.spot_instance.get (), &actor); });
            if (!completed) {
                throw framework_exception_t (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor disconnect callback failed");
            }
        }
    }

    local_spot_create_result_t
    create_spot_context_unlocked (std::string spot_name,
                                  spot_id_t spot_id,
                                  zlink::message_t request,
                                  std::unique_lock<std::recursive_mutex> &node_lock,
                                  std::uint64_t object_generation = 1,
                                  std::string mesh_name = {},
                                  std::function<task_t<void> (void *)>
                                    staged_restore = {},
                                  std::uint64_t authority_owner_generation = 1);
    result_t<spot_context_t> actor_join_context_unlocked (spot_id_t spot_id,
                                                          const zlink::message_t &request);
    result_t<std::reference_wrapper<spot_node_builder_state_t::actor_factory_registration_t>>
    actor_factory_unlocked (const actor_ref_t &actor_ref) const;
    result_t<std::reference_wrapper<spot_actor_admission_callbacks_t>>
    actor_admission_unlocked (spot_context_t &context,
                              std::type_index actor_type,
                              spot_id_t spot_id,
                              const actor_ref_t &actor_ref);
    // Releases node_lock while the previous spot runs the user leave callback on its
    // serial queue, then re-acquires it before mutating node state.
    void leave_previous_actor_route (const std::string &key,
                                     std::type_index actor_type,
                                     void *actor,
                                     std::unique_lock<std::recursive_mutex> &node_lock);
    void commit_accepted_actor_join_unlocked (const std::string &key,
                                              spot_context_t &context,
                                              const actor_ref_t &committed,
                                              std::type_index actor_type,
                                              void *actor,
                                              const spot_actor_admission_callbacks_t &admission,
                                              bool create_entry_actor,
                                              const zlink::message_t &create_request,
                                              std::string operation_id,
                                              bool &authority_committed);
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
