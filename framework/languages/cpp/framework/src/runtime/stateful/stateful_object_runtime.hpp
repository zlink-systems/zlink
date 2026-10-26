/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/execution/state_lane.hpp"
#include "runtime/stateful/object_ref.hpp"
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace zlink::framework::runtime
{
struct serial_application_record_t;
class serial_execution_queue_t;
}

namespace zlink::framework::runtime::stateful
{

enum class object_state_t
{
    creating,
    ready,
    moving,
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
    instance_manager_create_forbidden,
    unavailable
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

    // Source-retained rollback carries the same in-memory admission owner.
    // Codecs and target restore exclude this process-local ownership.
    std::shared_ptr<runtime::serial_application_record_t> accepted_owner;

    friend bool operator== (const turn_record_t &left, const turn_record_t &right)
    {
        return std::tie (left.sequence, left.payload, left.application_payload_bytes,
                         left.application_record, left.frozen_record)
               == std::tie (right.sequence, right.payload, right.application_payload_bytes,
                            right.application_record, right.frozen_record);
    }
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

struct retained_relocation_source_t
{
    object_ref_t owner;
    protocol::request_source_fence_t fence;
};

// Relocation flow §4.2: the existing seal attempt as the actual execution owner binds it.
// Preparing the attempt does not activate it; the owner sets bound in the producer cut that
// takes the participants' accepted, unstarted originals. Abort clears it on the state lane.
struct relocation_seal_binding_t
{
    std::atomic_bool bound{false};
    // The Kernel invokes this outside StateLane after restoring or committing the source.
    std::function<task_t<void> (bool)> finish;
};

struct stateful_object_runtime_test_access_t;

class stateful_object_runtime_t
{
    friend struct stateful_object_runtime_test_access_t;

  public:
    task_t<void> configure_population_metrics (std::string mesh_name)
    {
        return _lane.run_task ([this, mesh_name = std::move (mesh_name)] () mutable {
            if (_actor_metric)
                return;
            _metric_mesh = std::move (mesh_name);
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
            if (!dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ())) {
                auto meter = provider->GetMeter ("zlink.framework");
                if (!meter)
                    return;
                _actor_metric =
                  meter->CreateDoubleUpDownCounter ("zlink.actor.count", "", "{actor}");
                if (_actor_metric) {
                    _actor_metric->Add (0, {{"mesh_name", _metric_mesh}});
                    for (const auto &[key, object] : _objects)
                        record_actor_population (object, 1);
                }
            }
        });
    }

    task_t<void> retire_population_metrics_async ();

    using relocation_state_capture_t = std::function<task_t<frozen_object_state_t> (
      const object_ref_t &, const std::string &, std::stop_token)>;
    using relocation_state_restore_t =
      std::function<bool (const frozen_object_state_t &, const object_ref_t &, std::stop_token)>;
    // Application materialization is a second, internal boundary from the
    // persisted state machine.  Aggregate restore supplies the target Spot to
    // member Actors and invokes this callback in Spot-before-Actor order. The
    // restore continues when the returned task completes, so an application
    // lifecycle step it waits for does not hold the caller.
    using relocation_state_materialize_t =
      std::function<task_t<bool> (const frozen_object_state_t &,
                                  const object_ref_t &,
                                  const std::optional<object_ref_t> &,
                                  std::stop_token)>;
    using relocation_state_commit_t = std::function<bool (const std::vector<object_ref_t> &)>;
    using relocation_state_abort_t = std::function<void (const std::vector<object_ref_t> &)>;
    // Relocation flow §4.2 cut S of the participants' actual execution owner. In one producer
    // step it binds the attempt and takes the accepted, unstarted originals; it returns after
    // those originals entered their source FIFOs ahead of later ingress. The result holds the
    // owner's exported native suffix per participant, merged like the Actor suffix, or nullopt
    // when the attempt did not bind. Without a configured owner the attempt binds on the lane.
    using relocation_source_cut_t =
      std::function<task_t<std::optional<std::vector<std::vector<turn_record_t>>>> (
        std::vector<object_ref_t>, std::shared_ptr<relocation_seal_binding_t>, std::stop_token)>;

    stateful_object_runtime_t ();
    const std::shared_ptr<detail::actor_transfer_coordinator_t> &actor_transfers () const noexcept
    {
        return _actor_transfers;
    }
    ~stateful_object_runtime_t ();

    void configure_relocation_state (relocation_state_capture_t capture,
                                     relocation_state_restore_t restore);
    void configure_relocation_materialization (relocation_state_materialize_t materialize,
                                               relocation_state_commit_t commit,
                                               relocation_state_abort_t abort);
    void configure_relocation_cut (relocation_source_cut_t cut);

    void replace_placement_candidates (std::vector<placement_candidate_t> candidates);
    create_result_t begin_create (const create_request_t &request);
    task_t<create_result_t> begin_create_async (create_request_t request);
    task_t<create_result_t> begin_reserved_object_async (
      object_ref_t reserved, std::string stable_type, std::vector<std::uint8_t> creation_request);
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
    task_t<stateful_error_t> commit_create_async (std::uint64_t attempt);
    stateful_error_t abort_create (std::uint64_t attempt);
    create_result_t activate_instance (create_request_t request,
                                       const std::function<bool (const object_ref_t &)> &factory);
    // An identity lookup omits the state filter; execution eligibility remains
    // with the object's claim gate.
    std::optional<object_ref_t>
    find (object_kind_t kind,
          std::string key,
          std::optional<object_state_t> required_state = object_state_t::ready,
          std::optional<retained_relocation_source_t> *retained = nullptr) const;
    task_t<std::optional<object_ref_t>>
    find_async (object_kind_t kind,
                std::string key,
                std::optional<object_state_t> required_state = std::nullopt,
                std::optional<retained_relocation_source_t> *retained = nullptr) const;
    task_t<std::optional<object_ref_t>>
    resolve_spot_async (std::string key,
                        std::optional<retained_relocation_source_t> *retained = nullptr) const;
    stateful_error_t set_subscription (const object_ref_t &spot,
                                       const std::string &channel_name,
                                       const std::string &topic);
    void unset_subscription (const object_ref_t &spot,
                             const std::string &channel_name,
                             const std::string &topic);
    task_t<std::vector<object_ref_t>> multicast_local_targets (std::string channel_name,
                                                               std::string topic) const;
    task_t<bool> contains_async (object_ref_t reference) const;
    // Observes the existing source seal; does not resolve Store authority.
    bool finalized_relocation_source (object_kind_t kind,
                                      const std::string &key,
                                      std::uint64_t object_generation) const;
    task_t<std::optional<retained_relocation_source_t>>
    retained_relocation_source (object_ref_t owner) const;

    task_t<std::pair<stateful_error_t, membership_token_t>>
    begin_membership_move_async (const object_ref_t &actor, const object_ref_t &target_spot);
    task_t<std::pair<stateful_error_t, membership_token_t>>
    begin_remote_membership_move_async (const object_ref_t &actor, object_ref_t target_spot);
    task_t<std::pair<stateful_error_t, object_ref_t>>
    commit_membership_move_async (membership_token_t token);
    task_t<stateful_error_t> abort_membership_move_async (membership_token_t token);
    std::optional<std::string> actor_membership (const object_ref_t &actor) const;
    stateful_error_t destroy_actor (const object_ref_t &actor);
    task_t<stateful_error_t> destroy_actor_async (object_ref_t actor);
    std::pair<stateful_error_t, bool> close_spot (const object_ref_t &spot);
    task_t<std::pair<stateful_error_t, bool>> close_spot_async (const object_ref_t &spot);
    task_t<std::pair<stateful_error_t, bool>> can_close_spot_async (const object_ref_t &spot) const;

    stateful_error_t
    enqueue (const object_ref_t &owner, turn_domain_t domain, turn_record_t record);
    struct enqueue_outcome_t
    {
        stateful_error_t error = stateful_error_t::none;
        std::function<void ()> delivery;
        bool relocation_handoff = false;
    };
    using posted_enqueue_t =
      runtime::state_lane_internal::result_bridge_t<enqueue_outcome_t>::future_t;
    // Posts the turn to the state lane in caller order without waiting for it. The
    // caller keeps owner alive until it reads the outcome.
    posted_enqueue_t post_enqueue (const object_ref_t &owner,
                                   turn_domain_t domain,
                                   turn_record_t record,
                                   bool restored);
    struct admitted_turn_t
    {
        stateful_error_t error = stateful_error_t::none;
        turn_record_t turn;
        // Relocation flow §4.2: admitted after the cut S of its owner's bound seal.
        bool after_cut = false;
    };
    // Places one application turn in the lane turn that admits it. admit receives the binding
    // of the owner's seal attempt, or null, and may take the canonical mailbox original there,
    // so no accepted original is between its take and its FIFO when a later turn reads it.
    // The caller keeps owner alive until it reads the outcome.
    template <class Admit>
    task_t<enqueue_outcome_t>
    enqueue_admitted (const object_ref_t &owner, Admit admit, bool restored)
    {
        return _lane.run_task ([this, &owner, admit = std::move (admit), restored] () mutable {
            return admit_on_lane (owner, admit, restored);
        });
    }
    template <class Admit>
    posted_enqueue_t post_admitted (const object_ref_t &owner, Admit admit, bool restored)
    {
        return _lane.run ([this, &owner, admit = std::move (admit), restored] () mutable {
            return admit_on_lane (owner, admit, restored);
        });
    }
    std::pair<stateful_error_t, std::optional<turn_record_t>>
    try_claim (const object_ref_t &owner, turn_domain_t domain, std::string *stable_type = nullptr);
    // The claimed turn and the owner's stable type, both read in the claiming lane turn.
    struct owner_claim_t
    {
        stateful_error_t error = stateful_error_t::none;
        std::optional<turn_record_t> turn;
        std::string stable_type;
    };
    task_t<owner_claim_t> try_claim_async (object_ref_t owner, turn_domain_t domain);
    task_t<stateful_error_t> complete_claim_async (object_ref_t owner, turn_domain_t domain);
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
    // Projects queued work in the state lane; the existing claim gate still
    // decides execution eligibility when the caller visits each owner.
    task_t<std::vector<object_ref_t>>
    application_owner_refs_async (std::string local_node_id) const;
    task_t<std::vector<object_inventory_t>> inventory_async () const;
    task_t<std::vector<object_inventory_t>> actor_inventory () const;
    task_t<std::optional<std::vector<object_inventory_t>>>
    try_begin_maintenance_inventory (bool actors_only = false);
    task_t<void> end_maintenance_inventory ();
    task_t<aggregate_relocation_seal_attempt_t>
    try_seal_relocation_aggregate (const std::vector<object_ref_t> &participants,
                                   std::stop_token cancellation = {},
                                   const std::function<task_t<bool> ()> &before_capture = {});
    // Atomically separates the ingress accepted since capture from ingress
    // that arrives afterwards.  The caller must perform transport work after
    // this method returns; no callback runs while the aggregate mutex is held.
    std::pair<stateful_error_t, relocation_ingress_batch_t> begin_relocation_boundary (
      std::uint64_t token,
      std::function<void (const object_ref_t &, const turn_record_t &)> deliver = {},
      protocol::request_source_fence_t source_fence = {});
    // Only an exact target abort before Cutover may reopen source dispatch.
    task_t<stateful_error_t> abort_relocation_before_cutover (std::uint64_t token);
    // Makes the source permanently follow-only once Cutover has been queued
    // (or its enqueue outcome is uncertain).
    task_t<stateful_error_t> finalize_relocation_cutover (
      std::uint64_t token,
      std::optional<std::function<void (const object_ref_t &, const turn_record_t &)>> deliver =
        std::nullopt,
      relocation_ingress_batch_t *committed = nullptr);
    task_t<stateful_error_t>
    restore_relocation (frozen_object_state_t frozen,
                        object_ref_t target,
                        relocation_restore_identity_t identity,
                        std::stop_token cancellation = {},
                        std::optional<object_ref_t> target_spot = std::nullopt);
    task_t<stateful_error_t>
    restore_relocation_aggregate (std::vector<frozen_object_state_t> frozen,
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
    std::pair<stateful_error_t, object_ref_t>
    commit_membership_move_on_lane (const membership_token_t &token);
    stateful_error_t abort_membership_move_on_lane (const membership_token_t &token);
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
        std::set<std::pair<std::string, std::string>> subscriptions;
        // Handle of the source seal attempt in _relocation_seals; zero without one.
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
        // The only authority on whether the actual execution owner has cut S.
        std::shared_ptr<relocation_seal_binding_t> binding;
        relocation_ingress_phase_t ingress_phase = relocation_ingress_phase_t::holding;
        std::vector<std::vector<turn_record_t>> boundary_application;
        std::function<void (const object_ref_t &, const turn_record_t &)> deliver;
        protocol::request_source_fence_t source_fence;
    };

    std::optional<retained_relocation_source_t>
    retained_relocation_source_on_lane (const object_record_t &object) const;
    bool finalized_relocation_source_on_lane (const object_record_t &object) const;
    std::optional<std::uint64_t>
    relocation_source_seal_on_lane (const object_record_t &object) const;
    // The object's state as its owners observe it (relocation flow §4.2): an existing
    // source seal attempt reads as moving; the record keeps creation, membership move and
    // restore. Ingress follows the attempt's binding instead.
    std::pair<stateful_error_t, std::optional<turn_record_t>>
    claim_on_lane (const object_ref_t &owner, turn_domain_t domain, std::string *stable_type);
    stateful_error_t complete_claim_on_lane (const object_ref_t &owner, turn_domain_t domain);
    object_state_t state_on_lane (const object_record_t &object) const;
    bool sealed_by_on_lane (const object_record_t &object, std::uint64_t token) const;
    const relocation_seal_binding_t *seal_binding_on_lane (const object_record_t &object) const;
    template <class Admit>
    enqueue_outcome_t admit_on_lane (const object_ref_t &owner, Admit &admit, bool restored)
    {
        enqueue_outcome_t outcome;
        auto *object = find_record_locked (owner, outcome.error);
        if (object == nullptr)
            return outcome;
        auto admitted = admit (seal_binding_on_lane (*object));
        if (admitted.error != stateful_error_t::none) {
            outcome.error = admitted.error;
            return outcome;
        }
        return place_application_on_lane (*object, owner, std::move (admitted.turn),
                                          admitted.after_cut, restored);
    }
    enqueue_outcome_t place_application_on_lane (object_record_t &object,
                                                 const object_ref_t &owner,
                                                 turn_record_t record,
                                                 bool after_cut,
                                                 bool restored);
    void activate_object_on_lane (object_record_t &object);
    void finalize_relocation_source_on_lane (relocation_seal_state_t &seal);
    static bool valid_text (const std::string &value);
    static bool same_exact_ref (const object_ref_t &left, const object_ref_t &right);
    /* Return-relocation remnant (spec 28 §2/§8): after this node handed an
     * object's authority to another node the local record stays behind in
     * `moving` — nothing on the source side removes it. When the object later
     * relocates back here the incoming restore carries a strictly newer,
     * Store-validated authority generation, so that leftover is a replaceable
     * local remnant and must never veto the restore. */
    bool replaceable_relocation_remnant (const object_record_t &record,
                                         const object_ref_t &target) const;
    static object_key_t key_for (const object_ref_t &reference);
    const object_record_t *find_closeable_spot_locked (const object_ref_t &spot,
                                                       stateful_error_t &error) const;
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
    void notify_quiescence ();

    std::shared_ptr<detail::actor_transfer_coordinator_t> _actor_transfers =
      std::make_shared<detail::actor_transfer_coordinator_t> ();
    std::vector<object_inventory_t> inventory_on_lane (bool actors_only) const;

    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    // Lane-owned waiters of a seal; empty unless a seal waits for active turns.
    std::vector<std::shared_ptr<task_completion_source_t<void>>> _quiescence_waiters;
    std::vector<placement_candidate_t> _candidates;
    using object_iterator_t = std::map<object_key_t, object_record_t>::iterator;
    void record_actor_population (const object_record_t &object, double delta)
    {
        if (_actor_metric && object.reference.kind == object_kind_t::actor
            && object.state != object_state_t::creating
            && object.state != object_state_t::recovering
            && !finalized_relocation_source_on_lane (object))
            _actor_metric->Add (delta, {{"mesh_name", _metric_mesh}});
    }
    void erase_object_on_lane (object_iterator_t found)
    {
        if (found != _objects.end ()) {
            record_actor_population (found->second, -1);
            _objects.erase (found);
        }
    }
    void erase_object_on_lane (const object_key_t &key)
    {
        erase_object_on_lane (_objects.find (key));
    }
    std::string _metric_mesh;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>> _actor_metric;
    std::map<object_key_t, object_record_t> _objects;
    std::map<object_key_t, std::uint64_t> _last_generation;
    std::map<std::uint64_t, object_key_t> _attempts;
    std::map<std::uint64_t, membership_move_t> _membership_moves;
    std::map<std::uint64_t, relocation_seal_state_t> _relocation_seals;
    std::map<object_key_t, std::uint64_t> _relocation_restore_reservations;
    relocation_state_capture_t _relocation_state_capture;
    relocation_state_restore_t _relocation_state_restore;
    relocation_state_materialize_t _relocation_state_materialize;
    relocation_state_commit_t _relocation_state_commit;
    relocation_state_abort_t _relocation_state_abort;
    relocation_source_cut_t _relocation_cut;
    // Protects the object set after preceding Join claims have terminalized.
    bool _maintenance_inventory_active = false;
    std::uint64_t _next_attempt = 1;
    std::uint64_t _next_membership_token = 1;
    std::uint64_t _next_relocation_token = 1;
    std::uint64_t _next_relocation_restore_reservation = 1;
};

} // namespace zlink::framework::runtime::stateful
