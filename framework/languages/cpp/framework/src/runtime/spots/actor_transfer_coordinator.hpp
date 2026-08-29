/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/execution/state_lane.hpp"

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/message_follow_suppression_registry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::detail
{

enum class actor_move_phase_t
{
    source_reserved,
    local,
    source_remote,
    target_pending,
    target_committing,
    reconcile
};

struct pending_actor_admission_t
{
    std::string actor_key;
    actor_ref_t source_actor;
    spot_id_t source_spot_id;
    spot_id_t target_spot_id;
    // Only command-28's canonical non-entry branch creates this admission.
    // Legacy/maintenance transfers may use the same coordinator, but do not
    // carry the durable routed-Join recovery contract.
    bool canonical_user_spot_join = false;
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t completion_operation_id_high = 0;
    std::uint64_t completion_operation_id_low = 0;
    std::string reservation_token;
    std::uint64_t reserved_payload_bytes = 0;
    std::uint64_t target_spot_generation = 0;
    std::uint64_t target_spot_authority_owner_generation = 0;
    std::uint64_t source_actor_authority_owner_generation = 0;
    std::uint64_t source_node_generation = 0;
    std::uint64_t source_owner_lease_generation = 0;
    std::optional<message_t> admission_reply;
    std::vector<std::uint8_t> session_relocation_route;
    std::string session_relocation_actor_type;
    std::uint64_t session_relocation_target_owner_lease_generation = 0;
    std::uint64_t session_relocation_committed_previous_authority_owner_generation = 0;
    std::uint64_t session_relocation_committed_target_authority_owner_generation = 0;

    bool matches_prepare (const actor_ref_t &actor,
                          const spot_id_t &source_spot,
                          const spot_id_t &target_spot,
                          std::uint64_t operation_high,
                          std::uint64_t operation_low) const;
};

struct expired_actor_admission_t
{
    std::string transfer_id;
    pending_actor_admission_t admission;
};

struct removed_actor_message_follow_t
{
    std::string actor_key;
    runtime::protocol::actor_route_fence_t source_fence;
    std::string transfer_id;
};

struct actor_message_follow_target_t
{
    actor_ref_t actor;
    spot_route_t route;
    runtime::protocol::actor_route_fence_t source_fence;
    runtime::protocol::actor_route_fence_t target_fence;
};

// Captured at mark_reconcile time (the FINALIZE call site already holds all
// of this -- no Location Store re-derivation needed later) so a reconcile
// deadline can reconcile against the Location Store's authority instead of
// blindly replaying locally. See reconcile_keys_expired.
struct reconcile_target_context_t
{
    spot_route_t target_route;
    actor_ref_t target_actor;
    runtime::protocol::actor_route_fence_t source_fence;
    runtime::protocol::actor_route_fence_t target_fence;
    std::string transfer_id;
};

struct expired_reconcile_t
{
    std::string actor_key;
    std::optional<reconcile_target_context_t> context;
};

// One in-flight actor packet preserved while its actor is moving (spot-actor
// spec §10). Sends are preserved and replayed transparently; a request is also
// preserved (§10.2-1) so it still reaches the committed target's handler even
// after the caller's reply channel has re-resolved or timed out (§10.5 late
// reply) — the replayed request's reply is best-effort.
struct handoff_packet_t
{
    std::string packet_name;
    std::vector<std::uint8_t> payload;
    std::string content_type;
    std::map<std::string, std::string> metadata;
    bool is_request = false;
};

/* These framework-owned metadata keys preserve the routing context needed by
 * a late handoff relay. They never enter application metadata because the
 * public context projection filters the __zlink namespace. */
inline constexpr std::string_view actor_handoff_source_node_key =
  "__zlink.actorHandoffSourceNode";
/* The node that parked the request and holds its pending handoff entry (the
 * original reply token). The handoff terminal must return HERE — the source
 * node key above names the original requester, which only coincides with the
 * parking node when the requester was local to the current owner. A terminal
 * sent to a remote requester finds no pending entry and the reply is lost. */
inline constexpr std::string_view actor_handoff_parking_node_key =
  "__zlink.actorHandoffParkingNode";
inline constexpr std::string_view actor_handoff_route_actor_id_key =
  "__zlink.actorHandoffRouteActorId";
inline constexpr std::string_view actor_handoff_route_object_generation_key =
  "__zlink.actorHandoffRouteObjectGeneration";
inline constexpr std::string_view actor_handoff_route_target_node_key =
  "__zlink.actorHandoffRouteTargetNode";
inline constexpr std::string_view actor_handoff_route_target_node_generation_key =
  "__zlink.actorHandoffRouteTargetNodeGeneration";
inline constexpr std::string_view actor_handoff_route_authority_generation_key =
  "__zlink.actorHandoffRouteAuthorityGeneration";
inline constexpr std::string_view actor_handoff_route_lease_generation_key =
  "__zlink.actorHandoffRouteLeaseGeneration";
inline constexpr std::string_view actor_handoff_hop_count_key =
  "__zlink.actorHandoffHopCount";
inline constexpr std::string_view actor_handoff_operation_high_key =
  "__zlink.actorHandoffOperationHigh";
inline constexpr std::string_view actor_handoff_operation_low_key =
  "__zlink.actorHandoffOperationLow";
inline constexpr std::string_view actor_handoff_reply_route_key =
  "__zlink.actorHandoffReplyRoute";

struct actor_move_completion_t
{
    std::optional<std::chrono::steady_clock::duration> elapsed;
    std::vector<handoff_packet_t> backlog;
    bool completed = false;
};


enum class handoff_append_result_t
{
    appended,
    not_moving,
    duplicate_request
};

struct actor_transfer_dispatch_state_snapshot_t
{
    bool matches_message_follow_source = false;
    bool transfer_in_progress = false;
};

struct actor_transfer_packet_admission_t
{
    bool matches_message_follow_source = false;
    bool transfer_in_progress = false;
    handoff_append_result_t append_result = handoff_append_result_t::not_moving;
};

class actor_transfer_coordinator_t
{
  public:
    // The reservation may carry the transfer ID that the deferred join will
    // use, so packets preserved before transfer-out already emit their
    // handoff markers under the final transfer correlation.
    bool try_reserve_source (
      const std::string &actor_key,
      std::string transfer_id = {});
    bool try_begin_local (const std::string &actor_key);
    bool try_begin_source_remote (const std::string &actor_key, std::string transfer_id = {});
    void cancel_move (const std::string &actor_key);
    void mark_reconcile (const std::string &actor_key,
                        std::chrono::steady_clock::duration bound,
                        std::optional<reconcile_target_context_t> context = std::nullopt);
    // Returns every reconcile-phase move whose bound has passed, together
    // with the target identity captured when it entered reconcile (if any).
    // The caller reconciles against the Location Store's authority (spec 28
    // relay-ready irreversibility: never blind-replay locally once FINALIZE
    // may have reached the target) -- see move_state_t's reconcile_deadline
    // comment.
    std::vector<expired_reconcile_t>
    reconcile_keys_expired (std::chrono::steady_clock::time_point now) const;
    // Returns the out→commit-ack elapsed time when the completed move was a
    // source-remote transfer (runtime-metrics §4.3 duration window); local
    // moves complete with nullopt.
    std::optional<std::chrono::steady_clock::duration>
    complete_move (const std::string &actor_key);
    // Completes a source move while atomically taking packets that arrived
    // after the first handoff snapshot. The caller relays the retained batch
    // after the owner transition without leaving a race between queue drain
    // and move completion.
    actor_move_completion_t complete_move_and_take_backlog (const std::string &actor_key);
    // Used by a local commit: take one replay batch while the move still
    // blocks direct dispatch, then close the move only when no later batch is
    // present. Messages submitted after closure enter the actor queue after
    // the already-posted replay work.
    actor_move_completion_t finish_move_replay (const std::string &actor_key);
    bool blocks_dispatch (const std::string &actor_key) const;
    std::optional<actor_move_phase_t> phase (const std::string &actor_key) const;
    std::optional<std::string> transfer_id (const std::string &actor_key) const;
    bool matches_source_remote_transfer (const std::string &actor_key,
                                         const std::string &transfer_id) const;
    bool try_submit_source_leave (const std::string &actor_key,
                                  const std::string &transfer_id);
    bool source_leave_submitted (const std::string &actor_key,
                                 const std::string &transfer_id) const;

    // In-flight handoff (spot-actor spec §10). Packets are preserved in arrival
    // order until the commit path drains them into the commit request.
    handoff_append_result_t try_append_backlog (const std::string &actor_key,
                                                handoff_packet_t packet);
    // Projects the fresh Message Follow/transfer state and makes the backlog
    // admission decision in the same coordinator turn. A packet already on a
    // committed source edge bypasses the local moving backlog.
    actor_transfer_packet_admission_t admit_dispatch_packet (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t *source_fence,
      bool targets_current_authority,
      handoff_packet_t packet);
    // Stages one source-retained batch without allowing live target traffic to
    // interleave between its packets. The exact committing transfer owns the
    // append or the whole batch is rejected.
    bool stage_commit_backlog (const std::string &transfer_id,
                               std::vector<handoff_packet_t> backlog);
    std::vector<handoff_packet_t> take_backlog (const std::string &actor_key);

    // Message Follow route lifetime (§10.4): each committed source fence keeps
    // its own bounded route until the configured duration expires. Rapid
    // repeated relocations can retain more than one source fence for one Actor.
    void activate_message_follow (const std::string &actor_key,
                                  runtime::protocol::actor_route_fence_t source_fence,
                                  actor_ref_t target_actor,
                                  spot_route_t target_route,
                                  runtime::protocol::actor_route_fence_t target_fence,
                                  std::chrono::steady_clock::time_point remove_at,
                                  std::string transfer_id = {});
    bool matches_message_follow_source (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence) const;
    // Both values decide one dispatch admission phase. Project them in one
    // coordinator turn; callers take a fresh snapshot for a later phase.
    actor_transfer_dispatch_state_snapshot_t project_dispatch_state (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t *source_fence) const;
    std::optional<actor_message_follow_target_t> message_follow_target (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence) const;
    bool has_message_follow_route (const std::string &actor_key) const;
    // True when any active Message Follow route for actor_key points at the
    // given node. A pending handoff request recorded without a route fence
    // (the requester attached none) cannot name the exact followed edge, so
    // its terminal is admitted against the node identity of the retained
    // follow targets instead.
    bool message_follow_targets_node (const std::string &actor_key,
                                      const zlink::routing_id_t &node) const;
    // A cold hit (no incoming follow fence, e.g. a client probing the old
    // owner directly) has no fence to match against. It does carry the
    // ObjectGeneration it believes the Actor is still at, which relocation
    // preserves; find the retained route's own source fence for that
    // generation so the caller can drive the normal fence-validated relay
    // path with it.
    std::optional<runtime::protocol::actor_route_fence_t>
    message_follow_source_for_generation (const std::string &actor_key,
                                          std::uint64_t generation) const;
    result_t<std::optional<actor_message_follow_target_t>>
    try_acquire_message_follow (const std::string &actor_key,
                                std::uint64_t generation,
                                std::size_t payload_bytes,
                                std::size_t hop_count,
                                const runtime::protocol::actor_route_fence_t &source_fence);
    void release_message_follow (const std::string &actor_key,
                                 const runtime::protocol::actor_route_fence_t &source_fence,
                                 std::size_t payload_bytes);
    bool try_begin_message_follow_notification (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence,
      const runtime::protocol::actor_route_fence_t &target_fence);
    bool complete_message_follow_notification (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence,
      const runtime::protocol::actor_route_fence_t &target_fence,
      bool transport_accepted);
    std::vector<removed_actor_message_follow_t>
    remove_expired_message_follow (std::chrono::steady_clock::time_point now);
    std::optional<removed_actor_message_follow_t>
    remove_message_follow (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence,
      const runtime::protocol::actor_route_fence_t &target_fence);

    bool try_add_admission (std::string transfer_id, pending_actor_admission_t admission);
    std::optional<pending_actor_admission_t> admission (
      const std::string &transfer_id) const;
    // True iff transfer_id is still the move actor_key is tracking. A
    // caller that unlocks around external side effects (e.g. the joined
    // callback in prepare_remote_actor_to_spot) re-checks this immediately
    // before publishing an effect a newer, evicting attempt must not race
    // (spec 15 §4.2 newest-attempt-wins).
    bool is_current (const std::string &actor_key, const std::string &transfer_id) const;
    std::optional<pending_actor_admission_t> begin_commit (const std::string &transfer_id,
                                                           const actor_ref_t &source_actor,
                                                           const spot_id_t &target_spot_id);
    std::optional<pending_actor_admission_t> pending_commit (
      const std::string &transfer_id,
      const actor_ref_t &source_actor,
      const spot_id_t &target_spot_id) const;
    std::optional<pending_actor_admission_t> completed_commit (
      const std::string &transfer_id,
      const actor_ref_t &source_actor,
      const spot_id_t &target_spot_id) const;
    // Atomically closes the exact target move, publishes its completed
    // admission, and transfers every retained packet to the active Actor turn.
    // A missing value means the transfer or generation fence no longer matches.
    std::optional<std::vector<handoff_packet_t>>
    complete_commit_and_take_backlog (const std::string &transfer_id,
                                      const actor_ref_t &source_actor,
                                      const spot_id_t &target_spot_id);
    bool stage_session_relocation_route (
      const std::string &transfer_id,
      std::vector<std::uint8_t> route,
      std::string actor_type,
      std::uint64_t target_owner_lease_generation);
    bool commit_session_relocation_route_authority (
      const std::string &transfer_id,
      std::uint64_t previous_authority_owner_generation,
      std::uint64_t target_authority_owner_generation);
    std::optional<pending_actor_admission_t>
    session_relocation_admission (const std::string &transfer_id) const;
    void fail_commit (const std::string &transfer_id, bool reconcile);
    void complete_commit (const std::string &transfer_id);
    std::vector<expired_actor_admission_t>
    cleanup_expired (std::chrono::steady_clock::time_point now);
    std::size_t pending_count () const;

    std::string next_transfer_id (const std::string &node_rid);

  private:
    struct move_state_t
    {
        actor_move_phase_t phase;
        std::string transfer_id;
        std::optional<std::chrono::steady_clock::time_point> transfer_started_at;
        bool source_leave_submitted = false;
        // reconcile is entered for a genuinely ambiguous outcome (e.g. the
        // FINALIZE/cutover submission itself failed, so the source cannot
        // tell whether the target ever received it) where nothing today
        // reconciles against authority truth. Bound it so a stuck reconcile
        // cannot park requests in the backlog forever (spec 28: never
        // unbounded queueing) -- reconcile_keys_expired reports it once this
        // passes so the caller can reconcile against the Location Store.
        std::optional<std::chrono::steady_clock::time_point> reconcile_deadline;
        // Target identity captured at mark_reconcile time so the deadline
        // handler can adopt the target route (spec 28) without a second
        // Location Store resolver lookup for the target's spot address.
        std::optional<reconcile_target_context_t> reconcile_context;
    };

    struct message_follow_route_t
    {
        runtime::protocol::actor_route_fence_t source_fence;
        actor_ref_t target_actor;
        spot_route_t target_route;
        runtime::protocol::actor_route_fence_t target_fence;
        std::chrono::steady_clock::time_point remove_at;
        std::string transfer_id;
        std::size_t in_flight_messages = 0;
        std::size_t in_flight_bytes = 0;
        message_follow_suppression_key_t suppression_key;
    };

    bool matches_message_follow_source_unlocked (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence,
      std::chrono::steady_clock::time_point now) const;
    actor_transfer_dispatch_state_snapshot_t project_dispatch_state_unlocked (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t *source_fence,
      std::chrono::steady_clock::time_point now) const;
    handoff_append_result_t try_append_backlog_unlocked (const std::string &actor_key,
                                                         handoff_packet_t packet);

    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    std::map<std::string, move_state_t> _moves;
    std::map<std::string, pending_actor_admission_t> _admissions;
    std::map<std::string, pending_actor_admission_t> _completed_admissions;
    std::map<std::string, std::vector<handoff_packet_t>> _backlogs;
    std::map<std::string, std::vector<message_follow_route_t>> _message_follow_routes;
    message_follow_suppression_registry_t _message_follow_suppression;
    std::uint64_t _next_transfer_id = 1;
};

} // namespace zlink::framework::detail
