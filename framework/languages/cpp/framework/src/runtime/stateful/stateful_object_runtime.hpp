/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace zlink::framework::runtime::stateful
{

enum class object_kind_t
{
    actor,
    user_spot,
    instance_spot
};

enum class object_state_t
{
    creating,
    ready,
    moving,
    closing,
    recovering
};

enum class stateful_error_t
{
    none,
    invalid,
    not_found,
    type_mismatch,
    already_exists,
    generation_stale,
    moving,
    conflict,
    backpressured,
    instance_manager_create_forbidden
};

struct object_ref_t
{
    object_kind_t kind = object_kind_t::actor;
    std::string key;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string mesh_name;
    std::string node_id;

    friend bool operator== (const object_ref_t &, const object_ref_t &) = default;
};

struct placement_candidate_t
{
    std::string mesh_name;
    std::string node_id;
    std::set<std::string> stable_types;
    int weight = 100;
    std::size_t active_capacity = 0;
    std::size_t active_count = 0;
    std::size_t pending_capacity = 0;
    std::size_t pending_count = 0;
};

struct create_request_t
{
    object_kind_t kind = object_kind_t::actor;
    std::string key;
    std::string stable_type;
    std::optional<std::string> mesh_name;
    std::vector<std::uint8_t> creation_request;
    bool exclusive = false;
    bool instance_intent = false;
};

enum class create_status_t
{
    reserved,
    joined,
    existing,
    failed
};

struct create_result_t
{
    create_status_t status = create_status_t::failed;
    stateful_error_t error = stateful_error_t::none;
    std::uint64_t attempt = 0;
    object_ref_t object;
    bool factory_owner = false;
};

struct membership_token_t
{
    std::uint64_t value = 0;
    object_ref_t actor;
    object_ref_t target_spot;

    friend bool operator== (const membership_token_t &, const membership_token_t &) = default;
};

struct spot_close_token_t
{
    std::uint64_t value = 0;
    object_ref_t spot;

    friend bool operator== (const spot_close_token_t &, const spot_close_token_t &) = default;
};

enum class turn_domain_t
{
    application,
    infrastructure
};

struct turn_record_t
{
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
    // Application HWM counts the application payload part, not the canonical
    // relocation envelope retained by this queue.
    std::optional<std::size_t> application_payload_bytes;
    // Accepted application records retain their already validated typed input.
    // The canonical relocation bytes are created only when a relocation
    // snapshot is actually serialized.
    std::optional<protocol::frozen_application_record_t> application_record;
    // A relocation snapshot retains the complete, canonical record as well
    // as the typed admission input.  The latter is sufficient while the
    // source queue is live; the former is the durable saved-work fact and
    // keeps source/correlation/reply-route/deadline fields intact on a
    // C++ -> C++ restore.
    std::optional<protocol::frozen_record_t> frozen_record;

    friend bool operator== (const turn_record_t &, const turn_record_t &) = default;
};

struct logical_timer_t
{
    std::uint64_t timer_id = 0;
    std::uint64_t due_after_milliseconds = 0;
    std::uint64_t period_milliseconds = 0;
    std::uint64_t next_tick_sequence = 1;
    // These fields are deliberately part of the frozen model, rather than a
    // timer implementation detail.  A target needs the handler identity and
    // a structured pending tick to resume a sealed timer deterministically.
    std::string name;
    std::string handler_type;
    std::uint8_t overrun_policy = 0;
    std::uint64_t max_catch_up_ticks = 0;
    bool stop_on_unhandled_exception = false;
    std::uint64_t last_completed_delivery_index = 0;
    std::uint64_t last_completed_scheduled_index = 0;
    std::uint64_t next_scheduled_at_unix_milliseconds = 0;
    struct pending_tick_t
    {
        std::uint64_t delivery_index = 0;
        std::uint64_t scheduled_index = 0;
        std::uint64_t scheduled_at_unix_milliseconds = 0;
        std::uint64_t skipped_ticks = 0;

        friend bool operator== (const pending_tick_t &, const pending_tick_t &) = default;
    };
    std::vector<pending_tick_t> pending_ticks;

    friend bool operator== (const logical_timer_t &, const logical_timer_t &) = default;
};

struct frozen_object_state_t
{
    object_ref_t owner;
    std::string stable_type;
    std::vector<std::uint8_t> application_state;
    std::vector<turn_record_t> pending_application;
    std::vector<logical_timer_t> timers;

    friend bool operator== (const frozen_object_state_t &, const frozen_object_state_t &) = default;
};

struct relocation_restore_identity_t
{
    std::string reference;
    std::uint32_t checksum_crc32c = 0;
    std::array<std::uint8_t, 32> inventory_digest{};

    friend bool operator== (const relocation_restore_identity_t &,
                            const relocation_restore_identity_t &) = default;
};

struct relocation_seal_t
{
    std::uint64_t token = 0;
    frozen_object_state_t frozen;
};

struct object_inventory_t
{
    object_ref_t owner;
    std::string stable_type;
    object_state_t state = object_state_t::creating;
    std::string membership;

    friend bool operator== (const object_inventory_t &, const object_inventory_t &) = default;
};

struct aggregate_relocation_seal_t
{
    std::uint64_t token = 0;
    std::vector<frozen_object_state_t> participants;
};

struct relocation_seal_attempt_t
{
    stateful_error_t error = stateful_error_t::invalid;
    relocation_seal_t seal;
};

struct aggregate_relocation_seal_attempt_t
{
    stateful_error_t error = stateful_error_t::invalid;
    aggregate_relocation_seal_t seal;
};

// The source owns this state under its aggregate mutex.  It deliberately
// describes only post-capture ingress; the saved prefix remains in the
// relocation payload and is never a relay batch.
enum class relocation_ingress_phase_t
{
    holding,
    post_boundary,
    follow_only
};

struct relocation_ingress_batch_t
{
    struct participant_t
    {
        object_ref_t owner;
        std::vector<turn_record_t> records;
    };

    std::uint64_t token = 0;
    std::vector<participant_t> participants;
};

class stateful_object_runtime_t
{
  public:
    using relocation_state_capture_t = std::function<std::vector<std::uint8_t> (
      const object_ref_t &, const std::string &, std::stop_token)>;
    using relocation_state_restore_t =
      std::function<bool (const frozen_object_state_t &, const object_ref_t &, std::stop_token)>;
    // Application materialization is a second, internal boundary from the
    // persisted state machine.  Aggregate restore supplies the target Spot to
    // member Actors and invokes this callback in Spot-before-Actor order.
    using relocation_state_materialize_t = std::function<bool (const frozen_object_state_t &,
                                                               const object_ref_t &,
                                                               const std::optional<object_ref_t> &,
                                                               std::stop_token)>;
    using relocation_state_commit_t = std::function<bool (const std::vector<object_ref_t> &)>;
    using relocation_state_abort_t = std::function<void (const std::vector<object_ref_t> &)>;

    explicit stateful_object_runtime_t (
      std::size_t application_capacity = dispatch_limits::application_mailbox_messages,
      std::size_t infrastructure_capacity = dispatch_limits::control_mailbox_messages,
      std::size_t application_byte_capacity = dispatch_limits::application_mailbox_bytes,
      std::size_t infrastructure_byte_capacity = dispatch_limits::control_mailbox_bytes);

    void configure_relocation_state (relocation_state_capture_t capture,
                                     relocation_state_restore_t restore);
    void configure_relocation_materialization (relocation_state_materialize_t materialize,
                                               relocation_state_commit_t commit,
                                               relocation_state_abort_t abort);

    void replace_placement_candidates (std::vector<placement_candidate_t> candidates);
    create_result_t begin_create (const create_request_t &request);
    create_result_t begin_reserved_object (const object_ref_t &reserved,
                                           const std::string &stable_type,
                                           std::vector<std::uint8_t> creation_request);
    // Accepts the next authority fence for an actor that is retained as a
    // remote copy while ownership returns to this node.
    stateful_error_t adopt_reserved_actor_owner (const object_ref_t &reserved,
                                                 const std::string &stable_type);
    // Applies the next Store-committed authority generation when a local
    // Actor keeps the same owner node while its membership authority advances.
    stateful_error_t advance_local_actor_authority (const object_ref_t &committed,
                                                    const std::string &stable_type);
    stateful_error_t
    reconcile_relocation_restore_authority (const object_ref_t &staged,
                                            const object_ref_t &committed,
                                            const relocation_restore_identity_t &identity);
    stateful_error_t commit_create (std::uint64_t attempt);
    stateful_error_t abort_create (std::uint64_t attempt);
    create_result_t activate_instance (create_request_t request,
                                       const std::function<bool (const object_ref_t &)> &factory);
    std::optional<object_ref_t> find (object_kind_t kind, const std::string &key) const;

    std::pair<stateful_error_t, membership_token_t>
    begin_membership_move (const object_ref_t &actor, const object_ref_t &target_spot);
    std::pair<stateful_error_t, membership_token_t>
    begin_remote_membership_move (const object_ref_t &actor, object_ref_t target_spot);
    std::pair<stateful_error_t, object_ref_t>
    commit_membership_move (const membership_token_t &token);
    stateful_error_t abort_membership_move (const membership_token_t &token);
    std::optional<std::string> actor_membership (const object_ref_t &actor) const;
    stateful_error_t destroy_actor (const object_ref_t &actor);
    std::pair<stateful_error_t, bool> close_spot (const object_ref_t &spot);
    std::pair<stateful_error_t, std::optional<spot_close_token_t>>
    begin_close_spot (const object_ref_t &spot);
    stateful_error_t commit_close_spot (const spot_close_token_t &token);
    stateful_error_t abort_close_spot (const spot_close_token_t &token);

    stateful_error_t
    enqueue (const object_ref_t &owner, turn_domain_t domain, turn_record_t record);
    std::pair<stateful_error_t, std::optional<turn_record_t>> try_claim (const object_ref_t &owner,
                                                                         turn_domain_t domain);
    stateful_error_t complete_claim (const object_ref_t &owner, turn_domain_t domain);
    stateful_error_t yield_claim (const object_ref_t &owner, turn_record_t continuation);
    std::size_t pending (const object_ref_t &owner, turn_domain_t domain) const;
    std::size_t pending_bytes (const object_ref_t &owner, turn_domain_t domain) const;
    stateful_error_t discard_application (const object_ref_t &owner, std::uint64_t sequence);
    stateful_error_t register_timer (const object_ref_t &owner, logical_timer_t timer);
    stateful_error_t cancel_timer (const object_ref_t &owner, std::uint64_t timer_id);
    stateful_error_t enqueue_timer_tick (const object_ref_t &owner,
                                         std::uint64_t timer_id,
                                         std::vector<std::uint8_t> payload);
    std::vector<logical_timer_t> timers (const object_ref_t &owner) const;
    std::vector<object_inventory_t> inventory () const;
    std::optional<std::vector<object_inventory_t>> try_begin_maintenance_inventory ();
    void end_maintenance_inventory () noexcept;
    task_t<aggregate_relocation_seal_attempt_t>
    try_seal_relocation_aggregate (const std::vector<object_ref_t> &participants,
                                   std::stop_token cancellation = {},
                                   const std::function<task_t<bool> ()> &before_capture = {});
    // Atomically separates the ingress accepted since capture from ingress
    // that arrives afterwards.  The caller must perform transport work after
    // this method returns; no callback runs while the aggregate mutex is held.
    std::pair<stateful_error_t, relocation_ingress_batch_t>
    begin_relocation_boundary (std::uint64_t token);
    // Only an exact target abort before Cutover may reopen source dispatch.
    stateful_error_t abort_relocation_before_cutover (std::uint64_t token);
    // Makes the source permanently follow-only once Cutover has been queued
    // (or its enqueue outcome is uncertain).
    stateful_error_t finalize_relocation_cutover (std::uint64_t token);
    stateful_error_t abort_relocation (std::uint64_t token);
    std::pair<stateful_error_t, std::vector<object_ref_t>>
    commit_relocation_aggregate (std::uint64_t token, std::string target_node_id);
    stateful_error_t restore_relocation (frozen_object_state_t frozen,
                                         object_ref_t target,
                                         relocation_restore_identity_t identity,
                                         std::stop_token cancellation = {},
                                         std::optional<object_ref_t> target_spot = std::nullopt);
    stateful_error_t restore_relocation_aggregate (std::vector<frozen_object_state_t> frozen,
                                                   std::vector<object_ref_t> targets,
                                                   relocation_restore_identity_t identity,
                                                   std::stop_token cancellation = {});
    stateful_error_t commit_relocation_restore (const object_ref_t &target,
                                                const relocation_restore_identity_t &identity);
    stateful_error_t
    commit_relocation_restore_aggregate (const std::vector<object_ref_t> &targets,
                                         const relocation_restore_identity_t &identity);
    stateful_error_t abort_relocation_restore (const object_ref_t &target,
                                               const relocation_restore_identity_t &identity);
    stateful_error_t
    abort_relocation_restore_aggregate (const std::vector<object_ref_t> &targets,
                                        const relocation_restore_identity_t &identity);

  private:
    struct object_key_t
    {
        object_kind_t kind;
        std::string key;

        bool operator< (const object_key_t &other) const noexcept;
        friend bool operator== (const object_key_t &, const object_key_t &) = default;
    };

    struct queue_t
    {
        std::deque<turn_record_t> application;
        std::deque<turn_record_t> held_application;
        std::deque<turn_record_t> infrastructure;
        std::optional<turn_record_t> yielded_continuation;
        std::size_t application_bytes = 0;
        std::size_t held_application_bytes = 0;
        std::size_t infrastructure_bytes = 0;
        std::size_t application_active_bytes = 0;
        std::size_t infrastructure_active_bytes = 0;
        bool application_active = false;
        bool infrastructure_active = false;
    };

    struct object_record_t
    {
        object_ref_t reference;
        std::string stable_type;
        object_state_t state = object_state_t::creating;
        std::uint64_t attempt = 0;
        std::string membership;
        std::optional<relocation_restore_identity_t> restore_identity;
        queue_t queue;
        std::map<std::uint64_t, logical_timer_t> timers;
        std::uint64_t barrier_generation = 0;
    };

    struct membership_move_t
    {
        membership_token_t token;
        std::string previous_membership;
    };

    struct relocation_seal_state_t
    {
        std::vector<object_key_t> keys;
        std::vector<object_ref_t> sources;
        std::vector<frozen_object_state_t> frozen;
        relocation_ingress_phase_t ingress_phase = relocation_ingress_phase_t::holding;
        std::vector<std::vector<turn_record_t>> boundary_application;
    };

    struct relocation_hold_state_t
    {
        std::size_t record_count = 0;
        std::size_t byte_count = 0;
    };

    static bool valid_text (const std::string &value);
    static bool same_exact_ref (const object_ref_t &left, const object_ref_t &right);
    /* Return-relocation remnant (spec 28 §2/§8): after this node handed an
     * object's authority to another node the local record stays behind in
     * `moving` — nothing on the source side removes it. When the object later
     * relocates back here the incoming restore carries a strictly newer,
     * Store-validated authority generation, so that leftover is a replaceable
     * local remnant and must never veto the restore. */
    static bool replaceable_relocation_remnant (const object_record_t &record,
                                                const object_ref_t &target);
    static object_key_t key_for (const object_ref_t &reference);
    object_record_t *find_record_locked (const object_ref_t &reference, stateful_error_t &error);
    const object_record_t *find_record_locked (const object_ref_t &reference,
                                               stateful_error_t &error) const;
    std::optional<placement_candidate_t>
    select_candidate_locked (const create_request_t &request) const;
    stateful_error_t
    enqueue_locked (object_record_t &object, turn_domain_t domain, turn_record_t record);
    static std::size_t retained_bytes (const turn_record_t &record) noexcept;
    void move_held_application_locked (object_record_t &object);
    void release_pending_capacity_locked (const object_record_t &record);

    const std::size_t _application_capacity;
    const std::size_t _infrastructure_capacity;
    const std::size_t _application_byte_capacity;
    const std::size_t _infrastructure_byte_capacity;
    mutable std::mutex _mutex;
    std::condition_variable _quiescence;
    std::vector<placement_candidate_t> _candidates;
    std::map<object_key_t, object_record_t> _objects;
    std::map<object_key_t, std::uint64_t> _last_generation;
    std::map<std::uint64_t, object_key_t> _attempts;
    std::map<std::uint64_t, membership_move_t> _membership_moves;
    std::map<std::uint64_t, spot_close_token_t> _spot_closes;
    std::map<std::uint64_t, relocation_seal_state_t> _relocation_seals;
    std::map<std::uint64_t, relocation_hold_state_t> _relocation_holds;
    std::map<object_key_t, std::uint64_t> _relocation_restore_reservations;
    relocation_state_capture_t _relocation_state_capture;
    relocation_state_restore_t _relocation_state_restore;
    relocation_state_materialize_t _relocation_state_materialize;
    relocation_state_commit_t _relocation_state_commit;
    relocation_state_abort_t _relocation_state_abort;
    bool _maintenance_inventory_active = false;
    std::uint64_t _next_attempt = 1;
    std::uint64_t _next_membership_token = 1;
    std::uint64_t _next_spot_close_token = 1;
    std::uint64_t _next_relocation_token = 1;
    std::uint64_t _next_relocation_restore_reservation = 1;
};

} // namespace zlink::framework::runtime::stateful
