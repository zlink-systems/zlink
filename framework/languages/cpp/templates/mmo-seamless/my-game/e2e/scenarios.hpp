#pragma once
#include "client.hpp"
#include <future>

namespace MyGame::E2E
{
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline void cross (const std::string &endpoint)
{
    Client client (endpoint, "cross-" + unique_id ());
    const auto before = client.join (299);
    const auto identity = client.info ();
    require (before.zoneId == "zone-3" && before.nodeId == "zoneA",
             "Cross must start on Zone node A");
    const auto after = client.move (300, 1);
    same_actor (identity, client.info ());
    require (after.zoneId == "zone-4" && after.nodeId == "zoneB" && after.x == 300
               && after.sequence == 1,
             "Cross did not relocate to Zone node B");
    require (identity == client.info (), "Cross changed physical session");
    const auto continued = client.move (301, 2);
    same_actor (identity, client.info ());
    require (continued.x == 301 && continued.sequence == 2 && continued.nodeId == "zoneB",
             "Movement failed after cross");
    require (client.dungeon ("dungeon-" + unique_id ()).nodeId == "instance",
             "Dungeon placed outside Instance node");
    client.close ();
}
inline void border (const std::string &endpoint)
{
    Client observer (endpoint, "observer-" + unique_id ());
    const auto observed = observer.join (300);
    Client source (endpoint, "border-" + unique_id ());
    const auto origin = source.join (299);
    require (observed.nodeId == "zoneB" && origin.nodeId == "zoneA",
             "Border must cross physical nodes");
    auto waiting = observer.border (source.player_id);
    auto received =
      std::async (std::launch::async, [&] { return checked (waiting.submit ()).payload; });
    try {
        source.move (298, 1);
        const auto visible = received.get ();
        require (visible.observerZoneId == "zone-4" && visible.sourceZoneId == "zone-3"
                   && visible.x == 298 && visible.sequence == 1,
                 "Adjacent Zone did not observe authoritative border update");
    }
    catch (...) {
        if (received.valid ())
            received.wait ();
        throw;
    }
    observer.close ();
    source.close ();
}
inline void reconnect (const std::string &endpoint)
{
    const auto id = "reconnect-" + unique_id ();
    WorldState before;
    SessionInfo identity;
    {
        Client client (endpoint, id);
        client.join (299);
        before = client.move (300, 1);
        identity = client.info ();
        client.close ();
    }
    Client client (endpoint, id);
    const auto after = client.join (0);
    const auto rebound = client.info ();
    same_actor (identity, rebound);
    require (before.x == after.x && before.sequence == after.sequence
               && before.zoneId == after.zoneId,
             "Reconnect overwrote authoritative world state");
    require (identity.sessionId != rebound.sessionId, "Reconnect reused old physical session");
    const auto moved = client.move (301, 2);
    same_actor (identity, client.info ());
    require (moved.x == 301 && moved.sequence == 2, "Movement failed after reconnect");
    client.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    Client client (endpoint, "update-" + unique_id ());
    const auto before = client.join (299);
    const auto identity = client.info ();
    require (before.nodeId == "zoneA", "Update must start on Zone node A");
    client.move (298, 1);
    auto safe = std::async (std::launch::async, [&] {
        world.zone_a ().wait_line (Control::requested, Limits::relocation);
        world.zone_a ().wait_line (Control::relocated, Limits::relocation);
        world.zone_a ().wait_line (Control::safe, Limits::relocation);
    });
    world.zone_a ().command (Control::drain);
    int sequence = 1;
    WorldState state;
    try {
        do {
            ++sequence;
            state = client.move (sequence % 2 == 0 ? 299 : 298, sequence);
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
    require (identity == client.info (), "Drain changed physical session");
    world.replace_zone_a ();
    const auto after = client.move (299, sequence + 1);
    same_actor (identity, client.info ());
    require (after.nodeId == "zoneB" && identity == client.info (),
             "Replacement interrupted relocated player");
    require (client.dungeon ("after-update-" + unique_id ()).nodeId == "instance",
             "Dungeon access failed after replacement");
    // Actor placement remains a Framework decision; the fresh player enters the relocated Zone.
    Client fresh (endpoint, "replacement-" + unique_id ());
    const auto placed = fresh.join (299);
    require (placed.zoneId == "zone-3" && placed.nodeId == "zoneB",
             "New Actor could not access relocated Zone after replacement");
    const auto fresh_identity = fresh.info ();
    const auto moved = fresh.move (300, 1);
    require (moved.zoneId == "zone-4" && moved.nodeId == "zoneB" && moved.sequence == 1,
             "New Actor movement failed after replacement");
    same_actor (fresh_identity, fresh.info ());
    fresh.close ();
    client.close ();
}
}
