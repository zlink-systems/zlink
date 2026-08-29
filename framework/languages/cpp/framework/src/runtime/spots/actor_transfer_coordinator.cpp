/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "actor_transfer_coordinator.hpp"

#include <service_wire_constants.hpp>

#include <algorithm>
#include <limits>

namespace zlink::framework::detail
{

bool pending_actor_admission_t::matches_prepare (
  const actor_ref_t &actor,
  const spot_id_t &source_spot,
  const spot_id_t &target_spot,
  std::uint64_t operation_high,
  std::uint64_t operation_low) const
{
    return source_actor.actor_id () == actor.actor_id ()
           && actor_ref_access_t::actor_type (source_actor)
                == actor_ref_access_t::actor_type (actor)
           && source_actor.object_generation () == actor.object_generation ()
           && source_actor.node_rid ().value () == actor.node_rid ().value ()
           && source_spot_id == source_spot
           && target_spot_id == target_spot
           && completion_operation_id_high == operation_high
           && completion_operation_id_low == operation_low;
}

bool actor_transfer_coordinator_t::try_reserve_source (
  const std::string &actor_key,
  std::string transfer_id)
{
    return _lane.run ([&, this] {
    return _moves.emplace (
                   actor_key,
                   move_state_t{actor_move_phase_t::source_reserved, std::move (transfer_id)})
      .second;
    }).get ();
}

bool actor_transfer_coordinator_t::try_begin_local (const std::string &actor_key)
{
    return _lane.run ([&, this] {
    if (auto found = _moves.find (actor_key); found != _moves.end ()) {
        if (found->second.phase != actor_move_phase_t::source_reserved)
            return false;
        found->second.phase = actor_move_phase_t::local;
        return true;
    }
    return _moves.emplace (actor_key, move_state_t{actor_move_phase_t::local, std::string{}})
      .second;
    }).get ();
}

bool actor_transfer_coordinator_t::try_begin_source_remote (const std::string &actor_key,
                                                            std::string transfer_id)
{
    return _lane.run ([&, this] {
    if (auto found = _moves.find (actor_key); found != _moves.end ()) {
        if (found->second.phase != actor_move_phase_t::source_reserved)
            return false;
        found->second.phase = actor_move_phase_t::source_remote;
        found->second.transfer_id = std::move (transfer_id);
        found->second.transfer_started_at = std::chrono::steady_clock::now ();
        return true;
    }
    return _moves
      .emplace (actor_key, move_state_t{actor_move_phase_t::source_remote, std::move (transfer_id),
                                        std::chrono::steady_clock::now ()})
      .second;
    }).get ();
}

void actor_transfer_coordinator_t::cancel_move (const std::string &actor_key)
{
    return _lane.run ([&, this] {
    _moves.erase (actor_key);
    _backlogs.erase (actor_key);
    }).get ();
}

void actor_transfer_coordinator_t::mark_reconcile (
  const std::string &actor_key,
  std::chrono::steady_clock::duration bound,
  std::optional<reconcile_target_context_t> context)
{
    return _lane.run ([&, this] {
    const auto deadline = std::chrono::steady_clock::now () + bound;
    auto found = _moves.find (actor_key);
    if (found == _moves.end ()) {
        auto move = move_state_t{actor_move_phase_t::reconcile, std::string{}};
        move.reconcile_deadline = deadline;
        move.reconcile_context = std::move (context);
        _moves.emplace (actor_key, std::move (move));
        return;
    }
    found->second.phase = actor_move_phase_t::reconcile;
    found->second.reconcile_deadline = deadline;
    found->second.reconcile_context = std::move (context);
    }).get ();
}

std::vector<expired_reconcile_t> actor_transfer_coordinator_t::reconcile_keys_expired (
  std::chrono::steady_clock::time_point now) const
{
    return _lane.run ([&, this] {
    std::vector<expired_reconcile_t> expired;
    for (const auto &[key, move] : _moves) {
        if (move.phase == actor_move_phase_t::reconcile && move.reconcile_deadline
            && *move.reconcile_deadline <= now) {
            expired.push_back ({key, move.reconcile_context});
        }
    }
    return expired;
    }).get ();
}

std::optional<std::chrono::steady_clock::duration>
actor_transfer_coordinator_t::complete_move (const std::string &actor_key)
{
    return _lane.run ([&, this] () -> std::optional<std::chrono::steady_clock::duration> {
    const auto found = _moves.find (actor_key);
    if (found == _moves.end ()) {
        return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::duration> elapsed;
    if (found->second.transfer_started_at) {
        elapsed = std::chrono::steady_clock::now () - *found->second.transfer_started_at;
    }
    _moves.erase (found);
    return elapsed;
    }).get ();
}

actor_move_completion_t actor_transfer_coordinator_t::complete_move_and_take_backlog (
  const std::string &actor_key)
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    if (found == _moves.end ())
        return actor_move_completion_t{std::nullopt, {}, true};
    std::optional<std::chrono::steady_clock::duration> elapsed;
    if (found->second.transfer_started_at) {
        elapsed = std::chrono::steady_clock::now () - *found->second.transfer_started_at;
    }
    std::vector<handoff_packet_t> backlog;
    if (const auto queued = _backlogs.find (actor_key); queued != _backlogs.end ()) {
        backlog = std::move (queued->second);
        _backlogs.erase (queued);
    }
    _moves.erase (found);
    return actor_move_completion_t{std::move (elapsed), std::move (backlog), true};
    }).get ();
}

actor_move_completion_t actor_transfer_coordinator_t::finish_move_replay (
  const std::string &actor_key)
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    if (found == _moves.end ())
        return actor_move_completion_t{std::nullopt, {}, true};
    std::vector<handoff_packet_t> backlog;
    if (const auto queued = _backlogs.find (actor_key); queued != _backlogs.end ()) {
        backlog = std::move (queued->second);
        _backlogs.erase (queued);
    }
    if (!backlog.empty ())
        return actor_move_completion_t{std::nullopt, std::move (backlog), false};
    std::optional<std::chrono::steady_clock::duration> elapsed;
    if (found->second.transfer_started_at) {
        elapsed = std::chrono::steady_clock::now () - *found->second.transfer_started_at;
    }
    _moves.erase (found);
    return actor_move_completion_t{std::move (elapsed), {}, true};
    }).get ();
}

handoff_append_result_t actor_transfer_coordinator_t::try_append_backlog (
  const std::string &actor_key,
  handoff_packet_t packet)
{
    return _lane.run ([&, this] {
    const auto moving = _moves.find (actor_key);
    if (moving == _moves.end ()) {
        return handoff_append_result_t::not_moving;
    }
    // The source preserves packets before authority commit. On the target,
    // an arrival between admission Accepted and PREPARE (target_pending,
    // spec 15 §4.2 step 2-4) parks here exactly like one that arrives during
    // target_committing (after PREPARE, before lifecycle open) — both cases
    // retain packets in the same per-actor backlog until the real Actor
    // queue exists and can replay them in arrival order.
    if (moving->second.phase != actor_move_phase_t::source_reserved
        && moving->second.phase != actor_move_phase_t::local
        && moving->second.phase != actor_move_phase_t::source_remote
        && moving->second.phase != actor_move_phase_t::target_pending
        && moving->second.phase != actor_move_phase_t::target_committing
        && moving->second.phase != actor_move_phase_t::reconcile) {
        return handoff_append_result_t::not_moving;
    }
    const auto backlog_found = _backlogs.find (actor_key);
    const auto backlog_size = backlog_found == _backlogs.end ()
                                 ? std::size_t{0}
                                 : backlog_found->second.size ();
    if (packet.is_request) {
        const auto request_id = packet.metadata.find ("__zlink.actorRequestId");
        if (request_id != packet.metadata.end () && !request_id->second.empty ()
            && backlog_found != _backlogs.end ()) {
            const auto duplicate = std::find_if (
              backlog_found->second.begin (), backlog_found->second.end (),
              [&request_id] (const handoff_packet_t &queued) {
                  const auto queued_id = queued.metadata.find ("__zlink.actorRequestId");
                  return queued.is_request && queued_id != queued.metadata.end ()
                         && queued_id->second == request_id->second;
              });
            if (duplicate != backlog_found->second.end ()) {
                return handoff_append_result_t::duplicate_request;
            }
        }
    }
    auto &backlog = _backlogs[actor_key];
    backlog.push_back (std::move (packet));
    return handoff_append_result_t::appended;
    }).get ();
}

bool actor_transfer_coordinator_t::stage_commit_backlog (
  const std::string &transfer_id,
  std::vector<handoff_packet_t> backlog)
{
    return _lane.run ([&, this] {
    const auto admission = _admissions.find (transfer_id);
    if (admission == _admissions.end ())
        return false;
    const auto moving = _moves.find (admission->second.actor_key);
    if (moving == _moves.end () || moving->second.transfer_id != transfer_id
        || moving->second.phase != actor_move_phase_t::target_committing) {
        return false;
    }

    const auto request_id = [] (const handoff_packet_t &packet)
      -> std::optional<std::string_view> {
        if (!packet.is_request)
            return std::nullopt;
        const auto found = packet.metadata.find ("__zlink.actorRequestId");
        if (found == packet.metadata.end () || found->second.empty ())
            return std::nullopt;
        return found->second;
    };
    const auto existing = _backlogs.find (admission->second.actor_key);
    for (std::size_t index = 0; index < backlog.size (); ++index) {
        const auto id = request_id (backlog[index]);
        if (!id)
            continue;
        if (existing != _backlogs.end ()
            && std::any_of (existing->second.begin (), existing->second.end (),
                            [&] (const handoff_packet_t &packet) {
                                return request_id (packet) == id;
                            })) {
            return false;
        }
        if (std::any_of (backlog.begin (), backlog.begin () + index,
                         [&] (const handoff_packet_t &packet) {
                             return request_id (packet) == id;
                         })) {
            return false;
        }
    }

    auto &target = _backlogs[admission->second.actor_key];
    target.reserve (target.size () + backlog.size ());
    target.insert (target.end (),
                   std::make_move_iterator (backlog.begin ()),
                   std::make_move_iterator (backlog.end ()));
    return true;
    }).get ();
}

std::vector<handoff_packet_t>
actor_transfer_coordinator_t::take_backlog (const std::string &actor_key)
{
    return _lane.run ([&, this] () -> std::vector<handoff_packet_t> {
    const auto found = _backlogs.find (actor_key);
    if (found == _backlogs.end ()) {
        return {};
    }
    auto backlog = std::move (found->second);
    _backlogs.erase (found);
    return backlog;
    }).get ();
}

void actor_transfer_coordinator_t::activate_message_follow (
  const std::string &actor_key,
  runtime::protocol::actor_route_fence_t source_fence,
  actor_ref_t target_actor,
  spot_route_t target_route,
  runtime::protocol::actor_route_fence_t target_fence,
  std::chrono::steady_clock::time_point remove_at,
  std::string transfer_id)
{
    return _lane.run ([&, this] {
    auto &routes = _message_follow_routes[actor_key];
    const auto existing = std::find_if (
      routes.begin (), routes.end (), [&] (const auto &route) {
          return route.source_fence == source_fence;
      });
    if (existing == routes.end ()) {
        message_follow_suppression_key_t suppression_key{
          source_fence, target_fence};
        _message_follow_suppression.retain (suppression_key);
        routes.push_back (
          message_follow_route_t{std::move (source_fence), std::move (target_actor),
                                 std::move (target_route), std::move (target_fence), remove_at,
                                 std::move (transfer_id), 0, 0,
                                 std::move (suppression_key)});
        return;
    }
    const message_follow_suppression_key_t replacement_key{
      source_fence, target_fence};
    if (existing->suppression_key != replacement_key) {
        _message_follow_suppression.erase (existing->suppression_key);
        _message_follow_suppression.retain (replacement_key);
        existing->suppression_key = replacement_key;
    }
    existing->target_actor = std::move (target_actor);
    existing->target_route = std::move (target_route);
    existing->target_fence = std::move (target_fence);
    existing->remove_at = std::max (existing->remove_at, remove_at);
    existing->transfer_id = std::move (transfer_id);
    }).get ();
}

bool actor_transfer_coordinator_t::matches_message_follow_source (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence) const
{
    return _lane.run ([&, this] {
        return matches_message_follow_source_unlocked (actor_key, source_fence,
                                                       std::chrono::steady_clock::now ());
    }).get ();
}

bool actor_transfer_coordinator_t::matches_message_follow_source_unlocked (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::chrono::steady_clock::time_point now) const
{
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    return std::ranges::any_of (found->second, [&] (const auto &route) {
        return route.remove_at > now && route.source_fence == source_fence;
    });
}

actor_transfer_dispatch_state_snapshot_t actor_transfer_coordinator_t::project_dispatch_state (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t *source_fence) const
{
    return _lane.run ([&, this] {
        actor_transfer_dispatch_state_snapshot_t snapshot;
        snapshot.matches_message_follow_source =
          source_fence != nullptr
          && matches_message_follow_source_unlocked (actor_key, *source_fence,
                                                     std::chrono::steady_clock::now ());
        snapshot.transfer_in_progress = _moves.contains (actor_key);
        return snapshot;
    }).get ();
}

std::optional<actor_message_follow_target_t>
actor_transfer_coordinator_t::message_follow_target (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence) const
{
    return _lane.run ([&, this] () -> std::optional<actor_message_follow_target_t> {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return std::nullopt;
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence;
      });
    if (route == found->second.end ()
        || route->remove_at <= std::chrono::steady_clock::now ()) {
        return std::nullopt;
    }
    return actor_message_follow_target_t{
      route->target_actor, route->target_route, route->source_fence,
      route->target_fence};
    }).get ();
}

bool actor_transfer_coordinator_t::message_follow_targets_node (
  const std::string &actor_key, const zlink::routing_id_t &node) const
{
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto now = std::chrono::steady_clock::now ();
    return std::ranges::any_of (found->second, [&] (const auto &route) {
        return route.remove_at > now
               && zlink::routing_id_t::from (
                    std::string (route.target_route.node_rid.value ()))
                    .to_bytes ()
                    == node.to_bytes ();
    });
    }).get ();
}

bool actor_transfer_coordinator_t::has_message_follow_route (
  const std::string &actor_key) const
{
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto now = std::chrono::steady_clock::now ();
    return std::ranges::any_of (found->second, [&] (const auto &route) {
        return route.remove_at > now;
    });
    }).get ();
}

std::optional<runtime::protocol::actor_route_fence_t>
actor_transfer_coordinator_t::message_follow_source_for_generation (
  const std::string &actor_key, std::uint64_t generation) const
{
    return _lane.run (
      [&, this] () -> std::optional<runtime::protocol::actor_route_fence_t> {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return std::nullopt;
    const auto now = std::chrono::steady_clock::now ();
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.remove_at > now
                 && candidate.source_fence.object_generation == generation;
      });
    if (route == found->second.end ())
        return std::nullopt;
    return route->source_fence;
    }).get ();
}

result_t<std::optional<actor_message_follow_target_t>>
actor_transfer_coordinator_t::try_acquire_message_follow (
  const std::string &actor_key,
  std::uint64_t generation,
  std::size_t payload_bytes,
  std::size_t hop_count,
  const runtime::protocol::actor_route_fence_t &source_fence)
{
    constexpr std::size_t max_hops =
      zlink::framework::runtime::protocol::messageFollowHopCount;
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ()) {
        return result_t<std::optional<actor_message_follow_target_t>>::success (
          std::nullopt);
    }
    if (source_fence.object_generation != generation) {
        return result_t<std::optional<actor_message_follow_target_t>>::failure (
          framework_error_kind_t::invalid_operation,
          "Actor Message Follow generation does not match the committed source");
    }
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence;
      });
    if (route == found->second.end ()) {
        return result_t<std::optional<actor_message_follow_target_t>>::failure (
          framework_error_kind_t::unavailable,
          "Actor Message Follow source fence does not match the committed route");
    }
    if (route->remove_at <= std::chrono::steady_clock::now ()) {
        return result_t<std::optional<actor_message_follow_target_t>>::failure (
          framework_error_kind_t::unavailable,
          "Actor Message Follow route has expired");
    }
    if (hop_count >= max_hops) {
        return result_t<std::optional<actor_message_follow_target_t>>::failure (
          framework_error_kind_t::unavailable,
          "Actor Message Follow hop bound was exceeded");
    }
    constexpr auto size_max = std::numeric_limits<std::size_t>::max ();
    if (route->in_flight_messages == size_max
        || payload_bytes > size_max - route->in_flight_bytes) {
        return result_t<std::optional<actor_message_follow_target_t>>::failure (
          framework_error_kind_t::capacity_exceeded,
          "Actor Message Follow in-flight accounting overflowed");
    }
    ++route->in_flight_messages;
    route->in_flight_bytes += payload_bytes;
    return result_t<std::optional<actor_message_follow_target_t>>::success (
      actor_message_follow_target_t{
        route->target_actor, route->target_route,
        route->source_fence, route->target_fence});
    }).get ();
}

void actor_transfer_coordinator_t::release_message_follow (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::size_t payload_bytes)
{
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return;
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence;
      });
    if (route == found->second.end ())
        return;
    if (route->in_flight_messages != 0)
        --route->in_flight_messages;
    route->in_flight_bytes =
      payload_bytes >= route->in_flight_bytes
        ? 0
        : route->in_flight_bytes - payload_bytes;
    }).get ();
}

bool actor_transfer_coordinator_t::try_begin_message_follow_notification (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence;
      });
    if (route == found->second.end ()
        || route->target_fence != target_fence
        || route->remove_at <= std::chrono::steady_clock::now ())
        return false;
    return _message_follow_suppression.try_begin (route->suppression_key);
    }).get ();
}

bool actor_transfer_coordinator_t::complete_message_follow_notification (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence,
  bool transport_accepted)
{
    return _lane.run ([&, this] {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence
                 && candidate.target_fence == target_fence;
      });
    if (route == found->second.end ())
        return false;
    return transport_accepted
             ? _message_follow_suppression.mark_sent (route->suppression_key)
             : _message_follow_suppression.abort (route->suppression_key);
    }).get ();
}

std::vector<removed_actor_message_follow_t>
actor_transfer_coordinator_t::remove_expired_message_follow (
  std::chrono::steady_clock::time_point now)
{
    return _lane.run ([&, this] {
    std::vector<removed_actor_message_follow_t> removed;
    for (auto found = _message_follow_routes.begin ();
         found != _message_follow_routes.end ();) {
        auto &routes = found->second;
        for (auto route = routes.begin (); route != routes.end ();) {
            if (route->remove_at > now) {
                ++route;
                continue;
            }
            removed.push_back (removed_actor_message_follow_t{
              found->first, route->source_fence, route->transfer_id});
            _message_follow_suppression.erase (route->suppression_key);
            route = routes.erase (route);
        }
        if (routes.empty ()) {
            found = _message_follow_routes.erase (found);
        } else {
            ++found;
        }
    }
    return removed;
    }).get ();
}

std::optional<removed_actor_message_follow_t>
actor_transfer_coordinator_t::remove_message_follow (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    return _lane.run ([&, this] () -> std::optional<removed_actor_message_follow_t> {
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return std::nullopt;
    auto &routes = found->second;
    const auto route = std::find_if (
      routes.begin (), routes.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence
                 && candidate.target_fence == target_fence;
      });
    if (route == routes.end ())
        return std::nullopt;
    removed_actor_message_follow_t removed{
      actor_key, route->source_fence, route->transfer_id};
    _message_follow_suppression.erase (route->suppression_key);
    routes.erase (route);
    if (routes.empty ())
        _message_follow_routes.erase (found);
    return removed;
    }).get ();
}

bool actor_transfer_coordinator_t::blocks_dispatch (const std::string &actor_key) const
{
    return _lane.run ([&, this] {
    return _moves.contains (actor_key);
    }).get ();
}

std::optional<actor_move_phase_t>
actor_transfer_coordinator_t::phase (const std::string &actor_key) const
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    return found == _moves.end () ? std::nullopt : std::make_optional (found->second.phase);
    }).get ();
}

std::optional<std::string>
actor_transfer_coordinator_t::transfer_id (const std::string &actor_key) const
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    return found == _moves.end () || found->second.transfer_id.empty ()
             ? std::nullopt
             : std::make_optional (found->second.transfer_id);
    }).get ();
}

bool actor_transfer_coordinator_t::matches_source_remote_transfer (
  const std::string &actor_key,
  const std::string &transfer_id) const
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    return found != _moves.end ()
           && found->second.phase == actor_move_phase_t::source_remote
           && found->second.transfer_id == transfer_id;
    }).get ();
}

bool actor_transfer_coordinator_t::try_submit_source_leave (
  const std::string &actor_key,
  const std::string &transfer_id)
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    if (found == _moves.end ()
        || found->second.phase != actor_move_phase_t::source_remote
        || found->second.transfer_id != transfer_id
        || found->second.source_leave_submitted) {
        return false;
    }
    found->second.source_leave_submitted = true;
    return true;
    }).get ();
}

bool actor_transfer_coordinator_t::source_leave_submitted (
  const std::string &actor_key,
  const std::string &transfer_id) const
{
    return _lane.run ([&, this] {
    const auto found = _moves.find (actor_key);
    return found != _moves.end ()
           && found->second.phase == actor_move_phase_t::source_remote
           && found->second.transfer_id == transfer_id
           && found->second.source_leave_submitted;
    }).get ();
}

bool actor_transfer_coordinator_t::try_add_admission (std::string transfer_id,
                                                      pending_actor_admission_t admission)
{
    return _lane.run ([&, this] {
    if (_admissions.contains (transfer_id) || _completed_admissions.contains (transfer_id)) {
        return false;
    }
    const auto actor_key = admission.actor_key;
    if (const auto moving = _moves.find (actor_key); moving != _moves.end ()) {
        if ((moving->second.phase != actor_move_phase_t::target_pending
             && moving->second.phase != actor_move_phase_t::target_committing)
            || moving->second.transfer_id == transfer_id) {
            return false;
        }
        //  A newer exact identity (different RelocationId/transfer_id) for
        //  the same object displaces an older attempt that has not yet
        //  reached commit-authority — the later attempt wins and any
        //  arrivals it parked so far are discarded with it (spec 15 §4.2
        //  "같은 object의 relocation temporary queue는 하나만 존재한다" /
        //  "나중 attempt가 유효하며"). This reclaims exactly the state
        //  fail_commit/cleanup_expired already reclaim for a dead attempt
        //  (_admissions/_moves/_backlogs) — nothing more, nothing less.
        //  A target_committing eviction leaves a narrow, self-healing
        //  window: if PREPARE for the displaced identity is concurrently
        //  between commit-authority and installing its staged Actor, the
        //  caller must re-check is_current() before publishing that side
        //  effect (spec 15 §4.2 "이전 identity의 늦은 ... Restore는 조립에
        //  연결하지 않고 폐기한다") — see prepare_remote_actor_to_spot. If
        //  PREPARE already finished and installed its staged Actor before
        //  this eviction runs, that Actor is orphaned until the newer
        //  identity's own PREPARE overwrites the same actor_key slot, which
        //  is the same map-assignment the framework already relies on for
        //  a retried PREPARE.
        _admissions.erase (moving->second.transfer_id);
        _backlogs.erase (actor_key);
        _moves.erase (moving);
    }
    _moves.emplace (actor_key, move_state_t{actor_move_phase_t::target_pending, transfer_id});
    _admissions.emplace (std::move (transfer_id), std::move (admission));
    return true;
    }).get ();
}

bool actor_transfer_coordinator_t::is_current (
  const std::string &actor_key,
  const std::string &transfer_id) const
{
    return _lane.run ([&, this] {
    const auto moving = _moves.find (actor_key);
    return moving != _moves.end () && moving->second.transfer_id == transfer_id;
    }).get ();
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::admission (
  const std::string &transfer_id) const
{
    return _lane.run ([&, this] {
    const auto found = _admissions.find (transfer_id);
    return found == _admissions.end ()
             ? std::nullopt
             : std::make_optional (found->second);
    }).get ();
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::begin_commit (const std::string &transfer_id,
                                            const actor_ref_t &source_actor,
                                            const spot_id_t &target_spot_id)
{
    return _lane.run ([&, this] () -> std::optional<pending_actor_admission_t> {
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return std::nullopt;
    }
    if (found->second.deadline <= std::chrono::steady_clock::now ()) {
        _moves.erase (found->second.actor_key);
        _backlogs.erase (found->second.actor_key);
        _admissions.erase (found);
        return std::nullopt;
    }
    if (found->second.source_actor.actor_id () != source_actor.actor_id ()
        || ::zlink::framework::detail::actor_ref_access_t::actor_type (found->second.source_actor)
             != ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)
        || found->second.source_actor.object_generation () != source_actor.object_generation ()
        || found->second.source_actor.node_rid ().value () != source_actor.node_rid ().value ()
        || found->second.target_spot_id != target_spot_id) {
        return std::nullopt;
    }
    auto moving = _moves.find (found->second.actor_key);
    if (moving == _moves.end () || moving->second.transfer_id != transfer_id
        || moving->second.phase != actor_move_phase_t::target_pending) {
        return std::nullopt;
    }
    moving->second.phase = actor_move_phase_t::target_committing;
    return found->second;
    }).get ();
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::pending_commit (const std::string &transfer_id,
                                              const actor_ref_t &source_actor,
                                              const spot_id_t &target_spot_id) const
{
    return _lane.run ([&, this] () -> std::optional<pending_actor_admission_t> {
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return std::nullopt;
    }
    const auto moving = _moves.find (found->second.actor_key);
    if (moving == _moves.end () || moving->second.transfer_id != transfer_id
        || moving->second.phase != actor_move_phase_t::target_committing
        || found->second.source_actor.actor_id () != source_actor.actor_id ()
        || ::zlink::framework::detail::actor_ref_access_t::actor_type (found->second.source_actor)
             != ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)
        || found->second.source_actor.object_generation () != source_actor.object_generation ()
        || found->second.source_actor.node_rid ().value () != source_actor.node_rid ().value ()
        || found->second.target_spot_id != target_spot_id) {
        return std::nullopt;
    }
    return found->second;
    }).get ();
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::completed_commit (
  const std::string &transfer_id,
  const actor_ref_t &source_actor,
  const spot_id_t &target_spot_id) const
{
    return _lane.run ([&, this] () -> std::optional<pending_actor_admission_t> {
    const auto found = _completed_admissions.find (transfer_id);
    if (found == _completed_admissions.end ()
        || found->second.deadline <= std::chrono::steady_clock::now ()
        || !found->second.matches_prepare (
          source_actor, found->second.source_spot_id,
          target_spot_id,
          found->second.completion_operation_id_high,
          found->second.completion_operation_id_low))
        return std::nullopt;
    return found->second;
    }).get ();
}

std::optional<std::vector<handoff_packet_t>>
actor_transfer_coordinator_t::complete_commit_and_take_backlog (
  const std::string &transfer_id,
  const actor_ref_t &source_actor,
  const spot_id_t &target_spot_id)
{
    return _lane.run ([&, this] () -> std::optional<std::vector<handoff_packet_t>> {
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ())
        return std::nullopt;
    const auto moving = _moves.find (found->second.actor_key);
    if (moving == _moves.end () || moving->second.transfer_id != transfer_id
        || moving->second.phase != actor_move_phase_t::target_committing
        || found->second.source_actor.actor_id () != source_actor.actor_id ()
        || ::zlink::framework::detail::actor_ref_access_t::actor_type (
             found->second.source_actor)
             != ::zlink::framework::detail::actor_ref_access_t::actor_type (
               source_actor)
        || found->second.source_actor.object_generation ()
             != source_actor.object_generation ()
        || found->second.source_actor.node_rid ().value ()
             != source_actor.node_rid ().value ()
        || found->second.target_spot_id != target_spot_id) {
        return std::nullopt;
    }

    std::vector<handoff_packet_t> backlog;
    if (const auto queued = _backlogs.find (found->second.actor_key);
        queued != _backlogs.end ()) {
        backlog = std::move (queued->second);
        _backlogs.erase (queued);
    }
    _moves.erase (moving);
    _completed_admissions.insert_or_assign (transfer_id, found->second);
    _admissions.erase (found);
    return backlog;
    }).get ();
}

bool actor_transfer_coordinator_t::stage_session_relocation_route (
  const std::string &transfer_id,
  std::vector<std::uint8_t> route,
  std::string actor_type,
  std::uint64_t target_owner_lease_generation)
{
    if (route.empty () || actor_type.empty ()
        || target_owner_lease_generation == 0)
        return false;
    return _lane.run ([&, this] {
    auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        found = _completed_admissions.find (transfer_id);
        if (found == _completed_admissions.end ())
            return false;
    }
    auto &admission = found->second;
    if (!admission.session_relocation_route.empty ()) {
        return admission.session_relocation_route == route
               && admission.session_relocation_actor_type == actor_type
               && admission.session_relocation_target_owner_lease_generation
                    == target_owner_lease_generation;
    }
    admission.session_relocation_route = std::move (route);
    admission.session_relocation_actor_type = std::move (actor_type);
    admission.session_relocation_target_owner_lease_generation =
      target_owner_lease_generation;
    return true;
    }).get ();
}

bool actor_transfer_coordinator_t::commit_session_relocation_route_authority (
  const std::string &transfer_id,
  std::uint64_t previous_authority_owner_generation,
  std::uint64_t target_authority_owner_generation)
{
    if (previous_authority_owner_generation == 0
        || target_authority_owner_generation <= previous_authority_owner_generation)
        return false;
    return _lane.run ([&, this] {
    auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        found = _completed_admissions.find (transfer_id);
        if (found == _completed_admissions.end ())
            return false;
    }
    auto &admission = found->second;
    if (admission.session_relocation_route.empty ())
        return false;
    if (admission.session_relocation_committed_target_authority_owner_generation != 0) {
        return admission.session_relocation_committed_previous_authority_owner_generation
                 == previous_authority_owner_generation
               && admission.session_relocation_committed_target_authority_owner_generation
                    == target_authority_owner_generation;
    }
    admission.session_relocation_committed_previous_authority_owner_generation =
      previous_authority_owner_generation;
    admission.session_relocation_committed_target_authority_owner_generation =
      target_authority_owner_generation;
    return true;
    }).get ();
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::session_relocation_admission (
  const std::string &transfer_id) const
{
    return _lane.run ([&, this] () -> std::optional<pending_actor_admission_t> {
    if (const auto found = _admissions.find (transfer_id);
        found != _admissions.end ())
        return found->second;
    if (const auto found = _completed_admissions.find (transfer_id);
        found != _completed_admissions.end ())
        return found->second;
    return std::nullopt;
    }).get ();
}

void actor_transfer_coordinator_t::fail_commit (const std::string &transfer_id, bool reconcile)
{
    return _lane.run ([&, this] {
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return;
    }
    const auto actor_key = found->second.actor_key;
    _admissions.erase (found);
    if (reconcile) {
        //  reconcile stays a "moving" phase try_append_backlog still parks
        //  into, so any backlog accumulated since Accepted (target_pending)
        //  is retained, not discarded, across the transition.
        auto &move = _moves[actor_key];
        move.phase = actor_move_phase_t::reconcile;
        move.transfer_id.clear ();
    } else {
        //  Terminal failure (Rejected, invalid completion identity, prepare
        //  failure with no reconcile): the move ends here, so any arrival
        //  parked since Accepted must not be left stranded under a dead
        //  actor_key for a later admission to inherit (spec 15 §4.2 cleanup
        //  "exactly once").
        _moves.erase (actor_key);
        _backlogs.erase (actor_key);
    }
    }).get ();
}

void actor_transfer_coordinator_t::complete_commit (const std::string &transfer_id)
{
    return _lane.run ([&, this] {
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return;
    }
    _moves.erase (found->second.actor_key);
    _completed_admissions.insert_or_assign (
      transfer_id, found->second);
    _admissions.erase (found);
    }).get ();
}

std::vector<expired_actor_admission_t>
actor_transfer_coordinator_t::cleanup_expired (std::chrono::steady_clock::time_point now)
{
    return _lane.run ([&, this] {
    std::vector<expired_actor_admission_t> removed;
    for (auto found = _admissions.begin (); found != _admissions.end ();) {
        const auto moving = _moves.find (found->second.actor_key);
        const bool can_expire =
          moving != _moves.end () && moving->second.phase == actor_move_phase_t::target_pending;
        if (can_expire && found->second.deadline <= now) {
            _moves.erase (moving);
            _backlogs.erase (found->second.actor_key);
            removed.push_back (expired_actor_admission_t{found->first, found->second});
            found = _admissions.erase (found);
        } else {
            ++found;
        }
    }
    for (auto found = _completed_admissions.begin ();
         found != _completed_admissions.end ();) {
        if (found->second.deadline <= now)
            found = _completed_admissions.erase (found);
        else
            ++found;
    }
    return removed;
    }).get ();
}

std::size_t actor_transfer_coordinator_t::pending_count () const
{
    return _lane.run ([&, this] {
    return _admissions.size ();
    }).get ();
}

std::string actor_transfer_coordinator_t::next_transfer_id (const std::string &node_rid)
{
    return _lane.run ([&, this] {
    return node_rid + ":" + std::to_string (_next_transfer_id++);
    }).get ();
}

} // namespace zlink::framework::detail
