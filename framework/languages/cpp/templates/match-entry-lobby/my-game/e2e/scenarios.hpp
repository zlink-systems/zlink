#pragma once
#include "client.hpp"
namespace MyGame::E2E
{
struct Positions
{
    static constexpr double near_x = 10, target_x = 20, behind_wall_x = 60, far_x = 200,
                            lane_y = 10;
};
inline void same_actor (const SessionInfo &a, const SessionInfo &b)
{
    require (a.actorId == b.actorId && a.objectGeneration == b.objectGeneration
               && a.objectGeneration != 0,
             "Actor identity changed");
}
inline void same_state (const RoomView &a, const RoomView &b)
{
    require (a.roomId == b.roomId && a.players == b.players, "Room state changed");
}
inline RoomView pair (Client &a, Client &b)
{
    a.queue ();
    b.queue ();
    const auto fa = a.wait<MatchFound> (), fb = b.wait<MatchFound> ();
    require (fa.roomId == fb.roomId, "Matched different Rooms");
    const auto ready = [] (const RoomView &v) {
        return v.players.size () == Game::players_per_room;
    };
    const auto va = a.wait<RoomView> (0, ready), vb = b.wait<RoomView> (0, ready);
    require (va == vb && va.roomId == fa.roomId, "Matched Room views differ");
    for (const auto &p : va.players)
        require (p.hp == Game::initial_hp, "Initial HP differs");
    return va;
}
inline void move (Client &a, Client &b, Client &mover, double x, std::int64_t sequence)
{
    const auto ma = a.mark (), mb = b.mark ();
    mover.send (MoveRequest{x, Positions::lane_y, sequence});
    const auto moved = [&] (const RoomView &v) {
        return std::any_of (v.players.begin (), v.players.end (), [&] (const auto &p) {
            return p.playerId == mover.player_id && p.x == x && p.y == Positions::lane_y;
        });
    };
    require (a.wait<RoomView> (ma, moved) == b.wait<RoomView> (mb, moved), "Movement views differ");
}
inline RoomView damage (Client &a, Client &b, std::int64_t sequence, int hp)
{
    const auto ma = a.mark (), mb = b.mark ();
    a.send (ShootRequest{b.player_id, sequence});
    const auto damaged = [&] (const RoomView &v) {
        return std::any_of (v.players.begin (), v.players.end (), [&] (const auto &p) {
            return p.playerId == b.player_id && p.hp == hp;
        });
    };
    const auto va = a.wait<RoomView> (ma, damaged), vb = b.wait<RoomView> (mb, damaged);
    require (va == vb, "Shot views differ");
    return va;
}
inline void reject_shot (
  Client &a, Client &b, std::int64_t sequence, const char *reason, const std::string &target)
{
    const auto before = b.view ();
    const auto mark = a.mark ();
    a.send (ShootRequest{target, sequence});
    require (a.wait<ShotRejected> (mark).reason == reason, "Shot rejection differs");
    same_state (before, b.view ());
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
        const auto sa = a.info (), sb = b.info ();
        require (sa.actorId == a.player_id && sb.actorId == b.player_id,
                 "Bound Actor identity mismatch");
        move (a, b, a, Positions::near_x, 1);
        move (a, b, b, Positions::target_x, 1);
        const auto before = damage (a, b, 2, Game::initial_hp - Game::shot_damage);
        switch (name) {
            case Scenario::hit: {
                move (a, b, b, Positions::behind_wall_x, 2);
                reject_shot (a, b, 3, Rejection::blocked, b.player_id);
                move (a, b, b, Positions::far_x, 3);
                reject_shot (a, b, 4, Rejection::out_of_range, b.player_id);
                reject_shot (a, b, 5, Rejection::no_target, "missing-player");
                move (a, b, b, Positions::target_x, 4);
                const auto ma = a.mark (), mb = b.mark ();
                for (int shot = 2; shot <= Game::initial_hp / Game::shot_damage; ++shot)
                    damage (a, b, shot + 4, Game::initial_hp - Game::shot_damage * shot);
                require (a.wait<MatchEnded> (ma).winner == a.player_id
                           && b.wait<MatchEnded> (mb).winner == a.player_id,
                         "Winner mismatch");
                break;
            }
            case Scenario::reconnect: {
                b.close ();
                auto &again = create (b.player_id);
                again.queue ();
                const auto restored = again.wait<RoomView> ();
                const auto identity = again.info ();
                same_actor (sb, identity);
                require (sb.sessionId != identity.sessionId, "Reconnect reused physical Session");
                same_state (before, restored);
                damage (a, again, 3, Game::initial_hp - 2 * Game::shot_damage);
                again.healthy ();
                break;
            }
            case Scenario::update: {
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
                auto &fresh_a = create ("new-a-" + unique_id ()),
                     &fresh_b = create ("new-b-" + unique_id ());
                require (pair (fresh_a, fresh_b).nodeId == before.nodeId,
                         "Replacement did not accept a new Room");
                damage (a, b, 3, Game::initial_hp - 2 * Game::shot_damage);
                require (sa == a.info () && sb == b.info (),
                         "Replacement changed Session identity");
                break;
            }
        }
        a.healthy ();
        if (name != Scenario::reconnect)
            b.healthy ();
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
