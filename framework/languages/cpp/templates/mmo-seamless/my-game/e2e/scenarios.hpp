#pragma once
#include "client.hpp"
#include "server/zone/domain.hpp"
#include <future>

namespace MyGame::E2E
{
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline void same_state (const WorldState &expected, const WorldState &actual, const std::string &reason)
{
    require (expected.zoneId == actual.zoneId && expected.nodeId == actual.nodeId
               && expected.x == actual.x && expected.sequence == actual.sequence,
             reason + " changed authoritative state");
}
inline void cross (const std::string &endpoint)
{
    Client client (endpoint, "cross-" + unique_id ());
    const auto before = client.join (294);
    const auto identity = client.info ();
    require (before.zoneId == "zone-3" && before.nodeId == "zoneA",
             "Cross must start on Zone node A");
    const auto oversized = client.move_rejected (ZonePositionDomain::MaxStep + 1, 1);
    require (oversized.reason == "step-too-large" && oversized.retryAfterMs == 0,
             "MaxStep rejection did not return step-too-large");
    same_state (before, client.state (), "step-too-large rejection");

    auto first_response = client.observe<WorldState> ([] (const auto &state) { return state.sequence == 1; });
    auto budget_response = client.observe<ActionRejected> ([] (const auto &rejection) {
        return rejection.reason == "move-budget";
    });
    client.send_move (2, 1);
    client.send_move (ZonePositionDomain::MaxStep, 2);
    const auto first = first_response.wait ();
    require (first.x == 296 && first.sequence == 1, "The first server-calculated move was incorrect");
    const auto budget = budget_response.wait ();
    require (budget.reason == "move-budget" && budget.retryAfterMs > 0,
             "Move budget rejection did not include a positive server retry delay");
    same_state (first, client.state (), "move-budget rejection");
    std::this_thread::sleep_for (std::chrono::milliseconds (budget.retryAfterMs));
    const auto after = client.move (ZonePositionDomain::MaxStep, 2);
    same_actor (identity, client.info ());
    require (after.zoneId == "zone-4" && after.nodeId == "zoneB" && after.x == 306
               && after.sequence == 2,
             "Cross did not relocate to Zone node B after multiple accepted moves");
    require (client.connected (), "Cross disconnected the Session");

    const auto continued = client.move (1, 3);
    same_actor (identity, client.info ());
    require (continued.x == 307 && continued.nodeId == "zoneB",
             "Movement failed after cross");
    const auto forbidden = client.packet_rejected (EnterZone{Position{307}, 3, 0});
    require (forbidden.reason == "packet-not-allowed" && forbidden.retryAfterMs == 0,
             "Session relayed an internal packet");
    same_state (continued, client.state (), "packet-not-allowed rejection");

    Client edge (endpoint, "edge-" + unique_id ());
    const auto at_edge = edge.join (Names::world_width - 1);
    const auto outside = edge.move_rejected (1, 1);
    require (outside.reason == "outside-world" && outside.retryAfterMs == 0,
             "World boundary rejection did not return outside-world");
    same_state (at_edge, edge.state (), "outside-world rejection");

    require (client.dungeon ("dungeon-" + unique_id ()).nodeId == "instance",
             "Dungeon placed outside Instance node");
    edge.close ();
    client.close ();
}
inline void border (const std::string &endpoint)
{
    Client observer (endpoint, "observer-" + unique_id ());
    const auto observed = observer.join (300);
    Client source (endpoint, "border-" + unique_id ());
    auto appeared_waiter = observer.observe<PlayerAppeared> ([&] (const auto &message) {
        return message.playerId == source.player_id;
    });
    const auto origin = source.join (299);
    require (observed.nodeId == "zoneB" && origin.nodeId == "zoneA",
             "Border must cross physical nodes");
    const auto appeared = appeared_waiter.wait ();
    require (appeared.position.x == 299, "Adjacent Zone did not observe PlayerAppeared");

    auto moved_waiter = observer.observe<PlayerMoved> ([&] (const auto &message) {
        return message.playerId == source.player_id && message.sequence == 1;
    });
    const auto moved = source.move (-1, 1);
    const auto border_move = moved_waiter.wait ();
    require (moved.x == 298 && border_move.position.x == moved.x
               && border_move.sequence == moved.sequence,
             "Adjacent Zone did not observe the authoritative border movement");

    auto left_waiter = observer.observe<PlayerLeft> ([&] (const auto &message) {
        return message.playerId == source.player_id
               && message.reason == PlayerLeftReason::left_zone;
    });
    auto reappeared_waiter = observer.observe<PlayerAppeared> ([&] (const auto &message) {
        return message.playerId == source.player_id;
    });
    const auto crossed = source.move (2, 2);
    const auto left = left_waiter.wait ();
    const auto reappeared = reappeared_waiter.wait ();
    require (left.reason == PlayerLeftReason::left_zone,
             "Zone departure used the wrong reason");
    require (crossed.zoneId == "zone-4" && reappeared.position.x == 300,
             "Zone arrival was not observed by the destination Zone");

    auto local_move_waiter = observer.observe<PlayerMoved> ([&] (const auto &message) {
        return message.playerId == source.player_id && message.sequence == 3;
    });
    const auto settled = source.move (1, 3);
    const auto local_move = local_move_waiter.wait ();
    require (settled.x == 301 && settled.sequence == 3
               && local_move.position.x == settled.x && local_move.sequence == settled.sequence,
             "Same-Zone players did not observe the completed movement");

    auto disconnected_waiter = observer.observe<PlayerLeft> ([&] (const auto &message) {
        return message.playerId == source.player_id
               && message.reason == PlayerLeftReason::disconnected;
    });
    source.close ();
    const auto disconnected = disconnected_waiter.wait ();
    require (disconnected.reason == PlayerLeftReason::disconnected,
             "Connection loss used the wrong visibility end reason");
    observer.close ();
}
inline void reconnect (const std::string &endpoint)
{
    const auto id = "reconnect-" + unique_id ();
    WorldState before;
    SessionInfo identity;
    {
        Client client (endpoint, id);
        client.join (299);
        before = client.move (1, 1);
        identity = client.info ();
        client.close ();
    }
    Client reconnect (endpoint, id);
    const auto after = reconnect.join (0);
    same_actor (identity, reconnect.info ());
    require (before.x == after.x && before.sequence == after.sequence
               && before.zoneId == after.zoneId,
             "Reconnect overwrote authoritative world state");
    require (identity.sessionId != reconnect.info ().sessionId,
             "Reconnect reused old physical Session");
    const auto moved = reconnect.move (1, 2);
    same_actor (identity, reconnect.info ());
    require (moved.sequence == 2, "Movement failed after reconnect");
    reconnect.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    Client client (endpoint, "update-" + unique_id ());
    const auto before = client.join (299);
    const auto identity = client.info ();
    require (before.nodeId == "zoneA", "Update must start on Zone node A");
    auto state = client.move (-1, 1);
    world.zone_a ().command (Control::vacate_zone);
    world.zone_a ().wait_line (Control::zone_vacated, Limits::packet);
    auto safe = std::async (std::launch::async, [&] {
        world.zone_a ().wait_line (Control::requested, Limits::relocation);
        world.zone_a ().wait_line (Control::relocated, Limits::relocation);
        world.zone_a ().wait_line (Control::safe, Limits::safe);
    });
    world.zone_a ().command (Control::drain);
    auto sequence = state.sequence;
    try {
        do {
            ++sequence;
            const auto target = sequence % 2 == 0 ? 299.0 : 298.0;
            state = client.move (target - state.x, sequence);
            require (state.sequence == sequence, "Drain lost movement");
            same_actor (identity, client.info ());
            if (safe.wait_for (std::chrono::seconds (0)) == std::future_status::ready)
                safe.get ();
        } while (safe.valid () || state.nodeId != "zoneB");
    }
    catch (...) {
        if (safe.valid ())
            safe.wait ();
        throw;
    }
    require (state.zoneId == "zone-3", "Drain changed logical Zone");
    require (identity == client.info (), "Drain changed physical Session");
    world.replace_zone_a ();
    const auto after = client.move (299 - state.x, sequence + 1);
    same_actor (identity, client.info ());
    require (after.nodeId == "zoneB", "Replacement interrupted relocated player");
    require (client.dungeon ("after-update-" + unique_id ()).nodeId == "instance",
             "Dungeon access failed after replacement");

    Client fresh (endpoint, "replacement-" + unique_id ());
    const auto placed = fresh.join (Control::replacement_x);
    require (placed.zoneId == Names::zone_id (Control::replacement_x) && placed.nodeId == "zoneA",
             "Replacement did not accept the new Zone");
    const auto fresh_identity = fresh.info ();
    const auto moved = fresh.move (1, 1);
    require (moved.zoneId == placed.zoneId && moved.nodeId == "zoneA" && moved.sequence == 1,
             "New Actor movement failed after replacement");
    same_actor (fresh_identity, fresh.info ());
    fresh.close ();
    client.close ();
}
}
