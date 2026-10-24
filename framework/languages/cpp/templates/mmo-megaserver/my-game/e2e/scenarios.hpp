#pragma once
#include "client.hpp"
namespace MyGame::E2E
{
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline bool at (const RegionView &view, const std::string &id, int x)
{
    return std::any_of (view.players.begin (), view.players.end (), [&] (const auto &player) {
        return player.playerId == id && player.x == x;
    });
}
inline void region (const std::string &endpoint)
{
    Client p1 (endpoint, "p1"), p2 (endpoint, "p2"), p3 (endpoint, "p3");
    const auto first = p1.join ("forest");
    const auto joined = p2.join ("forest");
    const auto both = p1.wait_view ([&] (const auto &view) { return at (view, p2.player_id, 0); });
    require (first.regionId == "forest-1" && joined.regionId == first.regionId
               && both.players.size () == 2 && at (both, p1.player_id, 0),
             "First region membership mismatch");
    const auto second = p3.join ("forest");
    require (second.regionId == "forest-2" && second.players.size () == 1
               && at (second, p3.player_id, 0),
             "Capacity or instance isolation failed");
    p1.move (17, 1);
    const auto moved =
      p2.wait_view ([&] (const auto &view) { return at (view, p1.player_id, 17); });
    require (moved.regionId == first.regionId && moved.players.size () == 2,
             "Peer movement missing");
    p3.no_region_push ();
    p1.close ();
    p2.close ();
    p3.close ();
}
inline void dungeon (const std::string &endpoint)
{
    Client client (endpoint, "p1");
    client.join ("forest");
    const auto id = "dungeon-" + unique_id ();
    const auto opened = client.dungeon (id);
    require (opened.dungeonId == id && opened.nodeId == "instance",
             "Dungeon placed outside Instance node");
    client.close ();
}
inline void reconnect (const std::string &endpoint)
{
    RegionView before;
    SessionInfo identity;
    {
        Client client (endpoint, "p1");
        client.join ("forest");
        client.move (23, 1);
        before = client.wait_view ([] (const auto &view) { return at (view, "p1", 23); });
        identity = client.info ();
        client.close ();
    }
    Client client (endpoint, "p1");
    const auto after = client.join ("forest");
    const auto rebound = client.info ();
    same_actor (identity, rebound);
    require (before.regionId == after.regionId && before.players == after.players
               && before.nodeId == after.nodeId,
             "Reconnect changed region state");
    require (identity.sessionId != rebound.sessionId, "Reconnect reused physical session");
    client.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    Client p1 (endpoint, "p1"), p2 (endpoint, "p2");
    p1.join ("forest");
    p2.join ("forest");
    p1.move (31, 1);
    const auto before = p1.wait_view (
      [] (const auto &view) { return at (view, "p1", 31) && view.players.size () == 2; });
    p2.wait_view ([] (const auto &view) { return at (view, "p1", 31); });
    Client p3 (endpoint, "p3");
    const auto isolated = p3.join ("forest");
    require (isolated.regionId == "forest-2", "Second Region must occupy survivor capacity");
    const auto identity1 = p1.info (), identity2 = p2.info ();
    auto &owner = world.region (before.nodeId);
    owner.command (Control::drain);
    owner.wait_line (Control::requested, Limits::relocation);
    owner.wait_line (Control::relocated, Limits::relocation);
    owner.wait_line (Control::safe, Limits::safe_observation);
    const auto after =
      p1.view ([&before] (const auto &view) { return view.nodeId != before.nodeId; });
    require (before.regionId == after.regionId && before.players == after.players
               && before.nodeId != after.nodeId,
             "Relocation changed application state or retained owner");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Relocation changed Session identity");
    world.replace_region (before.nodeId);
    p1.move (32, 2);
    p2.wait_view ([] (const auto &view) { return at (view, "p1", 32); });
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Replacement changed Session identity");
    Client fresh (endpoint, "fresh");
    const auto created = fresh.join ("desert");
    require (created.nodeId == before.nodeId,
             "New Region was not accepted by the replacement Region node");
    require (created.players.size () == 1 && created.players.front ().playerId == "fresh",
             "New Region membership mismatch");
    fresh.close ();
    p3.close ();
    p1.close ();
    p2.close ();
}
}
