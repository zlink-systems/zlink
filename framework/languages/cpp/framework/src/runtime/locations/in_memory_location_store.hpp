/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/execution/state_lane.hpp"
#include "runtime/locations/location_repository.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

struct owner_lease_row_t
{
    std::string owner_id;
    zlink::routing_id_t node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    std::chrono::system_clock::time_point lease_expires_at{};
    std::chrono::system_clock::time_point updated_at{};
};

class in_memory_location_repository_t : public location_repository_t
{
    friend struct in_memory_store_continuation_test_access_t;
    friend struct in_memory_location_repository_test_access_t;

  public:
    in_memory_location_repository_t () = default;

    explicit in_memory_location_repository_t (std::uint64_t initial_store_revision);

    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override;

    task_t<location_write_status_t> remove_mesh_node (mesh_node_descriptor_key_t key,
                                                      location_owner_token_t owner) override;

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

    task_t<owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override;

    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner_id) override;

    task_t<owner_lease_renew_result_t>
    renew_owner_lease (location_owner_token_t token, std::chrono::milliseconds lease_ttl) override;

    task_t<owner_lease_release_result_t>
    release_owner_lease (location_owner_token_t token) override;

    task_t<authority_read_result_t>
    read_authority_core (authority_key_t key, std::stop_token cancellation = {}) override;

    task_t<authority_compare_exchange_result_t>
    compare_exchange_authority_core (authority_key_t key,
                                     std::string expected_store_version,
                                     authority_mutation_t mutation,
                                     std::stop_token cancellation = {}) override;

    task_t<authority_scan_result_t> list_authorities (std::string prefix,
                                                      std::optional<authority_scan_cursor_t> cursor,
                                                      std::size_t limit,
                                                      std::stop_token cancellation = {}) override;

    task_t<std::optional<creation_terminal_record_t>>
    read_creation_terminal (creation_operation_identity_t operation,
                            std::stop_token cancellation = {}) override;

    task_t<object_complete_creation_result_t>
    complete_creation_core (object_complete_creation_request_t request,
                            std::stop_token cancellation = {}) override;

    task_t<bool> release_ended_reservation (
      authority_key_t key,
      std::string expected_store_version,
      std::stop_token cancellation = {},
      std::optional<object_reserve_request_t> request = std::nullopt,
      std::optional<authority_snapshot_t> discarded = std::nullopt) override;

    task_t<object_reserve_result_t> reserve_core (object_reserve_request_t request,
                                                  std::stop_token cancellation = {}) override;

    task_t<object_commit_result_t>
    commit_core (object_commit_request_t request,
                 std::stop_token cancellation = {},
                 std::chrono::system_clock::time_point operation_deadline = {}) override;

    task_t<object_abort_result_t> abort (object_abort_request_t request,
                                         std::stop_token cancellation = {},
                                         std::chrono::system_clock::time_point = {}) override;

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

  private:
    using clock_t = std::chrono::system_clock;

    enum class reservation_status_t
    {
        prepared,
        committed,
        aborted
    };

    struct reservation_state_t
    {
        object_reserve_request_t request;
        object_reservation_fence_t fence;
        authority_snapshot_t snapshot;
        reservation_status_t status = reservation_status_t::prepared;
    };

    enum class aggregate_status_t
    {
        prepared,
        committed,
        aborted
    };

    struct aggregate_state_t
    {
        aggregate_prepare_request_t request;
        aggregate_status_t status = aggregate_status_t::prepared;
    };

    struct sealed_inventory_t
    {
        std::string digest;
        std::vector<aggregate_participant_t> participants;
    };

    struct authority_scan_state_t
    {
        std::vector<authority_entry_t> entries;
        clock_t::time_point created_at;
    };

    template <typename T> static task_t<T> completed (T value);

    template <typename T> static task_t<T> cancelled ();

    std::optional<framework_error_kind_t>
    user_membership_commit_failure (const std::optional<actor_authority_payload_t> &old_member,
                                    const std::optional<actor_authority_payload_t> &member,
                                    clock_t::time_point now) const;

    bool authority_mutation_locked_on_lane (const std::string &key) const;

    bool release_ended_reservation_on_lane (const std::string &key,
                                            const authority_snapshot_t &snapshot,
                                            clock_t::time_point now,
                                            const object_reserve_request_t *request = nullptr,
                                            const authority_snapshot_t *discarded = nullptr);

    std::vector<std::string> ended_spot_members_on_lane (const std::string &spot_key,
                                                         const authority_snapshot_t &spot,
                                                         const object_reserve_request_t &request,
                                                         clock_t::time_point now);

    bool owner_token_is_live (const location_owner_token_t &token, clock_t::time_point now) const;

    const mesh_node_descriptor_t *live_target_descriptor (const object_creation_target_t &target,
                                                          placement_object_kind_t kind,
                                                          const std::string &stable_type,
                                                          clock_t::time_point now,
                                                          bool new_target_selection) const;

    static bool valid_mesh_node_descriptor (const mesh_node_descriptor_t &descriptor);

    static bool same_capability (const object_capability_t &left, const object_capability_t &right);

    static bool same_capabilities (const std::vector<object_capability_t> &left,
                                   const std::vector<object_capability_t> &right);

    static bool same_mesh_node_identity (const mesh_node_descriptor_t &left,
                                         const mesh_node_descriptor_t &right);

    static bool same_mesh_node_descriptor (const mesh_node_descriptor_t &left,
                                           const mesh_node_descriptor_t &right);

    static bool
    valid_client_server_descriptor (const client_server_server_descriptor_t &descriptor);

    static bool same_client_server_identity (const client_server_server_descriptor_t &left,
                                             const client_server_server_descriptor_t &right);

    static bool same_client_server_descriptor (const client_server_server_descriptor_t &left,
                                               const client_server_server_descriptor_t &right);

    static bool valid_fanout_descriptor (const fanout_publisher_descriptor_t &descriptor);

    static bool valid_fanout_descriptor_text (std::string_view value) noexcept;

    static std::size_t fanout_descriptor_encoded_size_upper_bound (
      const fanout_publisher_descriptor_t &descriptor) noexcept;

    static bool same_fanout_identity (const fanout_publisher_descriptor_t &left,
                                      const fanout_publisher_descriptor_t &right);

    static bool same_fanout_descriptor (const fanout_publisher_descriptor_t &left,
                                        const fanout_publisher_descriptor_t &right);

    static bool same_channel_names (const std::map<std::string, int> &left,
                                    const std::map<std::string, int> &right);

    static std::string mesh_node_key (const std::string &mesh_name, const zlink::routing_id_t &rid);

    static std::string mesh_node_key (const std::string &mesh_name, const std::string &rid);

    static std::string client_server_key (const std::string &channel_name,
                                          const zlink::routing_id_t &rid);

    static std::string fanout_key (const std::string &channel_name, const zlink::routing_id_t &rid);

    static bool
    issue_generations (std::initializer_list<std::pair<std::uint64_t *, std::size_t>> counters);

    bool store_revisions_available (std::size_t count = 1) const;

    std::string next_store_version ();

    static std::string object_key (const object_creation_key_t &key);

    static std::string creation_operation_key (const creation_operation_identity_t &identity);

    static bool same_owner (const location_owner_token_t &left,
                            const location_owner_token_t &right);

    static bool same_target (const object_creation_target_t &left,
                             const object_creation_target_t &right);

    static bool same_fence (const object_reservation_fence_t &left,
                            const object_reservation_fence_t &right);

    static bool same_aggregate_request (const aggregate_prepare_request_t &left,
                                        const aggregate_prepare_request_t &right);

    static std::string capacity_node_key (const object_creation_target_t &target);

    static std::string actor_capacity_key (const object_creation_target_t &target);

    static std::string spot_capacity_key (const object_creation_target_t &target);

    static std::string spot_type_capacity_key (const object_creation_target_t &target,
                                               const spot_type_capacity_delta_t &spot_type);

    static bool valid_capacity_bundle (const placement_capacity_bundle_t &bundle);

    static bool bundle_matches_object (const placement_capacity_bundle_t &bundle,
                                       placement_object_kind_t kind,
                                       const std::string &stable_type);

    static bool same_capacity_bundle (const placement_capacity_bundle_t &left,
                                      const placement_capacity_bundle_t &right);

    bool capacity_available (const mesh_node_descriptor_t &descriptor,
                             const object_creation_target_t &target,
                             const placement_capacity_bundle_t &bundle) const;

    static void apply_capacity_bundle (std::map<std::string, std::uint64_t> &counters,
                                       const object_creation_target_t &target,
                                       const placement_capacity_bundle_t &bundle,
                                       bool add);

    static bool capacity_bundle_present (const std::map<std::string, std::uint64_t> &counters,
                                         const object_creation_target_t &target,
                                         const placement_capacity_bundle_t &bundle);

    void apply_capacity_projection (mesh_node_descriptor_t &descriptor) const;

    void release_pending (const reservation_state_t &reservation);

    static bool all_zero (const std::array<std::byte, 16> &value);

    static std::string aggregate_id_key (const aggregate_id_t &id);

    void cleanup_scans (clock_t::time_point now);

    static std::size_t parse_offset (const std::string &value);

    runtime::state_lane_t _lane;
    std::map<std::string, mesh_node_descriptor_t> _mesh_nodes;
    std::map<std::string, client_server_server_descriptor_t> _client_servers;
    std::map<std::string, fanout_publisher_descriptor_t> _fanout_publishers;
    std::map<std::string, owner_lease_row_t> _leases;
    std::map<std::string, std::int64_t> _active_lease_generations;
    std::uint64_t _lease_generation = 0;
    std::map<std::string, authority_snapshot_t> _authorities;
    std::map<std::string, reservation_state_t> _reservations;
    std::map<std::string, creation_terminal_record_t> _creation_terminals;
    std::map<std::string, aggregate_state_t> _aggregates;
    std::map<std::string, sealed_inventory_t> _sealed_inventories;
    std::map<std::string, std::uint64_t> _pending_by_placement;
    std::map<std::string, std::uint64_t> _active_by_placement;
    std::uint64_t _object_generation = 0;
    std::uint64_t _authority_owner_generation = 0;
    std::uint64_t _store_revision = 0;
    std::map<std::string, authority_scan_state_t> _authority_scans;
    std::uint64_t _next_scan_id = 0;
};

} // namespace zlink::framework::runtime
