#include "server/match/domain.hpp"
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
