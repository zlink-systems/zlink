#pragma once
#include "client.hpp"
#include "http.hpp"
#include <future>
namespace MyGame::E2E
{
struct Positions
{
    static constexpr double near_x = 10, target_x = 20, behind_wall_x = 60, far_x = 200,
                            lane_y = 10;
};
inline MatchResponse match_http (const std::string &endpoint, const std::string &player)
{
    net::io_context io;
    boost::beast::tcp_stream stream (io);
    stream.expires_after (Limits::packet);
    http::request<http::string_body> request{http::verb::post, Names::match_path, Http::version};
    request.set (http::field::host, endpoint.substr (Http::scheme.size ()));
    request.set (http::field::content_type, "application/json");
    request.body () = nlohmann::json (MatchRequest{player}).dump ();
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
inline const PlayerView &player (const RoomView &view, const std::string &id)
{
    const auto found = std::find_if (view.players.begin (), view.players.end (),
                                     [&] (const auto &p) { return p.playerId == id; });
    require (found != view.players.end (), "Player absent from RoomView");
    return *found;
}
inline void same_state (const RoomView &a, const RoomView &b)
{
    require (a.roomId == b.roomId && a.players == b.players, "Room state changed");
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
    Pair (const std::string &endpoint, World &world) :
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
        const auto from = a.mark ();
        a.join (room);
        const auto joined = b.join (room);
        require (joined.roomId == room && joined.players.size () == Game::players_per_room,
                 "Second player not admitted");
        a.receive<RoomView> (
          from, [] (const auto &v) { return v.players.size () == Game::players_per_room; });
    }
    void close ()
    {
        a.close ();
        b.close ();
    }
};
inline void move (Pair &pair, Client &mover, double x, int sequence)
{
    const auto ma = pair.a.mark (), mb = pair.b.mark ();
    mover.send (MoveRequest{x, Positions::lane_y, sequence});
    const auto moved = [&] (const auto &v) {
        const auto &p = player (v, mover.player_id);
        return p.x == x && p.y == Positions::lane_y;
    };
    const auto a = pair.a.receive<RoomView> (ma, moved), b = pair.b.receive<RoomView> (mb, moved);
    same_state (a, b);
}
inline RoomView shoot (Pair &pair, int sequence, int hp)
{
    const auto ma = pair.a.mark (), mb = pair.b.mark ();
    pair.a.send (ShootRequest{pair.b.player_id, sequence});
    const auto damaged = [&] (const auto &v) { return player (v, pair.b.player_id).hp == hp; };
    const auto a = pair.a.receive<RoomView> (ma, damaged),
               b = pair.b.receive<RoomView> (mb, damaged);
    same_state (a, b);
    return a;
}
inline void reject (Pair &pair, int sequence, const std::string &reason, const std::string &target)
{
    const auto before = pair.b.view (), from_view = pair.a.view ();
    same_state (before, from_view);
    const auto mark = pair.a.mark ();
    pair.a.send (ShootRequest{target, sequence});
    require (pair.a.receive<ShotRejected> (mark).reason == reason, "Unexpected shot rejection");
    same_state (before, pair.b.view ());
}
inline void lobby (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    pair.close ();
}
inline void hit (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    move (pair, pair.a, Positions::near_x, 1);
    move (pair, pair.b, Positions::target_x, 1);
    shoot (pair, 2, Game::initial_hp - Game::damage);
    move (pair, pair.b, Positions::behind_wall_x, 2);
    reject (pair, 3, Rejection::wall, pair.b.player_id);
    move (pair, pair.b, Positions::far_x, 3);
    reject (pair, 4, Rejection::range, pair.b.player_id);
    reject (pair, 5, Rejection::no_target, "missing");
    move (pair, pair.b, Positions::target_x, 4);
    const auto ma = pair.a.mark (), mb = pair.b.mark ();
    for (int damage = 2; damage <= Game::initial_hp / Game::damage; ++damage)
        shoot (pair, damage + 4, Game::initial_hp - Game::damage * damage);
    require (pair.a.receive<MatchEnded> (ma).winner == pair.a.player_id
               && pair.b.receive<MatchEnded> (mb).winner == pair.a.player_id,
             "Winner mismatch");
    pair.close ();
}
inline void reconnect (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    move (pair, pair.a, Positions::near_x, 1);
    move (pair, pair.b, Positions::target_x, 1);
    const auto before = shoot (pair, 2, Game::initial_hp - Game::damage);
    const auto identity = pair.b.info ();
    pair.b.close ();
    Client rebound (endpoint, pair.b.player_id);
    same_state (before, rebound.join (pair.room));
    const auto after = rebound.info ();
    same_actor (identity, after);
    require (after.sessionId != identity.sessionId, "Reconnect reused physical session");
    rebound.close ();
    pair.a.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    Pair pair (endpoint, world);
    move (pair, pair.a, Positions::near_x, 1);
    move (pair, pair.b, Positions::target_x, 1);
    const auto before = shoot (pair, 2, Game::initial_hp - Game::damage);
    const auto ia = pair.a.info (), ib = pair.b.info ();
    auto &host = world.match_node (before.nodeId);
    host.command (Control::drain);
    host.wait_line (Control::requested, Limits::relocation);
    host.wait_line (Control::relocated, Limits::relocation);
    host.wait_line (Control::safe, Limits::safe_shutdown);
    require (pair.a.info () == ia && pair.b.info () == ib, "Relocation changed Session binding");
    const auto after = pair.a.view ();
    same_state (before, after);
    same_state (before, pair.b.view ());
    require (after.nodeId != before.nodeId, "Room was not relocated");
    world.replace_match (before.nodeId);
    Pair fresh (endpoint, world);
    require (fresh.a.view ().nodeId == before.nodeId,
             "New Room is not on the replacement match node");
    shoot (pair, 3, Game::initial_hp - Game::damage * 2);
    require (pair.a.info () == ia && pair.b.info () == ib, "Replacement changed Session binding");
    fresh.close ();
    pair.close ();
}
}
