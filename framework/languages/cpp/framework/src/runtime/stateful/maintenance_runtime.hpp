/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/protocol/relocation_envelope_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/relocation_transfer.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"

#include <runtime/locations/location_repository.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace zlink::framework::runtime::stateful
{

using inventory_digest_t = std::array<std::uint8_t, 32>;

struct relocation_stored_t
{
    std::string reference;
    std::uint32_t checksum_crc32c = 0;
};

class relocation_store_port_t
{
  public:
    virtual ~relocation_store_port_t () = default;
    virtual relocation_stored_t put (
      const std::vector<std::uint8_t> &payload,
      std::chrono::hours retention) = 0;
    virtual std::optional<std::vector<std::uint8_t>>
    get (const std::string &reference) = 0;
    virtual void remove (const std::string &reference) = 0;
};

struct durable_session_journal_record_t
{
    std::uint64_t relocation_id_high = 0;
    std::uint64_t relocation_id_low = 0;
    object_ref_t actor;
    std::uint64_t binding_generation = 0;
    std::uint64_t last_accepted_session_sequence = 0;
    std::vector<std::uint8_t> accepted_journal;

    friend bool operator== (const durable_session_journal_record_t &,
                            const durable_session_journal_record_t &) = default;
};

struct durable_session_journal_root_t
{
    std::string reference;
    std::uint32_t checksum_crc32c = 0;

    friend bool operator== (const durable_session_journal_root_t &,
                            const durable_session_journal_root_t &) = default;
};

class durable_session_journal_store_t
{
  public:
    explicit durable_session_journal_store_t (
      std::shared_ptr<relocation_store_port_t> store);

    durable_session_journal_root_t prepare (
      const durable_session_journal_record_t &record);
    std::optional<durable_session_journal_record_t> recover (
      const durable_session_journal_root_t &root) const;
    void cleanup (const durable_session_journal_root_t &root);

  private:
    std::shared_ptr<relocation_store_port_t> _store;
};

struct authority_relocation_reference_t
{
    object_ref_t source;
    object_ref_t target;
    std::string relocation_reference;
    std::uint32_t checksum_crc32c = 0;
    inventory_digest_t inventory_digest{};
    location_owner_token_t target_owner;
    std::vector<std::byte> application_payload;
};

enum class authority_publish_status_t
{
    published,
    conflict,
    failed
};

struct authority_publish_result_t
{
    authority_publish_status_t status = authority_publish_status_t::failed;
    std::optional<authority_relocation_reference_t> current;
};

/* One participant of a relocation unit as the target derives it from
 * Location Store authority rows (28 §4.2): identity, stable type, and — for
 * an Actor — its Spot membership as projected into the authority payload. */
struct relocation_participant_identity_t
{
    object_ref_t owner;
    std::string stable_type;
    /* Actor rows only: (spotId, spotGeneration) membership projection. */
    std::optional<std::pair<std::string, std::uint64_t>> spot_membership;

    friend bool operator== (const relocation_participant_identity_t &,
                            const relocation_participant_identity_t &)
      = default;
};

class authority_relocation_port_t
{
  public:
    virtual ~authority_relocation_port_t () = default;
    /* Enumerates every live authority row (the repository scans the
     * "authority\0…" preimage prefix). The target reconstructs the
     * canonical ordered participant inventory of an inbound relocation
     * from this listing. std::nullopt means enumeration is unavailable. */
    virtual std::optional<std::vector<relocation_participant_identity_t>>
    list_participant_identities ()
    {
        return std::nullopt;
    }
    virtual authority_publish_result_t publish (
      const object_ref_t &source,
      const object_ref_t &target,
      location_owner_token_t target_owner,
      object_creation_target_t target_placement,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest,
      std::vector<std::byte> target_application_payload = {}) = 0;
    virtual std::optional<authority_relocation_reference_t>
    read (object_kind_t kind, const std::string &key) = 0;
    virtual authority_publish_result_t publish_completion (
      object_kind_t,
      const std::string &,
      const std::string &,
      std::uint64_t,
      std::string,
      std::uint32_t)
    {
        return {};
    }
    virtual authority_publish_result_t replace_completion (
      object_kind_t,
      const std::string &,
      std::uint64_t,
      const std::string &,
      std::uint32_t,
      std::string,
      std::uint32_t)
    {
        return {};
    }
    virtual bool release_completion (
      object_kind_t,
      const std::string &,
      std::uint64_t,
      const std::string &,
      std::uint32_t)
    {
        return false;
    }
};

struct aggregate_relocation_fence_t
{
    std::uint64_t value = 0;
    // The public port keeps the durable Store identity with the opaque
    // process-facing value so commit and abort can resume after restart.
    aggregate_fence_t durable_fence;
};

enum class aggregate_publish_status_t
{
    prepared,
    committed,
    conflict,
    failed
};

struct aggregate_publish_result_t
{
    aggregate_publish_status_t status = aggregate_publish_status_t::failed;
    aggregate_relocation_fence_t fence;
    std::vector<authority_relocation_reference_t> current;
};

class aggregate_authority_port_t
{
  public:
    virtual ~aggregate_authority_port_t () = default;
    virtual aggregate_publish_result_t prepare (
      const std::vector<object_ref_t> &sources,
      std::string target_node_id,
      location_owner_token_t target_owner,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) = 0;
    virtual aggregate_publish_result_t commit (
      aggregate_relocation_fence_t fence) = 0;
    virtual void abort (aggregate_relocation_fence_t fence) = 0;
};

struct relocation_unit_t
{
    std::vector<object_ref_t> participants;

    friend bool operator== (const relocation_unit_t &,
                            const relocation_unit_t &) = default;
};

struct eligible_relocation_unit_t
{
    relocation_unit_t unit;
    std::string target_node_id;
    location_owner_token_t target_owner;
    std::size_t encoded_upper_bound = 0;
    inventory_digest_t inventory_digest{};
    struct canonical_wire_context_t
    {
        protocol::relocation_id_t relocation;
        std::uint64_t target_attempt_generation = 0;
        protocol::relocation_coordinator_fence_t coordinator;
        std::vector<std::uint8_t> target_node_routing_id;
        std::uint64_t target_node_generation = 0;
        std::vector<protocol::session_relocation_route_t> session_routes;
        std::function<task_t<std::optional<
          std::vector<protocol::session_relocation_route_t>>> ()>
          capture_session_routes;
        /* Sends the Restore request carrying the payload manifest. The
         * request frame must be enqueued on the ordered mesh connection
         * before the first suspension so the relocationState chunks the
         * caller sends next arrive after it. The returned task completes
         * once the target's relay-ready reply arrives (after the target
         * assembled, verified, and restored the payload). The captured
         * bound-session commit routes ride beside the request: the schema
         * relocation-envelope-v1 payload deliberately has no session-route
         * section, and command 44 commit must still originate from the
         * target runtime (20 §5), so the target stages them here. */
        std::function<task_t<bool> (
          const std::vector<frozen_object_state_t> &,
          const relocation_payload_manifest_t &,
          const std::vector<protocol::session_relocation_route_t> &)>
          prepare_target;
        /* Sends one relocationState chunk on the same ordered mesh
         * connection as the Restore request and the relay lane. */
        std::function<task_t<bool> (
          const protocol::relocation_state_t &)> send_state_chunk;
        std::function<task_t<bool> (
          const std::vector<protocol::relocation_data_t> &,
          const relocation_ingress_batch_t &)> send_relocation_data;
        enum class cutover_enqueue_t
        {
            not_enqueued,
            enqueued,
            uncertain
        };
        std::function<task_t<cutover_enqueue_t> (
          const protocol::relocation_cutover_t &)> send_cutover;
        // This is valid only after an exact target failure and before a
        // Cutover enqueue.  It fences source admission restoration.
        std::function<bool ()> abort_target_before_cutover;
    };
    std::optional<canonical_wire_context_t> canonical_wire;
};

enum class target_preflight_status_t
{
    eligible,
    target_unavailable,
    store_unavailable,
    relocation_disabled,
    state_incompatible
};

struct target_preflight_result_t
{
    target_preflight_status_t status =
      target_preflight_status_t::target_unavailable;
    std::vector<eligible_relocation_unit_t> units;
};

class target_preflight_port_t
{
  public:
    virtual ~target_preflight_port_t () = default;
    virtual target_preflight_result_t preflight (
      const std::vector<relocation_unit_t> &units) = 0;
};

struct maintenance_provider_set_t
{
    std::shared_ptr<authority_relocation_port_t> authority;
    std::shared_ptr<aggregate_authority_port_t> aggregate_authority;
    std::shared_ptr<relocation_store_port_t> relocations;
    std::shared_ptr<target_preflight_port_t> targets;
};

struct relocation_limits_t
{
    std::size_t outbound_units = 64;
    std::size_t inbound_units = 64;
    std::size_t capture_callbacks = 8;
    std::size_t restore_callbacks = 8;
    /* Direct-transfer options (location_options_t snapshot at startup). */
    std::uint64_t payload_chunk_limit_bytes = 262144;
    std::uint64_t in_flight_payload_budget_bytes = 16777216;
    std::uint64_t node_in_flight_payload_budget_bytes = 0;
    std::chrono::milliseconds cutover_wait_timeout{1000};
    /* 18 §2.4: the Message Follow route stays installed on the source for
     * this long after cutover before it becomes removable. S4 (route_
     * convergence end / SafeToShutdown's per-unit condition) is anchored
     * on this deadline, not on the retransmission window. */
    std::chrono::milliseconds message_follow_duration{30000};
};

/* Phase 1 conservative in-flight cap. Until the Core exposes per-pipe
 * effective HWM and accounted-charge observation, the effective budget is
 * min(configured, this constant). */
inline constexpr std::uint64_t
  relocation_conservative_in_flight_budget_bytes = 16777216;

struct relocation_gate_snapshot_t
{
    std::size_t outbound_units = 0;
    std::size_t inbound_units = 0;
    std::size_t capture_callbacks = 0;
    std::size_t restore_callbacks = 0;

    friend bool operator== (const relocation_gate_snapshot_t &,
                            const relocation_gate_snapshot_t &) = default;
};

enum class relocation_terminal_t
{
    completed,
    blocked,
    conflict,
    store_failed,
    data_lost,
    recovery_required
};

enum class relocation_reason_t
{
    none,
    permit_unavailable,
    turn_active,
    payload_bound_exceeded,
    store_write_failed,
    checksum_mismatch,
    authority_conflict,
    authority_publish_failed,
    payload_missing,
    inventory_mismatch,
    restore_failed,
    bound_session_fence_incomplete
};

struct target_only_cas_t
{
    std::vector<object_ref_t> sources;
    std::string target_node_id;
    location_owner_token_t target_owner;
    inventory_digest_t inventory_digest{};
    relocation_payload_manifest_t manifest;
};

struct relocation_result_t
{
    relocation_terminal_t terminal = relocation_terminal_t::blocked;
    relocation_reason_t reason = relocation_reason_t::none;
    std::optional<authority_relocation_reference_t> authority;
    std::vector<protocol::relocation_data_t> replay_records;
    // Target-only Location Store CAS is deliberately outside the source
    // maintenance path.  The public-host integration consumes this handoff.
    std::optional<target_only_cas_t> target_handoff;
    /* S0 (source admission seal) to S1 (cutover submit terminal), on the
     * source clock. Zero when the unit never reached the seal. */
    std::chrono::steady_clock::duration source_stall{};
};

struct aggregate_relocation_result_t
{
    relocation_terminal_t terminal = relocation_terminal_t::blocked;
    relocation_reason_t reason = relocation_reason_t::none;
    std::vector<authority_relocation_reference_t> authority;
    std::vector<protocol::relocation_data_t> replay_records;
    std::optional<target_only_cas_t> target_handoff;
};

class raw_relocation_replay_coordinator_t;

class maintenance_runtime_t
{
  public:
    using observer_t = std::function<void (const relocation_result_t &)>;

    maintenance_runtime_t (
      stateful_object_runtime_t &objects,
      std::shared_ptr<authority_relocation_port_t> authority,
      std::shared_ptr<relocation_store_port_t> relocations,
      relocation_limits_t limits = {},
      observer_t observer = {},
      std::shared_ptr<aggregate_authority_port_t>
        aggregate_authority = {});
    maintenance_runtime_t (
      stateful_object_runtime_t &objects,
      maintenance_provider_set_t providers,
      relocation_limits_t limits = {},
      observer_t observer = {});

    task_t<relocation_result_t> relocate (
      const object_ref_t &source,
      std::string target_node_id,
      location_owner_token_t target_owner,
      std::size_t encoded_upper_bound,
      inventory_digest_t inventory_digest,
      const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
        &canonical_wire = std::nullopt,
      std::stop_token cancellation = {},
      /* Target-advertised inbound chunk-size cap (bytes), e.g. from a
       * join reply; 0 = not advertised, this unit's local budget applies
       * unchanged. */
      std::uint64_t advertised_receive_chunk_limit_bytes = 0);
    relocation_result_t recover (
      object_kind_t kind,
      const std::string &key,
      stateful_object_runtime_t &target,
      std::stop_token cancellation = {});
    aggregate_relocation_result_t recover_aggregate (
      const std::vector<object_ref_t> &sources,
      stateful_object_runtime_t &target,
      std::stop_token cancellation = {});
    task_t<aggregate_relocation_result_t> relocate_aggregate (
      const std::vector<object_ref_t> &sources,
      std::string target_node_id,
      location_owner_token_t target_owner,
      std::size_t encoded_upper_bound,
      inventory_digest_t inventory_digest,
      const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
        &canonical_wire = std::nullopt,
      std::stop_token cancellation = {},
      std::uint64_t advertised_receive_chunk_limit_bytes = 0);

    void attach_relocation_wire (
      raw_relocation_replay_coordinator_t &wire) noexcept;

    relocation_gate_snapshot_t gate_snapshot () const;

    /* Optional source-local metric (25 §"zlink.relocation.route_convergence"):
     * one unit's cutover submit terminal to its retransmission window
     * closing. */
    void configure_route_convergence_metric (
      std::function<void (double)> metric) noexcept;

    /* SafeToShutdown (24 §"State"): true only while no relocation unit this
     * source started still has an open retransmission window. A unit opens
     * its window at cutover submit terminal and closes it when the
     * retained payload/boundary copies are released (28 §4.4), which this
     * runtime also treats as the point the unit's Message Follow route
     * becomes removable — no separate Message Follow expiry timer exists
     * in this runtime to measure that condition independently. */
    bool relocation_units_settled () const noexcept;

    static std::uint32_t crc32c (
      const std::vector<std::uint8_t> &payload) noexcept;
    /* min(local_limit_bytes, advertised>0 ? advertised : local_limit_bytes)
     * — the direct-transfer chunk-plan cap applied when a target has
     * advertised a receive_chunk_limit_bytes (e.g. via a join reply); 0
     * for either side means "not advertised"/"unset", in which case the
     * other side's value applies unchanged. */
    static std::uint64_t apply_advertised_receive_chunk_limit (
      std::uint64_t local_limit_bytes,
      std::uint64_t advertised_receive_chunk_limit_bytes) noexcept;
    /* The direct-transfer wire payload is exactly the schema's
     * relocation-envelope-v1 logical stream (28 §4.2). The codec owns the
     * bytes; these statics own the mapping between the runtime's frozen
     * object model and the stream. */
    static std::vector<std::uint8_t> encode_envelope (
      const std::vector<frozen_object_state_t> &participants,
      const protocol::relocation_id_t &relocation);
    static std::optional<protocol::relocation_envelope_t> decode_envelope (
      const std::vector<std::uint8_t> &payload) noexcept;
    /* Target-side model mapping: the caller supplies the store-derived
     * participant inventory (authority rows), which this method sorts by
     * UTF-8 authority-key bytes to assign participantId = index + 1. */
    static std::optional<std::vector<frozen_object_state_t>>
    materialize_envelope (
      const protocol::relocation_envelope_t &envelope,
      std::vector<relocation_participant_identity_t> inventory) noexcept;
    /* The derived inventory digest (no wrapper digest exists any more):
     * SHA-256 over the (kind, key)-sorted participants' key bytes and
     * interleaved generation bytes — the same value every source computes
     * before publishing an authority row. */
    static inventory_digest_t compute_inventory_digest (
      const std::vector<object_ref_t> &participants);

  private:
    struct relocation_terminal_state_t;

    class permit_t
    {
      public:
        permit_t () = default;
        explicit permit_t (maintenance_runtime_t *owner);
        ~permit_t ();
        permit_t (permit_t &&other) noexcept;
        permit_t &operator= (permit_t &&other) noexcept;
        permit_t (const permit_t &) = delete;
        permit_t &operator= (const permit_t &) = delete;

        explicit operator bool () const noexcept;

      private:
        maintenance_runtime_t *_owner = nullptr;
    };

    permit_t try_acquire ();
    task_t<relocation_result_t> relocate_terminal (
      std::shared_ptr<relocation_terminal_state_t> state);
    task_t<bool> relocate_seal (
      std::shared_ptr<relocation_terminal_state_t> state);
    task_t<bool> capture_relocation_session_routes (
      eligible_relocation_unit_t::canonical_wire_context_t &context);
    bool relocate_encode (
      const std::shared_ptr<relocation_terminal_state_t> &state);
    task_t<bool> relocate_prepare_target (
      std::shared_ptr<relocation_terminal_state_t> state);
    task_t<bool> relocate_send_state_chunks (
      std::shared_ptr<relocation_terminal_state_t> state);
    task_t<bool> relocate_boundary_and_send (
      std::shared_ptr<relocation_terminal_state_t> state);
    task_t<bool> relocate_cutover (
      std::shared_ptr<relocation_terminal_state_t> state);
    std::uint64_t effective_in_flight_budget () const noexcept;
    task_t<void> acquire_transfer_budget (std::uint64_t bytes);
    void release_transfer_budget (std::uint64_t bytes) noexcept;
    void retain_retransmission_copies (
      std::shared_ptr<relocation_terminal_state_t> state);
    /* SafeToShutdown must observe a relocation unit as pending from the
     * moment it is sealed (registered), not only once it reaches S1
     * (cutover submit terminal, where retain_retransmission_copies used
     * to be the sole increment site) — otherwise a shutdown query racing
     * the seal-to-S1 window can see relocation_units_settled() report
     * true while a unit is still actively transferring. The returned
     * token increments pending_units immediately and decrements it again
     * on destruction (whether that is an early failure return dropping
     * the owning relocation_terminal_state_t, or the S4+window observer
     * in retain_retransmission_copies explicitly releasing it). */
    std::shared_ptr<void> begin_pending_relocation_unit () noexcept;
    void release () noexcept;
    relocation_result_t finish (relocation_result_t result);
    std::optional<std::vector<protocol::relocation_data_t>>
    build_boundary_records (
      const std::vector<frozen_object_state_t> &participants,
      const eligible_relocation_unit_t::canonical_wire_context_t &context,
      const relocation_ingress_batch_t &batch,
      relocation_reason_t &failure_reason) const;
    task_t<bool> prepare_target (
      const eligible_relocation_unit_t::canonical_wire_context_t &context,
      const std::vector<frozen_object_state_t> &participants,
      const relocation_payload_manifest_t &manifest);
    task_t<bool> send_boundary_records (
      const eligible_relocation_unit_t::canonical_wire_context_t &context,
      const std::vector<protocol::relocation_data_t> &records,
      const relocation_ingress_batch_t &batch);
    bool abort_target_before_cutover (
      const eligible_relocation_unit_t::canonical_wire_context_t &context) noexcept;

    stateful_object_runtime_t &_objects;
    std::shared_ptr<authority_relocation_port_t> _authority;
    std::shared_ptr<aggregate_authority_port_t> _aggregate_authority;
    std::shared_ptr<relocation_store_port_t> _relocations;
    relocation_limits_t _limits;
    observer_t _observer;
    mutable std::mutex _gate_mutex;
    relocation_gate_snapshot_t _gate;
    raw_relocation_replay_coordinator_t *_relocation_wire = nullptr;
    /* route_convergence metric + SafeToShutdown obligation count, held
     * separately from `this` in a shared_ptr: the retention coroutine in
     * retain_retransmission_copies is detached (self-keeping via
     * observe_task_completion) and outlives an owning maintenance_runtime_t
     * that is torn down while a retransmission window is still open. The
     * coroutine and its completion callback capture this shared_ptr by
     * value instead of `this`, so they never dereference a dead runtime;
     * the pending counter only ever reflects windows that were still open
     * when the runtime itself was destroyed. */
    struct shutdown_tracking_state_t
    {
        std::atomic<std::int64_t> pending_units{0};
        std::mutex metric_mutex;
        std::function<void (double)> route_convergence_metric;
    };
    std::shared_ptr<shutdown_tracking_state_t> _shutdown_tracking =
      std::make_shared<shutdown_tracking_state_t> ();
    /* Phase 1 in-flight transfer budget: accounted bytes of relocation
     * chunks between submit and send terminal, node-wide. Waiters queue
     * FIFO; a new unit acquires headroom before its source admission seal. */
    mutable std::mutex _budget_mutex;
    std::uint64_t _budget_in_flight_bytes = 0;
    std::deque<std::pair<std::uint64_t,
                         std::shared_ptr<detail::task_completion_source_t<bool>>>>
      _budget_waiters;
};

// This state gates maintenance admission only. The public seven-state host
// lifecycle projects into it one way; callers must not treat this narrower
// state machine as the public host authority.
enum class maintenance_admission_state_t
{
    preparing,
    serving,
    retiring,
    draining,
    stopped,
    error
};

enum class termination_intent_t
{
    retire,
    shutdown
};

enum class termination_outcome_t
{
    stopped,
    blocked,
    force_stopped
};

enum class termination_reason_t
{
    none,
    target_unavailable,
    store_unavailable,
    relocation_disabled,
    state_incompatible,
    deadline_exceeded,
    relocation_failed,
    teardown_failed,
    runtime_not_ready
};

struct termination_result_t
{
    termination_intent_t effective_intent =
      termination_intent_t::shutdown;
    termination_outcome_t outcome = termination_outcome_t::blocked;
    termination_reason_t reason = termination_reason_t::runtime_not_ready;

    friend bool operator== (const termination_result_t &,
                            const termination_result_t &) = default;
};

class host_maintenance_runtime_t
{
  public:
    using observer_t = std::function<void (const termination_result_t &)>;

    host_maintenance_runtime_t (
      stateful_object_runtime_t &objects,
      stream_session_registry_t &sessions,
      maintenance_runtime_t &relocation,
      std::shared_ptr<target_preflight_port_t> targets,
      observer_t observer = {});

    void mark_serving ();
    void mark_error ();
    maintenance_admission_state_t state () const;
    task_t<termination_result_t> terminate (termination_intent_t intent);
    std::optional<termination_result_t> terminal_result () const;
    std::optional<termination_intent_t> intent_snapshot () const;

  private:
    static std::vector<relocation_unit_t> inventory_units (
      std::vector<object_inventory_t> inventory);
    task_t<termination_result_t> run_retire ();
    task_t<termination_result_t> run_termination_attempt (
      termination_intent_t intent, std::uint64_t attempt);
    termination_result_t run_shutdown (
      termination_intent_t effective_intent);
    void complete_attempt (
      std::uint64_t attempt, const termination_result_t &result);

    stateful_object_runtime_t &_objects;
    stream_session_registry_t &_sessions;
    maintenance_runtime_t &_relocation;
    std::shared_ptr<target_preflight_port_t> _targets;
    observer_t _observer;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    maintenance_admission_state_t _state =
      maintenance_admission_state_t::preparing;
    bool _active = false;
    bool _inventory_sealed = false;
    bool _shutdown_claimed = false;
    std::optional<termination_intent_t> _effective_intent;
    std::uint64_t _active_attempt = 0;
    std::uint64_t _next_attempt = 1;
    std::map<std::uint64_t, termination_result_t> _attempt_results;
    std::shared_ptr<detail::task_completion_source_t<termination_result_t>>
      _active_completion;
    std::optional<termination_result_t> _terminal;
};

} // namespace zlink::framework::runtime::stateful
