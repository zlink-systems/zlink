#include "server/match/domain.hpp"
#include "server/lobby/matchmaker.hpp"
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
void lobby_queue ()
{
    Matchmaker lobby;
    const auto rejected = [&lobby] (const MatchRequest &request) {
        try {
            (void) lobby.enter (request);
        }
        catch (const fx::framework_exception_t &error) {
            return error.kind () == fx::framework_error_kind_t::invalid_operation;
        }
        return false;
    };
    require (rejected ({" ", Names::default_mode}) && rejected ({"a", ""}),
             "Blank player or mode accepted");
    const auto first = lobby.enter ({"a", Names::default_mode});
    require (first.pending && !first.partner, "First request must wait");
    require (rejected ({"a", Names::default_mode}), "Duplicate pending player accepted");
    const auto other = lobby.enter ({"c", "other"});
    require (other.pending != nullptr, "Another mode must wait in its own queue");
    const auto second = lobby.enter ({"b", Names::default_mode});
    require (!second.pending && second.partner == first.pending
               && second.room == std::string (Names::room_prefix) + "1",
             "Second request must pair with the first in the first Room");
    const MatchResponse assignment{second.room, "tcp://127.0.0.1:1"};
    second.partner->complete (fx::result_t<MatchResponse>::success (assignment));
    require (first.pending->task ().result ().value ().roomId == assignment.roomId,
             "Waiting request must receive the paired assignment");
    require (lobby.enter ({"a", Names::default_mode}).pending != nullptr,
             "Paired player must be able to queue again");
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
        lobby_queue ();
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
        std::cout
          << "Domain: lobby queue, admission, geometry, rejection, winner and snapshot passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
