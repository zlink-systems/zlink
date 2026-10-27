#include "server/match/domain.hpp"
#include "server/lobby/http.hpp"
#include <future>
#include <mutex>
#include <set>
#include <string_view>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <tuple>
namespace
{
using namespace std::chrono_literals;
using MyGame::ActionRejected;
using MyGame::Game;
using MyGame::RoomState;
using namespace MyGame;

struct TestClock
{
    RoomState::TimePoint value{};
    void advance (std::chrono::milliseconds duration) { value += duration; }
};

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void rejected (const std::optional<ActionRejected> &result,
               const char *reason,
               std::int64_t retry_after_ms = 0)
{
    require (result.has_value (), "Expected action rejection");
    if (result->reason != reason)
        throw std::runtime_error ("Expected rejection " + std::string (reason) + ", received "
                                  + result->reason);
    if (result->retryAfterMs != retry_after_ms)
        throw std::runtime_error (
          "Expected retry delay " + std::to_string (retry_after_ms) + " ms, received "
          + std::to_string (result->retryAfterMs) + " ms for " + result->reason);
}

nlohmann::json capture (const RoomState &room)
{
    return nlohmann::json (room);
}

RoomState new_room (TestClock &clock)
{
    RoomState room ([&] { return clock.value; });
    require (room.admit ("p1") && room.admit ("p2"), "Room admission failed");
    return room;
}

RoomState positioned (TestClock &clock,
                     double ax,
                     double ay,
                     double bx = 10,
                     double by = 10)
{
    auto room = new_room (clock);
    auto snapshot = capture (room);
    snapshot["players"][0]["x"] = ax;
    snapshot["players"][0]["y"] = ay;
    snapshot["players"][1]["x"] = bx;
    snapshot["players"][1]["y"] = by;
    snapshot.get_to (room);
    return room;
}

void test_sequence_and_move_boundaries ()
{
    TestClock clock;
    auto room = new_room (clock);
    require (!room.move ("p1", {0, 0, 1}), "Sequence baseline was rejected");
    const auto accepted = capture (room);

    rejected (room.move ("p1", {0, 0, 1}), MyGame::Rejection::stale);
    require (capture (room) == accepted, "Duplicate sequence changed Room state");
    rejected (room.shoot ("p1", {"p2", 0}), MyGame::Rejection::stale);
    require (capture (room) == accepted, "Reversed shared sequence changed Room state");
    rejected (room.move ("p1", {0, 0, Game::max_safe_integer + 1}),
              MyGame::Rejection::integer_out_of_range);
    require (capture (room) == accepted, "Out-of-range sequence changed Room state");

    auto overlapping = new_room (clock);
    overlapping.player ("p1").lastSequence = Game::max_safe_integer + 2;
    const auto overlapping_before = capture (overlapping);
    rejected (overlapping.move ("p1", {0, 0, Game::max_safe_integer + 1}),
              MyGame::Rejection::integer_out_of_range);
    require (capture (overlapping) == overlapping_before,
             "Out-of-range sequence did not take precedence over stale sequence");

    auto step_room = new_room (clock);
    require (!step_room.move ("p1", {Game::max_step, 0, 1}), "MaxStep boundary was rejected");
    const auto step_before = capture (step_room);
    rejected (step_room.move ("p1", {Game::max_step + 0.001, 0, 2}),
              MyGame::Rejection::step_too_large);
    require (capture (step_room) == step_before, "Oversized step changed Room state");
}

void test_move_budget ()
{
    TestClock clock;
    auto room = new_room (clock);
    require (!room.move ("p1", {Game::max_step, 0, 1}), "First MaxStep move was rejected");
    clock.advance (100ms);
    require (!room.move ("p1", {1, 0, 2}), "Partial movement budget was rejected");
    const auto first = capture (room);

    rejected (room.move ("p1", {0.1, 0, 3}), MyGame::Rejection::move_budget, 10);
    require (capture (room) == first, "Move-budget rejection changed Room state");
    clock.advance (10ms);
    require (!room.move ("p1", {0.1, 0, 3}), "Exact movement retry boundary was rejected");

    clock.advance (1s);
    require (!room.move ("p1", {Game::max_step, 0, 4}), "Capped MaxStep budget was rejected");
}

void test_wall_boundaries ()
{
    TestClock clock;
    for (const auto y : {Game::wall_min_y, Game::wall_max_y}) {
        auto room = positioned (clock, Game::wall_x - Game::max_step, y);
        const auto before = capture (room);
        rejected (room.move ("p1", {Game::max_step, 0, 1}), MyGame::Rejection::move_blocked);
        require (capture (room) == before, "Wall endpoint contact changed Room state");
    }

    auto crossing = positioned (clock, Game::wall_x - 1, 40);
    const auto crossing_before = capture (crossing);
    rejected (crossing.move ("p1", {Game::max_step, 0, 1}), MyGame::Rejection::move_blocked);
    require (capture (crossing) == crossing_before, "Wall crossing changed Room state");

    auto clear_lane = positioned (clock, Game::wall_x - Game::max_step, Game::wall_min_y - 1);
    require (!clear_lane.move ("p1", {Game::max_step, 0, 1}),
             "Movement below the wall endpoint was rejected");

    auto shot = positioned (clock, 40, Game::wall_min_y, 60, Game::wall_min_y);
    const auto before_shot = capture (shot);
    rejected (shot.shoot ("p1", {"p2", 1}), MyGame::Rejection::blocked);
    require (capture (shot) == before_shot, "Wall endpoint shot changed Room state");
}

void test_map_boundary ()
{
    TestClock clock;
    struct Edge
    {
        double x, y, dx, dy, target_x, target_y;
    };
    const std::array edges{
      Edge{2, 50, -2, 0, 0, 50}, Edge{98, 50, 2, 0, 100, 50},
      Edge{50, 2, 0, -2, 50, 0}, Edge{50, 98, 0, 2, 50, 100}};
    for (const auto &edge : edges) {
        auto room = positioned (clock, edge.x, edge.y);
        require (!room.move ("p1", {edge.dx, edge.dy, 1})
                   && room.player ("p1").x == edge.target_x
                   && room.player ("p1").y == edge.target_y,
                 "Inclusive map boundary differs");
    }
    struct Outside
    {
        double x, y, dx, dy;
    };
    const std::array outside{
      Outside{0, 50, -0.01, 0}, Outside{100, 50, 0.01, 0},
      Outside{50, 0, 0, -0.01}, Outside{50, 100, 0, 0.01}};
    for (const auto &edge : outside) {
        auto room = positioned (clock, edge.x, edge.y);
        const auto before = capture (room);
        rejected (room.move ("p1", {edge.dx, edge.dy, 1}), MyGame::Rejection::outside_map);
        require (capture (room) == before, "Outside-map rejection changed Room state");
    }
}

void test_shot_validation ()
{
    TestClock clock;
    const auto check_rejected_shot = [] (RoomState room, const std::string &target, const char *reason) {
        const auto before = capture (room);
        rejected (room.shoot ("p1", {target, 1}), reason);
        require (capture (room) == before, "Shot validation rejection changed Room state");
    };
    check_rejected_shot (positioned (clock, 40, 40, 60, 40), "p2", MyGame::Rejection::blocked);
    check_rejected_shot (positioned (clock, 10, 10, 95, 10), "p2", MyGame::Rejection::out_of_range);
    check_rejected_shot (new_room (clock), "missing", MyGame::Rejection::no_target);
}

void test_fire_interval ()
{
    TestClock clock;
    auto room = new_room (clock);
    require (!room.shoot ("p1", {"p2", 1}), "First shot was rejected");
    auto before = capture (room);
    rejected (room.shoot ("p1", {"p2", 2}), MyGame::Rejection::fire_too_soon,
              Game::fire_interval_ms);
    require (capture (room) == before, "Immediate fire rejection changed Room state");
    clock.advance (249ms);
    before = capture (room);
    rejected (room.shoot ("p1", {"p2", 2}), MyGame::Rejection::fire_too_soon, 1);
    require (capture (room) == before, "Fire-interval rejection changed Room state");
    clock.advance (1ms);
    require (!room.shoot ("p1", {"p2", 2}), "Shot at the exact fire interval was rejected");
    require (room.player ("p2").hp == Game::initial_hp - 2 * Game::shot_damage,
             "Exact-interval shot did not apply damage");
}

void test_snapshot_clock_origin ()
{
    TestClock source_clock{RoomState::TimePoint{} + 12h};
    auto source = new_room (source_clock);
    require (!source.move ("p1", {Game::max_step, 0, 1}) && !source.shoot ("p1", {"p2", 2}),
             "Snapshot setup action was rejected");
    source_clock.advance (60ms);
    const nlohmann::json snapshot = source;
    require (snapshot.at ("phase").is_string ()
               && snapshot.at ("phase").get<std::string> () == Game::playing_phase
               && snapshot.at ("players").at (0).at ("moveBudgetRemainingMs") == 140
               && snapshot.at ("players").at (0).at ("fireCooldownRemainingMs") == 190,
             "Snapshot did not store remaining milliseconds");
    require (snapshot.dump ().find ("Timestamp") == std::string::npos
               && snapshot == capture (source),
             "Snapshot stored an absolute timestamp or changed during capture");

    TestClock destination_clock{RoomState::TimePoint{} + 5000h};
    RoomState restored ([&] { return destination_clock.value; });
    snapshot.get_to (restored);
    require (restored.view () == source.view (), "Snapshot changed observable Room state");
    const auto before_move_retry = capture (restored);
    rejected (restored.move ("p1", {Game::max_step, 0, 3}), MyGame::Rejection::move_budget, 140);
    require (capture (restored) == before_move_retry, "Restored move rejection changed snapshot");
    rejected (restored.shoot ("p1", {"p2", 3}), MyGame::Rejection::fire_too_soon, 190);
    require (capture (restored) == before_move_retry, "Restored fire rejection changed snapshot");
    destination_clock.advance (140ms);
    require (!restored.move ("p1", {Game::max_step, 0, 3}),
             "Restored movement budget differs at its exact boundary");
    destination_clock.advance (50ms);
    require (!restored.shoot ("p1", {"p2", 4}), "Restored fire interval differs");
}

void test_finished_rejection ()
{
    TestClock clock;
    auto room = new_room (clock);
    for (std::int64_t sequence = 1; sequence <= Game::initial_hp / Game::shot_damage; ++sequence) {
        if (sequence > 1)
            clock.advance (std::chrono::milliseconds (Game::fire_interval_ms));
        require (!room.shoot ("p1", {"p2", sequence}), "Finishing shot was rejected");
    }
    require (room.phase () == Game::finished_phase && room.winner == "p1"
               && room.player ("p2").hp == 0,
             "Winner state differs");
    const auto finished = capture (room);
    require (finished.at ("phase").is_string ()
               && finished.at ("phase").get<std::string> () == Game::finished_phase,
             "Finished phase was not serialized as the exact phase string");
    rejected (room.move ("p1", {0, 0, 5}), MyGame::Rejection::finished);
    rejected (room.shoot ("p1", {"p2", 5}), MyGame::Rejection::finished);
    require (capture (room) == finished && room.winner == "p1",
             "Finished-match rejection changed Room state");
}

void test_nonfinite_input ()
{
    TestClock clock;
    for (const auto value : {std::numeric_limits<double>::quiet_NaN (),
                             std::numeric_limits<double>::infinity (),
                             -std::numeric_limits<double>::infinity ()}) {
        auto room = new_room (clock);
        const auto before = capture (room);
        rejected (room.move ("p1", {value, 0, 1}), MyGame::Rejection::invalid_move);
        require (capture (room) == before, "Nonfinite move changed Room state");
    }
}
int http_checks = 0;
void check_http(bool valid, const char *label) {
    require(valid, label);
    ++http_checks;
}
void verify(bool valid, const char *label) { require(valid, label); }
bool valid_room_id (const std::string &room)
{
    const auto prefix = std::string (Names::room_prefix);
    return room.starts_with (prefix) && room.size () == prefix.size () + 32
           && std::all_of (room.begin () + static_cast<std::ptrdiff_t> (prefix.size ()), room.end (),
                           [] (unsigned char value) {
                               return (value >= '0' && value <= '9')
                                      || (value >= 'a' && value <= 'f');
                           });
}

void send_match_request (boost::beast::tcp_stream &stream, const std::string &endpoint,
                         const std::string &player, std::optional<std::string> mode = std::nullopt)
{
    stream.connect (http_address (endpoint));
    http::request<http::string_body> message{http::verb::post, Names::match_path, Http::version};
    message.set (http::field::host, "localhost");
    message.set (http::field::content_type, "application/json");
    nlohmann::json body{{"playerId", player}};
    if (mode)
        body["mode"] = *mode;
    message.body () = body.dump ();
    message.prepare_payload ();
    http::write (stream, message);
}

nlohmann::json read_match_response (net::io_context &io, boost::beast::tcp_stream &stream)
{
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> message;
    boost::system::error_code result;
    stream.expires_after (Limits::packet);
    io.restart ();
    http::async_read (stream, buffer, message, [&] (auto error, auto) { result = error; });
    io.run ();
    verify (!result && message.result () == http::status::ok,
            "HTTP match response was not successful");
    return nlohmann::json::parse (message.body ());
}

struct ProvisionedRoom
{
    std::string room;
    std::vector<std::string> members;
};

struct ProvisionerCalls
{
    std::mutex mutex;
    std::vector<ProvisionedRoom> calls;

    void record (const std::string &room, const std::vector<std::string> &members)
    {
        std::lock_guard lock (mutex);
        calls.push_back ({room, members});
    }

    std::vector<ProvisionedRoom> snapshot ()
    {
        std::lock_guard lock (mutex);
        return calls;
    }
};

void test_disconnected_waiter ()
{
    NodeConfig config;
    config.HttpEndpoint = "http://127.0.0.1:0";
    config.SessionEndpoint = "tcp://localhost:1";
    HttpLobby lobby (config, [&] (const std::string &room, const std::vector<std::string> &) {
        return MatchResponse{room, config.SessionEndpoint};
    });
    net::io_context io;
    boost::beast::tcp_stream abandoned (io);
    send_match_request (abandoned, lobby.endpoint (), "abandoned");
    abandoned.socket ().shutdown (Tcp::socket::shutdown_send);
    char byte;
    boost::system::error_code ended;
    abandoned.expires_after (Limits::packet);
    abandoned.async_read_some (net::buffer (&byte, 1), [&] (auto error, auto) { ended = error; });
    io.run ();
    if (ended != net::error::eof)
        throw std::runtime_error ("Disconnected waiter was retained: " + ended.message ());
}

void test_http_matching ()
{
    NodeConfig config;
    config.HttpEndpoint = "http://127.0.0.1:0";
    config.SessionEndpoint = "tcp://localhost:1";
    ProvisionerCalls calls;
    HttpLobby lobby (config, [&] (const std::string &room, const std::vector<std::string> &members) {
        calls.record (room, members);
        return MatchResponse{room, config.SessionEndpoint};
    });
    net::io_context io;
    boost::beast::tcp_stream default_a (io), default_b (io);
    send_match_request (default_a, lobby.endpoint (), "default-a");
    send_match_request (default_b, lobby.endpoint (), "default-b", Names::default_mode);
    const auto default_response_a = read_match_response (io, default_a);
    const auto default_response_b = read_match_response (io, default_b);
    check_http (default_response_a == default_response_b,
                "Omitted HTTP mode did not default to duel");

    boost::beast::tcp_stream ranked_a (io), casual_a (io), ranked_b (io), casual_b (io);
    send_match_request (ranked_a, lobby.endpoint (), "ra", "ranked");
    send_match_request (casual_a, lobby.endpoint (), "ca", "casual");
    send_match_request (ranked_b, lobby.endpoint (), "rb", "ranked");
    send_match_request (casual_b, lobby.endpoint (), "cb", "casual");
    const auto ranked_response_a = read_match_response (io, ranked_a);
    const auto ranked_response_b = read_match_response (io, ranked_b);
    const auto casual_response_a = read_match_response (io, casual_a);
    const auto casual_response_b = read_match_response (io, casual_b);
    check_http (ranked_response_a == ranked_response_b && casual_response_a == casual_response_b,
                "Interleaved HTTP modes did not pair matching members");
    const auto ranked_room = ranked_response_a.at ("roomId").get<std::string> ();
    const auto casual_room = casual_response_a.at ("roomId").get<std::string> ();
    check_http (ranked_room != casual_room, "Different HTTP modes shared a Room");

    const auto provisioned = calls.snapshot ();
    const auto has_members = [&] (const std::vector<std::string> &expected) {
        return std::any_of (provisioned.begin (), provisioned.end (), [&] (const auto &call) {
            return std::set<std::string> (call.members.begin (), call.members.end ())
                   == std::set<std::string> (expected.begin (), expected.end ());
        });
    };
    check_http (provisioned.size () == 3
                  && has_members (std::vector<std::string>{"default-a", "default-b"})
                  && has_members (std::vector<std::string>{"ra", "rb"})
                  && has_members (std::vector<std::string>{"ca", "cb"}),
                "Provisioner did not receive each matched roster");

    ProvisionerCalls restarted_calls;
    HttpLobby restarted_lobby (
      config, [&] (const std::string &room, const std::vector<std::string> &members) {
          restarted_calls.record (room, members);
          return MatchResponse{room, config.SessionEndpoint};
      });
    net::io_context restarted_io;
    boost::beast::tcp_stream new_a (restarted_io), new_b (restarted_io);
    send_match_request (new_a, restarted_lobby.endpoint (), "new-a");
    send_match_request (new_b, restarted_lobby.endpoint (), "new-b");
    const auto new_response_a = read_match_response (restarted_io, new_a);
    const auto new_response_b = read_match_response (restarted_io, new_b);
    const auto new_room = new_response_a.at ("roomId").get<std::string> ();
    check_http (new_response_a == new_response_b && new_room != ranked_room
                  && new_room != casual_room,
                "Restarted lobby reused a Room ID");

    const auto restarted = restarted_calls.snapshot ();
    const bool valid_ids = std::all_of (provisioned.begin (), provisioned.end (), [] (const auto &call) {
                                return valid_room_id (call.room);
                            })
                           && std::all_of (restarted.begin (), restarted.end (),
                                           [] (const auto &call) {
                                               return valid_room_id (call.room);
                                           });
    check_http (valid_ids, "HTTP Room ID was not a prefix and lowercase UUID");
}

void test_preparation_waits_for_room_prepared ()
{
    NodeConfig config;
    config.HttpEndpoint = "http://127.0.0.1:0";
    config.SessionEndpoint = "tcp://localhost:1";
    std::promise<void> started;
    auto started_future = started.get_future ();
    std::promise<void> release;
    const auto prepared = release.get_future ().share ();
    HttpLobby lobby (config, [&] (const std::string &room, const std::vector<std::string> &) {
        started.set_value ();
        prepared.wait ();
        return MatchResponse{room, config.SessionEndpoint};
    });
    net::io_context io_a, io_b;
    boost::beast::tcp_stream a (io_a), b (io_b);
    send_match_request (a, lobby.endpoint (), "held-a");
    send_match_request (b, lobby.endpoint (), "held-b");
    const auto invoked = started_future.wait_for (Limits::packet) == std::future_status::ready;
    if (!invoked) {
        release.set_value ();
        check_http (false, "Room preparation was not invoked");
    }

    auto response_a = std::async (std::launch::async, [&] { return read_match_response (io_a, a); });
    auto response_b = std::async (std::launch::async, [&] { return read_match_response (io_b, b); });
    const bool waiting = response_a.wait_for (std::chrono::milliseconds::zero ())
                           == std::future_status::timeout
                         && response_b.wait_for (std::chrono::milliseconds::zero ())
                              == std::future_status::timeout;
    release.set_value ();
    const auto result_a = response_a.get ();
    const auto result_b = response_b.get ();
    check_http (invoked && waiting, "HTTP assignments did not wait for RoomPrepared");
    check_http (result_a == result_b, "RoomPrepared did not release both HTTP responses");
}

void test_roster ()
{
    RoomRoster unprepared;
    check_http (!unprepared.can_admit ("a") && !unprepared.can_admit ("outsider"),
                "Unprepared Room admitted a player");

    bool threw = false;
    try {
        RoomRoster invalid;
        invalid.prepare ("room-x", PrepareRoom{"other", {"a", "b"}});
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    check_http (threw, "Preparation accepted a different MatchId");

    threw = false;
    try {
        RoomRoster invalid;
        invalid.prepare ("room-x", PrepareRoom{"room-x", {"a", "a"}});
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    check_http (threw, "Preparation accepted duplicate members");

    threw = false;
    try {
        RoomRoster invalid;
        invalid.prepare ("room-x", PrepareRoom{"room-x", {"a"}});
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    check_http (threw, "Preparation accepted an incomplete roster");

    RoomRoster roster;
    auto members = std::vector<std::string>{"a", "b"};
    auto request = PrepareRoom{"room-x", members};
    const auto same_request = PrepareRoom{"room-x", {"a", "b"}};
    const auto prepared = roster.prepare ("room-x", request);
    check_http (prepared.matchId == "room-x", "Room preparation returned another MatchId");
    request.members[0] = "outsider";
    check_http (roster.can_admit ("a") && roster.can_admit ("b")
                  && !roster.can_admit ("outsider"),
                "RoomRoster did not own a copy of the member list");
    const auto snapshot = roster.capture ();
    const bool idempotent = roster.prepare ("room-x", same_request).matchId == "room-x"
                            && roster.prepare ("room-x", PrepareRoom{"room-x", {"b", "a"}})
                                 .matchId
                                 == "room-x"
                            && roster.capture ().matchId == snapshot.matchId
                            && roster.capture ().members == snapshot.members
                            && snapshot.members == std::vector<std::string>{"a", "b"};
    check_http (idempotent, "Same preparation was not idempotent");

    threw = false;
    try {
        roster.prepare ("room-x", PrepareRoom{"room-x", {"a", "c"}});
    }
    catch (const std::logic_error &) {
        threw = true;
    }
    check_http (threw && roster.capture ().matchId == snapshot.matchId
                  && roster.capture ().members == snapshot.members,
                "Different roster replaced prepared Room state");

    threw = false;
    try {
        RoomRoster invalid;
        invalid.prepare ("room-x", PrepareRoom{"room-x", {"a", " "}});
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    verify (threw, "Preparation accepted an empty member");

    RoomRoster restored;
    restored.restore (snapshot);
    threw = false;
    try {
        restored.prepare ("room-x", PrepareRoom{"room-x", {"a", "outsider"}});
    }
    catch (const std::logic_error &) {
        threw = true;
    }
    check_http (threw, "Relocated roster was replaced");

    threw = false;
    try {
        RoomRoster invalid;
        invalid.restore (RosterSnapshot{std::nullopt, {"a"}});
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    check_http (threw, "Unprepared snapshot retained roster members");
}

void test_http_snapshot () {
    TestClock clock;
    auto source = new_room(clock);
    RoomRoster roster;
    roster.prepare("room", PrepareRoom{"room", {"p1", "p2"}});
    const nlohmann::json envelope = HttpRoomSnapshot{source, roster.capture()};
    const auto restored = envelope.get<HttpRoomSnapshot>();
    RoomRoster restored_roster;
    restored_roster.restore(restored.Roster);
    check_http(envelope.contains("Game") && envelope.contains("Roster")
      && restored.Game.view() == source.view() && restored.Game.phase() == source.phase()
      && restored_roster.can_admit("p1") && restored_roster.can_admit("p2")
      && !restored_roster.can_admit("outsider"), "HTTP relocation retains game and roster");
}
}
int main () {
    try {
        test_sequence_and_move_boundaries();
        test_move_budget();
        test_wall_boundaries();
        test_map_boundary();
        test_fire_interval();
        test_shot_validation();
        test_snapshot_clock_origin();
        test_finished_rejection();
        test_nonfinite_input();
        std::cout << "Game domain checks: 9/9 categories passed\n";
        test_disconnected_waiter();
        test_http_matching();
        test_preparation_waits_for_room_prepared();
        test_roster();
        test_http_snapshot();
        require(http_checks == 19, "HTTP check count differs from 19");
        std::cout << "HTTP domain boundaries: " << http_checks << "/19 passed\n";
        return 0;
    } catch(const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
