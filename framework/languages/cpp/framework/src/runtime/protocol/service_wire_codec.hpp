/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include "runtime/operations/call_id.hpp"

#include <service_wire_constants.hpp>

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::runtime::protocol
{

class service_wire_error_t : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

struct liveness_record_t
{
    command kind;
    std::uint64_t probe_id;
};

struct service_wire_header_t
{
    command kind;
    std::uint8_t flags;
};

struct application_payload_t
{
    std::string packet_name;
    std::string content_type;
    std::vector<std::uint8_t> payload;
    /* Flow context is framework-owned and is present only on a traced
     * activation payload. It is not application metadata. */
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;

    friend bool operator== (const application_payload_t &,
                            const application_payload_t &) = default;
};

struct spot_route_fence_t
{
    std::string spot_id;
    std::uint64_t object_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;

    friend bool operator== (const spot_route_fence_t &,
                            const spot_route_fence_t &) = default;
};

struct actor_route_fence_t
{
    std::string actor_id;
    std::uint64_t object_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;

    friend bool operator== (const actor_route_fence_t &,
                            const actor_route_fence_t &) = default;
};

enum class bound_session_binding_state_t : std::uint8_t
{
    active = 1,
    tombstone = 2
};

struct bound_session_binding_transition_t
{
    bound_session_binding_state_t state =
      bound_session_binding_state_t::active;
    std::uint64_t generation = 0;

    friend bool operator== (const bound_session_binding_transition_t &,
                            const bound_session_binding_transition_t &) = default;
};

struct bound_session_send_t
{
    actor_route_fence_t actor;
    std::uint64_t expected_binding_generation = 0;

    friend bool operator== (const bound_session_send_t &,
                            const bound_session_send_t &) = default;
};

struct bound_session_bind_t
{
    std::uint64_t correlation = 0;
    actor_route_fence_t actor;
    std::vector<std::uint8_t> session_routing_id;
    bound_session_binding_transition_t binding;

    friend bool operator== (const bound_session_bind_t &,
                            const bound_session_bind_t &) = default;
};

struct retired_bound_session_route_fence_t
{
    std::vector<std::uint8_t> session_owner_node_routing_id;
    std::uint64_t session_owner_node_generation = 0;
    std::string session_owner_id;
    std::uint64_t session_owner_lease_generation = 0;
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t retired_binding_generation = 0;

    friend bool operator== (const retired_bound_session_route_fence_t &,
                            const retired_bound_session_route_fence_t &) = default;
};

struct bound_session_replaced_t
{
    actor_route_fence_t actor_authority;
    retired_bound_session_route_fence_t retired_session;

    friend bool operator== (const bound_session_replaced_t &,
                            const bound_session_replaced_t &) = default;
};

struct wire_operation_id_t
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator== (const wire_operation_id_t &,
                            const wire_operation_id_t &) = default;
};

using message_follow_route_t =
  std::variant<actor_route_fence_t, spot_route_fence_t>;

struct message_follow_notice_t
{
    message_follow_route_t source;
    message_follow_route_t target;
    std::uint8_t hop_count = 0;
    std::uint32_t queued_messages = 0;
    std::uint32_t queued_bytes = 0;
    wire_operation_id_t original_operation;
    std::uint64_t original_reply_route_id = 0;

    friend bool operator== (const message_follow_notice_t &,
                            const message_follow_notice_t &) = default;
};

struct spot_message_header_t
{
    wire_operation_id_t operation;
    std::uint8_t message_follow_hop_count = 0;
    std::optional<std::uint64_t> correlation;
    std::string source_spot_id;
    spot_route_fence_t target;
};

struct actor_message_header_t
{
    struct bound_session_source_t
    {
        std::vector<std::uint8_t> session_routing_id;
        std::uint64_t binding_generation = 0;
        std::uint64_t session_sequence = 0;

        friend bool operator== (const bound_session_source_t &,
                                const bound_session_source_t &) = default;
    };

    wire_operation_id_t operation;
    std::uint8_t message_follow_hop_count = 0;
    std::optional<std::uint64_t> correlation;
    std::optional<std::pair<std::string, std::uint64_t>> source_actor;
    actor_route_fence_t target;
    std::optional<bound_session_source_t> bound_session_source;
};

struct relocation_id_t
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator== (const relocation_id_t &,
                            const relocation_id_t &) = default;
};

struct relocation_coordinator_fence_t
{
    std::string owner_id;
    std::uint64_t lease_generation = 0;
    std::vector<std::uint8_t> node_routing_id;
    std::uint64_t node_generation = 0;
    std::string expected_authority_store_version;

    friend bool operator== (const relocation_coordinator_fence_t &,
                            const relocation_coordinator_fence_t &) = default;
};

struct request_source_fence_t
{
    std::string owner_id;
    std::uint64_t lease_generation = 0;
    std::vector<std::uint8_t> node_routing_id;
    std::uint64_t node_generation = 0;

    friend bool operator== (const request_source_fence_t &,
                            const request_source_fence_t &) = default;
};

enum class relocation_role_t : std::uint8_t
{
    source = 1,
    target = 2,
    coordinator = 3
};

enum class relocation_object_kind_t : std::uint8_t
{
    actor = 1,
    user_spot = 2,
    instance_spot = 3
};

struct relocation_target_fence_t
{
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::string target_owner_id;
    std::uint64_t target_owner_lease_generation = 0;
    friend bool operator== (const relocation_target_fence_t &,
                            const relocation_target_fence_t &) = default;
};

struct relocation_object_t
{
    relocation_object_kind_t kind = relocation_object_kind_t::actor;
    std::string stable_type;
    std::string object_id;
    std::uint64_t object_generation = 0;
    std::uint64_t expected_authority_owner_generation = 0;
    friend bool operator== (const relocation_object_t &left,
                            const relocation_object_t &right) noexcept
    {
        return left.kind == right.kind
               && left.object_id == right.object_id
               && left.object_generation == right.object_generation
               && left.expected_authority_owner_generation
                    == right.expected_authority_owner_generation
               && (left.kind != relocation_object_kind_t::instance_spot
                   || left.stable_type == right.stable_type);
    }
};

struct relocation_prepare_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_target_fence_t target;
    relocation_role_t initiator_role = relocation_role_t::source;
    relocation_object_t object;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t source_node_generation = 0;
    std::uint64_t payload_total_length = 0;
    std::uint32_t payload_chunk_count = 0;
    std::uint32_t payload_checksum_crc32c = 0;
    std::uint64_t application_version = 0;
    friend bool operator== (const relocation_prepare_t &,
                            const relocation_prepare_t &) = default;
};

struct relocation_ready_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_target_fence_t target;
    relocation_object_t object;
    relocation_role_t sender_role = relocation_role_t::target;
    friend bool operator== (const relocation_ready_t &,
                            const relocation_ready_t &) = default;
};

/* Explicit target-side pre-cutover failure for a relocation PREPARE. */
struct relocation_failed_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_target_fence_t target;
    relocation_object_t object;
    relocation_role_t sender_role = relocation_role_t::target;
    std::uint32_t failure_code = 0;
    friend bool operator== (const relocation_failed_t &,
                            const relocation_failed_t &) = default;
};

struct relocation_cutover_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_role_t sender_role = relocation_role_t::source;
    relocation_object_t object;
    std::uint64_t boundary_record_count = 0;
    std::uint32_t boundary_checksum_crc32c = 0;
    friend bool operator== (const relocation_cutover_t &,
                            const relocation_cutover_t &) = default;
};

enum class frozen_record_kind_t : std::uint8_t
{
    node_send = 1,
    node_request = 2,
    channel_send = 3,
    channel_request = 4,
    spot_send = 5,
    spot_request = 6,
    spot_multicast = 7,
    spot_control = 8,
    actor_send = 9,
    actor_request = 10,
    completion = 11,
    send_ready = 12,
    relocation_control = 13,
    instance_spot_activation = 14
};

enum class frozen_source_kind_t : std::uint8_t
{
    node = 1,
    spot = 2,
    actor = 3,
    bound_session = 4
};

struct frozen_target_identity_t
{
    relocation_object_kind_t kind = relocation_object_kind_t::actor;
    std::string object_id;
    std::uint64_t object_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;
    friend bool operator== (const frozen_target_identity_t &,
                            const frozen_target_identity_t &) = default;
};

struct frozen_record_t
{
    frozen_record_kind_t kind = frozen_record_kind_t::node_send;
    frozen_source_kind_t source_kind = frozen_source_kind_t::node;
    request_source_fence_t source;
    std::optional<std::string> source_spot_id;
    std::optional<std::pair<std::string, std::uint64_t>> source_actor;
    std::optional<std::vector<std::uint8_t>> source_session_routing_id;
    std::uint64_t source_binding_generation = 0;
    std::uint64_t source_session_sequence = 0;
    bool has_metadata = false;
    wire_operation_id_t operation;
    std::uint32_t operation_kind = 0;
    std::optional<std::uint64_t> reply_route_id;
    std::optional<frozen_target_identity_t> target;
    std::optional<application_payload_t> application;
    std::vector<std::uint8_t> canonical_bytes;
    friend bool operator== (const frozen_record_t &,
                            const frozen_record_t &) = default;
};

struct frozen_metadata_entry_t
{
    std::string key;
    std::string value;
    friend bool operator== (const frozen_metadata_entry_t &,
                            const frozen_metadata_entry_t &) = default;
};

struct frozen_spot_application_body_t
{
    spot_route_fence_t target;
    std::uint64_t expected_owner_lease_generation = 0;
    application_payload_t application;

    friend bool operator== (const frozen_spot_application_body_t &,
                            const frozen_spot_application_body_t &) = default;
};

struct frozen_actor_application_body_t
{
    actor_route_fence_t target;
    application_payload_t application;

    friend bool operator== (const frozen_actor_application_body_t &,
                            const frozen_actor_application_body_t &) = default;
};

// Typed capture input for the application records that can be admitted to an
// Actor or Spot mailbox. Infrastructure records use their dedicated writers.
struct frozen_application_record_t
{
    frozen_record_kind_t kind = frozen_record_kind_t::spot_send;
    frozen_source_kind_t source_kind = frozen_source_kind_t::node;
    request_source_fence_t source;
    std::optional<std::string> source_spot_id;
    std::optional<std::pair<std::string, std::uint64_t>> source_actor;
    std::optional<std::vector<std::uint8_t>> source_session_routing_id;
    std::uint64_t source_binding_generation = 0;
    std::uint64_t source_session_sequence = 0;
    std::vector<frozen_metadata_entry_t> metadata;
    wire_operation_id_t operation;
    std::uint32_t operation_kind = 0;
    std::optional<std::uint64_t> reply_route_id;
    std::variant<frozen_spot_application_body_t,
                 frozen_actor_application_body_t> body;

    friend bool operator== (const frozen_application_record_t &,
                            const frozen_application_record_t &) = default;
};

struct relocation_data_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_role_t sender_role = relocation_role_t::source;
    relocation_object_t object;
    frozen_record_t record;
    friend bool operator== (const relocation_data_t &,
                            const relocation_data_t &) = default;
};

struct relocation_state_t
{
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    relocation_role_t sender_role = relocation_role_t::source;
    relocation_object_t object;
    std::uint32_t chunk_ordinal = 0;
    std::vector<std::uint8_t> chunk_data;
    friend bool operator== (const relocation_state_t &,
                            const relocation_state_t &) = default;
};

using relocation_control_t = std::variant<
  relocation_prepare_t, relocation_ready_t, relocation_failed_t, relocation_data_t,
  relocation_cutover_t, relocation_state_t>;

struct reply_relay_t
{
    wire_operation_id_t operation;
    std::uint64_t reply_route_id = 0;
    relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    relocation_coordinator_fence_t coordinator;
    std::uint64_t participant_id = 0;
    std::uint64_t sequence = 0;
    std::uint32_t terminal_result = 0;
    framework_error_code failure_code = framework_error_code::none;

    friend bool operator== (const reply_relay_t &,
                            const reply_relay_t &) = default;
};

enum class reply_relay_ack_status_t : std::uint8_t
{
    terminal_received = 1,
    already_terminal = 2
};

struct reply_relay_ack_t
{
    relocation_id_t relocation;
    relocation_coordinator_fence_t coordinator;
    wire_operation_id_t operation;
    std::uint64_t reply_route_id = 0;
    request_source_fence_t request_source;
    reply_relay_ack_status_t status =
      reply_relay_ack_status_t::terminal_received;

    friend bool operator== (const reply_relay_ack_t &,
                            const reply_relay_ack_t &) = default;
};

struct actor_identity_t
{
    std::string actor_id;
    std::uint64_t object_generation = 0;

    friend bool operator== (const actor_identity_t &,
                            const actor_identity_t &) = default;
};

enum class session_relocation_route_action_t : std::uint8_t
{
    commit = 1,
    abort = 2
};

struct session_relocation_route_update_t
{
    session_relocation_route_action_t action =
      session_relocation_route_action_t::commit;
    std::uint64_t previous_authority_owner_generation = 0;
    std::uint64_t target_authority_owner_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t current_authority_owner_generation = 0;

    friend bool operator== (const session_relocation_route_update_t &,
                            const session_relocation_route_update_t &) = default;
};

struct session_relocation_seal_t
{
    relocation_id_t relocation;
    relocation_coordinator_fence_t coordinator;
    relocation_role_t sender_role = relocation_role_t::source;
    actor_route_fence_t actor;
    std::vector<std::uint8_t> session_owner_node_routing_id;
    std::uint64_t session_owner_node_generation = 0;
    std::string session_owner_id;
    std::uint64_t session_owner_lease_generation = 0;
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t binding_generation = 0;

    friend bool operator== (const session_relocation_seal_t &,
                            const session_relocation_seal_t &) = default;
};

struct session_relocation_sealed_t
{
    relocation_id_t relocation;
    relocation_coordinator_fence_t coordinator;
    actor_route_fence_t actor;
    std::vector<std::uint8_t> session_owner_node_routing_id;
    std::uint64_t session_owner_node_generation = 0;
    std::string session_owner_id;
    std::uint64_t session_owner_lease_generation = 0;
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t binding_generation = 0;

    friend bool operator== (const session_relocation_sealed_t &,
                            const session_relocation_sealed_t &) = default;
};

struct session_relocation_route_t
{
    relocation_id_t relocation;
    relocation_coordinator_fence_t coordinator;
    relocation_role_t sender_role = relocation_role_t::target;
    actor_identity_t actor;
    std::vector<std::uint8_t> session_owner_node_routing_id;
    std::uint64_t session_owner_node_generation = 0;
    std::string session_owner_id;
    std::uint64_t session_owner_lease_generation = 0;
    std::vector<std::uint8_t> session_routing_id;
    std::uint64_t binding_generation = 0;
    session_relocation_route_update_t route;

    friend bool operator== (const session_relocation_route_t &,
                            const session_relocation_route_t &) = default;
};

struct reply_header_t
{
    std::uint64_t correlation;
    std::uint32_t terminal_result;
    std::uint32_t failure_code;
};

struct user_spot_reservation_fence_t
{
    std::string reservation_id;
    std::string expected_store_version;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::string target_owner_id;
    std::uint64_t target_owner_lease_generation = 0;
    std::uint32_t pending_capacity_delta = 0;

    friend bool operator== (const user_spot_reservation_fence_t &,
                            const user_spot_reservation_fence_t &) = default;
};

struct user_spot_create_header_t
{
    std::uint64_t correlation = 0;
    wire_operation_id_t operation;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t source_node_generation = 0;
    std::string spot_id;
    std::string stable_type;
    user_spot_reservation_fence_t reservation;
    std::uint64_t deadline_unix_ms = 0;

    friend bool operator== (const user_spot_create_header_t &,
                            const user_spot_create_header_t &) = default;
};

using object_reservation_fence_t = user_spot_reservation_fence_t;

struct actor_create_header_t
{
    std::uint64_t correlation = 0;
    wire_operation_id_t operation;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t source_node_generation = 0;
    std::string actor_id;
    std::string stable_type;
    object_reservation_fence_t reservation;
    std::uint64_t deadline_unix_ms = 0;

    friend bool operator== (const actor_create_header_t &,
                            const actor_create_header_t &) = default;
};

struct instance_spot_activation_target_t
{
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::string spot_id;
    std::string mesh_name;
    std::string stable_type;
    std::string descriptor_version;
    std::uint64_t deadline_unix_ms = 0;

    friend bool operator== (const instance_spot_activation_target_t &,
                            const instance_spot_activation_target_t &) = default;
};

struct instance_spot_activation_header_t
{
    instance_spot_activation_target_t target;
    std::uint64_t source_node_generation = 0;
    std::vector<std::uint8_t> source_node_routing_id;
    std::optional<std::string> source_spot_id;
    bool request = false;
    wire_operation_id_t operation;
    std::uint64_t reply_route_id = 0;
    bool has_metadata = false;

    friend bool operator== (const instance_spot_activation_header_t &,
                            const instance_spot_activation_header_t &) = default;
};

struct instance_activation_recovery_t
{
    instance_spot_activation_header_t activation;
    std::optional<std::vector<std::uint8_t>> metadata;
    application_payload_t application_payload;

    friend bool operator== (const instance_activation_recovery_t &,
                            const instance_activation_recovery_t &) = default;
};

struct user_spot_close_fence_t
{
    std::string spot_id;
    std::uint64_t object_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string expected_store_version;

    friend bool operator== (const user_spot_close_fence_t &,
                            const user_spot_close_fence_t &) = default;
};

struct user_spot_close_header_t
{
    std::uint64_t correlation = 0;
    wire_operation_id_t operation;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t source_node_generation = 0;
    user_spot_close_fence_t target;
    std::uint64_t deadline_unix_ms = 0;

    friend bool operator== (const user_spot_close_header_t &,
                            const user_spot_close_header_t &) = default;
};

enum class user_spot_create_result_t : std::uint8_t
{
    existing = 1,
    created = 2,
    rejected = 3
};

struct user_spot_create_reply_t
{
    reply_header_t header;
    user_spot_create_result_t result =
      user_spot_create_result_t::rejected;
    std::string spot_id;
    std::uint64_t object_generation = 0;
};

enum class actor_create_result_t : std::uint8_t
{
    existing = 1,
    created = 2,
    rejected = 3
};

struct actor_create_reply_t
{
    reply_header_t header;
    actor_create_result_t result = actor_create_result_t::rejected;
    std::vector<std::uint8_t> node_routing_id;
    std::string actor_id;
    std::uint64_t object_generation = 0;
};

struct user_spot_close_reply_t
{
    reply_header_t header;
    bool closed = false;
};

/* service-wire-v1.schema.json: actor-join-result enum
 * (accepted=0, rejected=1). */
enum class actor_join_result_t : std::uint32_t
{
    accepted = 0,
    rejected = 1
};

/* service-wire-v1.schema.json: spot-ref (spotId text8 + objectGeneration
 * nonzero-u64). Named distinctly from
 * zlink::framework::detail::actor_join_reply_t (contracts/actors/actor.hpp),
 * an unrelated local actor-admission type this struct must not be confused
 * with. */
struct actor_join_reply_spot_ref_t
{
    std::string spot_id;
    std::uint64_t object_generation = 0;

    friend bool operator== (const actor_join_reply_spot_ref_t &,
                            const actor_join_reply_spot_ref_t &) = default;
};

/* service-wire-v1.schema.json: actor-join-reply-tail (reply(20) tail for
 * originalOperationKind actorJoin). Accepted case requires spot,
 * membership_epoch (nonzero) and receive_chunk_limit_bytes (bounded by
 * relocationChunkBytes); rejected case carries an optional spot. */
struct actor_join_reply_tail_t
{
    reply_header_t header;
    actor_join_result_t join_result = actor_join_result_t::rejected;
    std::optional<actor_join_reply_spot_ref_t> spot;
    std::uint64_t membership_epoch = 0;
    std::uint32_t receive_chunk_limit_bytes = 0;
};

/* actorJoin(28): correlation, actor-route-fence, bool8 entry, then
 * spot-route-fence.  The optional application payload is carried as a second
 * multipart frame by raw_mesh_node_owner_t, not in this header. */
struct actor_join_request_t
{
    std::uint64_t correlation = 0;
    actor_route_fence_t actor;
    bool entry = false;
    spot_route_fence_t target_spot;

    friend bool operator== (const actor_join_request_t &,
                            const actor_join_request_t &) = default;
};

struct client_server_client_admission_t
{
    std::string channel_name;
    std::string security_identity;
    std::uint32_t effective_max_message_bytes = 0;

    friend bool operator== (const client_server_client_admission_t &,
                            const client_server_client_admission_t &) = default;
};

struct client_server_server_admission_t
{
    std::string channel_name;
    std::vector<std::uint8_t> server_routing_id;
    std::uint64_t lifecycle_generation = 0;
    std::uint64_t descriptor_revision = 0;
    std::uint32_t weight = 100;
    mesh::service_node_state_t state =
      mesh::service_node_state_t::preparing;
    std::string security_identity;
    std::uint32_t effective_max_message_bytes = 0;
    std::string advertised_endpoint;

    friend bool operator== (const client_server_server_admission_t &,
                            const client_server_server_admission_t &) = default;
};

std::vector<std::uint8_t> encode_node_send_header ();
std::vector<std::uint8_t>
encode_node_request_header (std::uint64_t correlation);
std::uint64_t
decode_node_request_header (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_channel_request_header (std::uint64_t correlation,
                               const std::string &channel_name);
struct channel_request_header_t
{
    std::uint64_t correlation;
    std::string channel_name;
};
channel_request_header_t
decode_channel_request_header (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_channel_send_header (const std::string &channel_name);
std::vector<std::uint8_t> encode_spot_message_header (
  command kind,
  const std::string &source_spot_id,
  const spot_route_fence_t &target,
  wire_operation_id_t operation,
  std::optional<std::uint64_t> correlation = std::nullopt,
  std::uint8_t message_follow_hop_count = 0);
spot_message_header_t decode_spot_message_header (
  std::span<const std::uint8_t> bytes,
  command expected_kind);
std::vector<std::uint8_t> encode_actor_message_header (
  command kind,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const actor_route_fence_t &target,
  wire_operation_id_t operation,
  std::optional<std::uint64_t> correlation = std::nullopt,
  std::uint8_t message_follow_hop_count = 0,
  std::optional<actor_message_header_t::bound_session_source_t>
    bound_session_source = std::nullopt);
actor_message_header_t decode_actor_message_header (
  std::span<const std::uint8_t> bytes,
  command expected_kind);
std::vector<std::uint8_t>
encode_bound_session_send (const bound_session_send_t &record);
bound_session_send_t
decode_bound_session_send (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_bound_session_bind (const bound_session_bind_t &record);
bound_session_bind_t
decode_bound_session_bind (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_bound_session_replaced (const bound_session_replaced_t &record);
bound_session_replaced_t
decode_bound_session_replaced (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_message_follow (const message_follow_notice_t &notice);
message_follow_notice_t
decode_message_follow (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_session_relocation_route (
  const session_relocation_route_t &record);
session_relocation_route_t decode_session_relocation_route (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_session_relocation_seal (
  const session_relocation_seal_t &record);
session_relocation_seal_t decode_session_relocation_seal (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_session_relocation_sealed (
  const session_relocation_sealed_t &record);
session_relocation_sealed_t decode_session_relocation_sealed (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_reply_relay (const reply_relay_t &record);
reply_relay_t decode_reply_relay (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_reply_relay_ack (
  const reply_relay_ack_t &record);
reply_relay_ack_t decode_reply_relay_ack (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_relocation_control (
  const relocation_control_t &record);
relocation_control_t decode_relocation_control (
  std::span<const std::uint8_t> bytes);
/* CRC-32C (Castagnoli): poly 0x1EDC6F41, init 0xFFFFFFFF, reflected
 * input/output, xorout 0xFFFFFFFF, check("123456789") = 0xE3069283.
 * Shared checksum for relocation payload manifests and cutover boundaries. */
std::uint32_t relocation_checksum_crc32c (
  std::span<const std::uint8_t> payload) noexcept;
std::vector<std::uint8_t> encode_frozen_record (
  const frozen_record_t &record);
frozen_record_t encode_frozen_application_record (
  const frozen_application_record_t &record);
// Builds the validated frozen summary without allocating canonical relocation
// bytes. The relocation owner encodes those bytes only when a snapshot is
// actually retained or transmitted.
frozen_record_t summarize_frozen_application_record (
  const frozen_application_record_t &record);
/* capture_flow=false (flow-correlation §4): embedded application payload
 * flow pairs are skipped structurally instead of validated/materialized. */
frozen_record_t decode_frozen_record (
  std::span<const std::uint8_t> bytes, bool capture_flow = true);
/* Frozen records are self-delimiting (28 §4.2): decodes exactly one record
 * from the front of `bytes`, reports the consumed byte count, and permits
 * trailing bytes. canonical_bytes retains exactly the consumed range. */
frozen_record_t decode_frozen_record_prefix (
  std::span<const std::uint8_t> bytes,
  std::size_t &consumed,
  bool capture_flow = true);
std::vector<std::uint8_t> encode_user_spot_create_header (
  const user_spot_create_header_t &record);
user_spot_create_header_t decode_user_spot_create_header (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_actor_create_header (
  const actor_create_header_t &record);
actor_create_header_t decode_actor_create_header (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_instance_spot_activation_header (
  const instance_spot_activation_header_t &record);
instance_spot_activation_header_t decode_instance_spot_activation_header (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_instance_activation_recovery (
  const instance_activation_recovery_t &record);
/* capture_flow=false (flow-correlation §4): the recovered application
 * payload's flow pair is skipped structurally. */
instance_activation_recovery_t decode_instance_activation_recovery (
  std::span<const std::uint8_t> bytes, bool capture_flow = true);
std::vector<std::uint8_t> encode_user_spot_close_header (
  const user_spot_close_header_t &record);
user_spot_close_header_t decode_user_spot_close_header (
  std::span<const std::uint8_t> bytes);
service_wire_header_t decode_header (std::span<const std::uint8_t> bytes);
std::string
decode_channel_send_header (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_application_payload (const application_payload_t &payload);
/* capture_flow gates the observation-only flow pair (flow-correlation §4):
 * false skips it structurally — no semantic validation, no materialization —
 * so malformed observation fields cannot fail a frame at Off or at pure
 * frame-integrity guards. */
application_payload_t
decode_application_payload (std::span<const std::uint8_t> bytes,
                            bool capture_flow = true);
std::size_t
application_payload_hwm_bytes (const application_payload_t &payload);
// Validate an encoded application envelope and calculate its HWM charge
// without allocating the application payload vector. Ingress uses this
// before mailbox admission so a full queue does not pay the decode cost.
std::size_t
application_payload_hwm_bytes (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_route_mesh_admission (command kind,
                             const mesh::service_node_descriptor_t &descriptor);
mesh::service_node_descriptor_t
decode_route_mesh_admission (std::span<const std::uint8_t> bytes,
                             command expected_kind,
                             std::vector<std::uint8_t> source_routing_id);
std::vector<std::uint8_t> encode_client_server_client_admission (
  command kind,
  const client_server_client_admission_t &admission);
client_server_client_admission_t decode_client_server_client_admission (
  std::span<const std::uint8_t> bytes,
  command expected_kind);
std::vector<std::uint8_t> encode_client_server_server_admission (
  command kind,
  const client_server_server_admission_t &admission);
client_server_server_admission_t decode_client_server_server_admission (
  std::span<const std::uint8_t> bytes,
  command expected_kind);
std::vector<std::uint8_t> encode_reject (std::uint32_t reason);
std::uint32_t decode_reject (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t>
encode_reply_header (std::uint64_t correlation,
                     std::uint32_t terminal_result,
                     std::uint32_t failure_code);
reply_header_t decode_reply_header (std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_user_spot_create_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  user_spot_create_result_t result,
  const std::string &spot_id,
  std::uint64_t object_generation);
user_spot_create_reply_t decode_user_spot_create_reply (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_actor_create_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  actor_create_result_t result,
  const std::vector<std::uint8_t> &node_routing_id,
  const std::string &actor_id,
  std::uint64_t object_generation);
actor_create_reply_t decode_actor_create_reply (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_user_spot_close_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  bool closed);
user_spot_close_reply_t decode_user_spot_close_reply (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_actor_join_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  actor_join_result_t join_result,
  const std::optional<actor_join_reply_spot_ref_t> &spot,
  std::uint64_t membership_epoch,
  std::uint32_t receive_chunk_limit_bytes);
actor_join_reply_tail_t decode_actor_join_reply (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_actor_join_request (
  const actor_join_request_t &request);
actor_join_request_t decode_actor_join_request (
  std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_liveness (command kind, std::uint64_t probe_id);
liveness_record_t decode_liveness (std::span<const std::uint8_t> bytes);

} // namespace zlink::framework::runtime::protocol
