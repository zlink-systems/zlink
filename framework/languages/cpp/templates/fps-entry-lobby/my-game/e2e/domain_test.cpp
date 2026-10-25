#include "server/match/domain.hpp"
#include <iostream>

using namespace MyGame;

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
template <class T> bool equal (const T &a, const T &b)
{
    return nlohmann::json (a) == nlohmann::json (b);
}
}

void run_domain_checks ()
{
        auto checkedCount = 0;
        const auto check = [&] (bool condition, const char *name) {
            if (!condition)
                throw std::runtime_error (std::string ("Domain boundary failed: ") + name);
            ++checkedCount;
        };
        const Aabb box{{1, 1, 1}, {2, 2, 2}};
        check (box.intersect ({0, 1, 1}, {1, 0, 0}, 1) == 1, "closed face and exact limit");
        check (box.intersect ({0, 2, 2}, {1, 0, 0}, 3) == 1, "parallel corner tangent");
        check (!box.intersect ({0, 2.001, 2}, {1, 0, 0}, 3), "parallel outside slab");
        check (box.intersect ({1.5, 1.5, 1.5}, {1, 0, 0}, 3) == 0, "inside origin");
        check (box.intersect ({3, 1, 1}, {-1, 0, 0}, 1) == 1, "negative direction");
        check (!box.intersect ({0, 1, 1}, {-1, 0, 0}, 3), "box behind ray");
        check (!box.intersect ({0, 1, 1}, {1, 0, 0}, 0.999), "range cutoff");
        const auto tinyValue = std::numeric_limits<double>::denorm_min ();
        check (length ({tinyValue, 0, 0}) == tinyValue, "subnormal movement length");
        Vec3 huge;
        check (try_normalize ({std::numeric_limits<double>::max (),
                               std::numeric_limits<double>::max (), 0}, huge)
                 && std::abs (length (huge) - 1) < 1e-12,
               "large finite aim normalization");
        Vec3 tiny;
        check (try_normalize ({tinyValue, 0, 0}, tiny) && tiny == Vec3{1, 0, 0},
               "subnormal aim normalization");
        Vec3 invalid;
        check (!try_normalize ({std::numeric_limits<double>::quiet_NaN (), 0, 0}, invalid)
                 && !try_normalize ({std::numeric_limits<double>::infinity (), 0, 0}, invalid)
                 && !try_normalize ({}, invalid),
               "invalid aim");
        for (const auto &spawn : ArenaMap::spawns ())
            check (ArenaMap::bounds ().contains (spawn)
                     && std::none_of (ArenaMap::walls ().begin (), ArenaMap::walls ().end (),
                                      [&] (const auto &wall) { return wall.contains (spawn); }),
                   "spawn outside walls and inside map");

        auto now = 1000.0;
        RoomState room ([&] { return now; });
        room.admit ("a");
        room.admit ("b");
        check (!room.move ("a", {{0, 0, -2}, 1}), "first movement full budget");
        auto before = room.capture ();
        const auto budget = room.move ("a", {{0, 0, -2}, 2});
        check (budget && budget->reason == Rejection::move_budget && budget->retryAfterMs == 200
                 && equal (before, room.capture ()),
               "movement rejection leaves timing and sequence unchanged");
        const auto tinyBudget = room.move ("a", {{tinyValue, 0, 0}, 2});
        check (tinyBudget && tinyBudget->reason == Rejection::move_budget
                 && tinyBudget->retryAfterMs > 0 && equal (before, room.capture ()),
               "nonzero subnormal movement with zero elapsed budget leaves state unchanged");
        now += 100;
        check (!room.move ("a", {{0, 0, -1}, 2}), "partial elapsed movement budget");
        before = room.capture ();
        const auto oversize = room.move ("a", {{0, 0, -3}, 3});
        check (oversize && oversize->reason == Rejection::step_too_large
                 && equal (before, room.capture ()),
               "oversize rejection invariant");
        const auto upward = room.move ("a", {{0, 1, 0}, 3});
        check (upward && upward->reason == Rejection::vertical_move
                 && equal (before, room.capture ()),
               "upward movement rejection invariant");
        const auto downward = room.move ("a", {{0, -1, 0}, 3});
        check (downward && downward->reason == Rejection::vertical_move
                 && equal (before, room.capture ()),
               "downward movement rejection invariant");
        const auto positioned = [&] (Vec3 a, Vec3 b) {
            RoomState result ([&] { return now; });
            result.restore (RoomSnapshot{
              {CombatantSnapshot{"a", a, 100, 0, 0, 0}, CombatantSnapshot{"b", b, 100, 0, 0, 0}},
              MatchPhase::playing, std::nullopt});
            return result;
        };
        auto edge = positioned ({0, 0, 65}, {80, 0, 65});
        const auto edgeBefore = edge.capture ();
        const auto outside = edge.move ("a", {{-1, 0, 0}, 1});
        check (outside && outside->reason == Rejection::outside_map
                 && equal (edgeBefore, edge.capture ()),
               "horizontal map boundary invariant");
        const auto nonfinite = room.move ("a", {{std::numeric_limits<double>::quiet_NaN (), 0, 0}, 3});
        check (nonfinite && nonfinite->reason == Rejection::non_finite
                 && equal (before, room.capture ()),
               "nonfinite movement invariant");
        const auto miss = room.shoot ("a", {{0, 1, 0}, 3});
        check (miss.result && miss.result->outcome == ShotOutcome::miss, "miss is accepted");
        before = room.capture ();
        const auto stale = room.shoot ("a", {{1, 0, 0}, 3});
        check (stale.rejected && stale.rejected->reason == Rejection::stale
                 && equal (before, room.capture ()),
               "shared stale sequence invariant");
        const auto invalidAim = room.shoot ("a", {{}, 4});
        check (invalidAim.rejected && invalidAim.rejected->reason == Rejection::invalid_aim
                 && equal (before, room.capture ()),
               "invalid aim leaves cooldown unchanged");
        const auto fireSoon = room.shoot ("a", {{0, 1, 0}, 4});
        check (fireSoon.rejected && fireSoon.rejected->reason == Rejection::fire_too_soon
                 && fireSoon.rejected->retryAfterMs == Game::fire_interval_ms
                 && equal (before, room.capture ()),
               "fire interval invariant");
        now += Game::fire_interval_ms;
        check (room.shoot ("a", {{0, 1, 0}, 4}).result.has_value (), "exact fire interval");
        check (!room.move ("a", {{0, 0, -2}, 5}), "movement budget capped at max step");

        auto tie = positioned ({48, 0, 22}, {49.5, 0, 22});
        const auto tiedShot = tie.shoot ("a", {{1, 0, 0}, 1});
        check (tiedShot.result && tiedShot.result->outcome == ShotOutcome::blocked_by_wall
                 && tie.view ()[1].hp == 100,
               "equal wall and player distance selects wall");
        auto behind = positioned (ArenaMap::spawns ()[0], ArenaMap::spawns ()[1]);
        const auto behindShot = behind.shoot ("a", {{1, 0, 0}, 1});
        check (behindShot.result && behindShot.result->outcome == ShotOutcome::hit
                 && behind.view ()[1].hp == 75,
               "wall behind target does not block");
        auto exactRange = positioned ({0, 0, 65}, {80.5, 0, 65});
        check (exactRange.shoot ("a", {{1, 0, 0}, 1}).result->outcome == ShotOutcome::hit,
               "player face at exact range");
        auto beyondRange = positioned ({0, 0, 65}, {80.501, 0, 65});
        check (beyondRange.shoot ("a", {{1, 0, 0}, 1}).result->outcome == ShotOutcome::miss,
               "player face beyond range");
        auto crossing = positioned ({48, 0, 22}, ArenaMap::spawns ()[1]);
        before = crossing.capture ();
        const auto crossedWall = crossing.move ("a", {{2, 0, 0}, 1});
        check (crossedWall && crossedWall->reason == Rejection::move_blocked_by_wall
                 && equal (before, crossing.capture ()),
               "movement endpoint inside wall");
        auto touching = positioned ({48, 0, 20}, ArenaMap::spawns ()[1]);
        const auto touchedWall = touching.move ("a", {{1, 0, 0}, 1});
        check (touchedWall && touchedWall->reason == Rejection::move_blocked_by_wall,
               "movement touches wall face");

        now = 1000;
        RoomState source ([&] { return now; });
        source.admit ("a");
        source.admit ("b");
        source.move ("a", {{0, 0, -2}, 1});
        source.shoot ("a", {{0, 1, 0}, 2});
        now += 60;
        const auto snapshot = source.capture ();
        check (snapshot.players[0].fireRemainingMs == 190
                 && snapshot.players[0].moveRemainingMs == 140,
               "snapshot stores remaining milliseconds");
        check (equal (snapshot, source.capture ()), "capture does not mutate live state");
        auto destinationNow = 900000.0;
        RoomState destination ([&] { return destinationNow; });
        destination.restore (nlohmann::json (snapshot).get<RoomSnapshot> ());
        check (equal (source.view (), destination.view ()) && source.phase () == destination.phase (),
               "relocation keeps view under new clock origin");
        const auto relocatedMove = destination.move ("a", {{0, 0, -2}, 3});
        check (relocatedMove && relocatedMove->reason == Rejection::move_budget
                 && relocatedMove->retryAfterMs == 140,
               "relocated movement budget");
        const auto relocatedFire = destination.shoot ("a", {{0, 1, 0}, 3});
        check (relocatedFire.rejected && relocatedFire.rejected->reason == Rejection::fire_too_soon
                 && relocatedFire.rejected->retryAfterMs == 190,
               "relocated fire cooldown");
        destinationNow += 140;
        check (!destination.move ("a", {{0, 0, -2}, 3}), "relocated budget exact boundary");
        destinationNow += 50;
        check (destination.shoot ("a", {{0, 1, 0}, 4}).result.has_value (),
               "relocated cooldown exact boundary");
        auto lethal = positioned (ArenaMap::spawns ()[0], ArenaMap::spawns ()[1]);
        for (auto sequence = 1; sequence <= 4; ++sequence) {
            lethal.shoot ("a", {{1, 0, 0}, sequence});
            now += Game::fire_interval_ms;
        }
        check (lethal.phase () == MatchPhase::finished && lethal.winner () == "a"
                 && lethal.view ()[1].hp == 0,
               "four hits finish match");
        before = lethal.capture ();
        const auto finishedMove = lethal.move ("a", {{0, 0, 1}, 5});
        const auto finishedShot = lethal.shoot ("a", {{1, 0, 0}, 5});
        check (finishedMove && finishedMove->reason == Rejection::finished && finishedShot.rejected
                 && finishedShot.rejected->reason == Rejection::finished
                 && equal (before, lethal.capture ()),
               "finished actions leave state unchanged");
        RoomState finished ([&] { return destinationNow; });
        finished.restore (before);
        check (finished.phase () == MatchPhase::finished && finished.winner () == "a"
                 && equal (finished.view (), lethal.view ()),
               "finished snapshot preserves winner and sequence");
        const auto feetRemainGrounded = [&] (const auto &players) {
            return std::all_of (players.begin (), players.end (), [] (const auto &player) {
                return player.position.y == 0;
            });
        };
        check (feetRemainGrounded (room.view ()) && feetRemainGrounded (destination.view ())
                 && feetRemainGrounded (finished.view ()),
               "feet remain on ground through movement and relocation");
        require (checkedCount == 45, "Domain check count differs from .NET baseline");
        std::cout << "Domain boundaries: " << checkedCount << "/" << checkedCount << " passed"
                  << std::endl;
}

#ifdef MY_GAME_DOMAIN_TEST_MAIN
int main ()
{
    try {
        run_domain_checks ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
#endif
