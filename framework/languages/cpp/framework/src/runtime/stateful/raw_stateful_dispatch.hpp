/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::runtime::stateful
{

struct accepted_record_authority_query_t
{
    object_ref_t target;
    std::vector<std::uint8_t> source_node_routing_id;
    std::uint64_t source_node_generation = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    protocol::frozen_source_kind_t source_kind =
      protocol::frozen_source_kind_t::node;
    std::optional<std::string> source_spot_id;
    std::optional<std::pair<std::string, std::uint64_t>> source_actor;
};

struct accepted_record_authority_t
{
    protocol::request_source_fence_t source;
    std::uint64_t target_owner_lease_generation = 0;
};

using accepted_record_authority_resolver_t = std::function<
  std::optional<accepted_record_authority_t> (
    const accepted_record_authority_query_t &)>;

accepted_record_authority_resolver_t
make_location_store_authority_resolver (
  zlink::framework::location_repository_t &store);

struct stateful_delivery_t
{
    object_ref_t owner;
    turn_record_t turn;
    protocol::application_payload_t payload;
    bool request = false;

    friend bool operator== (const stateful_delivery_t &,
                            const stateful_delivery_t &) = default;
};

class raw_stateful_dispatch_t
{
  public:
    raw_stateful_dispatch_t (
      stateful_object_runtime_t &objects,
      mesh::raw_mesh_node_owner_t &transport,
      accepted_record_authority_resolver_t authority_resolver = {});

    stateful_error_t ingest (const object_ref_t &owner);
    std::pair<stateful_error_t, std::optional<stateful_delivery_t>>
    try_claim (const object_ref_t &owner);
    stateful_error_t complete (
      const stateful_delivery_t &delivery,
      std::optional<protocol::application_payload_t> reply = std::nullopt);
    stateful_error_t stage_relocated (
      const object_ref_t &owner,
      turn_record_t turn,
      std::function<bool (
        const std::optional<protocol::application_payload_t> &)> terminal);
    bool complete_relocated_source (
      const object_ref_t &owner,
      std::uint64_t sequence,
      const protocol::reply_relay_t &relay,
      const std::optional<protocol::application_payload_t> &reply);
    bool acknowledge_relocated_source (
      const object_ref_t &owner,
      const protocol::wire_operation_id_t &operation);
    stateful_error_t discard_pending (const object_ref_t &owner);
    stateful_error_t discard_pending (
      const object_ref_t &owner,
      std::uint64_t sequence);

  private:
    struct delivery_key_t
    {
        object_kind_t kind;
        std::string key;
        std::uint64_t sequence;

        bool operator< (const delivery_key_t &other) const noexcept;
    };

    struct pending_delivery_t
    {
        object_ref_t owner;
        protocol::application_payload_t payload;
        mesh::service_mailbox_record_t transport;
        bool request = false;
        std::function<bool (
          const std::optional<protocol::application_payload_t> &)>
          relocated_terminal;
        std::shared_ptr<mesh::service_mailbox_claim_t> mailbox_claim;
        bool relocated_completing = false;
    };

    static std::string mailbox_owner (const object_ref_t &owner);
    static bool exact_fence (const object_ref_t &owner,
                             const protocol::actor_route_fence_t &fence);
    static bool exact_fence (const object_ref_t &owner,
                             const protocol::spot_route_fence_t &fence);
    static delivery_key_t delivery_key (
      const object_ref_t &owner,
      std::uint64_t sequence);

    stateful_object_runtime_t *_objects;
    mesh::raw_mesh_node_owner_t *_transport;
    accepted_record_authority_resolver_t _authority_resolver;
    std::mutex _mutex;
    std::condition_variable _pending_condition;
    std::map<std::pair<object_kind_t, std::string>, std::uint64_t>
      _next_sequence;
    std::map<delivery_key_t, pending_delivery_t> _pending;
    std::map<delivery_key_t, object_ref_t> _pending_reservations;
    std::map<delivery_key_t, object_ref_t> _discarding_owners;
};

enum class raw_relocation_replay_result_t
{
    no_data,
    applied,
    duplicate,
    ack_advanced,
    ack_ignored,
    not_registered,
    stale_fence,
    sequence_gap,
    conflicting_duplicate,
    restore_failed,
    transport_failed,
    terminal_received,
    terminal_duplicate,
    relay_acknowledged,
    persistence_failed,
    invalid
};

struct raw_relocation_target_registration_t
{
    protocol::relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    protocol::relocation_coordinator_fence_t coordinator;
    std::uint64_t participant_id = 0;
    std::vector<std::uint8_t> relocation_source_node_routing_id;
    std::uint64_t relocation_source_node_generation = 0;
    protocol::relocation_object_t object;
    std::function<bool (const protocol::relocation_data_t &)> stage;
    std::function<void (const protocol::relocation_data_t &)> rollback;
};

struct raw_relocation_source_registration_t
{
    protocol::relocation_id_t relocation;
    std::uint64_t target_attempt_generation = 0;
    protocol::relocation_coordinator_fence_t coordinator;
    std::uint64_t participant_id = 0;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t sent_high_water = 0;
    std::function<void (std::uint64_t)> acknowledged;
    std::vector<protocol::relocation_data_t> records;
};

struct raw_relocation_terminal_source_registration_t
{
    protocol::relocation_id_t relocation;
    protocol::relocation_coordinator_fence_t coordinator;
    protocol::wire_operation_id_t operation;
    protocol::request_source_fence_t request_source;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    std::uint64_t target_attempt_generation = 0;
    std::uint64_t participant_id = 0;
    std::uint64_t sequence = 0;
    std::uint64_t reply_route_id = 0;
    std::function<bool (
      const protocol::reply_relay_t &,
      const std::optional<protocol::application_payload_t> &)> complete;
};

struct raw_relocation_terminal_target_registration_t
{
    protocol::reply_relay_t relay;
    protocol::request_source_fence_t request_source;
    std::optional<protocol::application_payload_t> application_reply;
    std::function<bool (protocol::reply_relay_ack_status_t)> persist_ack;
    std::function<bool ()> persist_source_lease_expiry;
    std::vector<std::uint8_t> relay_destination_node_routing_id;
};

class raw_relocation_replay_coordinator_t
{
  public:
    using clock_t = std::chrono::steady_clock;
    explicit raw_relocation_replay_coordinator_t (
      mesh::raw_mesh_node_owner_t &transport,
      std::size_t terminal_record_limit = 1024,
      std::size_t terminal_byte_limit = 64u * 1024u * 1024u,
      std::chrono::milliseconds relay_retry_interval =
        std::chrono::seconds (1),
      std::chrono::milliseconds terminal_tombstone_retention =
        std::chrono::hours (24));

    bool register_target (raw_relocation_target_registration_t registration);
    bool seal_target (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id);
    bool drain_target (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id);
    bool unregister_target (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id);
    bool register_source (raw_relocation_source_registration_t registration);
    bool arm_source (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id);
    bool unregister_source (
      const protocol::relocation_id_t &, std::uint64_t, std::uint64_t);
    bool register_terminal_source (
      raw_relocation_terminal_source_registration_t registration);
    bool unregister_terminal_source (
      const protocol::relocation_id_t &,
      const protocol::wire_operation_id_t &);
    bool register_terminal_target (
      raw_relocation_terminal_target_registration_t registration);
    std::size_t retry_terminal_relays (clock_t::time_point now);
    std::size_t retry_source_replays (clock_t::time_point now);
    std::size_t reap_terminal_tombstones (clock_t::time_point now);
    bool confirm_terminal_source_lease_expired (
      const protocol::relocation_id_t &relocation,
      const protocol::wire_operation_id_t &operation,
      const protocol::request_source_fence_t &exact_source);
    std::size_t pending_terminal_relays () const;
    std::size_t terminal_retained_bytes () const;
    raw_relocation_replay_result_t pump_one ();
    raw_relocation_replay_result_t process (
      const mesh::service_mailbox_record_t &record);
    std::uint64_t target_high_water (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id) const;
    std::uint64_t source_ack_high_water (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id) const;
    std::size_t target_retained_identity_bytes (
      const protocol::relocation_id_t &relocation,
      std::uint64_t target_attempt_generation,
      std::uint64_t participant_id) const;

  private:
    struct key_t
    {
        protocol::relocation_id_t relocation;
        std::uint64_t target_attempt_generation = 0;
        std::uint64_t participant_id = 0;
        bool operator< (const key_t &other) const noexcept;
    };
    struct target_activity_guard_t
    {
        raw_relocation_replay_coordinator_t *owner = nullptr;
        key_t target;
        bool active = false;

        target_activity_guard_t (
          raw_relocation_replay_coordinator_t *owner,
          key_t target) noexcept;
        ~target_activity_guard_t () noexcept;
        void activate () noexcept { active = true; }
    };
    struct target_state_t
    {
        raw_relocation_target_registration_t registration;
        std::uint64_t high_water = 0;
        std::map<std::uint64_t, std::array<std::byte, 32>> accepted;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>
          accepted_operations;
        std::optional<std::uint64_t> staging_sequence;
        std::size_t accepted_bytes = 0;
        std::size_t staging_bytes = 0;
        std::size_t active_stages = 0;
        bool closing = false;
        bool removing = false;
    };
    struct target_group_state_t
    {
        std::size_t participant_count = 0;
        std::size_t record_count = 0;
        std::size_t byte_count = 0;
        std::map<std::pair<std::uint64_t, std::uint64_t>, key_t>
          staging_operations;
    };
    struct source_state_t
    {
        raw_relocation_source_registration_t registration;
        std::uint64_t high_water = 0;
        bool armed = false;
        bool acknowledging = false;
        clock_t::time_point next_retry{};
    };
    struct terminal_key_t
    {
        protocol::relocation_id_t relocation;
        protocol::wire_operation_id_t operation;
        bool operator< (const terminal_key_t &other) const noexcept;
    };
    struct terminal_source_state_t
    {
        raw_relocation_terminal_source_registration_t registration;
        bool completing = false;
        bool completed = false;
        std::array<std::byte, 32> digest{};
        clock_t::time_point completed_at{};
    };
    struct terminal_target_state_t
    {
        raw_relocation_terminal_target_registration_t registration;
        std::size_t retained_bytes = 0;
        clock_t::time_point next_retry{};
        bool acknowledging = false;
    };

    static key_t key (const protocol::relocation_id_t &relocation,
                      std::uint64_t target_attempt_generation,
                      std::uint64_t participant_id);
    void release_target_activity (const key_t &target) noexcept;
    static std::string mailbox_owner (const std::vector<std::uint8_t> &rid);
    raw_relocation_replay_result_t process_data (
      const mesh::service_mailbox_record_t &record,
      const protocol::relocation_data_t &data);
    raw_relocation_replay_result_t process_ack (
      const mesh::service_mailbox_record_t &record,
      const protocol::relocation_ack_t &ack);
    raw_relocation_replay_result_t process_reply_relay (
      const mesh::service_mailbox_record_t &record,
      const protocol::reply_relay_t &relay);
    raw_relocation_replay_result_t process_reply_relay_ack (
      const mesh::service_mailbox_record_t &record,
      const protocol::reply_relay_ack_t &ack);
    static terminal_key_t terminal_key (
      const protocol::relocation_id_t &relocation,
      const protocol::wire_operation_id_t &operation);

    mesh::raw_mesh_node_owner_t *_transport;
    mutable std::mutex _gate;
    std::condition_variable _gate_condition;
    std::map<key_t, target_state_t> _targets;
    std::map<key_t, target_group_state_t> _target_groups;
    std::map<key_t, source_state_t> _sources;
    std::map<terminal_key_t, terminal_source_state_t> _terminal_sources;
    std::map<terminal_key_t, terminal_target_state_t> _terminal_targets;
    std::size_t _terminal_record_limit;
    std::size_t _terminal_byte_limit;
    std::size_t _terminal_retained_bytes = 0;
    std::chrono::milliseconds _relay_retry_interval;
    std::chrono::milliseconds _terminal_tombstone_retention;
};

} // namespace zlink::framework::runtime::stateful
