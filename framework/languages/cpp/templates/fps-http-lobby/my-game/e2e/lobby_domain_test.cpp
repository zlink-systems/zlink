#include "server/lobby/matchmaker.hpp"
#include "server/match/roster.hpp"
#include <iostream>
#include <set>

namespace
{
template <class T> bool equal (const T &a, const T &b)
{
    return nlohmann::json (a) == nlohmann::json (b);
}

template <class F> bool rejects (F &&action)
{
    try {
        action ();
        return false;
    }
    catch (const std::invalid_argument &) {
        return true;
    }
    catch (const std::logic_error &) {
        return true;
    }
}

struct Result
{
    std::optional<MyGame::MatchResponse> response;
    std::exception_ptr error;
};

using namespace MyGame;

std::optional<Matchmaker::Pairing> enqueue (Matchmaker &matchmaker,
                                            const MatchRequest &request,
                                            const void *token,
                                            Result &result)
{
    return matchmaker.enqueue (
      request, token,
      [&result] (const MatchResponse &response) { result.response = response; },
      [&result] (std::exception_ptr error) { result.error = error; }, [] {});
}

MatchResponse prepare (Matchmaker::Pairing &pairing,
                       std::vector<std::pair<std::string, std::vector<std::string>>> &calls)
{
    calls.emplace_back (pairing.roomId, pairing.members);
    return {pairing.roomId, "ws://127.0.0.1:1"};
}

bool valid_room_id (const std::string &roomId)
{
    if (!roomId.starts_with (Names::room_prefix))
        return false;
    const auto uuid = roomId.substr (std::string (Names::room_prefix).size ());
    return uuid.size () == 32
           && std::all_of (uuid.begin (), uuid.end (), [] (unsigned char c) {
                  return std::isxdigit (c) && !std::isupper (c);
              });
}
}

void run_lobby_domain_checks ()
{
    int checked = 0;
    const auto check = [&] (bool condition, const char *name) {
        if (!condition)
            throw std::runtime_error (std::string ("HTTP domain boundary failed: ") + name);
        ++checked;
    };

    RoomRoster roster;
    check (!roster.can_admit ("a") && !roster.can_admit ("outsider"),
           "unprepared Room rejects admission");
    check (rejects ([&] { roster.prepare ("room", PrepareRoom{"other", {"a", "b"}}); }),
           "wrong match identity");
    check (rejects ([&] { roster.prepare ("room", PrepareRoom{"room", {"a", "a"}}); }),
           "duplicate roster member");
    check (rejects ([&] { roster.prepare ("room", PrepareRoom{"room", {"a"}}); }),
           "incomplete roster");
    std::vector<std::string> members{"a", "b"};
    check (roster.prepare ("room", PrepareRoom{"room", members}).matchId == "room",
           "preparation acknowledges identity");
    members[0] = "outsider";
    check (roster.can_admit ("a") && roster.can_admit ("b") && !roster.can_admit ("outsider"),
           "roster owns member copy");
    const auto rosterSnapshot = roster.capture ();
    roster.prepare ("room", PrepareRoom{"room", {"b", "a"}});
    check (equal (rosterSnapshot, roster.capture ()), "same preparation is idempotent");
    check (rejects ([&] {
               roster.prepare ("room", PrepareRoom{"room", {"a", "outsider"}});
           })
             && equal (rosterSnapshot, roster.capture ()),
           "prepared roster cannot be replaced");

    double now = 1000;
    RoomState game ([&] { return now; });
    game.admit ("a");
    game.admit ("b");
    game.move ("a", {{0, 0, -1}, 1});
    game.shoot ("a", {{4, 0, 1}, 2});
    const HttpRoomSnapshot snapshot{game.capture (), roster.capture ()};
    const auto relocated = nlohmann::json (snapshot).get<HttpRoomSnapshot> ();
    RoomRoster restoredRoster;
    restoredRoster.restore (relocated.roster);
    RoomState restoredGame ([] { return 900000.0; });
    restoredGame.restore (relocated.game);
    check (restoredRoster.can_admit ("a") && restoredRoster.can_admit ("b")
             && !restoredRoster.can_admit ("outsider") && equal (game.view (), restoredGame.view ()),
           "relocation envelope retains roster, HP, position and sequence");
    check (rejects ([&] {
               restoredRoster.prepare ("room", PrepareRoom{"room", {"a", "outsider"}});
           }),
           "relocated roster cannot be replaced");
    check (rejects ([&] { RoomRoster{}.restore (RosterSnapshot{std::nullopt, {"a"}}); }),
           "unprepared snapshot with members");
    check (nlohmann::json::parse (R"({"playerId":"default"})")
                 .get<MatchRequest> ()
                 .mode
               == Names::default_mode,
           "omitted HTTP mode defaults to duel");

    std::vector<std::pair<std::string, std::vector<std::string>>> calls;
    Matchmaker firstProcess;
    Result rankedA, casualA, rankedB, casualB;
    int rankedAToken{}, casualAToken{}, rankedBToken{}, casualBToken{};
    auto rankedAQueue = enqueue (firstProcess, {"ra", "ranked"}, &rankedAToken, rankedA);
    auto casualAQueue = enqueue (firstProcess, {"ca", "casual"}, &casualAToken, casualA);
    auto rankedPair = enqueue (firstProcess, {"rb", "ranked"}, &rankedBToken, rankedB);
    if (!rankedPair)
        throw std::runtime_error ("Ranked players did not pair.");
    const auto rankedResponse = prepare (*rankedPair, calls);
    rankedPair->prepared (rankedResponse);
    check (!casualA.response && rankedA.response && rankedB.response
             && rankedA.response == rankedB.response
             && std::set<std::string> (rankedPair->members.begin (), rankedPair->members.end ())
                  == std::set<std::string>{"ra", "rb"},
           "interleaved modes pair only matching members");
    auto casualPair = enqueue (firstProcess, {"cb", "casual"}, &casualBToken, casualB);
    if (!casualPair)
        throw std::runtime_error ("Casual players did not pair.");
    const auto casualResponse = prepare (*casualPair, calls);
    casualPair->prepared (casualResponse);
    check (casualA.response && casualB.response && casualA.response == casualB.response
             && casualA.response->roomId != rankedA.response->roomId,
           "different modes have distinct Rooms");
    check (calls.size () == 2
             && std::set<std::string> (calls[0].second.begin (), calls[0].second.end ())
                  == std::set<std::string>{"ra", "rb"}
             && std::set<std::string> (calls[1].second.begin (), calls[1].second.end ())
                  == std::set<std::string>{"ca", "cb"},
           "provisioner receives each matched roster");

    Matchmaker restarted;
    Result newA, newB;
    int newAToken{}, newBToken{};
    auto newAQueue = enqueue (restarted, {"new-a"}, &newAToken, newA);
    auto newPair = enqueue (restarted, {"new-b"}, &newBToken, newB);
    if (!newPair)
        throw std::runtime_error ("Restarted lobby did not pair.");
    const auto newResponse = prepare (*newPair, calls);
    newPair->prepared (newResponse);
    check (newA.response && newA.response == newB.response
             && newA.response->roomId != rankedA.response->roomId
             && newA.response->roomId != casualA.response->roomId,
           "restart cannot reuse counter-based Room identity");
    check (std::all_of (calls.begin (), calls.end (), [] (const auto &call) {
               return valid_room_id (call.first);
           }),
           "Room ID contains a generated GUID");

    Matchmaker waiting;
    Result heldA, heldB;
    int heldAToken{}, heldBToken{};
    auto heldAQueue = enqueue (waiting, {"held-a"}, &heldAToken, heldA);
    auto heldPair = enqueue (waiting, {"held-b"}, &heldBToken, heldB);
    if (!heldPair)
        throw std::runtime_error ("Waiting players did not pair.");
    calls.emplace_back (heldPair->roomId, heldPair->members);
    check (!heldA.response && !heldB.response && calls.size () == 4,
           "HTTP assignments wait for Room preparation completion");
    heldPair->prepared ({heldPair->roomId, "ws://127.0.0.1:1"});
    check (heldA.response && heldA.response == heldB.response,
           "prepared assignment releases both HTTP responses");

    if (checked != 19)
        throw std::runtime_error ("HTTP domain check count differs from .NET baseline");
    std::cout << "HTTP domain boundaries: " << checked << "/" << checked << " passed" << std::endl;
}
