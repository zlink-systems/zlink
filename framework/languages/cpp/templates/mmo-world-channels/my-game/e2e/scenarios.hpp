#pragma once
#include "client.hpp"
#include "server/channel/domain.hpp"
#include "server/shared/http.hpp"
#include <algorithm>
#include <thread>
namespace MyGame::E2E
{
struct Test
{
    static constexpr std::int64_t earned_gold = GameRules::earn_amount;
    static constexpr int second_channel = 2;
};
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline void require_channel_players (const ChannelView &view, std::int64_t channel,
                                     std::vector<std::string> expected,
                                     const std::string &message)
{
    std::sort (expected.begin (), expected.end ());
    require (view.channelId == channel && view.playerIds == expected, message);
}
inline void domain_checks ()
{
    int checks = 0;
    const auto check = [&checks] (bool condition, const std::string &message) {
        ++checks;
        require (condition, message);
    };

    auto now = std::chrono::steady_clock::time_point{};
    GoldBalance gold (0, [&now] { return now; });
    const auto first = gold.evaluate_earn ();
    check (first.accepted () && first.newValue == GameRules::earn_amount,
           "Initial earn was not accepted");
    check (gold.value () == 0, "Gold changed before persistence completed");
    gold.commit_earn (first);
    check (gold.value () == GameRules::earn_amount, "Accepted gold was not committed");

    const auto too_soon = gold.evaluate_earn ();
    check (too_soon.reason == Rejection::earn_too_soon
             && too_soon.retryAfterMs == GameRules::earn_interval_ms,
           "Immediate earn did not return the full cooldown");
    check (gold.value () == GameRules::earn_amount
             && gold.capture ().earnCooldownRemainingMs == GameRules::earn_interval_ms,
           "Rejected earn changed gold or cooldown state");

    now += std::chrono::milliseconds (250);
    const auto snapshot = gold.capture ();
    check (snapshot.earnCooldownRemainingMs == 750,
           "Snapshot did not store remaining milliseconds");
    auto relocated_now = std::chrono::steady_clock::time_point{} + std::chrono::hours (1);
    GoldBalance relocated (snapshot.gold, [&relocated_now] { return relocated_now; });
    relocated.restore (snapshot);
    check (relocated.evaluate_earn ().retryAfterMs == 750,
           "Relocation did not restore cooldown with a new clock origin");
    relocated_now += std::chrono::milliseconds (750);
    const auto after_cooldown = relocated.evaluate_earn ();
    check (after_cooldown.accepted () && after_cooldown.newValue == GameRules::earn_amount * 2,
           "Earn was not accepted after the restored cooldown");

    auto boundary_now = std::chrono::steady_clock::time_point{};
    GoldBalance boundary (GameRules::max_integer - GameRules::earn_amount,
                         [&boundary_now] { return boundary_now; });
    const auto boundary_earn = boundary.evaluate_earn ();
    check (boundary_earn.accepted () && boundary_earn.newValue == GameRules::max_integer,
           "Gold at maximum minus reward was not accepted exactly at the limit");
    boundary.commit_earn (boundary_earn);
    boundary_now += std::chrono::milliseconds (GameRules::earn_interval_ms);
    const auto at_limit = boundary.evaluate_earn ();
    check (at_limit.reason == Rejection::gold_limit && at_limit.retryAfterMs == 0,
           "Gold at the limit was not rejected");
    check (boundary.value () == GameRules::max_integer,
           "Gold limit rejection changed the balance");

    GoldBalance over_boundary (GameRules::max_integer - GameRules::earn_amount + 1,
                               [] { return std::chrono::steady_clock::time_point{}; });
    const auto over = over_boundary.evaluate_earn ();
    check (over.reason == Rejection::gold_limit && over.retryAfterMs == 0,
           "Gold at maximum minus reward plus one was not rejected");
    check (over_boundary.value () == GameRules::max_integer - GameRules::earn_amount + 1,
           "Rejected boundary earn changed the balance");

    std::cout << "Domain checks: " << checks << "/" << checks << " passed" << std::endl;
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
        const auto initial_channel_mark = a.mark_channel_views ();
        require (a.enter () == PlayerView{"A", Names::first_channel, "channelA1", 0},
                 "Initial world A state mismatch");
        PlayerView before{};
        ActionRejected repeated_earn_rejection{};
        if (name == Scenario::worlds) {
            const auto responses = a.earn_back_to_back ();
            before = responses.first;
            repeated_earn_rejection = responses.second;
        }
        else
            before = a.earn (Test::earned_gold);
        require (before.gold == Test::earned_gold, "Gold was not earned");
        if (name == Scenario::worlds) {
            const auto &rejected = repeated_earn_rejection;
            require (rejected.reason == Rejection::earn_too_soon
                       && rejected.retryAfterMs > 0,
                     "Repeated earn did not return its server cooldown");
            require (a.view (before) == before, "Rejected earn changed the gold balance");
            std::this_thread::sleep_for (std::chrono::milliseconds (rejected.retryAfterMs));
            const auto earned_twice = a.earn (Test::earned_gold * 2);
            require (earned_twice.gold == Test::earned_gold * 2,
                     "Earn was not accepted after the server cooldown");
            auto &b = create ("B", id);
            require (b.enter () == PlayerView{"B", Names::first_channel, "channelB", 0},
                     "World data leaked");
            require (a.view (earned_twice) == earned_twice, "World B changed world A gold");
            require (b.info ().actorId == id, "World B Actor identity mismatch");
            require (a.info ().actorId == id, "World A Actor identity mismatch");
        } else {
            const auto binding = a.info ();
            require (binding.actorId == id, "Bound Actor identity mismatch");
            if (name == Scenario::channel || name == Scenario::reconnect) {
                if (name == Scenario::channel) {
                    auto &same = create ("A", id + "-same");
                    auto &moving = create ("A", id + "-moving");
                    const auto initial_a_mark = a.mark_channel_views ();
                    same.enter ();
                    const auto initial_same_mark = same.mark_channel_views ();
                    moving.enter ();
                    require_channel_players (a.wait_channel_view (
                                               initial_a_mark, [&] (const auto &view) {
                                                   return view.channelId == Names::first_channel
                                                     && view.playerIds == std::vector<std::string>{
                                                       id, moving.player_id, same.player_id};
                                               }),
                                             Names::first_channel,
                                             {id, same.player_id, moving.player_id},
                                             "Players in the same channel were not visible");
                    require_channel_players (same.wait_channel_view (
                                               initial_same_mark, [&] (const auto &view) {
                                                   return view.channelId == Names::first_channel
                                                     && view.playerIds == std::vector<std::string>{
                                                       id, moving.player_id, same.player_id};
                                               }),
                                             Names::first_channel,
                                             {id, same.player_id, moving.player_id},
                                             "Same-channel visibility was not mutual");
                    const auto oversized_channel =
                      moving.reject_channel (GameRules::max_integer + 1);
                    require (oversized_channel.reason == Rejection::integer_out_of_range
                               && oversized_channel.retryAfterMs == 0,
                             "Oversized channel request was not rejected");
                    require_channel_players (moving.channel_view ([&] (const auto &view) {
                                               return view.channelId == Names::first_channel
                                                 && view.playerIds == std::vector<std::string>{
                                                   id, moving.player_id, same.player_id};
                                           }), Names::first_channel,
                                             {id, same.player_id, moving.player_id},
                                             "Oversized channel request changed membership");
                    const auto moved_from_first_mark = a.mark_channel_views ();
                    const auto moved_to_second_mark = moving.mark_channel_views ();
                    require (moving.change (Test::second_channel)
                               == PlayerView{"A", Test::second_channel, "channelA2", 0},
                             "Channel move did not reach channelA2");
                    require_channel_players (a.wait_channel_view (
                                               moved_from_first_mark, [&] (const auto &view) {
                                                   return view.channelId == Names::first_channel
                                                     && view.playerIds
                                                       == std::vector<std::string>{id, same.player_id};
                                               }), Names::first_channel,
                                             {id, same.player_id},
                                             "Channel move left a stale player in the old list");
                    require_channel_players (moving.wait_channel_view (
                                               moved_to_second_mark, [&] (const auto &view) {
                                                   return view.channelId == Test::second_channel
                                                     && view.playerIds
                                                       == std::vector<std::string>{moving.player_id};
                                               }), Test::second_channel,
                                             {moving.player_id},
                                             "A player in another channel was visible");
                    const auto disconnected_mark = a.mark_channel_views ();
                    same.close ();
                    require_channel_players (a.wait_channel_view (
                                               disconnected_mark, [&] (const auto &view) {
                                                   return view.channelId == Names::first_channel
                                                     && view.playerIds == std::vector<std::string>{id};
                                               }), Names::first_channel, {id},
                                             "A disconnected player remained in the channel list");
                    const auto disallowed = a.reject_internal (before);
                    require (disallowed.reason == Rejection::packet_not_allowed
                               && disallowed.retryAfterMs == 0,
                             "Session relayed an internal packet");
                }
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
                require_channel_players (
                  a.wait_channel_view (initial_channel_mark, [&] (const auto &view) {
                      return view.channelId == Names::first_channel
                        && view.playerIds == std::vector<std::string>{id};
                  }), Names::first_channel, {id},
                  "Channel list was not pushed after entry");
                require_channel_players (a.channel_view (), Names::first_channel, {id},
                                         "Channel list was not available before relocation");
                auto &b = create ("B", id);
                const auto state_b = b.enter ();
                const auto binding_b = b.info ();
                world.drain_channel_a1 ();
                auto expected = before;
                expected.nodeId = "channelA2";
                require (a.view (expected) == expected && a.info () == binding,
                         "Drain lost player state or binding");
                require_channel_players (a.channel_view (), Names::first_channel, {id},
                                         "Relocation lost the Zone player list");
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
                  fresh.earn (Test::earned_gold)
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
