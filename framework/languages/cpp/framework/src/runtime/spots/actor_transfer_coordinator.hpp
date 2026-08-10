/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t completion_operation_id_high = 0;
    std::uint64_t completion_operation_id_low = 0;
    std::optional<message_t> admission_reply;
    std::optional<message_t> completion_reply;
    std::string completion_root_reference;
    std::uint32_t completion_root_checksum = 0;
    // Deferred-commit completion-loss window: the deferred finalize committed
    // durably while the completion-only leg is still outstanding. Once the
    // join window elapses without that leg, the target converges from the
    // durable owner state; next_completion_poll_at paces the re-poll.
    bool commit_finalized = false;
    bool completion_poll_due = false;
    std::chrono::steady_clock::time_point next_completion_poll_at{};

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

struct deferred_actor_completion_t
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


// Deferred-commit completion-loss convergence: the pace of the durable-state
// re-poll, and how long a converged commit stays visible so a late
// completion-only leg still terminates idempotently.
inline constexpr std::chrono::milliseconds
  actor_deferred_completion_retry_interval{250};
inline constexpr std::chrono::seconds actor_deferred_completion_retention{30};

enum class handoff_append_result_t
{
    appended,
    not_moving,
    duplicate_request,
    capacity_exceeded
};

class actor_transfer_coordinator_t
{
  public:
    // The reservation may carry the transfer ID that the deferred join will
    // use, so packets preserved before transfer-out already emit their
    // handoff markers under the final transfer correlation.
    bool try_reserve_source (const std::string &actor_key, std::string transfer_id = {});
    bool try_begin_local (const std::string &actor_key);
    bool try_begin_source_remote (const std::string &actor_key, std::string transfer_id = {});
    void cancel_move (const std::string &actor_key);
    void mark_reconcile (const std::string &actor_key);
    // Returns the out→commit-ack elapsed time when the completed move was a
    // source-remote transfer (runtime-metrics §4.3 duration window); local
    // moves complete with nullopt.
    std::optional<std::chrono::steady_clock::duration>
    complete_move (const std::string &actor_key);
    // Completes a source move while atomically taking packets that arrived
    // after the first handoff snapshot. The caller relays that bounded batch
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

    // In-flight handoff (spot-actor spec §10). Packets are preserved in arrival
    // order until the commit path drains them into the commit request. The
    // result identifies a full temporary queue so the caller can return
    // Unavailable for requests and drop one-way operations.
    handoff_append_result_t try_append_backlog (const std::string &actor_key,
                                                handoff_packet_t packet);
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
    std::optional<actor_message_follow_target_t> message_follow_target (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence) const;
    bool has_message_follow_route (const std::string &actor_key) const;
    result_t<std::optional<actor_message_follow_target_t>>
    try_acquire_message_follow (const std::string &actor_key,
                                std::uint64_t generation,
                                std::size_t payload_bytes,
                                std::size_t hop_count,
                                const runtime::protocol::actor_route_fence_t &source_fence);
    void release_message_follow (const std::string &actor_key,
                                 const runtime::protocol::actor_route_fence_t &source_fence,
                                 std::size_t payload_bytes) noexcept;
    bool mark_message_follow_notified (
      const std::string &actor_key,
      const runtime::protocol::actor_route_fence_t &source_fence,
      std::vector<std::uint8_t> source_node_routing_id);
    std::vector<removed_actor_message_follow_t>
    remove_expired_message_follow (std::chrono::steady_clock::time_point now);

    bool try_add_admission (std::string transfer_id, pending_actor_admission_t admission);
    std::optional<pending_actor_admission_t> admission (
      const std::string &transfer_id) const;
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
    bool update_completion_root (
      const std::string &transfer_id,
      std::string reference,
      std::uint32_t checksum);
    void fail_commit (const std::string &transfer_id, bool reconcile);
    void complete_commit (const std::string &transfer_id);
    // Deferred-commit convergence (completion-loss window): marks a commit
    // whose completion-only leg is still outstanding, and returns the
    // admissions whose join window elapsed without that leg so the runtime
    // can re-poll the durable owner state. Each returned attempt re-arms
    // after the retry interval; the admission deadline is extended so a late
    // completion leg still finds the completed commit and stays idempotent.
    bool mark_commit_finalized (const std::string &transfer_id);
    std::vector<deferred_actor_completion_t>
    due_deferred_completions (std::chrono::steady_clock::time_point now);
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
        std::set<std::vector<std::uint8_t>> notified_sources;
    };

    mutable std::mutex _mutex;
    std::map<std::string, move_state_t> _moves;
    std::map<std::string, pending_actor_admission_t> _admissions;
    std::map<std::string, pending_actor_admission_t> _completed_admissions;
    std::map<std::string, std::vector<handoff_packet_t>> _backlogs;
    std::map<std::string, std::size_t> _backlog_bytes;
    std::map<std::string, std::vector<message_follow_route_t>> _message_follow_routes;
    std::uint64_t _next_transfer_id = 1;
};

} // namespace zlink::framework::detail
