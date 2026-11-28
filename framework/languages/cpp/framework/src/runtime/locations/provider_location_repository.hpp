/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/diagnostics/capacity_metrics.hpp"
#include "runtime/locations/location_repository.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/detail/binary_text_codec.hpp>

#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{
namespace aggregate_inventory
{
struct tree_t;
}

namespace aggregate_status
{
inline constexpr char pending[] = "pending";
inline constexpr char preparing[] = "preparing";
inline constexpr char prepared[] = "prepared";
inline constexpr char committed[] = "committed";
inline constexpr char aborted[] = "aborted";
}


/*
 * Framework-owned domain repository over the public opaque Store SPI.
 * Providers never receive descriptor, lease, authority or placement DTOs.
 */

class provider_location_repository_t final : public location_repository_t
{
    class provider_boundary_t final : public location_store_t
    {
      public:
        explicit provider_boundary_t (location_store_t &provider,
                                      provider_location_repository_t &repository) noexcept;

        task_t<store_read_result_t> read (store_key_t key) override;

        task_t<store_write_result_t> write (store_write_request_t request) override;

        task_t<store_write_result_t> write_as (store_write_request_t request,
                                               const char *operation);

        task_t<store_scan_result_t> scan (store_scan_request_t request) override;

      private:
        [[noreturn]] static void rethrow_provider_failure ();

        location_store_t &_provider;
        provider_location_repository_t &_repository;
    };

  public:
    explicit provider_location_repository_t (
      location_store_t &store,
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring = {});

    task_t<void> ensure_store_format (std::stop_token cancellation) override;

    task_t<void> refresh_capacity_metrics (std::string mesh_name, zlink::routing_id_t rid) override;

    task_t<owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override;

    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner_id) override;

    task_t<owner_lease_renew_result_t>
    renew_owner_lease (location_owner_token_t token, std::chrono::milliseconds lease_ttl) override;

    task_t<owner_lease_release_result_t>
    release_owner_lease (location_owner_token_t token) override;

    task_t<owner_lease_release_result_t>
    release_owner_lease (location_owner_token_t token,
                         std::chrono::steady_clock::time_point deadline) override;

    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override;

    task_t<location_write_status_t> remove_mesh_node (mesh_node_descriptor_key_t key,
                                                      location_owner_token_t owner) override;

    task_t<location_write_status_t>
    remove_mesh_node (mesh_node_descriptor_key_t key,
                      location_owner_token_t owner,
                      std::chrono::steady_clock::time_point deadline) override;

    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string mesh_name, location_page_request_t page = {}) override;

    task_t<location_write_result_t>
    update_client_server (client_server_server_descriptor_t descriptor,
                          location_write_intent_t intent) override;

    task_t<location_write_status_t> remove_client_server (client_server_server_descriptor_key_t key,
                                                          location_owner_token_t owner) override;

    task_t<location_page_t<client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name, location_page_request_t page = {}) override;

    task_t<location_write_result_t>
    update_fanout_publisher (fanout_publisher_descriptor_t descriptor,
                             location_write_intent_t intent) override;

    task_t<location_write_status_t> remove_fanout_publisher (fanout_publisher_descriptor_key_t key,
                                                             location_owner_token_t owner) override;

    task_t<location_page_t<fanout_publisher_descriptor_t>>
    list_fanout_publishers (std::string channel_name, location_page_request_t page = {}) override;

    task_t<authority_read_result_t>
    read_authority_core (authority_key_t key, std::stop_token cancellation = {}) override;

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected_store_version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override;

  private:
    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected_store_version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation,
                                     std::optional<std::chrono::steady_clock::time_point> deadline);

  public:
    task_t<authority_scan_result_t> list_authorities (std::string key_prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit,
                                                      std::stop_token cancellation = {}) override;

    task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (creation_operation_identity_t operation,
                            std::stop_token cancellation = {}) override;

    task_t<object_reserve_result_t> reserve_core (object_reserve_request_t request,
                                                  std::stop_token cancellation = {}) override;

  private:
    static bool matches_store_version (const store_read_result_t &current,
                                       const std::optional<store_version_t> &original_version);

    task_t<bool> conflict_qualification_unchanged (
      store_key_t key,
      std::optional<store_version_t> original_version,
      location_owner_token_t owner,
      std::optional<std::chrono::system_clock::time_point> operation_deadline,
      const creation_terminal_record_t *terminal = nullptr,
      std::chrono::system_clock::time_point *terminal_store_now = nullptr);

    task_t<std::optional<framework_error_kind_t>>
    append_user_membership_conditions (const std::optional<actor_authority_payload_t> &old_member,
                                       const std::vector<std::byte> &payload,
                                       store_write_request_t &write_request);

    enum class stale_authority_reclaim_result_t;

    task_t<object_reserve_result_t> reserve_once (const object_reserve_request_t &request,
                                                  std::stop_token cancellation,
                                                  bool *retry_reclaim,
                                                  object_reservation_fence_t &fence,
                                                  std::optional<std::string> &reclaim_version);

    task_t<stale_authority_reclaim_result_t>
    try_reclaim_reserved_authority_async (store_key_t authority_key,
                                          store_found_t stored_authority,
                                          authority_snapshot_t current,
                                          const object_reserve_request_t *request = nullptr,
                                          std::stop_token cancellation = {},
                                          const location_owner_token_t *required_owner = nullptr,
                                          const authority_snapshot_t *discarded = nullptr);

  public:
    task_t<bool> release_ended_reservation (
      authority_key_t key,
      std::string expected_store_version,
      std::stop_token cancellation = {},
      std::optional<object_reserve_request_t> request = std::nullopt,
      std::optional<authority_snapshot_t> discarded = std::nullopt) override;

  private:
    task_t<bool>
    release_ended_reservation (authority_key_t key,
                               std::string expected_store_version,
                               std::stop_token cancellation,
                               std::optional<object_reserve_request_t> request,
                               std::optional<location_owner_token_t> required_owner,
                               std::optional<authority_snapshot_t> discarded = std::nullopt);

  public:
    task_t<object_complete_creation_result_t>
    complete_creation_core (object_complete_creation_request_t request,
                            std::stop_token cancellation = {}) override;

    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
                 std::stop_token cancellation = {},
                 std::chrono::system_clock::time_point operation_deadline = {}) override;

    task_t<object_abort_result_t>
    abort (object_abort_request_t request,
           std::stop_token cancellation = {},
           std::chrono::system_clock::time_point operation_deadline = {}) override;

  private:
    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
                 std::stop_token cancellation,
                 const creation_terminal_record_t *terminal,
                 std::chrono::system_clock::time_point terminal_store_now,
                 std::chrono::system_clock::time_point operation_deadline);

    task_t<object_abort_result_t> abort (object_abort_request_t request,
                                         std::stop_token cancellation,
                                         const creation_terminal_record_t *terminal,
                                         std::chrono::system_clock::time_point terminal_store_now,
                                         std::chrono::system_clock::time_point operation_deadline,
                                         std::chrono::steady_clock::time_point deadline =
                                           std::chrono::steady_clock::time_point::max ());

  public:
    task_t<aggregate_prepare_result_t>
    prepare_aggregate_core (aggregate_prepare_request_t request,
                            std::stop_token cancellation = {}) override;

    task_t<aggregate_commit_result_t> commit_aggregate (aggregate_fence_t fence,
                                                        std::stop_token cancellation = {}) override;

    task_t<aggregate_abort_result_t> abort_aggregate (aggregate_fence_t fence,
                                                      std::stop_token cancellation = {}) override;

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_aggregate_participants (aggregate_fence_t fence,
                                 std::stop_token cancellation = {}) override;

    task_t<bool>
    seal_relocation_inventory (aggregate_fence_t fence,
                               std::string digest,
                               std::vector<aggregate_participant_t> participants) override;

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_relocation_inventory (aggregate_fence_t fence, std::string digest) override;

    task_t<std::int64_t> remove_all_by_owner (location_owner_token_t owner) override;

    task_t<std::int64_t>
    remove_all_by_owner (location_owner_token_t owner,
                         std::chrono::steady_clock::time_point deadline) override;

  private:
    class store_format_t;

    using json_t = nlohmann::json;
    static constexpr std::string_view prefix = "zlink:v11:";
    static constexpr std::string_view aggregate_prefix = "zlink:v11:aggregate:";
    static constexpr std::uint64_t max_generation = std::numeric_limits<std::int64_t>::max ();
    inline static const store_key_t counter_key{std::string (prefix) + "owner-counter"};
    inline static const store_key_t generation_floor_key{"zlink:v12:generation-floor"};

    enum class generation_kind_t;

    struct stored_target_t;

    struct stored_aggregate_t;

    struct resolved_authority_t;

    static bool matches_aggregate_source_preserve (const authority_snapshot_t &snapshot,
                                                   const stored_aggregate_t &aggregate,
                                                   const authority_put_t *put);

    static void append_aggregate_abort (store_write_request_t &write, stored_aggregate_t aggregate);

    static bool matches_aggregate_fence (const json_t &record, const aggregate_fence_t &fence);

    static bool matches_aggregate_marker (const nlohmann::ordered_json &marker,
                                          const aggregate_fence_t &fence,
                                          json_t entry);

    static object_creation_target_t decode_aggregate_target (const json_t &record);

    task_t<bool> put_inventory_record (store_key_t key, std::vector<std::byte> bytes);

    task_t<bool> write_inventory (const aggregate_fence_t &fence,
                                  const std::vector<aggregate_participant_t> &participants,
                                  const aggregate_inventory::tree_t &inventory);

    task_t<std::optional<std::vector<aggregate_participant_t>>>
    read_inventory (const aggregate_fence_t &fence, const std::string &digest);

    task_t<resolved_authority_t> resolve_authority_async (const store_found_t &row);

    task_t<store_write_result_t>
    write_async (store_write_request_t request,
                 const creation_terminal_record_t *terminal = nullptr,
                 std::chrono::system_clock::time_point terminal_store_now = {},
                 const char *operation = "compare_exchange",
                 std::chrono::steady_clock::time_point deadline =
                   std::chrono::steady_clock::time_point::max ());

    task_t<std::optional<store_write_applied_t>>
    reconcile_write_async (const store_write_request_t &request,
                           std::chrono::steady_clock::time_point deadline);

    static store_condition_t missing_condition (store_key_t key);

    static store_condition_t version_condition (store_key_t key, store_version_t version);

    static store_condition_t version_condition (store_key_t key, std::string version);

    static store_condition_t value_condition (store_key_t key, std::vector<std::byte> expected);

    static std::vector<std::byte> owner_lease_bytes (const location_owner_token_t &owner);

    static store_condition_t owner_condition (const location_owner_token_t &owner);

    static store_condition_t condition_for (const store_key_t &key,
                                            const store_read_result_t &result);

    static std::vector<std::byte> to_bytes (std::string_view value);

    static std::string to_string (const std::vector<std::byte> &value);

    static json_t parse_json (const std::vector<std::byte> &value);

    static void require_record_version (const json_t &record, const char *record_name);

    static json_t parse_canonical_record (const std::vector<std::byte> &value,
                                          const char *record_name);

    static std::int64_t parse_i64 (const std::vector<std::byte> &value);

    static std::uint64_t read_counter (const std::vector<std::byte> &bytes, bool generation);

    task_t<std::uint64_t> generation_floor ();

    task_t<std::optional<std::uint64_t>> issue_generation (const store_key_t &authority,
                                                           generation_kind_t kind,
                                                           store_write_request_t &write_request);

    static std::int64_t owner_generation (const std::vector<std::byte> &value);

    static owner_lease_found_t decode_owner_lease (const store_found_t &found);

    static std::string segment (std::string_view value);

    static std::string preimage (std::initializer_list<std::string_view> segments);

    static store_key_t key_owner (std::string_view owner_id);


    static std::optional<authority_key_t> authority_from_store_key (const store_key_t &key);

    static std::string prefix_authority (std::string_view key_prefix = {});

    static std::string_view authority_kind_name (char kind);

    static store_key_t key_authority (std::string_view value);

    static store_key_t key_creation_terminal (const creation_operation_identity_t &operation);

    static store_key_t key_aggregate (std::string_view suffix);

    static store_key_t key_aggregate (const aggregate_fence_t &fence);

    static std::string inventory_prefix (std::string_view suffix);

    static store_key_t key_inventory_root (const aggregate_fence_t &fence);

    static store_key_t key_inventory_page (const aggregate_fence_t &fence, const json_t &page);

    static store_key_t key_inventory_payload (std::string_view suffix, std::size_t index);

    static store_key_t key_inventory_payload (const aggregate_fence_t &fence, std::size_t index);

    static store_key_t key_inventory_membership (const aggregate_fence_t &fence, std::size_t index);

    static std::string object_key (const object_creation_key_t &key);

    template <std::size_t N> static std::string hex (const std::array<std::byte, N> &value)
    {
        return zlink::framework::detail::encode_hex (std::span<const std::byte> (value));
    }

    static std::string hex (const std::vector<std::byte> &value);

    static std::vector<std::byte> unhex (std::string_view value);

    template <std::size_t N> static std::array<std::byte, N> unhex_array (std::string_view value)
    {
        const auto decoded = unhex (value);
        if (decoded.size () != N)
            throw std::invalid_argument ("hex value has an invalid length");
        std::array<std::byte, N> result{};
        std::copy (decoded.begin (), decoded.end (), result.begin ());
        return result;
    }

    static std::string prefix_mesh (std::string_view mesh_name);

    static store_key_t key_mesh (std::string_view mesh_name, const zlink::routing_id_t &rid);

    static std::string raw_rid (const zlink::routing_id_t &rid);


    static std::string capacity_prefix (std::string_view mesh_name, std::string_view rid_hex);

    static std::optional<std::string> capacity_scope_from_key (std::string_view key);

    static std::string capacity_counter_key (std::string_view scope,
                                             std::string_view dimension,
                                             std::string_view state);

    static std::string capacity_scope (const object_creation_target_t &target);

    static std::string prefix_client_server (std::string_view channel_name);

    static store_key_t key_client_server (std::string_view channel_name,
                                          const zlink::routing_id_t &rid);

    static std::string prefix_fanout (std::string_view channel_name);

    static store_key_t key_fanout (std::string_view channel_name, const zlink::routing_id_t &rid);

    task_t<location_write_result_t>
    update_descriptor (store_key_t row_key,
                       std::string owner_id,
                       std::int64_t lease_generation,
                       std::uint64_t lifecycle_generation,
                       std::uint64_t descriptor_revision,
                       json_t record,
                       location_write_intent_t intent,
                       std::function<bool (const json_t &)> immutable_fields_equal);

    task_t<location_write_status_t>
    remove_descriptor (store_key_t row_key,
                       location_owner_token_t owner,
                       std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::time_point::max ());

    template <typename T, typename TDecode>
    task_t<location_page_t<T>>
    list_descriptors (std::string row_prefix, location_page_request_t page, TDecode decode);

    static std::int64_t parse_i64_field (const json_t &value);

    static std::uint64_t parse_u64_field (const json_t &value);

    static std::string record_owner_id (const json_t &record);

    static std::int64_t record_lease_generation (const json_t &record);

    static std::uint64_t record_lifecycle_generation (const json_t &record);

    static std::uint64_t record_descriptor_revision (const json_t &record);

    static std::int64_t unix_ms (std::chrono::system_clock::time_point value);

    static std::chrono::system_clock::time_point from_unix_ms (std::int64_t value);

    static json_t encode_owner (const location_owner_token_t &value);

    static location_owner_token_t decode_owner (const json_t &value);

    static bool same_owner (const location_owner_token_t &left,
                            const location_owner_token_t &right);

    static bool owner_lease_is_live (const owner_lease_found_t &lease,
                                     const location_owner_token_t &owner);

    task_t<bool> owner_is_live_async (location_owner_token_t owner);

    task_t<std::optional<store_found_t>> read_live_owner_async (location_owner_token_t owner);

    static json_t encode_target (const object_creation_target_t &value);

    static object_creation_target_t decode_target (const json_t &value);

    template <typename T, std::size_t N>
    static const char *
    store_enum_name (T value, const std::array<std::pair<T, const char *>, N> &names, T fallback);

    template <typename T, std::size_t N>
    static T parse_store_enum (std::string_view value,
                               const std::array<std::pair<T, const char *>, N> &names,
                               T fallback);


    static const char *object_kind3_name (placement_object_kind_t value);

    static placement_object_kind_t parse_object_kind3 (const std::string &value);

    static json_t encode_bundle (const placement_capacity_bundle_t &value);

    static placement_capacity_bundle_t decode_bundle (const json_t &value);


    static const char *allocation_state_name (placement_allocation_state_t value);

    static placement_allocation_state_t parse_allocation_state (const json_t &value);

    static json_t encode_target_descriptor (const object_creation_target_t &value);

    static object_creation_target_t decode_target_descriptor (
      const json_t &value, location_owner_token_t owner, std::uint64_t node_lifecycle_generation);

    static json_t encode_allocation (const placement_allocation_t &value);

    static placement_allocation_t decode_allocation (const json_t &value,
                                                     const location_owner_token_t &owner);

    static json_t encode_pending (const std::optional<pending_object_creation_t> &value);

    static std::optional<pending_object_creation_t> decode_pending (const json_t &value);

    static std::vector<std::byte> encode_authority (const authority_snapshot_t &value,
                                                    std::string_view visible_version = {});

    static authority_snapshot_t decode_authority (const json_t &value,
                                                  std::string provider_version,
                                                  std::chrono::system_clock::time_point store_now);

    static authority_snapshot_t decode_authority (const std::vector<std::byte> &bytes,
                                                  const store_version_t &provider_version,
                                                  std::chrono::system_clock::time_point store_now);

    static bool same_fence (const object_reservation_fence_t &left,
                            const object_reservation_fence_t &right);

    task_t<authority_compare_exchange_result_t>
    authority_conflict (store_read_result_t current,
                        std::optional<framework_error_kind_t> membership_failure = std::nullopt);

    static std::string version_of (const store_write_applied_t &applied, const store_key_t &key);

    static json_t encode (const capacity_usage_t &value);

    static capacity_usage_t decode_capacity_usage (const json_t &value);


    static const char *runtime_state_name (framework_runtime_state_t value);

    static framework_runtime_state_t parse_runtime_state (const std::string &value);


    static const char *maintenance_policy_name (maintenance_policy_kind_t value);

    static maintenance_policy_kind_t parse_maintenance_policy (const std::string &value);


    static const char *object_role_name (object_role_t value);

    static object_role_t parse_object_role (const std::string &value);

    static json_t encode (const mesh_node_descriptor_t &value);

    static bool same_mesh_immutable (const mesh_node_descriptor_t &left,
                                     const mesh_node_descriptor_t &right);

    task_t<std::optional<stored_target_t>> read_target_descriptor_async (
      object_creation_target_t target, bool require_live = true, bool require_identity = true);

    static bool target_accepts (const mesh_node_descriptor_t &descriptor,
                                placement_object_kind_t kind,
                                const std::string &stable_type);

    static bool bundle_matches (const placement_capacity_bundle_t &bundle,
                                placement_object_kind_t kind,
                                const std::string &stable_type);

    static std::string capacity_type_dimension (placement_object_kind_t kind,
                                                std::string_view stable_type);

    class capacity_changes_t;

    task_t<bool> capacity_still_available (const capacity_changes_t &changes);

    task_t<authority_compare_exchange_result_t>
    publish_retarget (const authority_key_t &authority,
                      const store_key_t &authority_key,
                      const store_version_t &authority_version,
                      authority_snapshot_t target,
                      const placement_allocation_t &source,
                      stored_target_t descriptor,
                      store_write_request_t commit,
                      std::stop_token cancellation,
                      std::chrono::steady_clock::time_point deadline);

    task_t<bool> retarget_fences_unchanged (const std::vector<store_condition_t> &conditions);

    static bool batch_fits (const store_write_request_t &request);

    task_t<authority_read_result_t> read_authority_value_async (std::string key);

    static json_t encode_aggregate (const aggregate_prepare_request_t &request,
                                    std::string_view state,
                                    const aggregate_inventory::tree_t &inventory);

    static mesh_node_descriptor_t decode_mesh_descriptor (const json_t &value);

    static json_t encode (const client_server_server_descriptor_t &value);

    static client_server_server_descriptor_t decode_client_server (const json_t &value);

    static json_t encode (const fanout_publisher_descriptor_t &value);

    static fanout_publisher_descriptor_t decode_fanout (const json_t &value);

    static std::string generation_string (std::uint64_t value);
    static std::string generation_string (std::int64_t value);

    static json_t encode_mesh_record (std::uint64_t generation,
                                      const mesh_node_descriptor_t &descriptor);

    static json_t encode_descriptor_record (std::uint64_t generation,
                                            std::string owner_id,
                                            std::int64_t lease_generation,
                                            std::uint64_t lifecycle_generation,
                                            std::uint64_t descriptor_revision,
                                            json_t descriptor);

    template <typename T> static task_t<T> completed (T value);

    template <typename T> static task_t<T> unavailable (std::string message);

    template <typename T> static task_t<T> cancelled ();

    void record_instance_takeover (const object_reserve_request_t &request,
                                   const char *outcome) const;
    void record_store_error (const char *operation) const;
    using capacity_snapshot_t = std::map<std::string, std::uint64_t>;

    task_t<capacity_snapshot_t> read_capacity (const std::string &scope);

    static void project_capacity (const std::string &scope,
                                  placement_capacity_t &value,
                                  const capacity_snapshot_t &counters);

    task_t<void> confirm_capacity (const std::string &scope);

    std::atomic<std::uint64_t> _generation_floor{0};
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    capacity_metrics_t _capacity_metrics;
    provider_boundary_t _store;
};

} // namespace zlink::framework::runtime
