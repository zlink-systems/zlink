/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/operations/operation_id.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>
#include <zlink/framework/contracts/locations/values.hpp>
#include <runtime/locations/location_records.hpp>
#include <runtime/locations/location_repository_values.hpp>
#include <zlink/framework/contracts/placement.hpp>
#include <zlink/framework/contracts/spots/spot_identity.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zlink::framework
{

// Framework-private domain repository contracts. Store providers implement
// only the opaque location_store_t and relocation_store_t public SPI.

struct authority_key_t
{
    std::string value;
};

struct object_creation_target_t
{
    std::string mesh_name;
    node_rid_t node_rid;
    std::uint64_t node_lifecycle_generation = 0;
    location_owner_token_t owner;
};

enum class placement_allocation_state_t : std::uint8_t
{
    reserved = 1,
    active = 2
};

struct spot_type_capacity_delta_t
{
    placement_object_kind_t object_kind =
      placement_object_kind_t::user_spot;
    std::string stable_type;
    std::uint32_t slots = 0;
};

struct placement_capacity_bundle_t
{
    std::uint32_t actor_slots = 0;
    std::uint32_t spot_slots = 0;
    std::optional<spot_type_capacity_delta_t> spot_type;
};

struct placement_allocation_t
{
    placement_allocation_state_t state =
      placement_allocation_state_t::reserved;
    placement_object_kind_t object_kind =
      placement_object_kind_t::actor;
    std::string stable_type;
    object_creation_target_t target;
    placement_capacity_bundle_t capacity_bundle;
};

struct pending_object_creation_t
{
    std::string reservation_id;
    std::string request_content_reference;
    std::array<std::byte, 32> request_sha256{};
    std::uint32_t request_encoded_size = 0;
};

struct authority_snapshot_t
{
    std::string store_version;
    std::vector<std::byte> payload;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    location_owner_token_t owner;
    std::chrono::system_clock::time_point store_now;
    placement_allocation_t allocation;
    std::optional<pending_object_creation_t> pending_creation;
};

struct authority_missing_t
{
    std::chrono::system_clock::time_point store_now;
};

using authority_read_result_t =
  std::variant<authority_missing_t, authority_snapshot_t>;

enum class authority_generation_transition_t
{
    preserve = 1,
    new_owner = 2
};

struct authority_entry_t
{
    authority_key_t key;
    authority_snapshot_t snapshot;
};

class authority_scan_cursor_t final
{
  public:
    explicit authority_scan_cursor_t (std::string encoded) :
        _encoded (std::move (encoded))
    {
        if (_encoded.empty () || _encoded.size () > 4096)
            throw std::invalid_argument (
              "authority scan cursor must contain 1..4096 bytes");
    }

    std::string_view encoded () const noexcept { return _encoded; }

  private:
    std::string _encoded;
};

struct authority_page_t
{
    std::vector<authority_entry_t> items;
    std::optional<authority_scan_cursor_t> next_cursor;
};
struct authority_scan_expired_t
{
};
using authority_scan_result_t =
  std::variant<authority_page_t, authority_scan_expired_t>;

struct relocation_capacity_fence_t
{
    std::string value;
};

struct authority_put_t
{
    std::vector<std::byte> payload;
    authority_generation_transition_t generation_transition =
      authority_generation_transition_t::preserve;
    std::optional<location_owner_token_t> target_owner;
    std::optional<relocation_capacity_fence_t>
      relocation_capacity_fence;
};
struct authority_restore_t
{
    std::vector<std::byte> payload;
    location_owner_token_t expected_owner;
};
struct authority_delete_t
{
};
using authority_mutation_t =
  std::variant<authority_put_t, authority_restore_t, authority_delete_t>;

struct authority_stored_t
{
    authority_snapshot_t snapshot;
};
struct authority_deleted_t
{
    std::string store_version;
    std::chrono::system_clock::time_point store_now;
};
struct authority_conflict_t
{
    authority_read_result_t current;
};
struct authority_generation_exhausted_t
{
};
using authority_compare_exchange_result_t = std::variant<
  authority_stored_t,
  authority_deleted_t,
  authority_conflict_t,
  authority_generation_exhausted_t>;

struct object_creation_key_t
{
    placement_object_kind_t kind = placement_object_kind_t::actor;
    std::string global_id;
};
struct object_creation_intent_t
{
    std::string stable_type;
    std::string request_content_reference;
    std::array<std::byte, 32> request_sha256{};
    std::uint64_t request_encoded_size = 0;
};

using creation_operation_id_t = runtime::operation_id_t;

struct creation_operation_identity_t
{
    node_rid_t source_node_rid;
    std::uint64_t source_node_generation = 0;
    creation_operation_id_t operation_id;
};

struct object_reserve_request_t
{
    object_creation_key_t key;
    object_creation_intent_t intent;
    object_creation_target_t target;
    std::vector<std::byte> creating_payload;
    placement_capacity_bundle_t capacity_bundle;
};
struct object_reservation_fence_t
{
    std::string reservation_id;
    std::string expected_store_version;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    object_creation_target_t target;
    placement_capacity_bundle_t capacity_bundle;
};
struct object_reserved_t
{
    object_reservation_fence_t fence;
    authority_snapshot_t creating;
};
struct object_already_exists_t
{
    authority_snapshot_t current;
};
struct object_type_mismatch_t
{
    authority_snapshot_t current;
};
struct object_placement_capacity_exhausted_t
{
};
struct object_reserve_conflict_t
{
    authority_read_result_t current;
};

enum class creation_terminal_state_t : std::uint8_t
{
    created = 1,
    rejected = 2,
    failed = 3
};

struct creation_terminal_publication_t
{
    creation_operation_identity_t operation;
    std::vector<std::byte> terminal_envelope;
    std::array<std::byte, 32> sha256{};
    std::chrono::system_clock::time_point operation_deadline{};
};

struct creation_terminal_record_t
{
    creation_operation_identity_t operation;
    object_creation_key_t object;
    object_reservation_fence_t reservation;
    creation_terminal_state_t state =
      creation_terminal_state_t::created;
    std::vector<std::byte> terminal_envelope;
    std::array<std::byte, 32> sha256{};
    std::chrono::system_clock::time_point expires_at{};
};

using object_reserve_result_t = std::variant<
  object_reserved_t,
  object_already_exists_t,
  object_type_mismatch_t,
  object_placement_capacity_exhausted_t,
  object_reserve_conflict_t,
  authority_generation_exhausted_t>;

struct object_creation_completed_t
{
    std::vector<std::byte> ready_payload;
    creation_terminal_publication_t terminal;
};
struct object_creation_rejected_t
{
    creation_terminal_publication_t terminal;
};
struct object_creation_failed_t
{
    creation_terminal_publication_t terminal;
};
using object_creation_completion_t =
  std::variant<object_creation_completed_t,
               object_creation_rejected_t,
               object_creation_failed_t>;

struct object_complete_creation_request_t
{
    object_creation_key_t key;
    object_reservation_fence_t fence;
    object_creation_completion_t completion;
};

struct object_creation_completed_result_t
{
    creation_terminal_record_t terminal;
    std::optional<authority_snapshot_t> ready;
};
struct object_creation_already_completed_result_t
{
    creation_terminal_record_t terminal;
};
struct object_creation_completion_stale_t
{
};
struct object_creation_completion_conflict_t
{
    authority_read_result_t current;
};
using object_complete_creation_result_t =
  std::variant<object_creation_completed_result_t,
               object_creation_already_completed_result_t,
               object_creation_completion_stale_t,
               object_creation_completion_conflict_t,
               authority_generation_exhausted_t>;

struct object_commit_request_t
{
    object_creation_key_t key;
    object_reservation_fence_t fence;
    std::vector<std::byte> ready_payload;
};
struct object_committed_t
{
    authority_snapshot_t ready;
};
struct object_already_committed_t
{
    authority_snapshot_t ready;
};
struct object_commit_stale_t
{
};
struct object_commit_conflict_t
{
    authority_read_result_t current;
};
using object_commit_result_t = std::variant<
  object_committed_t,
  object_already_committed_t,
  object_commit_stale_t,
  object_commit_conflict_t,
  authority_generation_exhausted_t>;

struct object_abort_request_t
{
    object_creation_key_t key;
    object_reservation_fence_t fence;
};
struct object_aborted_t
{
};
struct object_already_aborted_t
{
};
struct object_abort_stale_t
{
};
struct object_abort_conflict_t
{
    authority_read_result_t current;
};
using object_abort_result_t = std::variant<
  object_aborted_t,
  object_already_aborted_t,
  object_abort_stale_t,
  object_abort_conflict_t,
  authority_generation_exhausted_t>;

struct relocation_capacity_reserve_request_t
{
    std::array<std::byte, 16> reservation_id{};
    authority_key_t key;
    std::string expected_store_version;
    placement_object_kind_t object_kind =
      placement_object_kind_t::actor;
    std::string stable_type;
    object_creation_target_t source;
    object_creation_target_t target;
    placement_capacity_bundle_t capacity_bundle;
};
struct relocation_capacity_reserved_t
{
    relocation_capacity_fence_t fence;
};
struct relocation_capacity_already_reserved_t
{
    relocation_capacity_fence_t fence;
};
struct relocation_capacity_conflict_t
{
    authority_read_result_t current;
};
struct relocation_capacity_target_unavailable_t
{
};
struct relocation_capacity_exhausted_t
{
};
using relocation_capacity_reserve_result_t = std::variant<
  relocation_capacity_reserved_t,
  relocation_capacity_already_reserved_t,
  relocation_capacity_conflict_t,
  relocation_capacity_target_unavailable_t,
  relocation_capacity_exhausted_t>;

enum class relocation_capacity_abort_result_t : std::uint8_t
{
    aborted = 1,
    already_aborted = 2,
    already_committed = 3,
    stale = 4
};

struct aggregate_id_t
{
    std::array<std::byte, 16> value{};
};
struct inventory_digest_t
{
    std::array<std::byte, 32> value{};
};
struct aggregate_participant_t
{
    authority_key_t key;
    std::string expected_store_version;
    authority_generation_transition_t owner_transition =
      authority_generation_transition_t::new_owner;
    std::vector<std::byte> authority_payload;
    std::vector<std::byte> membership_mutation;
    std::optional<relocation_capacity_fence_t> capacity_fence;
};
struct aggregate_prepare_request_t
{
    aggregate_id_t aggregate_id;
    std::uint64_t aggregate_generation = 0;
    std::vector<aggregate_participant_t> participants;
    inventory_digest_t inventory_digest;
    mesh_node_descriptor_key_t target_descriptor;
    std::uint64_t target_descriptor_lifecycle_generation = 0;
    placement_capacity_bundle_t capacity_bundle;
    location_owner_token_t target_owner;
    std::vector<relocation_capacity_fence_t> capacity_fences;
};
struct aggregate_fence_t
{
    aggregate_id_t aggregate_id;
    std::uint64_t aggregate_generation = 0;
    // Carries the manifest digest across private aggregate retries.
    std::optional<inventory_digest_t> inventory_digest;
};
struct aggregate_prepared_t
{
    aggregate_fence_t fence;
};
struct aggregate_already_prepared_t
{
    aggregate_fence_t fence;
};
struct aggregate_prepare_conflict_t
{
};
struct aggregate_prepare_stale_t
{
};
using aggregate_prepare_result_t = std::variant<
  aggregate_prepared_t,
  aggregate_already_prepared_t,
  aggregate_prepare_conflict_t,
  aggregate_prepare_stale_t,
  authority_generation_exhausted_t>;

enum class aggregate_commit_result_t : std::uint8_t
{
    committed = 1,
    already_committed = 2,
    stale = 3,
    generation_exhausted = 4
};
enum class aggregate_abort_result_t : std::uint8_t
{
    aborted = 1,
    already_aborted = 2,
    stale = 3
};

struct relocation_stored_t
{
    std::string reference;
    std::uint32_t checksum_crc32c = 0;
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point store_now;
};
struct relocation_found_t
{
    std::vector<std::byte> payload;
};
struct relocation_missing_t
{
};
using relocation_read_result_t =
  std::variant<relocation_found_t, relocation_missing_t>;
enum class relocation_delete_result_t
{
    deleted = 0,
    missing = 1
};
struct relocation_renewed_t
{
    std::chrono::system_clock::time_point expires_at;
    std::chrono::system_clock::time_point store_now;
};
struct relocation_renew_missing_t
{
};
using relocation_renew_result_t =
  std::variant<relocation_renewed_t, relocation_renew_missing_t>;

class location_repository_t
{
  public:
    virtual ~location_repository_t () = default;
    virtual task_t<location_write_result_t> update_mesh_node (
      mesh_node_descriptor_t descriptor,
      location_write_intent_t intent) = 0;
    virtual task_t<location_write_status_t> remove_mesh_node (
      mesh_node_descriptor_key_t key,
      location_owner_token_t owner) = 0;
    virtual task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh_name,
                     location_page_request_t page = {}) = 0;
    virtual task_t<location_write_result_t> update_client_server (
      client_server_server_descriptor_t descriptor,
      location_write_intent_t intent) = 0;
    virtual task_t<location_write_status_t> remove_client_server (
      client_server_server_descriptor_key_t key,
      location_owner_token_t owner) = 0;
    virtual task_t<location_page_t<client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name,
                         location_page_request_t page = {}) = 0;
    virtual task_t<location_write_result_t> update_fanout_publisher (
      fanout_publisher_descriptor_t descriptor,
      location_write_intent_t intent) = 0;
    virtual task_t<location_write_status_t> remove_fanout_publisher (
      fanout_publisher_descriptor_key_t key,
      location_owner_token_t owner) = 0;
    virtual task_t<location_page_t<fanout_publisher_descriptor_t>>
    list_fanout_publishers (std::string channel_name,
                            location_page_request_t page = {}) = 0;
    virtual task_t<owner_lease_claim_result_t> claim_owner_lease (
      std::string owner_id,
      std::chrono::milliseconds lease_ttl) = 0;
    virtual task_t<owner_lease_read_result_t> read_owner_lease (
      std::string owner_id) = 0;
    virtual task_t<owner_lease_renew_result_t> renew_owner_lease (
      location_owner_token_t token,
      std::chrono::milliseconds lease_ttl) = 0;
    virtual task_t<owner_lease_release_result_t> release_owner_lease (
      location_owner_token_t token) = 0;
    virtual task_t<authority_read_result_t> read_authority (
      authority_key_t key,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<authority_compare_exchange_result_t>
    compare_exchange_authority (
      authority_key_t key,
      std::string expected_store_version,
      authority_mutation_t mutation,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<authority_scan_result_t> list_authorities (
      std::string prefix,
      std::optional<authority_scan_cursor_t> cursor,
      std::size_t limit,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (
      creation_operation_identity_t operation,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<object_reserve_result_t> reserve (
      object_reserve_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<object_complete_creation_result_t> complete_creation (
      object_complete_creation_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<object_commit_result_t> commit (
      object_commit_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<object_abort_result_t> abort (
      object_abort_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<relocation_capacity_reserve_result_t>
    reserve_relocation_capacity (
      relocation_capacity_reserve_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<relocation_capacity_abort_result_t>
    abort_relocation_capacity (
      relocation_capacity_fence_t fence,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<aggregate_prepare_result_t> prepare_aggregate (
      aggregate_prepare_request_t request,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<aggregate_commit_result_t> commit_aggregate (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<aggregate_abort_result_t> abort_aggregate (
      aggregate_fence_t fence,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (
      aggregate_fence_t fence,
      std::stop_token cancellation = {})
    {
        if (cancellation.stop_requested ())
            return task_t<std::optional<std::vector<aggregate_participant_t>>> (
              detail::boundary_failure<
                std::optional<std::vector<aggregate_participant_t>>> (
                detail::boundary_error_t::cancelled,
                "aggregate read cancelled"));
        return task_t<std::optional<std::vector<aggregate_participant_t>>> (
          result_t<std::optional<std::vector<aggregate_participant_t>>>::success (
            std::nullopt));
    }
    virtual task_t<std::int64_t> remove_all_by_owner (
      location_owner_token_t owner) = 0;
    virtual task_t<std::optional<std::uint64_t>>
    get_mesh_node_change_stamp (std::string mesh_name)
    {
        (void) mesh_name;
        return task_t<std::optional<std::uint64_t>> (
          result_t<std::optional<std::uint64_t>>::success (
            std::nullopt));
    }
};

class relocation_repository_t
{
  public:
    virtual ~relocation_repository_t () = default;
    virtual task_t<relocation_stored_t> put_relocation (
      std::vector<std::byte> payload,
      std::chrono::hours retention,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<relocation_read_result_t> get_relocation (
      std::string reference,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<relocation_renew_result_t> renew_relocation (
      std::string reference,
      std::chrono::hours retention,
      std::stop_token cancellation = {}) = 0;
    virtual task_t<relocation_delete_result_t> delete_relocation (
      std::string reference,
      std::stop_token cancellation = {}) = 0;
};

} // namespace zlink::framework
