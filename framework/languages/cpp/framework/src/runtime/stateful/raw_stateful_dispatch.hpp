/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/execution/state_lane.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <atomic>
#include <map>
#include <variant>
#include <tuple>
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
    // The source original the publication owner read when the record was accepted.
    std::optional<mesh::request_source_t> source_original;
    std::vector<std::uint8_t> target_node_routing_id;
    std::uint64_t target_node_generation = 0;
    protocol::frozen_source_kind_t source_kind = protocol::frozen_source_kind_t::node;
    std::optional<std::string> source_spot_id;
    std::optional<std::pair<std::string, std::uint64_t>> source_actor;
    std::optional<std::vector<std::uint8_t>> source_session_routing_id;
    std::uint64_t source_binding_generation = 0;
    std::uint64_t source_session_sequence = 0;
    // Packet address fence; target remains the exact native mailbox owner.
    std::uint64_t requested_authority_owner_generation = 0;
    std::uint64_t requested_object_generation = 0;
};

struct accepted_record_authority_t
{
    protocol::request_source_fence_t source;
    std::uint64_t target_owner_lease_generation = 0;
};

using accepted_record_authority_resolver_t =
  std::function<task_t<std::optional<accepted_record_authority_t>> (
    const accepted_record_authority_query_t &)>;

accepted_record_authority_resolver_t
make_location_store_authority_resolver (zlink::framework::location_repository_t &store);

// The original carries the mailbox slot credit across each ownership transfer.
using accepted_mailbox_reservation_t = mesh::service_mailbox_settlement_t;

// Immutable values of a claimed original that survive the authority suspension.
// Header and body frames are shared; the original stays in the canonical slot until take.
struct claimed_ingress_t
{
    std::optional<zlink::message_t> header;
    std::optional<zlink::message_t> body;
    std::vector<std::uint8_t> source_routing_id;
    std::uint64_t source_node_generation = 0;
    std::optional<mesh::request_source_t> source_original;
    bool bound_session = false;
    std::size_t body_bytes = 0;
    std::exception_ptr error;

    static claimed_ingress_t prepare (mesh::service_mailbox_record_t &original,
                                      live_location_reader_t *sources = nullptr,
                                      const std::string &mesh_name = {});
};

struct stateful_delivery_t
{
    object_ref_t owner;
    turn_record_t turn;
    protocol::frozen_record_t frozen;
    bool request = false;
    std::shared_ptr<runtime::serial_application_record_t> application_record;

    friend bool operator== (const stateful_delivery_t &left, const stateful_delivery_t &right)
    {
        return std::tie (left.owner, left.turn, left.frozen, left.request)
               == std::tie (right.owner, right.turn, right.frozen, right.request);
    }
};

class raw_stateful_dispatch_t
{
    friend struct raw_stateful_dispatch_test_access_t;

  public:
    raw_stateful_dispatch_t (stateful_object_runtime_t &objects,
                             mesh::raw_mesh_node_owner_t &transport,
                             accepted_record_authority_resolver_t authority_resolver = {});

    /* Flow-capture provider (flow-correlation §4): gates whether ingest
     * validates/materializes wire flow fields as flow values. Unset keeps
     * capture on. Set once before dispatch begins. */
    void set_flow_capture_provider (std::function<bool ()> provider) noexcept
    {
        _flow_capture = std::move (provider);
    }

    // An accepted original: its canonical mailbox slot, taken in the lane turn that places
    // it, or an original a relocation cut already took before its owner's seal bound.
    using ingress_original_t =
      std::variant<mesh::service_mailbox_t::claim_handle_t, mesh::service_mailbox_record_t>;
    task_t<stateful_error_t> ingest_claimed (object_ref_t owner,
                                             ingress_original_t original,
                                             std::optional<retained_relocation_source_t> retained,
                                             std::optional<claimed_ingress_t> prepared = {});
    task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
    accepted_actor_authority (object_ref_t owner,
                              protocol::actor_message_header_t header,
                              std::vector<std::uint8_t> source_node_routing_id,
                              std::uint64_t source_node_generation,
                              std::optional<mesh::request_source_t> source_original,
                              bool bound_session_routed,
                              std::optional<retained_relocation_source_t> retained) const;
    task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
    accepted_spot_authority (object_ref_t owner,
                             protocol::spot_message_header_t header,
                             std::vector<std::uint8_t> source_node_routing_id,
                             std::uint64_t source_node_generation,
                             std::optional<mesh::request_source_t> source_original,
                             std::optional<retained_relocation_source_t> retained) const;
    task_t<bool> forward_accepted (object_ref_t owner,
                                   turn_record_t turn,
                                   std::vector<std::uint8_t> target_routing_id,
                                   std::uint64_t target_generation,
                                   std::uint64_t target_lease_generation,
                                   std::chrono::milliseconds window,
                                   std::optional<std::uint64_t> target_authority_generation);
    std::pair<stateful_error_t, std::optional<stateful_delivery_t>>
    try_claim (const object_ref_t &owner,
               std::function<void ()> *before_application_handler = nullptr,
               std::string *stable_type = nullptr);
    // taken receives the original when this admission takes it; its caller replies from
    // it when the turn is not accepted. The original's mailbox credit returns here.
    task_t<stateful_error_t> accept_local (
      const object_ref_t &owner,
      std::shared_ptr<runtime::serial_application_record_t> application_record,
      ingress_original_t original,
      std::optional<mesh::service_mailbox_record_t> &taken,
      std::function<bool (const std::optional<protocol::application_payload_t> &)> terminal);
    task_t<stateful_error_t>
    complete_async (const stateful_delivery_t &delivery,
                    std::optional<protocol::application_payload_t> reply = std::nullopt);
    // Transfer returns the native active claim while its request remains pending.
    stateful_error_t release_application_claim (const object_ref_t &owner);
    stateful_error_t transfer_application_claim (const object_ref_t &owner, std::uint64_t sequence);
    stateful_error_t complete (const stateful_delivery_t &delivery,
                               std::optional<protocol::application_payload_t> reply = std::nullopt);
    stateful_error_t
    complete (const stateful_delivery_t &delivery,
              std::optional<protocol::application_payload_t> reply,
              const std::shared_ptr<runtime::serial_application_record_t> &accepted_record);
    task_t<stateful_error_t> stage_relocated (
      const object_ref_t &owner,
      turn_record_t turn,
      std::function<bool (const std::optional<protocol::application_payload_t> &)> terminal);
    task_t<bool>
    complete_relocated_source_async (const object_ref_t &owner,
                                     std::uint64_t sequence,
                                     const protocol::reply_relay_t &relay,
                                     const std::optional<protocol::application_payload_t> &reply);
    stateful_error_t discard_pending (const object_ref_t &owner);
    void release_relocated_payloads (const object_ref_t &owner);
    /* The expired-owner relocation terminal (28 §4.4): each pending request
     * of the owner gets one Unavailable terminal on its original reply
     * route, then every pending record is dropped as by discard_pending. */
    stateful_error_t fail_pending_unavailable (const object_ref_t &owner);
    stateful_error_t discard_pending (const object_ref_t &owner, std::uint64_t sequence);

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
        // Terminal identity only. The native turn retains the accepted body
        // and canonical bytes; claim reads its dispatch view from that owner.
        protocol::frozen_record_t frozen;
        mesh::service_mailbox_record_t transport;
        bool request = false;
        std::function<bool (const std::optional<protocol::application_payload_t> &)>
          relocated_terminal;
        accepted_mailbox_reservation_t mailbox_claim;
        bool relocated_completing = false;
    };

    stateful_error_t complete_with_claim (const stateful_delivery_t &delivery,
                                          std::optional<protocol::application_payload_t> reply,
                                          const std::function<stateful_error_t ()> &release_claim);
    task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
    resolve_accepted_authority (accepted_record_authority_query_t query,
                                std::uint64_t target_owner_lease_generation,
                                std::optional<retained_relocation_source_t> retained) const;
    stateful_error_t discard_owner_pending (const object_ref_t &owner, bool reply_unavailable);
    static bool matches_application_route (const object_ref_t &owner,
                                           const protocol::actor_route_fence_t &route);
    static bool matches_application_route (const object_ref_t &owner,
                                           const protocol::spot_route_fence_t &route);
    static delivery_key_t delivery_key (const object_ref_t &owner, std::uint64_t sequence);
    // Values the lane turn of one ingress admission returns to its caller.
    struct ingress_admission_t
    {
        bool registered = false;
        delivery_key_t pending_key{};
        bool inserted_sequence = false;
        bool trace_admitted = false;
        std::optional<protocol::application_payload_t> admitted_application;
        protocol::frozen_record_kind_t admitted_kind{};
        protocol::wire_operation_id_t admitted_operation{};
        // The pending entry of a taken original this admission did not accept.
        std::optional<pending_delivery_t> rejected;
    };
    // Relocation flow §4.2: take moves the original into pending in the state-lane turn
    // that places the turn and reports whether its owner's seal cut preceded the take; it
    // returns false when the original is gone.
    template <class Take>
    auto admit_accepted_ingress (const object_ref_t &owner,
                                 turn_record_t &turn,
                                 pending_delivery_t &pending,
                                 Take &take,
                                 stateful_error_t collision_error,
                                 ingress_admission_t &admission);
    template <class Take>
    task_t<stateful_error_t> commit_accepted_ingress (const object_ref_t &owner,
                                                      turn_record_t turn,
                                                      pending_delivery_t pending,
                                                      Take take,
                                                      stateful_error_t collision_error,
                                                      ingress_admission_t &admission);
    stateful_error_t register_accepted_ingress (const object_ref_t &owner,
                                                turn_record_t &turn,
                                                pending_delivery_t &pending,
                                                stateful_error_t collision_error,
                                                ingress_admission_t &admission) noexcept;
    stateful_error_t finish_accepted_ingress (const object_ref_t &owner,
                                              stateful_object_runtime_t::enqueue_outcome_t outcome,
                                              ingress_admission_t &admission);

    bool capture_flow () const { return !_flow_capture || _flow_capture (); }

    stateful_object_runtime_t *_objects;
    mesh::raw_mesh_node_owner_t *_transport;
    std::function<bool ()> _flow_capture;
    accepted_record_authority_resolver_t _authority_resolver;
    std::mutex _mutex;
    std::map<std::pair<object_kind_t, std::string>, std::uint64_t> _next_sequence;
    std::map<delivery_key_t, pending_delivery_t> _pending;
    std::map<delivery_key_t, object_ref_t> _discarding_owners;
};

enum class raw_relocation_replay_result_t
{
    no_data,
    applied,
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
    std::vector<std::uint8_t> relocation_source_node_routing_id;
    std::uint64_t relocation_source_node_generation = 0;
    protocol::relocation_object_t object;
    std::function<bool (const protocol::relocation_data_t &)> stage;
    std::function<void (const protocol::relocation_data_t &)> rollback;
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
    std::function<bool (const protocol::reply_relay_t &,
                        const std::optional<protocol::application_payload_t> &)>
      complete;
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
      std::chrono::milliseconds relay_retry_interval = std::chrono::seconds (1),
      std::chrono::milliseconds terminal_tombstone_retention = std::chrono::hours (24));

    /* Flow-capture provider (flow-correlation §4): gates whether replayed
     * reply payloads validate/materialize wire flow fields as flow values.
     * Unset keeps capture on. Set once before replay begins. */
    void set_flow_capture_provider (std::function<bool ()> provider) noexcept
    {
        _flow_capture = std::move (provider);
    }

    bool register_target (raw_relocation_target_registration_t registration);
    bool unregister_target (const protocol::relocation_id_t &relocation,
                            std::uint64_t target_attempt_generation,
                            const protocol::relocation_object_t &object);
    bool register_terminal_source (raw_relocation_terminal_source_registration_t registration);
    task_t<void> unregister_terminal_sources (const protocol::relocation_id_t &relocation);
    bool unregister_terminal_source (const protocol::relocation_id_t &,
                                     const protocol::wire_operation_id_t &);
    bool register_terminal_target (raw_relocation_terminal_target_registration_t registration);
    // True when a relay retry or a tombstone reap may be due at now. The
    // coordinator keeps an early bound of its next due instant (spec 08
    // section 4.2), so a rotation with nothing due enters no lane.
    bool due (clock_t::time_point now) const noexcept;
    task_t<std::size_t> retry_terminal_relays (clock_t::time_point now);
    std::size_t reap_terminal_tombstones (clock_t::time_point now);
    std::optional<clock_t::time_point> next_activity () const;
    bool
    confirm_terminal_source_lease_expired (const protocol::relocation_id_t &relocation,
                                           const protocol::wire_operation_id_t &operation,
                                           const protocol::request_source_fence_t &exact_source);
    std::size_t pending_terminal_relays () const;
    std::size_t terminal_retained_bytes () const;
    task_t<raw_relocation_replay_result_t> pump_one ();
    task_t<raw_relocation_replay_result_t> process (const mesh::service_mailbox_record_t &record);

  private:
    struct key_t
    {
        protocol::relocation_id_t relocation;
        std::uint64_t target_attempt_generation = 0;
        protocol::relocation_object_t object;
        bool operator< (const key_t &other) const noexcept;
    };
    struct target_activity_guard_t
    {
        raw_relocation_replay_coordinator_t *owner = nullptr;
        key_t target;
        bool active = false;

        target_activity_guard_t (raw_relocation_replay_coordinator_t *owner, key_t target) noexcept;
        ~target_activity_guard_t () noexcept;
        void activate () noexcept { active = true; }
    };
    struct target_state_t
    {
        raw_relocation_target_registration_t registration;
        std::size_t active_stages = 0;
        bool closing = false;
        bool removing = false;
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
                      const protocol::relocation_object_t &object);
    void release_target_activity (const key_t &target) noexcept;
    static std::string mailbox_owner (const std::vector<std::uint8_t> &rid);
    raw_relocation_replay_result_t process_data (const mesh::service_mailbox_record_t &record,
                                                 const protocol::relocation_data_t &data);
    task_t<raw_relocation_replay_result_t>
    process_reply_relay (const mesh::service_mailbox_record_t &record,
                         const protocol::reply_relay_t &relay);
    task_t<raw_relocation_replay_result_t>
    process_reply_relay_ack (const mesh::service_mailbox_record_t &record,
                             const protocol::reply_relay_ack_t &ack);
    static terminal_key_t terminal_key (const protocol::relocation_id_t &relocation,
                                        const protocol::wire_operation_id_t &operation);

    bool capture_flow () const { return !_flow_capture || _flow_capture (); }

    mesh::raw_mesh_node_owner_t *_transport;
    std::function<bool ()> _flow_capture;
    offload_executor_t _lane_executor;
    mutable state_lane_t _lane;
    std::mutex _activity_wait_mutex;
    std::condition_variable _activity_changed;
    std::map<key_t, target_state_t> _targets;
    std::map<terminal_key_t, terminal_source_state_t> _terminal_sources;
    std::map<terminal_key_t, terminal_target_state_t> _terminal_targets;
    std::size_t _terminal_retained_bytes = 0;
    std::chrono::milliseconds _relay_retry_interval;
    std::chrono::milliseconds _terminal_tombstone_retention;
    // Earliest instant a retry or reap can be due, written on the lane by the
    // scan; a lane turn that can make work due earlier resets it to due_now.
    static constexpr clock_t::rep due_now = std::numeric_limits<clock_t::rep>::min ();
    static constexpr clock_t::rep never_due = std::numeric_limits<clock_t::rep>::max ();
    mutable std::atomic<clock_t::rep> _next_due{due_now};
    void publish_next_due_on_lane () const;
    task_t<std::size_t> retry_due_terminal_relays (clock_t::time_point now);
};

} // namespace zlink::framework::runtime::stateful
