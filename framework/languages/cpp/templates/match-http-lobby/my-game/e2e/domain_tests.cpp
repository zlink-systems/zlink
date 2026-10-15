#include "server/match/domain.hpp"
#include "server/lobby/http.hpp"
#include <iostream>
#include <limits>
namespace
{
using namespace MyGame;
void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}
void disconnected_waiter ()
{
    NodeConfig config;
    config.HttpEndpoint = "http://127.0.0.1:0";
    config.SessionEndpoint = "tcp://localhost:1";
    HttpLobby lobby (config, [&config] (const std::string &room) {
        return MatchResponse{room, config.SessionEndpoint};
    });
    net::io_context io;
    const auto address = http_address (lobby.endpoint ());
    auto request = [&] (boost::beast::tcp_stream &stream, const std::string &player) {
        stream.connect (address);
        http::request<http::string_body> message{http::verb::post, Names::match_path,
                                                 Http::version};
        message.set (http::field::host, "localhost");
        message.body () = nlohmann::json (MatchRequest{player, Names::default_mode}).dump ();
        message.prepare_payload ();
        http::write (stream, message);
    };
    boost::beast::tcp_stream abandoned (io);
    request (abandoned, "a");
    abandoned.socket ().shutdown (Tcp::socket::shutdown_send);
    char byte;
    boost::system::error_code ended;
    abandoned.expires_after (Limits::packet);
    abandoned.async_read_some (net::buffer (&byte, 1), [&] (auto error, auto) { ended = error; });
    io.run ();
    require (ended == net::error::eof, "Disconnected waiter must be removed before server EOF");
    boost::beast::tcp_stream first (io), second (io);
    request (first, "a");
    request (second, "b");
    auto response = [&] (boost::beast::tcp_stream &stream) {
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> message;
        boost::system::error_code result;
        stream.expires_after (Limits::packet);
        io.restart ();
        http::async_read (stream, buffer, message, [&] (auto error, auto) { result = error; });
        io.run ();
        require (!result && message.result () == http::status::ok, "Rejoined waiter must match");
        return nlohmann::json::parse (message.body ());
    };
    const auto a = response (first), b = response (second);
    require (a == b && a.at ("roomId") == std::string (Names::room_prefix) + "1",
             "Rejoined A and B must share the first prepared room");
}
RoomState pair (double ax, double ay, double bx, double by)
{
    RoomState state;
    require (state.admit ("a") && state.admit ("b"), "Two players must fit");
    require (state.move ("a", {ax, ay, 1}).empty () && state.move ("b", {bx, by, 1}).empty (),
             "Initial movement failed");
    return state;
}
void expect_shot (RoomState state, const std::string &reason)
{
    const auto before = state.view ();
    require (state.shoot ("a", {"b", 2}) == reason, "Shot classification differs");
    const auto after = state.view ();
    require (after[1].hp == before[1].hp - (reason.empty () ? Game::damage : 0),
             "Rejected shot changed HP or hit lost damage");
}
}
int main ()
{
    try {
        disconnected_waiter ();
        RoomState room;
        require (room.admit ("a") && room.admit ("a") && room.admit ("b") && !room.admit ("c"),
                 "Capacity or idempotent admission failed");
        expect_shot (pair (0, Game::wall_max_y + 1, Game::range, Game::wall_max_y + 1), "");
        expect_shot (pair (0, Game::wall_max_y + 1, Game::range + 0.01, Game::wall_max_y + 1),
                     Rejection::range);
        for (const auto y : {Game::wall_min_y, Game::wall_max_y})
            expect_shot (pair (Game::wall_x - 1, y, Game::wall_x + 1, y), Rejection::wall);
        expect_shot (pair (Game::wall_x, Game::wall_min_y - 1, Game::wall_x, Game::wall_min_y + 1),
                     Rejection::wall);
        expect_shot (pair (Game::wall_x, Game::wall_max_y + 1, Game::wall_x, Game::wall_max_y + 2),
                     "");
        auto state = pair (0, Game::wall_max_y + 1, 1, Game::wall_max_y + 1);
        require (state.shoot ("a", {"missing", 2}) == Rejection::no_target,
                 "Missing target accepted");
        require (state.shoot ("a", {"a", 3}) == Rejection::no_target, "Self target accepted");
        require (state.move ("a", {1, 1, 3}) == Rejection::stale, "Stale sequence accepted");
        require (state.move ("a", {std::numeric_limits<double>::quiet_NaN (), 1, 4})
                   == Rejection::invalid_move,
                 "Invalid coordinate accepted");
        for (int sequence = 5; sequence < 5 + Game::initial_hp / Game::damage; ++sequence)
            require (state.shoot ("a", {"b", sequence}).empty (), "Valid shot rejected");
        require (state.winner == "a" && state.view ()[1].hp == 0, "Winner or HP incorrect");
        require (state.move ("a", {2, 2, 9}) == Rejection::finished,
                 "Finished match accepted movement");
        const auto restored = nlohmann::json (state).get<RoomState> ();
        require (nlohmann::json (restored) == nlohmann::json (state),
                 "Relocation application snapshot changed state");
        std::cout << "Domain: admission, geometry, rejection, winner and snapshot passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
