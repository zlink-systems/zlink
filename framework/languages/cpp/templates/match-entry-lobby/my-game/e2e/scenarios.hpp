#pragma once
#include "client.hpp"
#include <chrono>
#include <cmath>
#include <string_view>
#include <thread>
namespace MyGame::E2E
{
struct Positions
{
    static constexpr double near_x = 10, target_x = Game::second_spawn_x,
                            far_x = Game::map_max_x, far_y = Game::map_max_y - 10,
                            lane_y = Game::first_spawn_y,
                            wall_center_y = (Game::wall_min_y + Game::wall_max_y) / 2;
};

inline void same_actor (const SessionInfo &a, const SessionInfo &b)
{
    require (a.actorId == b.actorId && a.objectGeneration == b.objectGeneration
               && a.objectGeneration != 0,
             "Actor identity changed");
}

inline void same_state (const RoomView &before, const RoomView &after)
{
    auto comparable = before;
    comparable.nodeId = after.nodeId;
    require (comparable == after, "Room state changed");
}

inline const PlayerView &player_view (const RoomView &view, const std::string &id)
{
    const auto found = std::find_if (view.players.begin (), view.players.end (),
                                     [&] (const auto &player) { return player.playerId == id; });
    require (found != view.players.end (), "Room view lacks player");
    return *found;
}

inline RoomView pair (Client &a, Client &b)
{
    a.queue ();
    b.queue ();
    const auto fa = a.wait<MatchFound> (), fb = b.wait<MatchFound> ();
    require (fa.roomId == fb.roomId, "Matched different Rooms");
    const auto ready = [] (const RoomView &view) {
        return view.players.size () == Game::players_per_room;
    };
    const auto va = a.wait<RoomView> (0, ready), vb = b.wait<RoomView> (0, ready);
    require (va == vb && va.roomId == fa.roomId, "Matched Room views differ");
    require (va.phase == Game::playing_phase && !va.winner,
             "Initial Room phase differs");
    require (va.players.size () == 2, "Initial Room player count differs");
    require (va.players[0].hp == Game::initial_hp && va.players[0].lastSequence == 0
               && va.players[1].hp == Game::initial_hp && va.players[1].lastSequence == 0,
             "Initial player state differs");
    const auto first = player_view (va, a.player_id);
    const auto second = player_view (va, b.player_id);
    const auto spawns_match =
      (first.x == Game::first_spawn_x && first.y == Game::first_spawn_y
       && second.x == Game::second_spawn_x && second.y == Game::second_spawn_y)
      || (second.x == Game::first_spawn_x && second.y == Game::first_spawn_y
          && first.x == Game::second_spawn_x && first.y == Game::second_spawn_y);
    require (spawns_match, "Initial spawn positions differ");
    return va;
}

inline RoomView move_by (Client &a,
                         Client &b,
                         Client &mover,
                         double dx,
                         double dy,
                         std::int64_t sequence,
                         std::optional<RoomView> prior = std::nullopt,
                         bool require_move_budget = false)
{
    const auto before = prior ? *prior : mover.view ();
    const auto &player = player_view (before, mover.player_id);
    const auto target_x = player.x + dx, target_y = player.y + dy;
    require (std::hypot (dx, dy) <= Game::max_step, "MoveBy exceeded MaxStep");
    const auto moved = [&] (const RoomView &view) {
        return std::any_of (view.players.begin (), view.players.end (), [&] (const auto &candidate) {
            return candidate.playerId == mover.player_id && candidate.x == target_x
                   && candidate.y == target_y && candidate.lastSequence == sequence;
        });
    };
    const MoveRequest request{dx, dy, sequence};
    bool move_budget_rejected = false;
    while (true) {
        const auto from = mover.mark ();
        auto &other = mover.player_id == a.player_id ? b : a;
        const auto other_from = other.mark ();
        mover.send (request);
        auto result = mover.wait_action (from, moved);
        if (const auto *rejection = std::get_if<ActionRejected> (&result)) {
            require (!move_budget_rejected, "Move was rejected again after RetryAfterMs");
            move_budget_rejected = true;
            require (rejection->reason == Rejection::move_budget && rejection->retryAfterMs > 0,
                     "Unexpected movement rejection");
            same_state (before, mover.view ());
            std::this_thread::sleep_for (std::chrono::milliseconds (rejection->retryAfterMs));
            continue;
        }
        const auto &mover_view = std::get<RoomView> (result);
        const auto other_view = other.wait<RoomView> (other_from, moved);
        require (mover_view == other_view, "Movement views differ");
        require (!require_move_budget || move_budget_rejected,
                 "Move budget rejection was not observed");
        return mover_view;
    }
}

inline RoomView walk (Client &a,
                      Client &b,
                      Client &mover,
                      double destination_x,
                      double destination_y)
{
    auto view = mover.view ();
    auto player = player_view (view, mover.player_id);
    while (player.x != destination_x || player.y != destination_y) {
        const auto remaining_x = destination_x - player.x;
        const auto dx = remaining_x == 0
                          ? 0
                          : std::clamp (remaining_x, -Game::max_step, Game::max_step);
        const auto remaining_y = destination_y - player.y;
        const auto dy = dx != 0 || remaining_y == 0
                          ? 0
                          : std::clamp (remaining_y, -Game::max_step, Game::max_step);
        view = move_by (a, b, mover, dx, dy, player.lastSequence + 1, view);
        player = player_view (view, mover.player_id);
    }
    return view;
}

inline std::int64_t next_sequence (Client &player)
{
    return player_view (player.view (), player.player_id).lastSequence + 1;
}

inline RoomView hit (Client &a,
                     Client &b,
                     std::int64_t sequence,
                     int hp,
                     bool allow_fire_retry = true)
{
    const auto target_hp = [&] (const RoomView &view) {
        return player_view (view, b.player_id).hp == hp
               && (hp != 0 || (view.phase == Game::finished_phase && view.winner == a.player_id));
    };
    auto before = a.view ();
    const ShootRequest request{b.player_id, sequence};
    bool fire_too_soon_rejected = false;
    while (true) {
        const auto from = a.mark ();
        const auto peer_from = b.mark ();
        a.send (request);
        auto result = a.wait_action (from, target_hp);
        if (const auto *rejection = std::get_if<ActionRejected> (&result)) {
            require (!fire_too_soon_rejected, "Shot was rejected again after RetryAfterMs");
            fire_too_soon_rejected = true;
            require (rejection->reason == Rejection::fire_too_soon && rejection->retryAfterMs > 0,
                     "Unexpected shot rejection");
            same_state (before, a.view ());
            require (allow_fire_retry, "Shot was rejected after the explicit RetryAfterMs wait");
            std::this_thread::sleep_for (std::chrono::milliseconds (rejection->retryAfterMs));
            before = a.view ();
            continue;
        }
        const auto view = std::get<RoomView> (result);
        const auto peer_view = b.wait<RoomView> (peer_from, target_hp);
        require (view == peer_view, "Hit views differ");
        return view;
    }
}

inline ActionRejected reject_action (Client &player,
                                     const auto &request,
                                     const char *reason,
                                     Client *peer = nullptr)
{
    const auto before = player.view ();
    const auto from = player.mark ();
    player.send (request);
    const auto response = player.wait<ActionRejected> (from);
    require (response.reason == reason, "Action rejection reason differs");
    const auto reason_text = std::string_view (reason);
    const bool time_limited = reason_text == Rejection::move_budget
                              || reason_text == Rejection::fire_too_soon;
    require (time_limited ? response.retryAfterMs > 0 : response.retryAfterMs == 0,
             "Action rejection retry delay differs");
    same_state (before, player.view ());
    if (peer)
        same_state (before, peer->view ());
    return response;
}

inline void input_scenario (Client &a, Client &b)
{
    const auto before_move = a.view ();
    const auto first_spawn = std::find_if (before_move.players.begin (), before_move.players.end (), [] (const auto &p) {
        return p.x == Game::first_spawn_x && p.y == Game::first_spawn_y;
    });
    require (first_spawn != before_move.players.end (), "First spawn is missing");
    Client &mover = first_spawn->playerId == a.player_id ? a : b;
    Client &other = mover.player_id == a.player_id ? b : a;
    const auto &source = player_view (before_move, mover.player_id);
    const auto &target = player_view (before_move, other.player_id);
    const auto move_delta = target.x >= source.x ? Game::max_step : -Game::max_step;
    const auto move_sequence = source.lastSequence + 1;
    const MoveRequest accepted_move{move_delta, 0, move_sequence};
    const MoveRequest rejected_move{move_delta, 0, move_sequence + 1};
    const auto move_start = mover.mark ();
    mover.send (accepted_move);
    mover.send (rejected_move);
    const auto move_result = mover.wait<MoveResult> (move_start, [&] (const auto &result) {
        return result.sequence == move_sequence;
    });
    const auto move_rejection = mover.wait<ActionRejected> (move_start);
    require (move_result.sequence == move_sequence, "Accepted move result sequence differs");
    require (move_rejection.reason == Rejection::move_budget && move_rejection.retryAfterMs > 0,
             "Expected movement-budget rejection");
    const auto after_move_a = mover.view ();
    const auto &after_mover = player_view (after_move_a, mover.player_id);
    require (after_mover.x == move_result.x && after_mover.y == move_result.y
                 && after_mover.lastSequence == move_sequence
                 && player_view (after_move_a, other.player_id).hp == target.hp,
             "Movement-budget rejection changed the accepted movement state");
    std::this_thread::sleep_for (std::chrono::milliseconds (move_rejection.retryAfterMs));
    const auto retry_move_start = mover.mark ();
    mover.send (rejected_move);
    const auto retried_move = mover.wait<MoveResult> (retry_move_start, [&] (const auto &result) {
        return result.sequence == rejected_move.sequence;
    });
    require (retried_move.sequence == rejected_move.sequence, "Server movement retry delay was insufficient");
    const auto after_move_b = mover.view ();
    const auto &after_retry_move = player_view (after_move_b, mover.player_id);
    require (after_retry_move.lastSequence == rejected_move.sequence
                 && after_retry_move.x == retried_move.x && after_retry_move.y == retried_move.y,
             "Retried movement result differs from RoomView");

    const auto before_shot = a.view ();
    const auto &shot_target = player_view (before_shot, b.player_id);
    const auto shot_sequence = player_view (before_shot, a.player_id).lastSequence + 1;
    const ShootRequest accepted_shot{b.player_id, shot_sequence};
    const ShootRequest rejected_shot{b.player_id, shot_sequence + 1};
    const auto shot_start = a.mark ();
    a.send (accepted_shot);
    a.send (rejected_shot);
    const auto shot_result = a.wait<ShotResult> (shot_start, [&] (const auto &result) {
        return result.sequence == shot_sequence;
    });
    const auto fire_rejection = a.wait<ActionRejected> (shot_start);
    require (shot_result.sequence == shot_sequence && shot_result.outcome == "hit",
             "Expected accepted hit before fire-too-soon rejection");
    require (fire_rejection.reason == Rejection::fire_too_soon && fire_rejection.retryAfterMs > 0,
             "Expected fire-interval rejection");
    const auto after_shot_a = a.view ();
    require (player_view (after_shot_a, b.player_id).hp == shot_target.hp - Game::shot_damage
                 && player_view (after_shot_a, a.player_id).lastSequence == shot_sequence,
             "Fire-too-soon rejection changed the accepted shot state");
    const auto retry_shot_start = a.mark ();
    std::this_thread::sleep_for (std::chrono::milliseconds (fire_rejection.retryAfterMs));
    a.send (rejected_shot);
    const auto retried_shot = a.wait<ShotResult> (retry_shot_start, [&] (const auto &result) {
        return result.sequence == rejected_shot.sequence;
    });
    require (retried_shot.sequence == rejected_shot.sequence, "Server fire retry delay was insufficient");
    const auto after_shot_b = a.view ();
    require (player_view (after_shot_b, b.player_id).hp == shot_target.hp - 2 * Game::shot_damage
                 && player_view (after_shot_b, a.player_id).lastSequence == rejected_shot.sequence,
             "Retried shot did not update the target state");

    const auto stale_sequence = player_view (mover.view (), mover.player_id).lastSequence;
    reject_action (mover, MoveRequest{0, 0, stale_sequence}, Rejection::stale, &other);
    reject_action (mover, MoveRequest{0, 0, move_sequence}, Rejection::stale, &other);
    reject_action (mover,
                   MoveRequest{Game::max_step + 0.01, 0, stale_sequence + 1},
                   Rejection::step_too_large,
                   &other);
    reject_action (mover,
                   MoveRequest{0, 0, Game::max_safe_integer + 1},
                   Rejection::integer_out_of_range,
                   &other);

    walk (a, b, mover, 1, Positions::lane_y);
    const auto outside_sequence = player_view (mover.view (), mover.player_id).lastSequence + 1;
    reject_action (mover,
                   MoveRequest{-Game::max_step, 0, outside_sequence},
                   Rejection::outside_map,
                   &other);
    const auto before_wall = walk (a, b, mover, Game::wall_x - Game::max_step, Positions::wall_center_y);
    const auto wall_sequence = player_view (before_wall, mover.player_id).lastSequence + 1;
    reject_action (mover,
                   MoveRequest{Game::max_step, 0, wall_sequence},
                   Rejection::move_blocked,
                   &other);
    move_by (a, b, mover, -0.1, 0, Game::max_safe_integer);
    require (player_view (mover.view (), mover.player_id).lastSequence == Game::max_safe_integer,
             "Maximum safe sequence was not accepted");
    reject_action (mover,
                   MoveRequest{0, 0, Game::max_safe_integer + 1},
                   Rejection::integer_out_of_range,
                   &other);
}

inline void new_match (const std::string &endpoint, World &world, const std::string &replacement_id)
{
    auto a = std::make_unique<Client> (endpoint, "new-a-" + unique_id (), world.client_log (2));
    auto b = std::make_unique<Client> (endpoint, "new-b-" + unique_id (), world.client_log (3));
    a->queue ();
    b->queue ();
    const auto first = a->wait<MatchFound> (), second = b->wait<MatchFound> ();
    require (first.roomId == second.roomId, "Replacement matchmaking failed");
    const auto room = a->wait<RoomView> (0, [] (const auto &view) {
        return view.players.size () == Game::players_per_room;
    });
    b->wait<RoomView> (0, [] (const auto &view) {
        return view.players.size () == Game::players_per_room;
    });
    require (room.nodeId == replacement_id, "Replacement node did not accept the new Room");
    a->close ();
    b->close ();
}

inline void scenario (Scenario name, const std::string &endpoint, World &world)
{
    std::vector<std::unique_ptr<Client>> clients;
    const auto create = [&] (std::string id) -> Client & {
        clients.push_back (
          std::make_unique<Client> (endpoint, std::move (id), world.client_log (clients.size ())));
        return *clients.back ();
    };
    std::exception_ptr failure;
    try {
        auto &a = create ("p1-" + unique_id ()), &b = create ("p2-" + unique_id ());
        pair (a, b);
        if (name == Scenario::input) {
            input_scenario (a, b);
            a.healthy ();
            b.healthy ();
        }
        else {
            const auto sa = a.info (), sb = b.info ();
            require (sa.actorId == a.player_id && sb.actorId == b.player_id,
                     "Bound Actor identity mismatch");
            move_by (a, b, a, 0, 0, 1);
            move_by (a, b, b, 0, 0, 1);
            const auto before = hit (a, b, 2, Game::initial_hp - Game::shot_damage);
            if (name == Scenario::hit) {
                reject_action (a, RoomAssignment{before.roomId}, Rejection::packet_not_allowed, &b);
                walk (a, b, a, Game::first_spawn_x, Game::first_spawn_y);
                walk (a, b, b, Positions::target_x, Positions::wall_center_y);
                reject_action (a,
                               ShootRequest{b.player_id, next_sequence (a)},
                               Rejection::blocked,
                               &b);
                walk (a, b, b, Positions::far_x, Positions::far_y);
                reject_action (a,
                               ShootRequest{b.player_id, next_sequence (a)},
                               Rejection::out_of_range,
                               &b);
                reject_action (a,
                               ShootRequest{"missing-player", next_sequence (a)},
                               Rejection::no_target,
                               &b);
                walk (a, b, b, Positions::target_x, Positions::lane_y);
                const auto ma = a.mark (), mb = b.mark ();
                for (auto hp = Game::initial_hp - 2 * Game::shot_damage; hp >= 0;
                     hp -= Game::shot_damage)
                    hit (a, b, next_sequence (a), hp);
                require (a.wait<MatchEnded> (ma).winner == a.player_id
                           && b.wait<MatchEnded> (mb).winner == a.player_id,
                         "Winner mismatch");
            }
            else if (name == Scenario::wall) {
                const auto left = Game::wall_x - Game::max_step;
                const auto right = Game::wall_x + Game::max_step;
                walk (a, b, a, left, Positions::wall_center_y);
                walk (a, b, b, right, Positions::wall_center_y);
                const auto before_wall = a.view ();
                const auto target_before = player_view (before_wall, b.player_id);
                reject_action (a,
                               MoveRequest{Game::max_step, 0, next_sequence (a)},
                               Rejection::move_blocked,
                               &b);
                reject_action (a,
                               ShootRequest{b.player_id, next_sequence (a)},
                               Rejection::blocked,
                               &b);
                const auto after_wall = a.view ();
                same_state (before_wall, after_wall);
                require (player_view (after_wall, b.player_id).hp == target_before.hp,
                         "Wall-blocked shot changed HP");

                walk (a, b, a, Game::first_spawn_x, Game::first_spawn_y);
                walk (a, b, b, Game::second_spawn_x, Game::second_spawn_y);
                const auto before_clear_shot = a.view ();
                const auto expected_hp = player_view (before_clear_shot, b.player_id).hp
                                         - Game::shot_damage;
                const auto clear_shot = hit (a, b, next_sequence (a), expected_hp);
                require (player_view (clear_shot, b.player_id).hp == expected_hp,
                         "Wall behind target blocked a clear shot");
            }
            else if (name == Scenario::reconnect) {
                b.close ();
                auto &again = create (b.player_id);
                again.queue ();
                const auto restored = again.wait<RoomView> (0, [] (const auto &) { return true; });
                const auto identity = again.info ();
                same_actor (sb, identity);
                require (sb.sessionId != identity.sessionId,
                         "Reconnect reused physical Session");
                same_state (before, restored);
                require (restored.phase == Game::playing_phase && !restored.winner
                           && player_view (restored, again.player_id).lastSequence == 1,
                         "Reconnect did not restore the active phase and last sequence");
                move_by (again, a, again, 0, 0, 2);
                hit (a, again, 3, Game::initial_hp - Game::shot_damage * 2);
                hit (a, again, 4, Game::initial_hp - Game::shot_damage * 3);
                const auto finished = hit (a, again, 5, 0);
                require (finished.phase == Game::finished_phase && finished.winner == a.player_id,
                         "Room view did not restore the finished phase and winner");
                auto &finished_reconnect = create (again.player_id);
                finished_reconnect.queue ();
                const auto restored_finished = finished_reconnect.wait<RoomView> (
                  0, [] (const RoomView &view) { return view.phase == Game::finished_phase; });
                const auto finished_identity = finished_reconnect.info ();
                require (finished_identity.sessionId != identity.sessionId
                           && finished_identity.actorId == identity.actorId
                           && finished_identity.objectGeneration == identity.objectGeneration,
                         "Finished reconnect changed Actor identity");
                same_state (finished, restored_finished);
                require (restored_finished.winner == a.player_id
                           && player_view (restored_finished, again.player_id).lastSequence == 2,
                         "Finished reconnect did not restore the winner and last sequence");
            }
            else if (name == Scenario::update) {
                auto &node = world.node (before.nodeId);
                node.command (Control::drain);
                node.wait_line (Control::requested, Limits::relocation);
                node.wait_line (Control::relocated, Limits::relocation);
                node.wait_line (Control::safe, Limits::safe_observation);
                require (sa == a.info () && sb == b.info (), "Drain changed Session identity");
                const auto after = a.view (), other = b.view ();
                same_state (before, after);
                same_state (before, other);
                require (after.nodeId != before.nodeId && other.nodeId == after.nodeId,
                         "Room did not relocate");
                world.replace (before.nodeId);
                new_match (endpoint, world, before.nodeId);
                hit (a, b, 3, Game::initial_hp - Game::shot_damage * 2);
                require (sa == a.info () && sb == b.info (),
                         "Replacement changed Session identity");
            }
            a.healthy ();
            if (name != Scenario::reconnect)
                b.healthy ();
        }
    }
    catch (...) {
        failure = std::current_exception ();
    }
    for (auto &client : clients)
        try {
            client->close ();
        }
        catch (const std::exception &error) {
            std::cerr << "Client cleanup failed: " << error.what () << std::endl;
            if (!failure)
                failure = std::current_exception ();
        }
    if (failure)
        std::rethrow_exception (failure);
}
}
