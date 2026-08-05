/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/mesh_node.hpp>
#include <runtime/locations/location_repository.hpp>

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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zlink::framework
{
class zlink_builder_t;
}

namespace zlink::framework::detail
{
namespace host = zlink::framework::runtime::host;

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
    std::optional<std::string> advertise_host;
    std::optional<zlink::routing_id_t> routing_id;
    object_role_t object_role = object_role_t::server;
    bool has_node_direct_handler = false;
    int placement_weight = 100;
    std::int32_t actor_limit = 10000;
    std::int32_t spot_limit = 128;
    std::chrono::milliseconds instance_spot_idle_timeout{0};
    std::int32_t activation_concurrency_limit = 128;
    std::map<std::string, mesh_channel_registration_t> channels;
    std::function<void (const std::string &)> channel_name_observer;
    route_handler_registry_t handlers;
    std::vector<mesh_peer_connection_t> peer_connections;
    mesh_node_socket_config_t socket;
    std::chrono::milliseconds default_request_timeout{std::chrono::seconds (30)};
    zlink::auto_hwm_profile auto_hwm_profile =
      zlink::auto_hwm_profile::balanced;
    std::size_t max_pending = 1024;
    std::atomic<std::uint64_t> next_join_completion_operation{1};
    std::shared_ptr<spot_node_builder_state_t> spot_state;
    spot_node_builder_t spot_builder;
};

class mesh_node_runtime_t
{
  public:
    struct operation_completion_t
    {
        host::receive_record_t record;
        std::vector<zlink::message_t> parts;
    };
    using actor_join_completion_t = std::function<void (
      result_t<actor_join_reply_t>)>;
    explicit mesh_node_runtime_t (std::shared_ptr<mesh_node_builder_state_t> state);
    ~mesh_node_runtime_t ();

    mesh_node_runtime_t (const mesh_node_runtime_t &) = delete;
    mesh_node_runtime_t &operator= (const mesh_node_runtime_t &) = delete;

    void start ();
    void stop () noexcept;
    void bind_serializers (serializer_registry_t &serializers) noexcept;
    void bind_descriptor_publisher (
      std::function<void (const std::map<std::string, int> &,
                          int,
                          std::uint64_t)> publisher);
    void configure_user_spot_operations (
      std::shared_ptr<location_repository_t> store,
      host::user_spot_materializer_t materializer);
    void configure_spot_route_fence_resolver (
      host::spot_route_fence_resolver_t resolver,
      std::chrono::milliseconds route_cache_max_age);
    void configure_actor_route_resolver (
      std::function<std::optional<runtime::spot_address_t> (
        const actor_ref_t &)> resolver,
      std::function<void (const runtime::protocol::actor_route_fence_t &)>
        invalidator = {});
    void configure_actor_create_operations (
      host::actor_create_operation_target_t target);
    void configure_instance_spot_operations (
      std::shared_ptr<location_repository_t> store,
      std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
      location_owner_token_t owner,
      host::instance_spot_activation_materializer_t materializer);
    void configure_relocation_runtime (
      std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority,
      std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
      std::shared_ptr<runtime::stateful::aggregate_authority_port_t>
        aggregate_authority = {});
    runtime::stateful::relocation_result_t relocate_application_actor (
      const actor_ref_t &actor,
      const mesh_node_descriptor_t &target,
      const authority_snapshot_t &authority,
      relocation_capacity_fence_t capacity_fence);
    bool application_actor_transfer_in_progress (
      const actor_ref_t &actor) const;
    result_t<bool> destroy_application_actor (const actor_ref_t &actor);
    runtime::stateful::aggregate_relocation_result_t
    relocate_application_unit (
      std::vector<runtime::stateful::object_ref_t> sources,
      std::vector<std::string> stable_types,
      const mesh_node_descriptor_t &target,
      const std::vector<authority_snapshot_t> &authorities,
      std::vector<relocation_capacity_fence_t> capacity_fences);
    void configure_session_route_owner (
      std::function<std::optional<location_owner_token_t> ()>
        owner_resolver);
    void configure_stateful_dispatch (
      runtime::stateful::accepted_record_authority_resolver_t resolver);
    void set_message_follow_invalidation_handler (
      std::function<void (const runtime::protocol::message_follow_notice_t &)>
        handler);
    bool activate_instance_spot_remote (
      const zlink::routing_id_t &target_node,
      zlink::framework::runtime::protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      zlink::framework::runtime::protocol::application_payload_t application_payload,
      std::chrono::milliseconds timeout,
      host::instance_spot_activation_completion_t completion);
    bool send_instance_spot_activation_remote (
      const zlink::routing_id_t &target_node,
      zlink::framework::runtime::protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      zlink::framework::runtime::protocol::application_payload_t application_payload);
    void connect_peer (const zlink::routing_id_t &expected_routing_id,
                       const std::string &endpoint,
                       std::uint64_t expected_lifecycle_generation = 0,
                       std::string security_identity = "default");
    void expect_peer (const zlink::routing_id_t &expected_routing_id,
                      const std::string &endpoint,
                      std::uint64_t expected_lifecycle_generation,
                      std::string security_identity);
    void forget_peer (const zlink::routing_id_t &expected_routing_id,
                      const std::string &endpoint);
    void disconnect_peer (const std::string &endpoint) noexcept;
    bool wait_for_peer_ready (
      const zlink::routing_id_t &target,
      std::chrono::milliseconds timeout) const;

    zlink::submit_result_t send_to_node (const zlink::routing_id_t &target,
                                         const std::vector<zlink::message_t> &parts,
                                         std::vector<std::uint8_t> metadata = {});
    zlink::submit_result_t
    send_to_node (const zlink::routing_id_t &target,
                  const std::vector<zlink::message_t> &parts,
                  const std::map<std::string, std::string> &metadata);
    zlink::submit_result_t request_to_node (
      const zlink::routing_id_t &target,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      std::vector<std::uint8_t> metadata = {});
    zlink::submit_result_t request_to_node (
      const zlink::routing_id_t &target,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      const std::map<std::string, std::string> &metadata);
    zlink::submit_result_t send_to_channel (const std::string &channel_name,
                                            const std::vector<zlink::message_t> &parts,
                                            std::vector<std::uint8_t> metadata = {});
    zlink::submit_result_t
    send_to_channel (const std::string &channel_name,
                     const std::vector<zlink::message_t> &parts,
                     const std::map<std::string, std::string> &metadata);
    zlink::submit_result_t request_to_channel (
      const std::string &channel_name,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      std::vector<std::uint8_t> metadata = {});
    zlink::submit_result_t request_to_channel (
      const std::string &channel_name,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      const std::map<std::string, std::string> &metadata);
    host::spot_handle_t get_or_create_spot (std::string spot_id);
    zlink::submit_result_t send_to_spot (
      const std::string &source_spot_id,
      const zlink::routing_id_t &target_node_rid,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      std::vector<std::uint8_t> metadata = {});
    zlink::submit_result_t request_to_spot (
      const std::string &source_spot_id,
      const zlink::routing_id_t &target_node_rid,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      std::vector<std::uint8_t> metadata = {});
    host::actor_handle_t create_actor (
      std::string actor_type,
      std::string actor_id,
      const std::vector<zlink::message_t> &creation_parts = {},
      std::chrono::milliseconds timeout = {});
    zlink::submit_result_t send_to_actor (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      std::vector<std::uint8_t> metadata = {},
      std::uint64_t authority_owner_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    zlink::submit_result_t request_to_actor (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      host::operation_id_t &operation_id,
      std::chrono::milliseconds timeout,
      std::vector<std::uint8_t> metadata = {},
      std::uint64_t authority_owner_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    zlink::submit_result_t send_actor_bound_session (
      const actor_ref_t &actor,
      std::uint64_t expected_binding_generation,
      const std::vector<zlink::message_t> &parts);
    zlink::context_t &native_context ();
    host::public_host_runtime_t &native_node ();
    bool prepare_actor_transfer (
      const host::actor_transfer_prepare_t &prepare,
      std::chrono::milliseconds timeout,
      host::actor_transfer_token_t &token,
      host::actor_transfer_prepare_result_t &result);
    result_t<actor_ref_t> create_application_actor (
      std::string actor_type,
      std::string actor_id,
      const std::optional<zlink::message_t> &creation_payload,
      std::chrono::milliseconds timeout);
    result_t<actor_ref_t> create_application_actor (
      std::string actor_type,
      std::string actor_id,
      const std::optional<zlink::message_t> &creation_payload,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation,
      std::chrono::milliseconds timeout);
    result_t<actor_join_reply_t> join_application_actor_to_entry_spot (
      const actor_ref_t &actor,
      const node_rid_t &target_node,
      const zlink::message_t &request,
      std::chrono::milliseconds timeout);
    result_t<void> submit_application_actor_entry_spot_join (
      const actor_ref_t &actor,
      const node_rid_t &target_node,
      const zlink::message_t &request,
      std::chrono::milliseconds timeout,
      actor_join_completion_t completion);
    bool complete_application_actor_entry_spot_join (
      const host::receive_record_t &record,
      const std::vector<zlink::message_t> &parts);
    result_t<actor_join_reply_t> join_application_actor_to_spot (
      actor_ref_t actor,
      const node_rid_t &target_node,
      const spot_id_t &target_spot,
      std::uint64_t target_spot_generation,
      const zlink::message_t &request,
      std::chrono::milliseconds timeout,
      std::optional<zlink::routing_id_t> bound_session_node_rid = std::nullopt,
      std::optional<zlink::routing_id_t> bound_session_rid = std::nullopt);
    result_t<std::shared_ptr<deferred_barrier_t>>
    reserve_application_actor_join_barrier (const actor_ref_t &actor);
    result_t<std::optional<zlink::message_t>> relay_application_actor (
      const actor_ref_t &actor,
      const stream_header_t &header,
      const zlink::message_t &payload,
      std::chrono::milliseconds timeout);
    result_t<std::optional<zlink::message_t>> relay_application_actor (
      const actor_ref_t &actor,
      const runtime::messaging::envelope_header_t &header,
      const zlink::message_t &payload,
      std::chrono::milliseconds timeout);
    result_t<std::optional<zlink::message_t>> relay_application_actor (
      const actor_ref_t &actor,
      const runtime::messaging::envelope_header_t &header,
      const zlink::message_t &payload,
      std::chrono::milliseconds timeout,
      const zlink::routing_id_t &source_node,
      const runtime::protocol::actor_route_fence_t &stale_route,
      std::uint8_t hop_count,
      const runtime::protocol::wire_operation_id_t &operation,
      std::uint64_t reply_route_id);
    result_t<void> bind_application_actor_session (
      const actor_ref_t &actor,
      const node_rid_t &session_node,
      std::chrono::milliseconds timeout);
    result_t<void> notify_application_actor_disconnected (
      const actor_ref_t &actor,
      const node_rid_t &target_node,
      std::chrono::milliseconds timeout);
    std::optional<actor_ref_t> follow_relocated_actor (const actor_ref_t &actor);
    result_t<operation_completion_t> wait_for_completion (
      const host::operation_id_t &operation,
      std::chrono::milliseconds timeout);
    std::size_t dispatch_ready (
      const std::function<void (const host::ready_record_t &,
                                const host::receive_record_t &,
                                std::vector<zlink::message_t>)> &dispatch,
      bool accept_application_receive = true);
    host::node_status_t status () const;
    void dispatch_message_follow (
      const runtime::protocol::message_follow_notice_t &notice);
    /* Admitted RouteMesh membership size. Vertical and E2E checks wait on this
     * instead of reaching into the transport topology. */
    std::size_t admitted_peer_count () const;
    bool has_admitted_peer (const zlink::routing_id_t &peer_rid,
                            std::uint64_t lifecycle_generation) const;
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

    static std::shared_ptr<mesh_node_runtime_t> from (zlink_builder_t &builder,
                                                      const std::string &mesh_name);
    static std::vector<std::shared_ptr<mesh_node_builder_state_t>>
    registrations (zlink_builder_t &builder);

  private:
    result_t<actor_join_reply_t> actor_join_reply_from_completion (
      const host::receive_record_t &record,
      const std::vector<zlink::message_t> &parts,
      const actor_ref_t &actor);
    result_t<actor_join_reply_t> wait_for_join_completion (
      const host::operation_id_t &operation,
      const actor_ref_t &actor,
      std::chrono::milliseconds timeout);
    std::optional<zlink::submit_result_t>
    classify_node_direct_target (
      const zlink::routing_id_t &target) const;
    std::shared_ptr<mesh_node_builder_state_t> _state;
    serializer_registry_t *_serializers = nullptr;
    std::shared_ptr<location_repository_t> _user_spot_store;
    host::user_spot_materializer_t _user_spot_materializer;
    host::spot_route_fence_resolver_t _spot_route_fence_resolver;
    std::function<std::optional<runtime::spot_address_t> (
      const actor_ref_t &)> _actor_route_resolver;
    std::function<void (const runtime::protocol::actor_route_fence_t &)>
      _actor_route_invalidator;
    std::chrono::milliseconds _route_cache_max_age{15'000};
    host::actor_create_operation_target_t _actor_create_target;
    host::instance_spot_activation_materializer_t
      _instance_spot_materializer;
    std::shared_ptr<runtime::stateful::relocation_store_port_t>
      _instance_spot_relocations;
    std::shared_ptr<runtime::stateful::authority_relocation_port_t>
      _relocation_authority;
    std::shared_ptr<runtime::stateful::relocation_store_port_t>
      _relocation_store;
    std::shared_ptr<runtime::stateful::aggregate_authority_port_t>
      _aggregate_relocation_authority;
    location_owner_token_t _instance_spot_owner;
    std::function<std::optional<location_owner_token_t> ()>
      _session_route_owner_resolver;
    runtime::stateful::accepted_record_authority_resolver_t
      _stateful_dispatch_resolver;
    std::function<void (const std::map<std::string, int> &,
                        int,
                        std::uint64_t)> _descriptor_publisher;
    std::shared_ptr<host::public_host_runtime_t> _node;
    std::mutex _message_follow_mutex;
    std::function<void (const runtime::protocol::message_follow_notice_t &)>
      _message_follow_handler;
    std::map<std::string, host::spot_handle_t> _spots;
    std::map<std::string, host::actor_handle_t> _actors;
    std::mutex _peer_mutex;
    std::atomic_uint64_t _pending_application_callbacks{0};
    std::atomic_uint64_t _active_application_callbacks{0};
    std::map<std::string, std::uint64_t> _peer_connection_intents;
    std::mutex _completion_mutex;
    std::condition_variable _completion_ready;
    std::atomic_bool _stopping{false};
    zlink::framework::runtime::exactly_once_table_t<
      host::operation_id_t,
      operation_completion_t,
      zlink::framework::runtime::operation_id_hash_t>
      _completed_operations;
    struct actor_join_continuation_t
    {
        actor_ref_t actor;
        actor_join_completion_t completion;
    };
    std::unordered_map<host::operation_id_t,
                       actor_join_continuation_t,
                       zlink::framework::runtime::operation_id_hash_t>
      _actor_join_continuations;
};

} // namespace zlink::framework::detail
