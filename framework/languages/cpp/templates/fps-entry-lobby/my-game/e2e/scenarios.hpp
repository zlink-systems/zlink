#pragma once
#include "client.hpp"
#include "server/match/domain.hpp"
#include <limits>
#include <string_view>
#include <thread>

namespace MyGame::E2E
{
inline const PlayerView &player (const RoomView &view, const Client &client)
{
    const auto found = std::find_if (view.players.begin (), view.players.end (), [&] (const auto &p) {
        return p.playerId == client.player_id;
    });
    require (found != view.players.end (), "Player missing from Room view");
    return *found;
}

inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Reconnect changed Actor identity.");
}

inline void same_state (RoomView before, const RoomView &after)
{
    before.nodeId = after.nodeId;
    require (before == after, "Room state changed.");
}

inline RoomView view (Client &client) { return client.view (); }
inline std::int64_t next (const RoomView &current, const Client &client)
{
    return player (current, client).lastSequence + 1;
}

inline RoomView pair (Client &a, Client &b)
{
    a.queue ();
    b.queue ();
    const auto foundA = a.wait<MatchFound> (), foundB = b.wait<MatchFound> ();
    require (foundA.roomId == foundB.roomId, "Same-mode players entered different rooms.");
    const auto ready = [] (const RoomView &room) {
        return room.players.size () == Game::players_per_room;
    };
    const auto roomA = a.wait<RoomView> (0, ready), roomB = b.wait<RoomView> (0, ready);
    require (roomA == roomB && roomA.roomId == foundA.roomId
               && roomA.phase == MatchPhase::playing
               && std::all_of (roomA.players.begin (), roomA.players.end (), [] (const auto &p) {
                      return p.hp == Game::initial_hp && p.lastSequence == 0;
                  }),
             "Initial room state differs.");
    return roomA;
}

inline void move (Client &client, const Vec3 &delta)
{
    const auto before = view (client);
    const MoveRequest request{delta, next (before, client)};
    while (true) {
        const auto response = client.action (request);
        if (const auto *retry = std::get_if<ActionRejected> (&response);
            retry && retry->reason == Rejection::move_budget && retry->retryAfterMs > 0) {
            same_state (before, view (client));
            std::this_thread::sleep_for (std::chrono::milliseconds (retry->retryAfterMs));
            continue;
        }
        const auto *moved = std::get_if<MoveResult> (&response);
        require (moved && moved->sequence == request.sequence
                   && moved->position == player (before, client).position + delta,
                 "Movement was not accepted.");
        const auto after = view (client);
        require (player (after, client).lastSequence == request.sequence,
                 "Movement lost sequence.");
        require (std::all_of (after.players.begin (), after.players.end (), [] (const auto &p) {
                    return p.position.y == 0;
                }),
                 "Movement changed feet height.");
        return;
    }
}

inline void walk (Client &client, const Vec3 &destination)
{
    auto position = player (view (client), client).position;
    while (position != destination) {
        const auto remaining = destination - position;
        // Admission order chooses spawn positions; cross X in the clear lane before changing Z.
        const auto delta = remaining.x != 0
                             ? Vec3{std::clamp (remaining.x, -Game::max_step, Game::max_step), 0, 0}
                         : remaining.y != 0
                             ? Vec3{0, std::clamp (remaining.y, -Game::max_step, Game::max_step), 0}
                             : Vec3{0, 0, std::clamp (remaining.z, -Game::max_step, Game::max_step)};
        move (client, delta);
        position = player (view (client), client).position;
    }
}

inline Vec3 aim (Client &shooter, const Client &target)
{
    const auto current = view (shooter);
    return player (current, target).position - player (current, shooter).position;
}

inline void shoot (Client &shooter, const Vec3 &direction, std::string_view expected)
{
    const auto before = view (shooter);
    const ShootRequest request{direction, next (before, shooter)};
    while (true) {
        const auto response = shooter.action (request);
        if (const auto *retry = std::get_if<ShotRejected> (&response);
            retry && retry->reason == Rejection::fire_too_soon && retry->retryAfterMs > 0) {
            same_state (before, view (shooter));
            std::this_thread::sleep_for (std::chrono::milliseconds (retry->retryAfterMs));
            continue;
        }
        const auto *shot = std::get_if<ShotResult> (&response);
        require (shot && shot->outcome == expected && shot->sequence == request.sequence,
                 "Shot result differs.");
        return;
    }
}

inline void hit (Client &shooter, Client &target, int expectedHp)
{
    shoot (shooter, aim (shooter, target), ShotOutcome::hit);
    const auto first = view (shooter), second = view (target);
    require (first == second && player (first, target).hp == expectedHp,
             "Hit views or damage differ.");
}

inline void ended (Client &a, Client &b)
{
    require (a.wait<MatchEnded> ().winner == a.player_id
               && b.wait<MatchEnded> ().winner == a.player_id,
             "Wrong match winner.");
    const auto current = view (a);
    require (current.phase == MatchPhase::finished && current.winner == a.player_id,
             "Finished state differs.");
}

template <class T> void rejected (Client &client, const T &request, const char *reason)
{
    const auto before = view (client);
    const auto response = client.action (request);
    bool matches = false;
    if (const auto *moveRejected = std::get_if<ActionRejected> (&response))
        matches = moveRejected->retryAfterMs == 0 && moveRejected->reason == reason;
    if (const auto *shotRejected = std::get_if<ShotRejected> (&response))
        matches = shotRejected->retryAfterMs == 0 && shotRejected->reason == reason;
    require (matches, std::string ("Expected rejection ") + reason + ".");
    same_state (before, view (client));
    std::cout << "input rejected: " << reason << "; state unchanged" << std::endl;
}

inline void matchmaking (const std::string &endpoint, World &world)
{
    const auto suffix = unique_id ();
    Client rankedA (endpoint, "ranked-a-" + suffix, world.client_log (0));
    Client casualA (endpoint, "casual-a-" + suffix, world.client_log (1));
    Client rankedB (endpoint, "ranked-b-" + suffix, world.client_log (2));
    Client casualB (endpoint, "casual-b-" + suffix, world.client_log (3));
    // Interleaving the queues exposes matchers that ignore the requested mode.
    rankedA.queue ("ranked");
    casualA.queue ("casual");
    rankedB.queue ("ranked");
    casualB.queue ("casual");
    const auto rankedRoom = rankedA.wait<MatchFound> (), casualRoom = casualA.wait<MatchFound> ();
    require (rankedRoom.roomId == rankedB.wait<MatchFound> ().roomId
               && casualRoom.roomId == casualB.wait<MatchFound> ().roomId
               && rankedRoom.roomId != casualRoom.roomId,
             "Different modes were not separated.");
    const auto ranked = rankedA.wait<RoomView> (0, [] (const auto &v) {
        return v.players.size () == Game::players_per_room;
    });
    const auto casual = casualA.wait<RoomView> (0, [] (const auto &v) {
        return v.players.size () == Game::players_per_room;
    });
    const auto contains = [] (const RoomView &room, const std::string &first,
                              const std::string &second) {
        return room.players.size () == 2
               && std::any_of (room.players.begin (), room.players.end (), [&] (const auto &p) {
                      return p.playerId == first;
                  })
               && std::any_of (room.players.begin (), room.players.end (), [&] (const auto &p) {
                      return p.playerId == second;
                  });
    };
    require (contains (ranked, rankedA.player_id, rankedB.player_id)
               && contains (casual, casualA.player_id, casualB.player_id)
               && ranked.phase == MatchPhase::playing && casual.phase == MatchPhase::playing
               && std::all_of (ranked.players.begin (), ranked.players.end (), [] (const auto &p) {
                      return p.hp == Game::initial_hp && p.lastSequence == 0;
                  })
               && std::all_of (casual.players.begin (), casual.players.end (), [] (const auto &p) {
                      return p.hp == Game::initial_hp && p.lastSequence == 0;
                  }),
             "Matched Room members or initial state differ.");
    const auto internal = rankedA.action (RoomAssignment{casualRoom.roomId});
    const auto *rejection = std::get_if<ActionRejected> (&internal);
    require (rejection && rejection->reason == Rejection::packet_not_allowed,
             "Session accepted an internal RoomAssignment.");
    same_state (ranked, view (rankedA));
    rankedA.healthy ();
    rankedB.healthy ();
    casualA.healthy ();
    casualB.healthy ();
    rankedA.close ();
    rankedB.close ();
    casualA.close ();
    casualB.close ();
}

inline void new_match (const std::string &endpoint, World &world, const std::string &replacementId)
{
    const auto suffix = unique_id ();
    Client a (endpoint, "new-a-" + suffix, world.client_log (4));
    Client b (endpoint, "new-b-" + suffix, world.client_log (5));
    const auto room = pair (a, b);
    require (room.nodeId == replacementId, "Replacement node did not accept the new Room.");
    a.close ();
    b.close ();
}

inline void input (Client &a, Client &b)
{
    // Verify the max-step pair and server-provided retry delay.
    const auto beforeBudget = view (a);
    const auto moveSequence = next (beforeBudget, a);
    const MoveRequest acceptedMoveRequest{{0, 0, -2}, moveSequence};
    const MoveRequest moveRequest{{0, 0, -2}, moveSequence + 1};
    const auto moveStart = a.mark ();
    a.send (acceptedMoveRequest);
    a.send (moveRequest);
    const auto acceptedMove = a.wait<MoveResult> (moveStart);
    require (acceptedMove.sequence == moveSequence,
             "Expected accepted movement before movement-budget rejection.");
    const auto budgetResponse = a.wait<ActionRejected> (moveStart);
    require (budgetResponse.reason == Rejection::move_budget && budgetResponse.retryAfterMs > 0,
             "Expected movement-budget rejection.");
    const auto afterBudget = view (a);
    const auto beforeBudgetPlayer = player (beforeBudget, a);
    const auto afterBudgetPlayer = player (afterBudget, a);
    require (afterBudgetPlayer.position.x == acceptedMove.position.x
                 && afterBudgetPlayer.position.y == acceptedMove.position.y
                 && afterBudgetPlayer.position.z == acceptedMove.position.z
                 && afterBudgetPlayer.lastSequence == moveSequence,
             "Movement-budget rejection changed the accepted movement position or sequence.");
    require (beforeBudgetPlayer.hp == afterBudgetPlayer.hp
                 && player (beforeBudget, b).hp == player (afterBudget, b).hp,
             "Movement-budget rejection changed player HP.");
    std::this_thread::sleep_for (std::chrono::milliseconds (budgetResponse.retryAfterMs));
    const auto retriedMoveResponse = a.action (moveRequest);
    const auto *retriedMove = std::get_if<MoveResult> (&retriedMoveResponse);
    require (retriedMove && retriedMove->sequence == moveRequest.sequence,
             "Server movement retry delay was insufficient.");

    auto current = view (a);
    auto sequence = player (current, a).lastSequence;
    rejected (a, MoveRequest{{0, 0, 1}, sequence}, Rejection::stale);
    rejected (a, ShootRequest{{1, 0, 0}, sequence - 1}, Rejection::stale);
    rejected (a, MoveRequest{{Game::max_step + 1, 0, 0}, sequence + 1}, Rejection::step_too_large);
    rejected (a, MoveRequest{{0, 1, 0}, sequence + 1}, Rejection::vertical_move);
    rejected (a, MoveRequest{{0, -1, 0}, sequence + 1}, Rejection::vertical_move);
    walk (a, {player (current, a).position.x, 0, 0});
    sequence = player (view (a), a).lastSequence;
    rejected (a, MoveRequest{{0, 0, -1}, sequence + 1}, Rejection::outside_map);
    rejected (a, ShootRequest{{}, sequence + 1}, Rejection::invalid_aim);
    walk (a, {48, 0, 22});
    sequence = next (view (a), a);
    rejected (a, MoveRequest{{2, 0, 0}, sequence}, Rejection::move_blocked_by_wall);
    const auto beforeFire = view (a);
    const auto shotSequence = next (beforeFire, a);
    const ShootRequest acceptedShotRequest{{0, 1, 0}, shotSequence};
    const ShootRequest shotRequest{{0, 1, 0}, shotSequence + 1};
    const auto shotStart = a.mark ();
    a.send (acceptedShotRequest);
    a.send (shotRequest);
    const auto acceptedShot = a.wait<ShotResult> (shotStart);
    require (acceptedShot.sequence == shotSequence && acceptedShot.outcome == ShotOutcome::miss,
             "Expected accepted miss before fire-too-soon rejection.");
    const auto fireResponse = a.wait<ShotRejected> (shotStart);
    require (fireResponse.reason == Rejection::fire_too_soon && fireResponse.retryAfterMs > 0,
             "Expected fire-interval rejection.");
    const auto afterFire = view (a);
    const auto beforeFirePlayer = player (beforeFire, a);
    const auto afterFirePlayer = player (afterFire, a);
    require (afterFirePlayer.position.x == beforeFirePlayer.position.x
                 && afterFirePlayer.position.y == beforeFirePlayer.position.y
                 && afterFirePlayer.position.z == beforeFirePlayer.position.z
                 && afterFirePlayer.lastSequence == shotSequence,
             "Fire-too-soon rejection changed the accepted shot position or sequence.");
    require (beforeFirePlayer.hp == afterFirePlayer.hp
                 && player (beforeFire, b).hp == player (afterFire, b).hp,
             "Fire-too-soon rejection changed player HP.");
    std::this_thread::sleep_for (std::chrono::milliseconds (fireResponse.retryAfterMs));
    const auto retriedShotResponse = a.action (shotRequest);
    const auto *retriedShot = std::get_if<ShotResult> (&retriedShotResponse);
    require (retriedShot && retriedShot->sequence == shotRequest.sequence
                 && retriedShot->outcome == ShotOutcome::miss,
             "Server fire retry delay was insufficient.");
    require (player (view (b), b).hp == Game::initial_hp, "Input checks damaged target.");
    std::cout << "input: move-budget and fire-too-soon retry delays accepted unchanged requests"
              << std::endl;
}

inline void scenario (Scenario name, const std::string &endpoint, World &world)
{
    if (name == Scenario::matchmaking) {
        matchmaking (endpoint, world);
        return;
    }
    const auto suffix = unique_id ();
    Client a (endpoint, "p1-" + suffix, world.client_log (0));
    Client b (endpoint, "p2-" + suffix, world.client_log (1));
    const auto initial = pair (a, b);
    const auto sessionA = a.info (), sessionB = b.info ();
    require (sessionA.actorId == a.player_id && sessionB.actorId == b.player_id,
             "Bound Actor identity mismatch");

    switch (name) {
        case Scenario::matchmaking:
            break;
        case Scenario::hit: {
            hit (a, b, Game::initial_hp - Game::shot_damage);
            shoot (a, {0, 1, 0}, ShotOutcome::miss);
            require (player (view (b), b).hp == 75, "An upward miss caused damage.");
            walk (a, {5, 0, 18});
            walk (a, {5, 0, 65});
            walk (b, {52, 0, 65});
            walk (b, {95, 0, 65});
            shoot (a, aim (a, b), ShotOutcome::miss);
            require (player (view (b), b).hp == 75, "An out-of-range ray caused damage.");
            walk (a, {20, 0, 65});
            for (auto hp = 50; hp >= 0; hp -= Game::shot_damage)
                hit (a, b, hp);
            ended (a, b);
            auto current = view (a);
            rejected (a, MoveRequest{{0, 0, 1}, next (current, a)}, Rejection::finished);
            current = view (a);
            rejected (a, ShootRequest{{1, 0, 0}, next (current, a)}, Rejection::finished);
            break;
        }
        case Scenario::wall: {
            walk (a, {48, 0, 22});
            walk (b, {52, 0, 22});
            const auto before = view (b);
            shoot (a, aim (a, b), ShotOutcome::blocked_by_wall);
            require (player (view (b), b).hp == player (before, b).hp,
                     "A wall-blocked ray caused damage.");
            walk (a, ArenaMap::spawns ()[0]);
            walk (b, ArenaMap::spawns ()[1]);
            hit (a, b, Game::initial_hp - Game::shot_damage);
            std::cout << "wall: intervening wall blocks; wall behind target permits hit" << std::endl;
            break;
        }
        case Scenario::input:
            input (a, b);
            break;
        case Scenario::reconnect: {
            walk (b, {54, 0, 18});
            hit (a, b, 75);
            const auto beforeReconnect = view (b);
            b.close ();
            Client again (endpoint, b.player_id, world.client_log (2));
            again.queue ();
            same_state (beforeReconnect, again.wait<RoomView> ());
            const auto identity = again.info ();
            same_actor (sessionB, identity);
            require (sessionB.sessionId != identity.sessionId,
                     "Reconnect reused physical Session.");
            move (again, {0, 0, 1});
            for (auto hp = 50; hp >= 0; hp -= Game::shot_damage)
                hit (a, again, hp);
            ended (a, again);
            {
                Client finished (endpoint, b.player_id, world.client_log (3));
                finished.queue ();
                const auto restored = finished.wait<RoomView> ();
                require (restored.phase == MatchPhase::finished && restored.winner == a.player_id
                           && player (restored, finished).hp == 0,
                         "Finished reconnect lost winner or HP.");
                finished.healthy ();
                finished.close ();
            }
            again.healthy ();
            again.close ();
            break;
        }
        case Scenario::update: {
            move (a, {0, 0, -1});
            hit (a, b, 75);
            const auto before = view (a);
            auto &node = world.node (before.nodeId);
            node.command (Control::drain);
            node.wait_line (Control::requested, Limits::relocation);
            node.wait_line (Control::relocated, Limits::relocation);
            node.wait_line (Control::safe, Limits::safe_observation);
            require (sessionA == a.info () && sessionB == b.info (),
                     "Update changed session binding.");
            same_state (before, a.view_if ([&] (const RoomView &current) {
                return current.nodeId != before.nodeId;
            }));
            same_state (before, b.view_if ([&] (const RoomView &current) {
                return current.nodeId != before.nodeId;
            }));
            world.replace (before.nodeId);
            new_match (endpoint, world, before.nodeId);
            hit (a, b, 50);
            require (sessionA == a.info () && sessionB == b.info (),
                     "Replacement changed session binding.");
            break;
        }
    }
    a.healthy ();
    if (name != Scenario::reconnect)
        b.healthy ();
    a.close ();
    b.close ();
}
}
