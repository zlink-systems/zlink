/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/mesh_node.hpp>
#include <runtime/locations/location_repository.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/operations/exactly_once_table.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zlink::framework
{
class zlink_builder_t;
}

namespace zlink::framework::detail
{

enum class application_actor_session_bind_outcome_t : std::uint8_t
{
    bound,
    stale_route,
    actor_not_ready
};

enum class application_actor_session_bind_attempt_t : std::uint8_t
{
    initial,
    retry
};

constexpr bool can_retry_application_actor_session_bind (
  application_actor_session_bind_outcome_t outcome,
  application_actor_session_bind_attempt_t attempt) noexcept
{
    return attempt
             == application_actor_session_bind_attempt_t::initial
           && (outcome
                 == application_actor_session_bind_outcome_t::stale_route
               || outcome
                    == application_actor_session_bind_outcome_t::
                      actor_not_ready);
}

inline result_t<runtime::stateful::object_ref_t>
make_local_application_actor_session_ref (
  const runtime::stateful::object_ref_t &materialized,
  const actor_ref_t &actor,
  const runtime::spot_address_t &route)
{
    if (materialized.kind
          != runtime::stateful::object_kind_t::actor
        || materialized.key != actor.actor_id ().value ()
        || materialized.object_generation != actor.object_generation ()
        || route.object_generation != actor.object_generation ()
        || route.authority_owner_generation == 0
        || route.mesh_name.empty ()
        || route.node_generation == 0
        || route.owner.lease_generation <= 0) {
        return result_t<runtime::stateful::object_ref_t>::failure (
          framework_error_kind_t::unavailable,
          "Local Actor materialization does not match its Location route");
    }
    return result_t<runtime::stateful::object_ref_t>::success (
      runtime::stateful::object_ref_t{
        runtime::stateful::object_kind_t::actor,
        std::string (actor.actor_id ().value ()),
        actor.object_generation (),
        route.authority_owner_generation,
        route.mesh_name,
        route.node_rid.to_string ()});
}

namespace host = zlink::framework::runtime::host;

struct framework_options_state_t;
struct handler_group_options_state_t;

struct mesh_channel_registration_t
{
    int weight = 100;
    std::string handler_group;
    bool role_selected = false;
    bool server = false;
};

struct mesh_node_builder_state_t
{
    explicit mesh_node_builder_state_t (std::string name);

    std::mutex mutex;
    std::string mesh_name;
    std::string listen_endpoint;
    std::optional<std::uint16_t> listen_port;
    std::string bind_host = "127.0.0.1";
    std::optional<std::string> bind_host_override;
    std::optional<std::string> advertise_host;
    std::optional<std::string> advertise_host_override;
    std::weak_ptr<framework_options_state_t> framework_options;
    std::shared_ptr<zlink::context_t> core_context;
    service_collection_t *services = nullptr;
    std::vector<std::function<void (service_collection_t &)>> pending_handler_service_registrars;
    std::shared_ptr<handler_group_options_state_t> handler_groups;
    std::optional<zlink::routing_id_t> routing_id;
    std::optional<std::string> automatic_routing_id_prefix;
    object_role_t object_role = object_role_t::server;
    bool has_node_direct_handler = false;
    int placement_weight = 100;
    std::int32_t actor_limit = 0;
    std::int32_t spot_limit = 0;
    std::chrono::milliseconds instance_spot_idle_timeout{0};
    std::int32_t activation_concurrency_limit = 128;
    std::map<std::string, mesh_channel_registration_t> channels;
    std::function<void (const std::string &)> channel_name_observer;
    route_handler_registry_t handlers;
    std::vector<mesh_peer_connection_t> peer_connections;
    std::function<void (const mesh_peer_connection_t &)> runtime_peer_connect;
    std::function<void (const mesh_peer_connection_t &)> runtime_peer_disconnect;
    mesh_node_socket_config_t socket;
    std::chrono::milliseconds default_request_timeout{std::chrono::seconds (30)};
    zlink::auto_hwm_profile auto_hwm_profile = zlink::auto_hwm_profile::balanced;
    std::size_t max_pending = 1024;
    std::atomic<std::uint64_t> next_join_completion_operation{1};
    std::shared_ptr<spot_node_builder_state_t> spot_state;
    spot_node_builder_t spot_builder;
};

struct bound_session_relocation_route_t
{
    zlink::routing_id_t session_owner_node;
    std::uint64_t session_owner_node_generation = 0;
    location_owner_token_t session_owner;
    zlink::routing_id_t session;
    std::uint64_t binding_generation = 0;
    std::uint64_t observed_sequence = 0;

    friend bool operator== (
      const bound_session_relocation_route_t &,
      const bound_session_relocation_route_t &) = default;
};

class mesh_node_runtime_t
{
  public:
    using message_follow_subscription_id_t = std::uint64_t;

    struct operation_completion_t
    {
        host::receive_record_t record;
        std::vector<zlink::message_t> parts;
    };
    using actor_join_completion_t = std::function<void (result_t<actor_join_reply_t>)>;
    explicit mesh_node_runtime_t (std::shared_ptr<mesh_node_builder_state_t> state);
    ~mesh_node_runtime_t ();

    mesh_node_runtime_t (const mesh_node_runtime_t &) = delete;
    mesh_node_runtime_t &operator= (const mesh_node_runtime_t &) = delete;

    void start ();
    void stop () noexcept;
    void bind_serializers (serializer_registry_t &serializers) noexcept;
    void bind_descriptor_publisher (
      std::function<void (const std::map<std::string, int> &, int, std::uint64_t)> publisher);
    void configure_user_spot_operations (std::shared_ptr<location_repository_t> store,
                                         host::user_spot_materializer_t materializer);
    void configure_spot_route_fence_resolver (
      host::spot_route_fence_resolver_t resolver,
      std::chrono::milliseconds route_cache_max_age,
      std::chrono::milliseconds owner_lease_fencing_margin = std::chrono::seconds (5),
      std::chrono::milliseconds session_relocation_seal_timeout =
        location_options_t{}.session_relocation_seal_timeout);
    void configure_actor_route_resolver (
      std::function<std::optional<runtime::spot_address_t> (const actor_ref_t &)> resolver,
      std::function<void (const runtime::protocol::actor_route_fence_t &)> invalidator = {});
    void configure_actor_create_operations (host::actor_create_operation_target_t target);
    void configure_instance_spot_operations (
      std::shared_ptr<location_repository_t> store,
      std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
      location_owner_token_t owner,
      host::instance_spot_activation_materializer_t materializer);
    void configure_relocation_runtime (
      std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority,
      std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
      std::shared_ptr<runtime::stateful::aggregate_authority_port_t> aggregate_authority = {});
    task_t<runtime::stateful::relocation_result_t>
    relocate_application_actor (const actor_ref_t &actor,
                                const mesh_node_descriptor_t &target,
                                const authority_snapshot_t &authority);
    bool application_actor_transfer_in_progress (const actor_ref_t &actor) const;
    result_t<void> cleanup_application_actor_stateful (const actor_ref_t &actor);
    result_t<bool> destroy_application_actor (const actor_ref_t &actor);
    task_t<runtime::stateful::aggregate_relocation_result_t>
    relocate_application_unit (std::vector<runtime::stateful::object_ref_t> sources,
                               std::vector<std::string> stable_types,
                               const mesh_node_descriptor_t &target,
                               const std::vector<authority_snapshot_t> &authorities);
    void configure_session_route_owner (
      std::function<std::optional<location_owner_token_t> ()> owner_resolver);
    void configure_session_route_target_owner (
      host::public_host_runtime_t::session_route_target_owner_resolver_t
        owner_resolver);
    void configure_bound_session_relocation_resolver (
      std::function<std::optional<bound_session_relocation_route_t> (
        const runtime::stateful::object_ref_t &)> resolver);
    void
    configure_stateful_dispatch (runtime::stateful::accepted_record_authority_resolver_t resolver);
    void configure_bound_session_operations (host::bound_session_operations_t operations);
    message_follow_subscription_id_t subscribe_message_follow_invalidation (
      std::function<void (const runtime::protocol::message_follow_notice_t &)> handler);
    void unsubscribe_message_follow_invalidation (
      message_follow_subscription_id_t subscription_id) noexcept;
    task_t<bool> activate_instance_spot_remote (
      const zlink::routing_id_t &target_node,
      zlink::framework::runtime::protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      zlink::framework::runtime::protocol::application_payload_t application_payload,
      std::chrono::milliseconds timeout,
      host::instance_spot_activation_completion_t completion);
    task_t<bool> send_instance_spot_activation_remote (
      const zlink::routing_id_t &target_node,
      zlink::framework::runtime::protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      zlink::framework::runtime::protocol::application_payload_t application_payload);
    void connect_peer (const zlink::routing_id_t &expected_routing_id,
                       const std::string &endpoint,
                       std::uint64_t expected_lifecycle_generation = 0,
                       std::string security_identity = "default");
    void connect_peer (const std::string &endpoint, std::string security_identity = "default");
    void expect_peer (const zlink::routing_id_t &expected_routing_id,
                      const std::string &endpoint,
                      std::uint64_t expected_lifecycle_generation,
                      std::string security_identity);
    void forget_peer (const zlink::routing_id_t &expected_routing_id, const std::string &endpoint);
    void disconnect_peer (const std::string &endpoint) noexcept;
    void disconnect_peer (const zlink::routing_id_t &expected_routing_id,
                          const std::string &endpoint) noexcept;
    bool wait_for_peer_ready (const zlink::routing_id_t &target,
                              std::chrono::milliseconds timeout) const;

    task_t<zlink::submit_result_t> send_to_node (const zlink::routing_id_t &target,
                                         const std::vector<zlink::message_t> &parts,
                                         std::vector<std::uint8_t> metadata = {});
    task_t<zlink::submit_result_t> send_to_node (const zlink::routing_id_t &target,
                                         const std::vector<zlink::message_t> &parts,
                                         const std::map<std::string, std::string> &metadata);
    task_t<zlink::submit_result_t> request_to_node (const zlink::routing_id_t &target,
                                            const std::vector<zlink::message_t> &parts,
                                            host::call_id_t &operation_id,
                                            std::chrono::milliseconds timeout,
                                            std::vector<std::uint8_t> metadata = {});
    task_t<zlink::submit_result_t> request_to_node (const zlink::routing_id_t &target,
                                            const std::vector<zlink::message_t> &parts,
                                            host::call_id_t &operation_id,
                                            std::chrono::milliseconds timeout,
                                            const std::map<std::string, std::string> &metadata);
    task_t<zlink::submit_result_t> send_to_channel (const std::string &channel_name,
                                            const std::vector<zlink::message_t> &parts,
                                            std::vector<std::uint8_t> metadata = {});
    task_t<zlink::submit_result_t> send_to_channel (const std::string &channel_name,
                                            const std::vector<zlink::message_t> &parts,
                                            const std::map<std::string, std::string> &metadata);
    task_t<zlink::submit_result_t> request_to_channel (const std::string &channel_name,
                                               const std::vector<zlink::message_t> &parts,
                                               host::call_id_t &operation_id,
                                               std::chrono::milliseconds timeout,
                                               std::vector<std::uint8_t> metadata = {});
    task_t<zlink::submit_result_t> request_to_channel (const std::string &channel_name,
                                               const std::vector<zlink::message_t> &parts,
                                               host::call_id_t &operation_id,
                                               std::chrono::milliseconds timeout,
                                               const std::map<std::string, std::string> &metadata);
    host::spot_handle_t get_or_create_spot (std::string spot_id);
    task_t<zlink::submit_result_t> send_to_spot (const std::string &source_spot_id,
                                         const zlink::routing_id_t &target_node_rid,
                                         const std::string &target_spot_id,
                                         std::uint64_t target_spot_generation,
                                         const std::vector<zlink::message_t> &parts,
                                         std::vector<std::uint8_t> metadata = {});
    task_t<zlink::submit_result_t> request_to_spot (const std::string &source_spot_id,
                                            const zlink::routing_id_t &target_node_rid,
                                            const std::string &target_spot_id,
                                            std::uint64_t target_spot_generation,
                                            const std::vector<zlink::message_t> &parts,
                                            host::call_id_t &operation_id,
                                            std::chrono::milliseconds timeout,
                                            std::vector<std::uint8_t> metadata = {});
    host::actor_handle_t create_actor (std::string actor_type,
                                       std::string actor_id,
                                       const std::vector<zlink::message_t> &creation_parts = {},
                                       std::chrono::milliseconds timeout = {});
    task_t<zlink::submit_result_t> send_to_actor (const actor_ref_t &target,
                                          const std::vector<zlink::message_t> &parts,
                                          std::vector<std::uint8_t> metadata = {},
                                          std::uint64_t authority_owner_generation = 0,
                                          std::uint64_t owner_lease_generation = 0,
                                          std::optional<runtime::protocol::actor_message_header_t::bound_session_source_t>
                                            bound_session_source = std::nullopt);
    task_t<zlink::submit_result_t> request_to_actor (const actor_ref_t &target,
                                             const std::vector<zlink::message_t> &parts,
                                             host::call_id_t &operation_id,
                                             std::chrono::milliseconds timeout,
                                             std::vector<std::uint8_t> metadata = {},
                                             std::uint64_t authority_owner_generation = 0,
                                             std::uint64_t owner_lease_generation = 0,
                                             std::optional<runtime::protocol::actor_message_header_t::bound_session_source_t>
                                               bound_session_source = std::nullopt);
    zlink::context_t &native_context ();
    host::public_host_runtime_t &native_node ();
    bool prepare_actor_transfer (const host::actor_transfer_prepare_t &prepare,
                                 std::chrono::milliseconds timeout,
                                 host::actor_transfer_token_t &token,
                                 host::actor_transfer_prepare_result_t &result);
    result_t<actor_ref_t>
    create_application_actor (std::string actor_type,
                              std::string actor_id,
                              const std::optional<zlink::message_t> &creation_payload,
                              std::chrono::milliseconds timeout);
    result_t<actor_ref_t>
    create_application_actor (std::string actor_type,
                              std::string actor_id,
                              const std::optional<zlink::message_t> &creation_payload,
                              std::uint64_t object_generation,
                              std::uint64_t authority_owner_generation,
                              std::chrono::milliseconds timeout);
    result_t<actor_join_reply_t>
    join_application_actor_to_entry_spot (const actor_ref_t &actor,
                                          const node_rid_t &target_node,
                                          const zlink::message_t &request,
                                          std::chrono::milliseconds timeout);
    result_t<void> submit_application_actor_entry_spot_join (const actor_ref_t &actor,
                                                             const node_rid_t &target_node,
                                                             const zlink::message_t &request,
                                                             std::chrono::milliseconds timeout,
                                                             actor_join_completion_t completion);
    bool complete_application_actor_entry_spot_join (const host::receive_record_t &record,
                                                     const std::vector<zlink::message_t> &parts);
    task_t<actor_join_reply_t> join_application_actor_to_spot (
      actor_ref_t actor,
      const runtime::spot_address_t &target,
      const zlink::message_t &request,
      std::chrono::milliseconds timeout,
      std::optional<zlink::routing_id_t> bound_session_node_rid = std::nullopt,
      std::optional<zlink::routing_id_t> bound_session_rid = std::nullopt);
    result_t<std::shared_ptr<deferred_barrier_t>>
    reserve_application_actor_join_barrier (const actor_ref_t &actor);
    task_t<std::optional<zlink::message_t>>
    relay_application_actor (const actor_ref_t &actor,
                             const stream_header_t &header,
                             const zlink::message_t &payload,
                             std::chrono::milliseconds timeout,
                             bool await_remote_admission = false,
                             std::optional<bound_session_relay_source_t>
                               bound_session_source = std::nullopt);
    task_t<std::optional<zlink::message_t>>
    relay_application_actor (const actor_ref_t &actor,
                             const runtime::messaging::envelope_header_t &header,
                             const zlink::message_t &payload,
                             std::chrono::milliseconds timeout);
    task_t<std::optional<zlink::message_t>>
    relay_application_actor (actor_ref_t actor,
                             runtime::messaging::envelope_header_t header,
                             zlink::message_t payload,
                             std::chrono::milliseconds timeout,
                             zlink::routing_id_t source_node,
                             runtime::protocol::actor_route_fence_t stale_route,
                             std::uint8_t hop_count,
                             runtime::protocol::wire_operation_id_t operation,
                             std::uint64_t reply_route_id,
                             bool await_remote_admission = false,
                             std::optional<bound_session_relay_source_t>
                               bound_session_source = std::nullopt);
    task_t<application_actor_session_bind_outcome_t>
    bind_application_actor_session (const actor_ref_t &actor,
                                    const zlink::routing_id_t &session_rid,
                                    std::uint64_t binding_generation,
                                    const runtime::spot_address_t &actor_route,
                                    std::chrono::milliseconds timeout);
    std::optional<runtime::spot_address_t>
    resolve_application_actor_route (const actor_ref_t &actor) const;
    std::optional<runtime::spot_address_t>
    wait_for_application_actor_route_change (
      const actor_ref_t &actor,
      const runtime::spot_address_t &stale_route,
      std::chrono::milliseconds timeout) const;
    task_t<void> notify_application_actor_disconnected (const actor_ref_t &actor,
                                                        const node_rid_t &target_node,
                                                        std::chrono::milliseconds timeout);
    result_t<operation_completion_t>
    wait_for_completion (const host::call_id_t &operation,
                         std::chrono::milliseconds timeout,
                         std::optional<zlink::routing_id_t> target = std::nullopt);
    task_t<operation_completion_t>
    await_completion (const host::call_id_t &operation);
    task_t<std::size_t> dispatch_ready (const std::function<void (const host::ready_record_t &,
                                                          const host::receive_record_t &,
                                                          std::vector<zlink::message_t>)> &dispatch,
                                bool accept_application_receive = true);
    host::node_status_t status () const;
    void dispatch_message_follow (const runtime::protocol::message_follow_notice_t &notice);
    /* Admitted RouteMesh membership size. Vertical and E2E checks wait on this
     * instead of reaching into the transport topology. */
    std::size_t admitted_peer_count () const;
    bool has_admitted_peer (const zlink::routing_id_t &peer_rid,
                            std::uint64_t lifecycle_generation) const;
    bool has_admitted_peer (const zlink::routing_id_t &peer_rid) const;
    std::string mesh_name () const;
    std::optional<zlink::routing_id_t> routing_id () const;
    std::string listen_endpoint () const;
    object_role_t object_role () const;
    std::vector<std::string> channel_names () const;
    std::map<std::string, int> channel_weights () const;
    std::size_t max_pending () const noexcept;
    void set_channel_weight (const std::string &channel_name, int weight);
    int placement_weight () const;
    void set_placement_weight (int weight);
    std::int32_t actor_limit () const;
    std::int32_t spot_limit () const;
    std::int32_t activation_concurrency_limit () const;
    void application_work_enqueued () noexcept;
    void application_work_started () noexcept;
    void application_work_finished () noexcept;
    void note_local_node_submit_attempt ();
    void local_application_work_finished () noexcept;
    std::uint64_t pending_application_callbacks () const noexcept;
    std::uint64_t active_application_callbacks () const noexcept;
    std::size_t pending_transport_operations () const noexcept;
    std::uint64_t active_completion_waiters () const noexcept;

    static std::shared_ptr<mesh_node_runtime_t> from (zlink_builder_t &builder,
                                                      const std::string &mesh_name);
    static std::vector<std::shared_ptr<mesh_node_builder_state_t>>
    registrations (zlink_builder_t &builder);

  private:
    struct session_relocation_checkpoint_t
    {
        runtime::stateful::object_ref_t source;
        authority_snapshot_t authority;
        bound_session_relocation_route_t session;
        host::session_relocation_seal_result_t seal;
        std::uint64_t seal_boundary_sequence = 0;
    };

    struct session_relocation_seal_outcome_t
    {
        bool completed = false;
        bool recovery_required = false;
        std::vector<session_relocation_checkpoint_t> checkpoints;
    };

    // Keep the remote Join coroutine frame small enough for GCC 13.  The
    // public entry point owns only local dispatch; remote admission and the
    // sealed transfer each have a separate named coroutine frame.
    struct remote_actor_join_state_t;

    task_t<actor_join_reply_t> join_remote_application_actor_to_spot (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> admit_remote_application_actor_join (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> seal_remote_application_actor_join (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> seal_remote_application_actor_join_call (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> prepare_remote_application_actor_join (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> finalize_remote_application_actor_join (
      std::shared_ptr<remote_actor_join_state_t> state);
    result_t<void> deliver_remote_actor_join (
      const remote_actor_join_state_t &state,
      const result_t<actor_join_reply_t> &joined);
    result_t<actor_join_reply_t> fail_remote_actor_join (
      const remote_actor_join_state_t &state,
      const result_t<actor_join_reply_t> &failed,
      std::string message);
    task_t<bool> abort_remote_actor_join_seal (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<actor_join_reply_t> complete_remote_application_actor_join (
      std::shared_ptr<remote_actor_join_state_t> state);
    task_t<runtime::messaging::message_parts_t> request_actor_join_spot_route (
      const runtime::spot_address_t &target,
      runtime::messaging::message_parts_t encoded,
      std::chrono::milliseconds timeout);

    //  Coroutine: parameters are taken by value so the frame owns them for
    //  the whole suspended seal exchange (callers pass temporaries).
    task_t<session_relocation_seal_outcome_t> seal_bound_sessions (
      std::vector<std::pair<runtime::stateful::object_ref_t,
                            authority_snapshot_t>> participants,
      runtime::protocol::relocation_id_t relocation,
      runtime::protocol::relocation_coordinator_fence_t coordinator,
      std::chrono::milliseconds timeout);
    task_t<std::optional<std::vector<
      runtime::protocol::session_relocation_route_t>>>
    capture_session_routes (
      std::vector<std::pair<runtime::stateful::object_ref_t,
                            authority_snapshot_t>> participants,
      runtime::protocol::relocation_id_t relocation,
      runtime::protocol::relocation_coordinator_fence_t coordinator,
      mesh_node_descriptor_t target,
      std::shared_ptr<session_relocation_seal_outcome_t> outcome,
      std::shared_ptr<bool> attempted);
    runtime::protocol::session_relocation_route_t
    make_session_relocation_route (
      const session_relocation_checkpoint_t &checkpoint,
      const zlink::routing_id_t &target_node,
      std::uint64_t target_node_generation,
      runtime::protocol::session_relocation_route_action_t action) const;
    task_t<bool> route_bound_sessions (
      const std::vector<session_relocation_checkpoint_t> &checkpoints,
      const mesh_node_descriptor_t &target,
      runtime::protocol::session_relocation_route_action_t action);

    struct peer_callback_gate_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool stopping = false;
        std::size_t active = 0;
    };

    struct message_follow_subscription_state_t;

    result_t<actor_join_reply_t>
    actor_join_reply_from_completion (const host::receive_record_t &record,
                                      const std::vector<zlink::message_t> &parts,
                                      const actor_ref_t &actor);
    result_t<actor_join_reply_t> wait_for_join_completion (const host::call_id_t &operation,
                                                           const actor_ref_t &actor,
                                                           std::chrono::milliseconds timeout);
    std::optional<zlink::submit_result_t>
    classify_node_direct_target (const zlink::routing_id_t &target) const;
    std::shared_ptr<mesh_node_builder_state_t> _state;
    serializer_registry_t *_serializers = nullptr;
    std::shared_ptr<location_repository_t> _user_spot_store;
    host::user_spot_materializer_t _user_spot_materializer;
    host::spot_route_fence_resolver_t _spot_route_fence_resolver;
    std::function<std::optional<runtime::spot_address_t> (const actor_ref_t &)>
      _actor_route_resolver;
    std::function<void (const runtime::protocol::actor_route_fence_t &)> _actor_route_invalidator;
    std::chrono::milliseconds _route_cache_max_age{15'000};
    std::chrono::milliseconds _owner_lease_fencing_margin{5'000};
    std::chrono::milliseconds _session_relocation_seal_timeout =
      location_options_t{}.session_relocation_seal_timeout;
    host::actor_create_operation_target_t _actor_create_target;
    host::instance_spot_activation_materializer_t _instance_spot_materializer;
    std::shared_ptr<runtime::stateful::relocation_store_port_t> _instance_spot_relocations;
    std::shared_ptr<runtime::stateful::authority_relocation_port_t> _relocation_authority;
    std::shared_ptr<runtime::stateful::relocation_store_port_t> _relocation_store;
    std::shared_ptr<runtime::stateful::aggregate_authority_port_t> _aggregate_relocation_authority;
    location_owner_token_t _instance_spot_owner;
    std::function<std::optional<location_owner_token_t> ()> _session_route_owner_resolver;
    host::public_host_runtime_t::session_route_target_owner_resolver_t
      _session_route_target_owner_resolver;
    std::function<std::optional<bound_session_relocation_route_t> (
      const runtime::stateful::object_ref_t &)>
      _bound_session_relocation_resolver;
    runtime::stateful::accepted_record_authority_resolver_t _stateful_dispatch_resolver;
    std::optional<host::bound_session_operations_t> _bound_session_operations;
    std::function<void (const std::map<std::string, int> &, int, std::uint64_t)>
      _descriptor_publisher;
    std::shared_ptr<host::public_host_runtime_t> _node;
    std::shared_ptr<peer_callback_gate_t> _peer_callback_gate =
      std::make_shared<peer_callback_gate_t> ();
    std::mutex _message_follow_mutex;
    message_follow_subscription_id_t _next_message_follow_subscription_id = 1;
    std::map<message_follow_subscription_id_t,
             std::shared_ptr<message_follow_subscription_state_t>>
      _message_follow_subscriptions;
    std::map<std::string, host::spot_handle_t> _spots;
    std::map<std::string, host::actor_handle_t> _actors;
    std::mutex _peer_mutex;
    std::atomic_uint64_t _pending_application_callbacks{0};
    std::atomic_uint64_t _active_application_callbacks{0};
    std::atomic_uint64_t _active_completion_waiters{0};
    std::map<std::string, std::uint64_t> _peer_connection_intents;
    std::mutex _completion_mutex;
    std::condition_variable _completion_ready;
    std::atomic_bool _stopping{false};
    static constexpr std::size_t timed_out_operation_capacity = 65'536;
    static constexpr std::size_t completion_capacity = 65'536;
    zlink::framework::runtime::exactly_once_table_t<host::call_id_t,
                                                    operation_completion_t,
                                                    zlink::framework::runtime::call_id_hash_t>
      _completed_operations{completion_capacity};
    std::unordered_map<host::call_id_t,
                       std::shared_ptr<detail::task_completion_source_t<operation_completion_t>>,
                       zlink::framework::runtime::call_id_hash_t>
      _completion_awaiters;
    // A timed-out waiter must leave a bounded tombstone. A late completion
    // must be dropped instead of recreating an orphan holding-slot entry.
    std::unordered_set<host::call_id_t,
                       zlink::framework::runtime::call_id_hash_t>
      _timed_out_operations;
    std::deque<host::call_id_t> _timed_out_operation_order;
    // A completion that cannot enter the bounded holding table must remain
    // observable by its waiter instead of silently degrading into a timeout.
    std::unordered_set<host::call_id_t,
                       zlink::framework::runtime::call_id_hash_t>
      _completion_overflow_operations;
    std::deque<host::call_id_t> _completion_overflow_order;
    struct actor_join_continuation_t
    {
        actor_ref_t actor;
        actor_join_completion_t completion;
    };
    std::unordered_map<host::call_id_t,
                       actor_join_continuation_t,
                       zlink::framework::runtime::call_id_hash_t>
      _actor_join_continuations;
};

} // namespace zlink::framework::detail
