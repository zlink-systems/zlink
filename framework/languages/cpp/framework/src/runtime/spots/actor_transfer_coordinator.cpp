/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "actor_transfer_coordinator.hpp"

#include <service_wire_constants.hpp>

#include <algorithm>

namespace zlink::framework::detail
{

namespace
{

std::size_t handoff_packet_bytes (const handoff_packet_t &packet) noexcept
{
    std::size_t bytes = 0;
    const auto add = [&bytes] (std::size_t value) {
        if (bytes >= actor_handoff_backlog_max_bytes
            || value > actor_handoff_backlog_max_bytes - bytes) {
            bytes = actor_handoff_backlog_max_bytes + 1;
        } else {
            bytes += value;
        }
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

bool actor_transfer_coordinator_t::try_begin_local (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    return _moves.emplace (actor_key, move_state_t{actor_move_phase_t::local, std::string{}})
      .second;
}

bool actor_transfer_coordinator_t::try_begin_source_remote (const std::string &actor_key,
                                                            std::string transfer_id)
{
    std::lock_guard lock (_mutex);
    return _moves
      .emplace (actor_key, move_state_t{actor_move_phase_t::source_remote, std::move (transfer_id),
                                        std::chrono::steady_clock::now ()})
      .second;
}

void actor_transfer_coordinator_t::cancel_move (const std::string &actor_key)
{
    std::lock_guard lock (_mutex);
    _moves.erase (actor_key);
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
    // Only the source side of a move preserves packets; the target side keeps
    // rejecting until the commit installs the actor (§3.4).
    if (moving->second.phase != actor_move_phase_t::local
        && moving->second.phase != actor_move_phase_t::source_remote
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
    if (backlog_size >= actor_handoff_backlog_max_messages
        || packet_bytes > actor_handoff_backlog_max_bytes
        || current_bytes > actor_handoff_backlog_max_bytes - packet_bytes) {
        return handoff_append_result_t::capacity_exceeded;
    }
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
  std::uint64_t old_generation,
  actor_ref_t target_actor,
  spot_route_t target_route,
  std::chrono::steady_clock::time_point remove_at,
  std::string transfer_id)
{
    std::lock_guard lock (_mutex);
    // At most one route per actor: a later relocation replaces the previous
    // target and restarts the bounded Message Follow duration.
    _message_follow_routes.insert_or_assign (
      actor_key,
      message_follow_route_t{old_generation, std::move (target_actor),
                             std::move (target_route), remove_at,
                             std::move (transfer_id), 0, 0, {}});
}

bool actor_transfer_coordinator_t::can_follow_stale_generation (
  const std::string &actor_key,
  std::uint64_t generation) const
{
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    return found != _message_follow_routes.end ()
           && generation <= found->second.old_generation;
}

std::optional<actor_message_follow_target_t>
actor_transfer_coordinator_t::message_follow_target (const std::string &actor_key,
                                                     std::uint64_t generation) const
{
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ()
        || found->second.old_generation != generation
        || found->second.remove_at <= std::chrono::steady_clock::now ()) {
        return std::nullopt;
    }
    return actor_message_follow_target_t{
      found->second.target_actor, found->second.target_route};
}

std::optional<actor_message_follow_target_t>
actor_transfer_coordinator_t::try_acquire_message_follow (
  const std::string &actor_key,
  std::uint64_t generation,
  std::size_t payload_bytes,
  std::size_t hop_count)
{
    constexpr std::size_t max_messages =
      zlink::framework::runtime::protocol::messageFollowMessages;
    constexpr std::size_t max_bytes =
      zlink::framework::runtime::protocol::messageFollowBytes;
    constexpr std::size_t max_hops =
      zlink::framework::runtime::protocol::messageFollowHopCount;
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ()
        || found->second.old_generation != generation
        || found->second.remove_at <= std::chrono::steady_clock::now ()
        || hop_count >= max_hops
        || payload_bytes > max_bytes
        || found->second.in_flight_messages >= max_messages
        || found->second.in_flight_bytes > max_bytes - payload_bytes) {
        return std::nullopt;
    }
    ++found->second.in_flight_messages;
    found->second.in_flight_bytes += payload_bytes;
    return actor_message_follow_target_t{
      found->second.target_actor, found->second.target_route};
}

void actor_transfer_coordinator_t::release_message_follow (
  const std::string &actor_key,
  std::uint64_t generation,
  std::size_t payload_bytes) noexcept
{
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ()
        || found->second.old_generation != generation)
        return;
    if (found->second.in_flight_messages != 0)
        --found->second.in_flight_messages;
    found->second.in_flight_bytes =
      payload_bytes >= found->second.in_flight_bytes
        ? 0
        : found->second.in_flight_bytes - payload_bytes;
}

bool actor_transfer_coordinator_t::mark_message_follow_notified (
  const std::string &actor_key,
  std::uint64_t generation,
  std::vector<std::uint8_t> source_node_routing_id)
{
    if (source_node_routing_id.empty ())
        return false;
    std::lock_guard lock (_mutex);
    const auto found = _message_follow_routes.find (actor_key);
    if (found == _message_follow_routes.end ()
        || found->second.old_generation != generation
        || found->second.remove_at <= std::chrono::steady_clock::now ())
        return false;
    return found->second.notified_sources.insert (
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
        if (found->second.remove_at <= now) {
            removed.push_back (
              removed_actor_message_follow_t{found->first, found->second.old_generation,
                                              found->second.transfer_id});
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
    if (_admissions.contains (transfer_id) || _moves.contains (admission.actor_key)) {
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
    _admissions.erase (found);
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
