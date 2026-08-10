/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "actor_transfer_coordinator.hpp"

#include <service_wire_constants.hpp>

#include <algorithm>

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

namespace
{

std::size_t handoff_packet_bytes (const handoff_packet_t &packet) noexcept
{
    //  The backlog has no stored-size bound; the accounting only reports how
    //  much a packet holds, saturating instead of overflowing.
    std::size_t bytes = 0;
    const auto add = [&bytes] (std::size_t value) {
        constexpr auto ceiling = std::numeric_limits<std::size_t>::max ();
        bytes = value > ceiling - bytes ? ceiling : bytes + value;
    };
    add (packet.packet_name.size ());
    add (packet.payload.size ());
    add (packet.content_type.size ());
    for (const auto &[key, value] : packet.metadata) {
        add (key.size ());
        add (value.size ());
    }
    return bytes;
}

} // namespace

bool actor_transfer_coordinator_t::try_reserve_source (const std::string &actor_key,
                                                       std::string transfer_id)
{
    std::lock_guard lock (_mutex);
    return _moves.emplace (
                   actor_key,
                   move_state_t{actor_move_phase_t::source_reserved, std::move (transfer_id)})
      .second;
}

bool actor_transfer_coordinator_t::try_begin_local (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    if (auto found = _moves.find (actor_key); found != _moves.end ()) {
        if (found->second.phase != actor_move_phase_t::source_reserved)
            return false;
        found->second.phase = actor_move_phase_t::local;
        return true;
    }
    return _moves.emplace (actor_key, move_state_t{actor_move_phase_t::local, std::string{}})
      .second;
}

bool actor_transfer_coordinator_t::try_begin_source_remote (const std::string &actor_key,
                                                            std::string transfer_id)
{
    std::lock_guard lock (_mutex);
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
}

void actor_transfer_coordinator_t::cancel_move (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    _moves.erase (actor_key);
    _backlogs.erase (actor_key);
    _backlog_bytes.erase (actor_key);
}

void actor_transfer_coordinator_t::mark_reconcile (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    auto found = _moves.find (actor_key);
    if (found == _moves.end ()) {
        _moves.emplace (actor_key, move_state_t{actor_move_phase_t::reconcile, std::string{}});
        return;
    }
    found->second.phase = actor_move_phase_t::reconcile;
}

std::optional<std::chrono::steady_clock::duration>
actor_transfer_coordinator_t::complete_move (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
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
}

actor_move_completion_t actor_transfer_coordinator_t::complete_move_and_take_backlog (
  const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
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
        _backlog_bytes.erase (actor_key);
    }
    _moves.erase (found);
    return actor_move_completion_t{std::move (elapsed), std::move (backlog), true};
}

actor_move_completion_t actor_transfer_coordinator_t::finish_move_replay (
  const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    const auto found = _moves.find (actor_key);
    if (found == _moves.end ())
        return actor_move_completion_t{std::nullopt, {}, true};
    std::vector<handoff_packet_t> backlog;
    if (const auto queued = _backlogs.find (actor_key); queued != _backlogs.end ()) {
        backlog = std::move (queued->second);
        _backlogs.erase (queued);
        _backlog_bytes.erase (actor_key);
    }
    if (!backlog.empty ())
        return actor_move_completion_t{std::nullopt, std::move (backlog), false};
    std::optional<std::chrono::steady_clock::duration> elapsed;
    if (found->second.transfer_started_at) {
        elapsed = std::chrono::steady_clock::now () - *found->second.transfer_started_at;
    }
    _moves.erase (found);
    return actor_move_completion_t{std::move (elapsed), {}, true};
}

handoff_append_result_t actor_transfer_coordinator_t::try_append_backlog (
  const std::string &actor_key,
  handoff_packet_t packet)
{
    std::lock_guard lock (_mutex);
    const auto moving = _moves.find (actor_key);
    if (moving == _moves.end ()) {
        return handoff_append_result_t::not_moving;
    }
    // The source preserves packets before authority commit. During target
    // commit, packets admitted through the committed route are also retained
    // until lifecycle activation and completion delivery establish the final
    // serial order.
    if (moving->second.phase != actor_move_phase_t::source_reserved
        && moving->second.phase != actor_move_phase_t::local
        && moving->second.phase != actor_move_phase_t::source_remote
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
    const auto packet_bytes = handoff_packet_bytes (packet);
    const auto current_bytes = _backlog_bytes.contains (actor_key)
                                  ? _backlog_bytes.at (actor_key)
                                  : std::size_t{0};
    auto &backlog = _backlogs[actor_key];
    backlog.push_back (std::move (packet));
    _backlog_bytes[actor_key] = current_bytes + packet_bytes;
    return handoff_append_result_t::appended;
}

std::vector<handoff_packet_t>
actor_transfer_coordinator_t::take_backlog (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    const auto found = _backlogs.find (actor_key);
    if (found == _backlogs.end ()) {
        return {};
    }
    auto backlog = std::move (found->second);
    _backlogs.erase (found);
    _backlog_bytes.erase (actor_key);
    return backlog;
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
    std::lock_guard lock (_mutex);
    auto &routes = _message_follow_routes[actor_key];
    const auto existing = std::find_if (
      routes.begin (), routes.end (), [&] (const auto &route) {
          return route.source_fence == source_fence;
      });
    if (existing == routes.end ()) {
        routes.push_back (
          message_follow_route_t{std::move (source_fence), std::move (target_actor),
                                 std::move (target_route), std::move (target_fence), remove_at,
                                 std::move (transfer_id), 0, 0, {}});
        return;
    }
    existing->target_actor = std::move (target_actor);
    existing->target_route = std::move (target_route);
    existing->target_fence = std::move (target_fence);
    existing->remove_at = std::max (existing->remove_at, remove_at);
    existing->transfer_id = std::move (transfer_id);
}

bool actor_transfer_coordinator_t::matches_message_follow_source (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence) const
{
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto now = std::chrono::steady_clock::now ();
    return std::ranges::any_of (found->second, [&] (const auto &route) {
        return route.remove_at > now && route.source_fence == source_fence;
    });
}

std::optional<actor_message_follow_target_t>
actor_transfer_coordinator_t::message_follow_target (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence) const
{
    std::lock_guard lock (_mutex);
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
}

bool actor_transfer_coordinator_t::has_message_follow_route (
  const std::string &actor_key) const
{
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto now = std::chrono::steady_clock::now ();
    return std::ranges::any_of (found->second, [&] (const auto &route) {
        return route.remove_at > now;
    });
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
    std::lock_guard lock (_mutex);
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
    ++route->in_flight_messages;
    route->in_flight_bytes += payload_bytes;
    return result_t<std::optional<actor_message_follow_target_t>>::success (
      actor_message_follow_target_t{
        route->target_actor, route->target_route,
        route->source_fence, route->target_fence});
}

void actor_transfer_coordinator_t::release_message_follow (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::size_t payload_bytes) noexcept
{
    std::lock_guard lock (_mutex);
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
}

bool actor_transfer_coordinator_t::mark_message_follow_notified (
  const std::string &actor_key,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::vector<std::uint8_t> source_node_routing_id)
{
    if (source_node_routing_id.empty ())
        return false;
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ())
        return false;
    const auto route = std::find_if (
      found->second.begin (), found->second.end (), [&] (const auto &candidate) {
          return candidate.source_fence == source_fence;
      });
    if (route == found->second.end ()
        || route->remove_at <= std::chrono::steady_clock::now ())
        return false;
    return route->notified_sources.insert (
             std::move (source_node_routing_id))
           .second;
}

std::vector<removed_actor_message_follow_t>
actor_transfer_coordinator_t::remove_expired_message_follow (
  std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
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
            route = routes.erase (route);
        }
        if (routes.empty ()) {
            found = _message_follow_routes.erase (found);
        } else {
            ++found;
        }
    }
    return removed;
}

bool actor_transfer_coordinator_t::blocks_dispatch (const std::string &actor_key) const
{
    std::lock_guard lock (_mutex);
    return _moves.contains (actor_key);
}

std::optional<actor_move_phase_t>
actor_transfer_coordinator_t::phase (const std::string &actor_key) const
{
    std::lock_guard lock (_mutex);
    const auto found = _moves.find (actor_key);
    return found == _moves.end () ? std::nullopt : std::make_optional (found->second.phase);
}

std::optional<std::string>
actor_transfer_coordinator_t::transfer_id (const std::string &actor_key) const
{
    std::lock_guard lock (_mutex);
    const auto found = _moves.find (actor_key);
    return found == _moves.end () || found->second.transfer_id.empty ()
             ? std::nullopt
             : std::make_optional (found->second.transfer_id);
}

bool actor_transfer_coordinator_t::try_add_admission (std::string transfer_id,
                                                      pending_actor_admission_t admission)
{
    std::lock_guard lock (_mutex);
    if (_admissions.contains (transfer_id)
        || _completed_admissions.contains (transfer_id)
        || _moves.contains (admission.actor_key)) {
        return false;
    }
    const auto actor_key = admission.actor_key;
    _moves.emplace (actor_key, move_state_t{actor_move_phase_t::target_pending, transfer_id});
    _admissions.emplace (std::move (transfer_id), std::move (admission));
    return true;
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::admission (
  const std::string &transfer_id) const
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    return found == _admissions.end ()
             ? std::nullopt
             : std::make_optional (found->second);
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::begin_commit (const std::string &transfer_id,
                                            const actor_ref_t &source_actor,
                                            const spot_id_t &target_spot_id)
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return std::nullopt;
    }
    if (found->second.deadline <= std::chrono::steady_clock::now ()) {
        _moves.erase (found->second.actor_key);
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
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::pending_commit (const std::string &transfer_id,
                                              const actor_ref_t &source_actor,
                                              const spot_id_t &target_spot_id) const
{
    std::lock_guard lock (_mutex);
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
}

std::optional<pending_actor_admission_t>
actor_transfer_coordinator_t::completed_commit (
  const std::string &transfer_id,
  const actor_ref_t &source_actor,
  const spot_id_t &target_spot_id) const
{
    std::lock_guard lock (_mutex);
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
}

bool actor_transfer_coordinator_t::update_completion_root (
  const std::string &transfer_id,
  std::string reference,
  std::uint32_t checksum)
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ())
        return false;
    found->second.completion_root_reference =
      std::move (reference);
    found->second.completion_root_checksum = checksum;
    return true;
}

void actor_transfer_coordinator_t::fail_commit (const std::string &transfer_id, bool reconcile)
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return;
    }
    const auto actor_key = found->second.actor_key;
    _admissions.erase (found);
    if (reconcile) {
        auto &move = _moves[actor_key];
        move.phase = actor_move_phase_t::reconcile;
        move.transfer_id.clear ();
    } else {
        _moves.erase (actor_key);
    }
}

void actor_transfer_coordinator_t::complete_commit (const std::string &transfer_id)
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return;
    }
    _moves.erase (found->second.actor_key);
    _completed_admissions.insert_or_assign (
      transfer_id, found->second);
    _admissions.erase (found);
}

bool actor_transfer_coordinator_t::mark_commit_finalized (const std::string &transfer_id)
{
    std::lock_guard lock (_mutex);
    const auto found = _admissions.find (transfer_id);
    if (found == _admissions.end ()) {
        return false;
    }
    const auto moving = _moves.find (found->second.actor_key);
    if (moving == _moves.end () || moving->second.transfer_id != transfer_id
        || moving->second.phase != actor_move_phase_t::target_committing) {
        return false;
    }
    found->second.commit_finalized = true;
    return true;
}

std::vector<deferred_actor_completion_t>
actor_transfer_coordinator_t::due_deferred_completions (
  std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
    std::vector<deferred_actor_completion_t> due;
    for (auto &[transfer_id, admission] : _admissions) {
        if (!admission.commit_finalized) {
            continue;
        }
        const auto moving = _moves.find (admission.actor_key);
        if (moving == _moves.end () || moving->second.transfer_id != transfer_id
            || moving->second.phase != actor_move_phase_t::target_committing) {
            continue;
        }
        if (!admission.completion_poll_due) {
            if (admission.deadline > now) {
                continue;
            }
            // The join window elapsed without the completion-only leg. Keep
            // the admission visible past its original deadline so a late leg
            // still terminates on the completed commit, then let the runtime
            // converge from the durable owner state.
            admission.completion_poll_due = true;
            admission.deadline = now + actor_deferred_completion_retention;
        }
        if (admission.next_completion_poll_at > now) {
            continue;
        }
        admission.next_completion_poll_at =
          now + actor_deferred_completion_retry_interval;
        due.push_back (deferred_actor_completion_t{transfer_id, admission});
    }
    return due;
}

std::vector<expired_actor_admission_t>
actor_transfer_coordinator_t::cleanup_expired (std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
    std::vector<expired_actor_admission_t> removed;
    for (auto found = _admissions.begin (); found != _admissions.end ();) {
        const auto moving = _moves.find (found->second.actor_key);
        const bool can_expire =
          moving != _moves.end () && moving->second.phase == actor_move_phase_t::target_pending;
        if (can_expire && found->second.deadline <= now) {
            _moves.erase (moving);
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
}

std::size_t actor_transfer_coordinator_t::pending_count () const
{
    std::lock_guard lock (_mutex);
    return _admissions.size ();
}

std::string actor_transfer_coordinator_t::next_transfer_id (const std::string &node_rid)
{
    std::lock_guard lock (_mutex);
    return node_rid + ":" + std::to_string (_next_transfer_id++);
}

} // namespace zlink::framework::detail
