#pragma once
#include "client.hpp"
#include "server/shared/http.hpp"
#include <future>
#include <thread>

namespace MyGame::E2E
{
inline MatchResponse match_http (const std::string &endpoint, const std::string &player)
{
    net::io_context io;
    boost::beast::tcp_stream stream (io);
    stream.expires_after (Limits::packet);
    http::request<http::string_body> request{http::verb::post, Names::match_path, Http::version};
    request.set (http::field::host, endpoint.substr (Http::scheme.size ()));
    request.set (http::field::content_type, "application/json");
    request.body () = nlohmann::json{{"playerId", player}}.dump ();
    request.prepare_payload ();
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code failure;
    stream.async_connect (http_address (endpoint), [&] (auto error) {
        if (error) {
            failure = error;
            return;
        }
        http::async_write (stream, request, [&] (auto error, auto) {
            if (error) {
                failure = error;
                return;
            }
            http::async_read (stream, buffer, response,
                              [&] (auto error, auto) { failure = error; });
        });
    });
    io.run ();
    require (!failure, "HTTP match transport failed: " + failure.message ());
    require (response.result () == http::status::ok, "HTTP match failed: " + response.body ());
    return nlohmann::json::parse (response.body ()).get<MatchResponse> ();
}

inline const PlayerView &player_view (const RoomView &view, const std::string &id)
{
    const auto found = std::find_if (view.players.begin (), view.players.end (),
                                     [&] (const auto &p) { return p.playerId == id; });
    require (found != view.players.end (), "Player absent from RoomView");
    return *found;
}

inline void same_state (const RoomView &a, const RoomView &b)
{
    require (a.roomId == b.roomId && a.players == b.players && a.phase == b.phase
               && a.winner == b.winner,
             "Room state changed");
}

inline void same_actor (const SessionInfo &a, const SessionInfo &b)
{
    require (a.actorId == b.actorId && a.objectGeneration == b.objectGeneration,
             "Actor identity changed");
}

struct Pair
{
    Client a, b;
    std::string room;
    Pair (const std::string &endpoint, World &world, bool join_members = true) :
        a (endpoint, "p1-" + unique_id ()), b (endpoint, "p2-" + unique_id ())
    {
        auto first = std::async (std::launch::async,
                                 [&] { return match_http (world.lobby_endpoint (), a.player_id); });
        const auto second = match_http (world.lobby_endpoint (), b.player_id);
        const auto assignment = first.get ();
        require (assignment.roomId == second.roomId && assignment.sessionEndpoint == endpoint
                   && second.sessionEndpoint == endpoint,
                 "HTTP assignments differ");
        room = assignment.roomId;
        if (join_members)
            join_pair ();
    }
    void join_pair ()
    {
        a.join (room);
        b.join (room);
        const auto ready_a = a.view ();
        const auto ready_b = b.view ();
        same_state (ready_a, ready_b);
        require (ready_a.players.size () == Game::players_per_room
                   && ready_a.phase == Game::playing_phase && !ready_a.winner,
                 "Prepared Room did not admit both players");
        require (player_view (ready_a, a.player_id).lastSequence == 0
                   && player_view (ready_a, b.player_id).lastSequence == 0,
                 "Initial action sequences differ");
        const auto &first_player = player_view (ready_a, a.player_id);
        const auto &second_player = player_view (ready_a, b.player_id);
        require (first_player.x == Game::first_spawn_x && first_player.y == Game::first_spawn_y
                   && first_player.x < Game::wall_x
                   && second_player.x == Game::second_spawn_x
                   && second_player.y == Game::second_spawn_y && second_player.x > Game::wall_x,
                 "Server-selected spawns differ or are on the wrong wall sides");
        require (std::hypot (first_player.x - second_player.x,
                             first_player.y - second_player.y)
                   <= Game::shot_range,
                 "Players spawned outside shot range");
    }
    void close ()
    {
        a.close ();
        b.close ();
    }
};

struct Positions
{
    static constexpr double lane_y = Game::first_spawn_y,
                            wall_center_y = (Game::wall_min_y + Game::wall_max_y) / 2;
};

inline RoomView move_by (Client &a,
                         Client &b,
                         Client &mover,
                         double dx,
                         double dy,
                         std::int64_t sequence,
                         std::optional<RoomView> prior = std::nullopt)
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
        auto result = mover.receive_action (from, moved);
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
        const auto other_view = other.receive<RoomView> (other_from, moved);
        require (mover_view == other_view, "Movement views differ");
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

inline ActionRejected reject_action (Client &player,
                                     const auto &request,
                                     const char *reason,
                                     Client *peer = nullptr)
{
    const auto before = player.view ();
    const auto from = player.mark ();
    player.send (request);
    const auto response = player.receive<ActionRejected> (from);
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
    const auto move_result = mover.receive<MoveResult> (move_start, [&] (const auto &result) {
        return result.sequence == move_sequence;
    });
    const auto move_rejection = mover.receive<ActionRejected> (move_start);
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
    const auto retried_move = mover.receive<MoveResult> (retry_move_start, [&] (const auto &result) {
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
    const auto shot_result = a.receive<ShotResult> (shot_start, [&] (const auto &result) {
        return result.sequence == shot_sequence;
    });
    const auto fire_rejection = a.receive<ActionRejected> (shot_start);
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
    const auto retried_shot = a.receive<ShotResult> (retry_shot_start, [&] (const auto &result) {
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


inline ActionRejected reject_move (Pair &pair, Client &mover, double dx, double dy,
                                   std::int64_t sequence, const std::string &reason)
{
    Client &observer = &mover == &pair.a ? pair.b : pair.a;
    const auto before = observer.view ();
    const auto from = mover.mark ();
    mover.send (MoveRequest{dx, dy, sequence});
    const auto result = mover.receive_action (from, [] (const auto &) { return true; });
    require (std::holds_alternative<ActionRejected> (result), "Rejected movement was accepted");
    const auto rejected = std::get<ActionRejected> (result);
    require (rejected.reason == reason, "Wrong movement rejection");
    require (reason == Rejection::move_budget ? rejected.retryAfterMs > 0
                                               : rejected.retryAfterMs == 0,
             "Movement rejection returned an invalid RetryAfterMs");
    same_state (before, observer.view ());
    return rejected;
}

inline RoomView move (Pair &pair, Client &mover, double dx, double dy, std::int64_t sequence)
{
    return move_by(pair.a, pair.b, mover, dx, dy, sequence);
}
inline RoomView walk (Pair &pair, Client &mover, double target_x, double target_y)
{
    return walk(pair.a, pair.b, mover, target_x, target_y);
}

inline ActionRejected reject_shot (Pair &pair, std::int64_t sequence,
                                   const std::string &target, const std::string &reason)
{
    const auto before = pair.b.view ();
    const auto from = pair.a.mark ();
    pair.a.send (ShootRequest{target, sequence});
    const auto result = pair.a.receive_action (from, [] (const auto &) { return true; });
    require (std::holds_alternative<ActionRejected> (result), "Rejected shot was accepted");
    const auto rejected = std::get<ActionRejected> (result);
    require (rejected.reason == reason, "Wrong shot rejection");
    require (rejected.retryAfterMs == 0, "Non-cooldown shot rejection returned RetryAfterMs");
    same_state (before, pair.b.view ());
    return rejected;
}

inline RoomView hit (Pair &pair, std::int64_t sequence, int hp)
{
    const auto damaged = [&] (const RoomView &view) {
        return player_view (view, pair.b.player_id).hp == hp
               && player_view (view, pair.a.player_id).lastSequence == sequence;
    };
    while (true) {
        const auto before = pair.b.view ();
        const auto actor_from = pair.a.mark ();
        const auto observer_from = pair.b.mark ();
        pair.a.send (ShootRequest{pair.b.player_id, sequence});
        const auto result = pair.a.receive_action (actor_from, damaged);
        if (const auto *rejected = std::get_if<ActionRejected> (&result)) {
            same_state (before, pair.b.view ());
            if (rejected->reason != Rejection::fire_too_soon)
                throw std::runtime_error ("Shot rejected: " + rejected->reason);
            require (rejected->retryAfterMs > 0,
                     "Fire cooldown rejection omitted RetryAfterMs");
            std::this_thread::sleep_for (std::chrono::milliseconds (rejected->retryAfterMs));
            continue;
        }
        const auto &accepted = std::get<RoomView> (result);
        const auto observed = pair.b.receive_action (observer_from, damaged);
        require (std::holds_alternative<RoomView> (observed), "Observer received a rejection");
        same_state (accepted, std::get<RoomView> (observed));
        return accepted;
    }
}

inline void lobby (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world, false);
    const auto room = pair.room;

    Client outsider (endpoint, "outsider-" + unique_id ());
    const auto outsider_from = outsider.mark ();
    outsider.send (JoinRoomRequest{outsider.player_id, room});
    const auto admission = outsider.receive_action (outsider_from, [] (const auto &) { return true; });
    require (std::holds_alternative<ActionRejected> (admission)
               && std::get<ActionRejected> (admission)
                      == ActionRejected{Rejection::admission_rejected, 0},
             "Unlisted player was admitted to the prepared Room");
    outsider.close ();
    pair.join_pair ();

    const auto before = pair.a.view ();
    const auto packet_from = pair.a.mark ();
    pair.a.send (PrepareRoom{room, {pair.a.player_id, pair.b.player_id}});
    const auto packet = pair.a.receive_action (packet_from, [] (const auto &) { return true; });
    require (std::holds_alternative<ActionRejected> (packet)
               && std::get<ActionRejected> (packet)
                      == ActionRejected{Rejection::packet_not_allowed, 0},
             "Client packet allowlist accepted PrepareRoom");
    same_state (before, pair.b.view ());
    pair.close ();

    world.restart_lobby ();
    Pair after_restart (endpoint, world);
    require (after_restart.room != room, "Lobby restart reused a Room ID");
    after_restart.close ();
}

inline void hit (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    reject_shot (pair, 1, "missing", Rejection::no_target);
    constexpr std::int64_t travel_steps = 48;
    const auto dx = 29.0 / travel_steps;
    const auto dy = 90.0 / travel_steps;
    RoomView far_view;
    for (std::int64_t sequence = 1; sequence <= travel_steps; ++sequence)
        far_view = move (pair, pair.b, dx, dy, sequence);
    const auto &far_player = player_view (far_view, pair.b.player_id);
    require (std::hypot (far_player.x - (Game::second_spawn_x + 29.0),
                         far_player.y - Game::map_max_y)
                   < 1e-6,
             "Target did not reach the out-of-range position");
    reject_shot (pair, 1, pair.b.player_id, Rejection::out_of_range);
    RoomView returned_view;
    for (std::int64_t sequence = travel_steps + 1; sequence <= travel_steps * 2; ++sequence)
        returned_view = move (pair, pair.b, -dx, -dy, sequence);
    const auto &returned_player = player_view (returned_view, pair.b.player_id);
    require (std::hypot (returned_player.x - Game::second_spawn_x,
                         returned_player.y - Game::second_spawn_y)
               < 1e-6,
             "Target did not return to its spawn position");
    for (std::int64_t sequence = 1; sequence <= Game::initial_hp / Game::shot_damage; ++sequence) {
        const auto final_a = pair.a.mark ();
        const auto final_b = pair.b.mark ();
        const auto finished = hit (pair, sequence,
                                   Game::initial_hp - Game::shot_damage * static_cast<int> (sequence));
        if (sequence == Game::initial_hp / Game::shot_damage) {
            require (finished.phase == Game::finished_phase && finished.winner == pair.a.player_id,
                     "RoomView omitted the finished phase or winner");
            require (player_view (finished, pair.a.player_id).lastSequence == sequence,
                     "RoomView omitted the accepted action sequence");
            require (pair.a.receive<MatchEnded> (final_a).winner == pair.a.player_id
                       && pair.b.receive<MatchEnded> (final_b).winner == pair.a.player_id,
                     "Winner event mismatch");
        }
    }
    pair.close ();
}

inline void wall (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    const auto at_wall = walk (pair, pair.a, Game::wall_x - Game::max_step, 24);
    const auto &shooter_before = player_view (at_wall, pair.a.player_id);
    const auto &target_before = player_view (at_wall, pair.b.player_id);
    const auto sequence = shooter_before.lastSequence + 1;
    require (std::hypot (shooter_before.x - (Game::wall_x - Game::max_step),
                         shooter_before.y - 24)
                   < 1e-6
               && target_before.x == Game::second_spawn_x
               && target_before.y == Game::second_spawn_y,
             "Players did not reach opposite sides of the wall");
    reject_move (pair, pair.a, Game::max_step, 0, sequence, Rejection::move_blocked);
    reject_shot (pair, sequence, pair.b.player_id, Rejection::blocked);
    const auto after_wall = pair.b.view ();
    same_state (at_wall, after_wall);
    require (player_view (after_wall, pair.b.player_id).hp == target_before.hp
               && player_view (after_wall, pair.a.player_id).lastSequence
                    == shooter_before.lastSequence,
             "Wall rejection changed HP or accepted sequence");

    const auto at_spawn = walk (pair, pair.a, Game::first_spawn_x, Game::first_spawn_y);
    const auto clear = hit (pair, player_view (at_spawn, pair.a.player_id).lastSequence + 1,
                            player_view (at_spawn, pair.b.player_id).hp - Game::shot_damage);
    require (player_view (clear, pair.b.player_id).hp
                 == player_view (at_spawn, pair.b.player_id).hp - Game::shot_damage
               && player_view (clear, pair.a.player_id).lastSequence
                    == player_view (at_spawn, pair.a.player_id).lastSequence + 1,
             "Clear shot outside the wall segment did not apply");
    pair.close ();
}

inline void input (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    input_scenario (pair.a, pair.b);
    pair.a.healthy ();
    pair.b.healthy ();
    pair.close ();
}

inline void reconnect (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    const SessionInfo old_identity = pair.b.info ();
    RoomView before;
    for (std::int64_t sequence = 1; sequence <= Game::initial_hp / Game::shot_damage; ++sequence)
        before = hit (pair, sequence,
                      Game::initial_hp - Game::shot_damage * static_cast<int> (sequence));
    pair.b.close ();
    Client rebound (endpoint, pair.b.player_id);
    const auto restored = rebound.join (pair.room);
    same_state (before, restored);
    const auto identity = rebound.info ();
    same_actor (old_identity, identity);
    require (identity.sessionId != old_identity.sessionId, "Reconnect reused physical session");
    require (restored.phase == Game::finished_phase && restored.winner == pair.a.player_id
               && player_view (restored, pair.a.player_id).lastSequence == Game::initial_hp / Game::shot_damage,
             "Reconnect did not restore sequence, phase and winner");
    rebound.close ();
    pair.a.close ();
}

inline void update (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    const auto moved = move (pair, pair.a, 1, 0, 1);
    const auto before = hit (pair, 2, Game::initial_hp - Game::shot_damage);
    require (player_view (before, pair.a.player_id).lastSequence == 2,
             "Update snapshot omitted the accepted action sequence");
    require (moved.phase == Game::playing_phase && !moved.winner,
             "Update input changed match phase");

    const auto identity_a = pair.a.info ();
    const auto identity_b = pair.b.info ();
    auto &host = world.match_node (before.nodeId);
    host.command (Control::drain);
    host.wait_line (Control::requested, Limits::relocation);
    host.wait_line (Control::relocated, Limits::relocation);
    host.wait_line (Control::safe, Limits::safe_shutdown);
    require (pair.a.info () == identity_a && pair.b.info () == identity_b,
             "Update changed Session binding");
    const auto after_a = pair.a.view ();
    const auto after_b = pair.b.view ();
    same_state (before, after_a);
    same_state (before, after_b);
    require (after_a.nodeId != before.nodeId, "Room was not relocated");

    world.replace_match (before.nodeId);
    Pair fresh (endpoint, world);
    require (fresh.room != pair.room, "Matchmaker reused a Room ID");
    require (fresh.a.view ().nodeId == before.nodeId,
             "New Room is not on the replacement match node");
    const auto updated = hit (pair, 3, Game::initial_hp - Game::shot_damage * 2);
    require (updated.phase == Game::playing_phase && !updated.winner,
             "Replacement changed match phase or winner");
    require (pair.a.info () == identity_a && pair.b.info () == identity_b,
             "Replacement changed Session binding");
    fresh.close ();
    pair.close ();
}
}
