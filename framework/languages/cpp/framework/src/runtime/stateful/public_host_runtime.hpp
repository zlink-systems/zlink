/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/foundation/operation_registry.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/operations/exactly_once_table.hpp"
#include "runtime/operations/operation_id.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::stateful
{
class raw_relocation_replay_coordinator_t;
}

namespace zlink::framework::runtime::host
{

using operation_id_t = runtime::operation_id_t;

using spot_request_completion_t = std::function<void (
  foundation::operation_terminal_t,
  result_t<std::vector<zlink::message_t>>)>;

enum class record_kind_t
{
    node_send,
    node_request,
    channel_send,
    channel_request,
    spot_send,
    spot_request,
    actor_send,
    actor_request,
    completion,
    send_ready,
    spot_control,
    spot_multicast
};

enum class ready_domain_t
{
    application,
    infrastructure
};

enum class owner_kind_t
{
    node,
    channel,
    spot,
    actor
};

enum class operation_kind_t
{
    none,
    actor_join
};

enum class lifecycle_kind_t
{
    joined,
    left
};

enum class actor_join_result_t
{
    accepted,
    rejected
};

enum class join_admission_t
{
    accepted,
    rejected
};

using route_fence_t = std::pair<std::uint64_t, std::uint64_t>;
using spot_route_fence_resolver_t = std::function<std::optional<route_fence_t> (
  const zlink::routing_id_t &, std::string_view, std::uint64_t)>;

struct actor_join_completion_t
{
    join_admission_t join_result = join_admission_t::rejected;
    actor_ref_t current_actor;
};

struct actor_control_t
{
    lifecycle_kind_t kind = lifecycle_kind_t::joined;
    actor_ref_t current_actor;
};

struct send_ready_data_t
{
    enum class destination_kind_t
    {
        node,
        channel,
        spot,
        actor,
        bound_session
    };

    destination_kind_t destination_kind = destination_kind_t::node;
    zlink::routing_id_t target_node_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::string target_spot_id;
    std::string channel_name;
    actor_ref_t target_actor;
};

class public_host_runtime_t;

struct reply_token_t
{
    std::weak_ptr<public_host_runtime_t> host;
    std::shared_ptr<mesh::service_mailbox_record_t> request;
    std::function<bool (const std::vector<zlink::message_t> &)>
      local_reply;
    std::function<bool (actor_join_result_t,
                        const std::vector<zlink::message_t> &)>
      local_actor_join;
};

struct receive_record_t
{
    record_kind_t kind = record_kind_t::node_send;
    ready_domain_t domain = ready_domain_t::application;
    operation_id_t operation_id;
    operation_kind_t operation_kind = operation_kind_t::none;
    zlink::routing_id_t source_node_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::uint64_t source_binding_generation = 0;
    /* Preserve the target fence until the Spot owner admits the message.
     * The owner must be able to reject stale work before body deserialization. */
    std::optional<protocol::spot_route_fence_t> spot_route;
    std::optional<protocol::actor_route_fence_t> actor_route;
    std::uint8_t message_follow_hop_count = 0;
    std::uint64_t reply_route_id = 0;
    std::string channel_name;
    std::string topic;
    int terminal_result = 0;
    int failure_errno = 0;
    reply_token_t reply_token;
    std::optional<actor_join_completion_t> join_completion;
    std::optional<actor_control_t> actor_control;
    std::optional<send_ready_data_t> send_ready;
    /* An application mailbox claim remains reserved until the framework
     * handler reaches a terminal result. A callback that submits the record
     * asynchronously calls retain_mailbox_reservation before returning and
     * calls release_mailbox_reservation at that terminal boundary.
     * Infrastructure and local records leave both callbacks empty. */
    std::function<void ()> retain_mailbox_reservation;
    std::function<void ()> release_mailbox_reservation;
    std::function<void ()> complete_stateful_dispatch;
};

struct ready_record_t
{
    owner_kind_t owner_kind = owner_kind_t::node;
    ready_domain_t domain = ready_domain_t::application;
    std::string spot_id;
    std::optional<actor_ref_t> actor;
    std::string channel_name;
};

struct node_status_t
{
    enum class state_t
    {
        preparing,
        serving,
        draining,
        stopped,
        error
    };

    state_t state = state_t::stopped;
    zlink::routing_id_t node_routing_id =
      zlink::routing_id_t::from (std::uint32_t{0});
    std::string endpoint;
    std::uint64_t generation = 0;

    zlink::routing_id_t routing_id () const;
    std::string local_endpoint () const;
    std::uint64_t lifecycle_generation () const noexcept;
};

struct spot_status_t
{
    std::uint64_t generation = 0;
    std::uint64_t lifecycle_generation () const noexcept;
};

struct host_options_t
{
    mesh::raw_mesh_node_options_t mesh;
    std::string entry_spot_name = "entry";
    std::set<std::string> object_stable_types;
    std::chrono::milliseconds route_cache_max_age{15'000};
    std::size_t user_spot_operation_capacity = 65'536;
    std::chrono::milliseconds user_spot_operation_replay_retention =
      std::chrono::minutes (5);
};

struct user_spot_materialize_result_t
{
    bool accepted = false;
    std::optional<protocol::application_payload_t> application_reply;
};

using user_spot_materializer_t = std::function<
  user_spot_materialize_result_t (
    const stateful::object_ref_t &,
    const std::string &,
    const std::vector<std::byte> &)>;

using user_spot_create_completion_t = std::function<void (
  foundation::operation_terminal_t,
  protocol::user_spot_create_reply_t,
  std::optional<protocol::application_payload_t>)>;

struct actor_create_operation_result_t
{
    protocol::actor_create_reply_t reply;
    std::optional<protocol::application_payload_t> application_reply;
};

using actor_create_operation_target_completion_t = std::function<void (
  actor_create_operation_result_t)>;

using actor_create_operation_target_t = std::function<
  void (const protocol::actor_create_header_t &,
        actor_create_operation_target_completion_t)>;

using actor_create_operation_completion_t = std::function<void (
  foundation::operation_terminal_t,
  protocol::actor_create_reply_t,
  std::optional<protocol::application_payload_t>)>;

using user_spot_close_completion_t = std::function<void (
  foundation::operation_terminal_t,
  protocol::user_spot_close_reply_t)>;

using session_relocation_route_completion_t = std::function<void (
  foundation::operation_terminal_t,
  std::optional<protocol::session_relocation_routed_t>)>;

struct session_relocation_seal_result_t
{
    protocol::session_relocation_sealed_t sealed;
    stateful::durable_session_journal_root_t journal_root;

    friend bool operator== (const session_relocation_seal_result_t &,
                            const session_relocation_seal_result_t &) = default;
};

using session_relocation_journal_capture_t =
  std::function<std::vector<std::uint8_t> ()>;
using session_relocation_seal_completion_t = std::function<void (
  foundation::operation_terminal_t,
  std::optional<session_relocation_seal_result_t>)>;

struct instance_spot_activation_result_t
{
    std::uint32_t terminal_result = 0;
    std::uint32_t failure_code = 0;
    std::optional<protocol::application_payload_t> application_reply;
};

struct instance_spot_activation_materializer_t
{
    std::function<bool (
      const protocol::instance_spot_activation_header_t &)> prepare;
    std::function<instance_spot_activation_result_t (
      const protocol::instance_spot_activation_header_t &,
      const std::optional<std::vector<std::uint8_t>> &,
      const protocol::application_payload_t &)> dispatch;

    explicit operator bool () const noexcept
    {
        return static_cast<bool> (prepare)
               && static_cast<bool> (dispatch);
    }
};

using instance_spot_activation_completion_t = std::function<void (
  foundation::operation_terminal_t,
  protocol::reply_header_t,
  std::optional<protocol::application_payload_t>)>;

enum class actor_transfer_role_t
{
    source,
    target
};

struct actor_transfer_prepare_t
{
    actor_transfer_role_t role = actor_transfer_role_t::source;
    std::string transfer_id;
    actor_ref_t actor;
    std::string source_spot_id;
    std::string target_spot_id;
    std::uint64_t target_spot_generation = 0;
    zlink::routing_id_t target_node_rid =
      zlink::routing_id_t::from (std::uint32_t{0});
};

struct actor_transfer_prepare_result_t
{
    actor_ref_t current_actor;
    std::uint64_t membership_epoch = 0;
};

class actor_transfer_token_t
{
  public:
    actor_transfer_token_t () = default;
    bool valid () const noexcept;
    bool commit (std::uint64_t membership_epoch);
    bool activate ();
    void abort () noexcept;

  private:
    friend class public_host_runtime_t;
    std::weak_ptr<public_host_runtime_t> _host;
    stateful::membership_token_t _membership;
    actor_transfer_role_t _role = actor_transfer_role_t::source;
    std::uint64_t _membership_epoch = 0;
    bool _terminal = false;
};

class spot_handle_t
{
  public:
    spot_handle_t () = default;
    spot_handle_t (std::shared_ptr<public_host_runtime_t> host,
                   stateful::object_ref_t object);

    spot_status_t status () const;
    const std::string &spot_id () const noexcept;
    zlink::submit_result_t send_to_spot (
      const zlink::routing_id_t &target_node_rid,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      zlink::send_flags_t flags = zlink::send_flags_t::none,
      std::span<const std::uint8_t> metadata = {});
    zlink::submit_result_t request_to_spot (
      const zlink::routing_id_t &target_node_rid,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      zlink::send_flags_t flags,
      std::chrono::milliseconds timeout,
      std::span<const std::uint8_t> metadata = {},
      spot_request_completion_t completion = {});
    zlink::submit_result_t publish (
      const std::string &channel_name,
      const std::string &topic,
      const std::vector<zlink::message_t> &parts,
      zlink::send_flags_t flags = zlink::send_flags_t::none,
      std::span<const std::uint8_t> metadata = {});
    void set_subscription (const std::string &channel_name,
                           const std::string &topic);
    void unset_subscription (const std::string &channel_name,
                             const std::string &topic);
    bool close () noexcept;

  private:
    std::shared_ptr<public_host_runtime_t> _host;
    stateful::object_ref_t _object;
};

class actor_handle_t
{
  public:
    actor_handle_t () = default;
    actor_handle_t (std::shared_ptr<public_host_runtime_t> host,
                    actor_ref_t actor,
                    stateful::object_ref_t object);

    const actor_ref_t &ref () const noexcept;
    zlink::submit_result_t join_entry_spot (
      const zlink::routing_id_t &target_node_rid,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      std::chrono::milliseconds timeout);
    zlink::submit_result_t join_spot (
      const zlink::routing_id_t &target_node_rid,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      std::chrono::milliseconds timeout);
    zlink::submit_result_t send_to (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      zlink::send_flags_t flags = zlink::send_flags_t::none,
      std::span<const std::uint8_t> metadata = {});
    zlink::submit_result_t request_to (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      zlink::send_flags_t flags,
      std::chrono::milliseconds timeout,
      std::span<const std::uint8_t> metadata = {});

  private:
    std::shared_ptr<public_host_runtime_t> _host;
    actor_ref_t _actor;
    stateful::object_ref_t _object;
};

class public_host_runtime_t :
    public std::enable_shared_from_this<public_host_runtime_t>
{
  public:
    explicit public_host_runtime_t (host_options_t options);
    ~public_host_runtime_t ();

    public_host_runtime_t (const public_host_runtime_t &) = delete;
    public_host_runtime_t &operator= (const public_host_runtime_t &) = delete;

    void start ();
    void close () noexcept;
    bool connect_peer (const std::string &endpoint,
                       std::optional<zlink::routing_id_t> expected = std::nullopt,
                       std::uint64_t expected_lifecycle_generation = 0,
                       std::string security_identity = "default");
    void expect_peer (const std::string &endpoint,
                      const zlink::routing_id_t &expected,
                      std::uint64_t expected_lifecycle_generation,
                      std::string security_identity);
    void forget_peer (const std::string &endpoint,
                      const zlink::routing_id_t &expected);
    void disconnect_peer (const std::string &endpoint) noexcept;
    node_status_t status () const;
    void set_channel_weight (const std::string &channel_name,
                             std::uint32_t weight);
    mesh::raw_mesh_node_owner_t &transport () noexcept;
    bool send_message_follow (
      const std::vector<std::uint8_t> &target_routing_id,
      const protocol::message_follow_notice_t &notice);
    stateful::stateful_object_runtime_t &objects () noexcept;
    stateful::stream_session_registry_t &sessions () noexcept;
    void configure_maintenance (
      stateful::maintenance_provider_set_t providers,
      stateful::relocation_limits_t limits = {},
      stateful::maintenance_runtime_t::observer_t relocation_observer = {},
      stateful::host_maintenance_runtime_t::observer_t
        termination_observer = {});
    void configure_relocation (
      std::shared_ptr<stateful::authority_relocation_port_t> authority,
      std::shared_ptr<stateful::relocation_store_port_t> relocations,
      std::shared_ptr<stateful::aggregate_authority_port_t>
        aggregate_authority = {},
      stateful::relocation_limits_t limits = {},
      stateful::maintenance_runtime_t::observer_t relocation_observer = {});
    stateful::maintenance_runtime_t *maintenance () noexcept;
    stateful::host_maintenance_runtime_t *termination () noexcept;
    stateful::raw_relocation_replay_coordinator_t &
    relocation_wire () noexcept;
    void configure_user_spot_operations (
      std::shared_ptr<zlink::framework::location_repository_t> store,
      user_spot_materializer_t materializer);
    void configure_spot_route_fence_resolver (
      spot_route_fence_resolver_t resolver);
    void configure_actor_create_operations (
      actor_create_operation_target_t target);
    void configure_instance_spot_operations (
      std::shared_ptr<zlink::framework::location_repository_t> store,
      std::shared_ptr<stateful::relocation_store_port_t> relocations,
      location_owner_token_t owner,
      instance_spot_activation_materializer_t materializer);
    bool evict_instance_spot (
      const std::string &stable_type,
      const std::string &spot_id,
      std::uint64_t object_generation,
      std::uint64_t authority_owner_generation,
      std::function<bool ()> close_local);
    void configure_session_route_owner (
      std::function<std::optional<location_owner_token_t> ()>
        owner_resolver);
    void configure_stateful_dispatch (
      std::function<std::optional<stateful::accepted_record_authority_t> (
        const stateful::accepted_record_authority_query_t &)> resolver);
    void configure_session_relocation_store (
      std::shared_ptr<stateful::relocation_store_port_t> relocations);
    void configure_message_follow_handler (
      std::function<void (const protocol::message_follow_notice_t &)> handler);
    bool seal_session_remote (
      const zlink::routing_id_t &session_owner_node,
      protocol::session_relocation_seal_t seal,
      std::chrono::milliseconds timeout,
      session_relocation_journal_capture_t capture_journal,
      session_relocation_seal_completion_t completion);
    bool activate_instance_spot_remote (
      const zlink::routing_id_t &target_node,
      protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      protocol::application_payload_t application_payload,
      std::chrono::milliseconds timeout,
      instance_spot_activation_completion_t completion);
    bool send_instance_spot_activation_remote (
      const zlink::routing_id_t &target_node,
      protocol::instance_spot_activation_header_t request,
      std::optional<std::vector<std::uint8_t>> metadata,
      protocol::application_payload_t application_payload);
    bool prepare_relocation_remote (
      const zlink::routing_id_t &target_node,
      protocol::relocation_prepare_t prepare,
      std::chrono::milliseconds timeout);
    bool complete_relocation_remote (
      const zlink::routing_id_t &target_node,
      protocol::relocation_complete_t complete,
      protocol::request_source_fence_t expected_target,
      std::chrono::milliseconds timeout,
      bool wait_for_target);
    bool complete_relocated_source (
      const stateful::object_ref_t &owner,
      std::uint64_t sequence,
      const protocol::reply_relay_t &relay,
      const std::optional<protocol::application_payload_t> &reply);
    bool acknowledge_relocated_source (
      const stateful::object_ref_t &owner,
      const protocol::wire_operation_id_t &operation);
    stateful::stateful_error_t ingest_stateful (
      const stateful::object_ref_t &owner);
    bool route_session_remote (
      const zlink::routing_id_t &session_owner_node,
      protocol::session_relocation_route_t route,
      std::chrono::milliseconds timeout,
      session_relocation_route_completion_t completion);
    std::size_t recover_instance_spot_activations ();
    bool create_user_spot_remote (
      const zlink::routing_id_t &target_node,
      protocol::user_spot_create_header_t request,
      std::chrono::milliseconds timeout,
      user_spot_create_completion_t completion);
    bool create_actor_remote (
      const zlink::routing_id_t &target_node,
      protocol::actor_create_header_t request,
      std::chrono::milliseconds timeout,
      actor_create_operation_completion_t completion);
    bool close_user_spot_remote (
      const zlink::routing_id_t &target_node,
      protocol::user_spot_close_header_t request,
      std::chrono::milliseconds timeout,
      user_spot_close_completion_t completion);

    spot_handle_t entry_spot ();
    spot_handle_t get_or_create_spot (std::string spot_id);
    actor_handle_t create_actor (std::string actor_type, std::string actor_id);
    actor_handle_t create_reserved_actor (
      std::string actor_type,
      stateful::object_ref_t reserved);
    zlink::submit_result_t send_to_actor (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      std::span<const std::uint8_t> metadata = {},
      std::uint64_t authority_owner_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    zlink::submit_result_t request_to_actor (
      const actor_ref_t &target,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      std::chrono::milliseconds timeout,
      std::span<const std::uint8_t> metadata = {},
      std::uint64_t authority_owner_generation = 0,
      std::uint64_t owner_lease_generation = 0);
    zlink::submit_result_t send_to_node (
      const zlink::routing_id_t &target,
      const std::vector<zlink::message_t> &parts);
    zlink::submit_result_t request_to_node (
      const zlink::routing_id_t &target,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      std::chrono::milliseconds timeout);
    zlink::submit_result_t send_to_channel (
      const std::string &channel_name,
      const std::vector<zlink::message_t> &parts);
    zlink::submit_result_t request_to_channel (
      const std::string &channel_name,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation,
      std::chrono::milliseconds timeout);
    std::size_t dispatch_ready (
      const std::function<void (const ready_record_t &,
                                const receive_record_t &,
                                std::vector<zlink::message_t>)> &dispatch,
      bool accept_application_receive = true);
    bool wait_for_dispatch_activity (
      std::chrono::milliseconds timeout,
      bool accept_application_receive = true) noexcept;
    bool prepare_actor_transfer (const actor_transfer_prepare_t &prepare,
                                 actor_transfer_token_t &token,
                                 actor_transfer_prepare_result_t &result);
    bool reply (const reply_token_t &token,
                const std::vector<zlink::message_t> &parts);

    static actor_ref_t remote_actor_ref (
      const zlink::routing_id_t &node,
      std::string actor_id,
      std::uint64_t generation)
    {
        return ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (node.to_string ()), {},
          std::move (actor_id), generation);
    }

    std::optional<stateful::object_ref_t>
    resolve_actor (const actor_ref_t &actor) const;
    std::optional<stateful::object_ref_t>
    resolve_spot (const std::string &spot_id) const;

  private:
    friend class spot_handle_t;
    friend class actor_handle_t;
    friend class actor_transfer_token_t;

    protocol::application_payload_t encode_application (
      const std::vector<zlink::message_t> &parts,
      std::span<const std::uint8_t> metadata = {}) const;
    std::vector<zlink::message_t> decode_application (
      const protocol::application_payload_t &payload) const;
    actor_ref_t framework_actor_ref (
      const stateful::object_ref_t &object,
      std::string actor_type) const;
    operation_id_t next_operation ();
    bool try_reserve_completion (operation_id_t operation);
    void release_completion (operation_id_t operation) noexcept;
    bool enqueue_completion (
      operation_id_t operation,
      receive_record_t record,
      std::vector<zlink::message_t> parts);
    zlink::submit_result_t begin_local_actor_join (
      const actor_ref_t &actor,
      const std::string &target_spot_id,
      std::uint64_t target_spot_generation,
      const std::vector<zlink::message_t> &parts,
      operation_id_t &operation);
    bool complete_local_actor_join (
      operation_id_t operation,
      std::string actor_type,
      stateful::membership_token_t membership,
      actor_join_result_t result,
      const std::vector<zlink::message_t> &parts);
    bool enqueue_local_actor_message (
      const actor_ref_t &target,
      record_kind_t kind,
      const std::vector<zlink::message_t> &parts,
      std::optional<operation_id_t> operation = std::nullopt);
    zlink::submit_result_t enqueue_local_spot_request (
      const protocol::spot_route_fence_t &target,
      const std::vector<zlink::message_t> &parts,
      operation_id_t operation,
      std::chrono::milliseconds timeout,
      std::span<const std::uint8_t> metadata,
      spot_request_completion_t completion = {});
    void expire_local_spot_requests () noexcept;
    void terminate_local_spot_requests (
      foundation::operation_terminal_t terminal) noexcept;
    bool finish_local_spot_request (
      operation_id_t operation,
      foundation::operation_terminal_t terminal,
      result_t<std::vector<zlink::message_t>> result) noexcept;
    std::optional<std::chrono::steady_clock::time_point>
    next_local_spot_request_deadline () const;
    std::optional<route_fence_t> resolve_spot_route_fence (
      const zlink::routing_id_t &target_node_rid,
      std::string_view target_spot_id,
      std::uint64_t target_spot_generation);
    void invalidate_spot_route_fence (
      const protocol::message_follow_notice_t &notice);
    bool complete_local_request (
      operation_id_t operation,
      const std::vector<zlink::message_t> &parts);
    void complete_operation (operation_id_t operation,
                             operation_kind_t kind,
                             foundation::operation_terminal_t terminal,
                             std::vector<std::uint8_t> payload);
    std::size_t dispatch_user_spot_operations ();

    host_options_t _options;
    std::string _entry_spot_id;
    std::shared_ptr<mesh::raw_mesh_node_owner_t> _transport;
    std::unique_ptr<stateful::raw_relocation_replay_coordinator_t>
      _relocation_wire;
    std::unique_ptr<stateful::raw_stateful_dispatch_t>
      _stateful_dispatch;
    stateful::stateful_object_runtime_t _objects;
    stateful::stream_session_registry_t _sessions;
    std::unique_ptr<stateful::maintenance_runtime_t> _maintenance;
    std::unique_ptr<stateful::host_maintenance_runtime_t> _termination;
    std::shared_ptr<zlink::framework::location_repository_t>
      _user_spot_store;
    user_spot_materializer_t _user_spot_materializer;
    spot_route_fence_resolver_t _spot_route_fence_resolver;
    struct cached_spot_route_fence_t
    {
        route_fence_t fence;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::mutex _route_cache_mutex;
    std::map<std::string, cached_spot_route_fence_t> _spot_route_fences;
    actor_create_operation_target_t _actor_create_target;
    instance_spot_activation_materializer_t
      _instance_spot_materializer;
    std::shared_ptr<stateful::relocation_store_port_t>
      _instance_spot_relocations;
    location_owner_token_t _instance_spot_owner;
    std::function<std::optional<location_owner_token_t> ()>
      _session_route_owner_resolver;
    std::function<void (const protocol::message_follow_notice_t &)>
      _message_follow_handler;
    std::shared_ptr<stateful::relocation_store_port_t>
      _session_relocations;
    struct session_seal_terminal_record_t
    {
        protocol::session_relocation_seal_t seal;
        protocol::session_relocation_sealed_t sealed;
        stateful::stream_barrier_t barrier;
        bool consumed = false;
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             session_seal_terminal_record_t>
      _session_seal_terminals;
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             std::pair<protocol::session_relocation_seal_t,
                       session_relocation_seal_result_t>>
      _session_journal_terminals;
    std::map<
      std::pair<std::uint64_t, std::uint64_t>,
      std::pair<protocol::session_relocation_route_t,
                protocol::session_relocation_routed_t>>
      _session_route_terminals;
    using relocation_attempt_key_t =
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    struct relocation_target_attempt_t;
    bool try_finalize_relocation_target (
      const relocation_attempt_key_t &key);
    bool send_relocation_target_terminal (
      const relocation_attempt_key_t &key);
    bool relocation_target_authority_committed (
      const relocation_target_attempt_t &attempt) const noexcept;
    struct relocation_target_attempt_t
    {
        protocol::relocation_prepare_t prepare;
        stateful::relocation_restore_identity_t restore_identity;
        std::vector<stateful::object_ref_t> targets;
        std::map<std::uint64_t, std::uint64_t> expected_high_water;
        bool reserved = false;
        std::optional<protocol::relocation_complete_t> completion;
        std::vector<std::uint8_t> completion_source_routing_id;
        bool target_finalized = false;
        std::optional<protocol::relocation_complete_t> terminal_response;
        std::chrono::steady_clock::time_point attempt_expires_at{};
    };
    std::vector<relocation_target_attempt_t>
    take_expired_relocation_target_attempts_locked (
      std::chrono::steady_clock::time_point now);
    void expire_relocation_target_attempts ();
    void cleanup_expired_relocation_target_attempts (
      std::vector<relocation_target_attempt_t> attempts) noexcept;
    struct relocation_completion_wait_t
    {
        protocol::relocation_coordinator_fence_t coordinator;
        protocol::request_source_fence_t expected_target;
        protocol::source_cleanup_state_t source_cleanup_state =
          protocol::source_cleanup_state_t::pending;
        bool accepted = false;
    };
    std::map<relocation_attempt_key_t, protocol::relocation_ready_t>
      _relocation_ready_responses;
    std::map<relocation_attempt_key_t, relocation_completion_wait_t>
      _relocation_complete_responses;
    std::map<relocation_attempt_key_t, relocation_target_attempt_t>
      _relocation_target_attempts;
    std::shared_ptr<stateful::authority_relocation_port_t>
      _relocation_authority;
    static constexpr std::size_t relocation_terminal_capacity = 65'536;
    static constexpr auto relocation_attempt_retention =
      std::chrono::minutes (5);
    std::condition_variable _relocation_changed;
    struct user_spot_terminal_record_t
    {
        protocol::command kind = protocol::command::userSpotCreate;
        std::uint64_t deadline_unix_ms = 0;
        std::vector<std::uint8_t> request_fingerprint;
        std::vector<std::uint8_t> header;
        std::optional<protocol::application_payload_t> application_reply;
    };
    std::map<std::string, user_spot_terminal_record_t>
      _user_spot_terminals;
    std::function<void ()> _maintenance_started;
    std::function<void ()> _maintenance_closing;
    mutable std::mutex _mutex;
    static constexpr std::size_t completion_capacity = 65'536;
    using completion_value_t =
      std::pair<receive_record_t, std::vector<zlink::message_t>>;
    zlink::framework::runtime::exactly_once_table_t<
      operation_id_t,
      completion_value_t,
      zlink::framework::runtime::operation_id_hash_t>
      _completions{completion_capacity};
    using local_spot_deadline_index_t =
      std::multimap<std::chrono::steady_clock::time_point, operation_id_t>;
    struct local_spot_request_state_t
    {
        std::chrono::steady_clock::time_point deadline;
        std::size_t payload_bytes = 0;
        spot_request_completion_t completion;
        local_spot_deadline_index_t::iterator deadline_index;
        bool queued = true;
        bool terminal_claimed = false;
    };
    std::unordered_map<
      operation_id_t,
      local_spot_request_state_t,
      zlink::framework::runtime::operation_id_hash_t>
      _local_spot_requests;
    local_spot_deadline_index_t _local_spot_request_deadlines;
    std::size_t _local_spot_request_bytes = 0;
    struct local_application_dispatch_t
    {
        ready_record_t owner;
        receive_record_t record;
        std::vector<zlink::message_t> parts;
    };
    std::deque<local_application_dispatch_t>
      _local_application_dispatches;
    std::map<std::string, stateful::object_ref_t> _spots;
    std::map<std::string, std::pair<std::string, stateful::object_ref_t>> _actors;
    std::map<std::string, std::string> _peer_endpoints;
    std::uint64_t _next_operation = 1;
    bool _started = false;
    bool _closing = false;
};

zlink::submit_result_t reply (const reply_token_t &token,
                              const std::vector<zlink::message_t> &parts);
bool actor_join_reply (const reply_token_t &token,
                       actor_join_result_t result,
                       const std::vector<zlink::message_t> &parts);

using mesh_node_t = public_host_runtime_t;
using spot_t = spot_handle_t;
using actor_t = actor_handle_t;

} // namespace zlink::framework::runtime::host
