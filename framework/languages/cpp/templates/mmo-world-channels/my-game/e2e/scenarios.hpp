#pragma once
#include "client.hpp"
#include "server/shared/http.hpp"
namespace MyGame::E2E
{
struct Test
{
    static constexpr std::int64_t earned_gold = 10;
    static constexpr int second_channel = 2;
};
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline void world_list (const World &world)
{
    const auto worlds = fetch_worlds (world.accounts ());
    require (worlds.size () == 2, "Accounts must list two worlds");
    std::set<std::string> ids;
    for (const auto &listed : worlds) {
        require (ids.insert (listed.worldId).second && !listed.name.empty (),
                 "Duplicate or unnamed world");
        require (listed.sessionEndpoint == world.endpoint (listed.worldId),
                 "Accounts endpoint mismatch");
    }
}
inline void scenario (Scenario name, World &world)
{
    world_list (world);
    const auto id = "player-" + unique_id ();
    std::vector<std::unique_ptr<Client>> clients;
    auto create = [&] (const std::string &world_id, const std::string &player_id) -> Client & {
        clients.push_back (std::make_unique<Client> (world.endpoint (world_id), player_id));
        return *clients.back ();
    };
    std::exception_ptr failure;
    try {
        auto &a = create ("A", id);
        require (a.enter () == PlayerView{"A", Names::first_channel, "channelA1", 0},
                 "Initial world A state mismatch");
        const auto before = a.earn (Test::earned_gold, Test::earned_gold);
        require (before.gold == Test::earned_gold, "Gold was not earned");
        if (name == Scenario::worlds) {
            auto &b = create ("B", id);
            require (b.enter () == PlayerView{"B", Names::first_channel, "channelB", 0},
                     "World data leaked");
            require (a.view (before) == before, "World B changed world A gold");
            require (b.info ().actorId == id, "World B Actor identity mismatch");
            require (a.info ().actorId == id, "World A Actor identity mismatch");
        } else {
            const auto binding = a.info ();
            require (binding.actorId == id, "Bound Actor identity mismatch");
            if (name == Scenario::channel || name == Scenario::reconnect) {
                const auto moved = a.change (Test::second_channel);
                require (moved
                           == PlayerView{"A", Test::second_channel, "channelA2", Test::earned_gold},
                         "Channel move lost state");
                require (a.info () == binding, "Channel move changed SessionInfo");
                if (name == Scenario::reconnect) {
                    a.close ();
                    clients.erase (clients.begin ());
                    auto &again = create ("A", id);
                    require (again.enter () == moved, "Reconnect changed channel or gold");
                    const auto rebound = again.info ();
                    same_actor (binding, rebound);
                    require (binding.sessionId != rebound.sessionId,
                             "Reconnect reused physical Session");
                }
            } else if (name == Scenario::update) {
                auto &b = create ("B", id);
                const auto state_b = b.enter ();
                const auto binding_b = b.info ();
                world.drain_channel_a1 ();
                auto expected = before;
                expected.nodeId = "channelA2";
                require (a.view (expected) == expected && a.info () == binding,
                         "Drain lost player state or binding");
                require (b.view (state_b) == state_b && b.info () == binding_b,
                         "Drain affected world B");
                world.replace_channel_a1 ();
                require (a.view (expected) == expected && a.info () == binding,
                         "Replacement interrupted player");
                world.create_update_zone ();
                auto &fresh = create ("A", id + "-replacement");
                const auto admitted = fresh.enter ();
                require (admitted == PlayerView{"A", Names::first_channel, "channelA2", 0},
                         "Fresh player did not enter relocated Zone");
                require (fresh.change (Names::update_channel)
                           == PlayerView{"A", Names::update_channel, "channelA1", 0},
                         "New Zone was not placed on the replacement");
                require (
                  fresh.earn (Test::earned_gold, Test::earned_gold)
                    == PlayerView{"A", Names::update_channel, "channelA1", Test::earned_gold},
                  "Replacement did not accept new work");
            }
        }
        for (auto &client : clients)
            client->healthy ();
    }
    catch (...) {
        failure = std::current_exception ();
    }
    for (auto &client : clients) {
        try {
            client->close ();
        }
        catch (const std::exception &error) {
            std::cerr << "Client cleanup failed: " << error.what () << std::endl;
            if (!failure)
                failure = std::current_exception ();
        }
    }
    if (failure)
        std::rethrow_exception (failure);
}
}
