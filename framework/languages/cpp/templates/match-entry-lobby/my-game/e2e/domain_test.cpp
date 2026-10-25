#include "server/match/domain.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <type_traits>
namespace
{
using namespace std::chrono_literals;
using MyGame::ActionRejected;
using MyGame::Game;
using MyGame::MatchmakingState;
using MyGame::RoomState;

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

void require_wire_fields (const nlohmann::json &value,
                          std::initializer_list<const char *> fields,
                          const char *message)
{
    require (value.is_object () && value.size () == fields.size (), message);
    for (const auto *field : fields)
        require (value.contains (field), message);
}

template <class T> void require_wire_round_trip (const T &value, const char *message)
{
    const auto wire = nlohmann::json (value);
    require (wire.get<T> () == value, message);
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

void test_queue ()
{
    MatchmakingState queue;
    queue.enqueue ({"other", "squad"});
    queue.enqueue ({"p1"});
    queue.enqueue ({"p1", "squad"});
    require (queue.pair ().empty (), "Duplicate player entered another mode");
    queue.enqueue ({"p2"});
    const auto pair = queue.pair ();
    require (pair.size () == Game::players_per_room && pair.front ().playerId == "p1",
             "Mode/FIFO pairing differs");
    require (queue.allocate_room () == "room-1", "Room sequence differs");
    queue.remove (pair);
    auto restored = nlohmann::json (queue).get<MatchmakingState> ();
    require (restored.allocate_room () == "room-2"
               && restored.waiting.front ().second.front ().playerId == "other",
             "Queue state did not survive capture");
    bool invalid = false;
    try {
        queue.enqueue ({"bad", " \t"});
    }
    catch (const std::invalid_argument &) {
        invalid = true;
    }
    require (invalid, "Blank mode accepted");
}

void test_wire_contract_names_and_phase ()
{
    static_assert (std::is_same_v<decltype (ActionRejected::retryAfterMs), std::int32_t>);

    const MyGame::PlayerView player{"p1", 1, 2, 100, 9};
    const auto player_json = nlohmann::json (player);
    require_wire_fields (player_json, {"playerId", "x", "y", "hp", "lastSequence"},
                         "PlayerView did not use camelCase fields");

    const auto rejected_json = nlohmann::json (
      ActionRejected{MyGame::Rejection::move_budget, 40});
    require_wire_fields (rejected_json, {"reason", "retryAfterMs"},
                         "ActionRejected did not use camelCase fields");
    require (rejected_json.at ("reason").is_string ()
               && rejected_json.at ("retryAfterMs").is_number_integer (),
             "ActionRejected wire values have the wrong JSON types");
    require_wire_round_trip (ActionRejected{MyGame::Rejection::move_budget, 40},
                             "ActionRejected camelCase round-trip failed");

    for (const auto &view : {
           MyGame::RoomView{"room-1", "matchA", Game::playing_phase, std::nullopt, {player}},
           MyGame::RoomView{"room-1", "matchA", Game::finished_phase, std::string ("p1"),
                            {player}}}) {
        const auto view_json = nlohmann::json (view);
        require_wire_fields (view_json, {"roomId", "nodeId", "phase", "winner", "players"},
                             "RoomView did not use camelCase fields");
        require (view_json.at ("phase").is_string ()
                   && view_json.at ("phase").get<std::string> () == view.phase,
                 "RoomView phase was not serialized as the exact phase string");
        require (view_json.at ("players").at (0).contains ("lastSequence"),
                 "RoomView player omitted lastSequence");
        require_wire_round_trip (view, "RoomView camelCase round-trip failed");
    }
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
}

int main ()
{
    try {
        test_queue ();
        test_wire_contract_names_and_phase ();
        test_sequence_and_move_boundaries ();
        test_move_budget ();
        test_wall_boundaries ();
        test_map_boundary ();
        test_fire_interval ();
        test_snapshot_clock_origin ();
        test_finished_rejection ();
        test_nonfinite_input ();
        std::cout << "Domain: sequence/map/wall/move-budget/fire/snapshot/finished checks passed"
                  << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
