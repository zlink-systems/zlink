#include "server/match/domain.hpp"
#include <iostream>
namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
}
int main ()
{
    try {
        using namespace MyGame;
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
        auto restored_queue = nlohmann::json (queue).get<MatchmakingState> ();
        require (restored_queue.allocate_room () == "room-2"
                   && restored_queue.waiting.front ().second.front ().playerId == "other",
                 "Queue state did not survive capture");
        bool invalid = false;
        try {
            queue.enqueue ({"bad", " \t"});
        }
        catch (const std::invalid_argument &) {
            invalid = true;
        }
        require (invalid, "Blank mode accepted");
        RoomState room;
        require (room.admit ("p1") && room.admit ("p2") && room.admit ("p1")
                   && !room.admit ("third"),
                 "Room admission differs");
        require (!room.move ("p1", {10, 10, 1}) && !room.move ("p2", {20, 10, 1}),
                 "Valid movement rejected");
        require (!room.shoot ("p1", {"p2", 2}) && room.player ("p2").hp == 75,
                 "Unobstructed shot did not hit");
        require (!room.move ("p2", {60, 10, 2}), "Wall placement failed");
        require (room.shoot ("p1", {"p2", 3}) == Rejection::blocked && room.player ("p2").hp == 75,
                 "Wall did not block");
        require (!room.move ("p2", {200, 10, 3})
                   && room.shoot ("p1", {"p2", 4}) == Rejection::out_of_range,
                 "Range rejection differs");
        require (room.shoot ("p1", {"missing", 5}) == Rejection::no_target,
                 "Missing target accepted");
        require (room.move ("p1", {0, 0, 5}) == Rejection::stale, "Stale action accepted");
        auto restored = nlohmann::json (room).get<RoomState> ();
        require (restored.view () == room.view ()
                   && restored.player ("p1").sequence == room.player ("p1").sequence,
                 "Room state did not survive capture");
        require (!restored.move ("p2", {20, 10, 4}), "Restored movement failed");
        for (int shot = 0; shot < 3; ++shot)
            require (!restored.shoot ("p1", {"p2", 6 + shot}), "Finishing shot rejected");
        require (restored.winner == "p1" && restored.player ("p2").hp == 0, "Winner differs");
        require (restored.move ("p1", {0, 0, 9}) == Rejection::finished,
                 "Finished match accepted action");
        RoomState boundary;
        boundary.admit ("p1");
        boundary.admit ("p2");
        boundary.move ("p1", {Game::wall_x, Game::wall_max_y, 1});
        boundary.move ("p2", {Game::wall_x, Game::wall_max_y + 1, 1});
        require (boundary.shoot ("p1", {"p2", 2}) == Rejection::blocked,
                 "Closed wall endpoint did not block");
        boundary.move ("p1", {0, Game::wall_max_y + 1, 3});
        boundary.move ("p2", {Game::shot_range, Game::wall_max_y + 1, 2});
        require (!boundary.shoot ("p1", {"p2", 4}), "Exact range beyond wall rejected");
        require (boundary.move ("p1", {std::numeric_limits<double>::infinity (), 0, 5})
                   == Rejection::invalid_move,
                 "Nonfinite position accepted");
        std::cout
          << "Domain: mode/FIFO/admission/movement/hit/wall/range/sequence/winner/state-roundtrip "
             "passed"
          << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
