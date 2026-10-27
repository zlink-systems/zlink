#pragma once
#include "client.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

namespace MyGame::E2E
{
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.ActorId == after.ActorId
               && before.ObjectGeneration == after.ObjectGeneration
               && before.ObjectGeneration != 0,
             "Actor identity changed");
}

inline bool at (const RegionView &view,
                const std::string &id,
                double x,
                std::int64_t sequence)
{
    return std::any_of (view.Players.begin (), view.Players.end (), [&] (const auto &player) {
        return player.PlayerId == id && player.X == x && player.Sequence == sequence;
    });
}

inline bool at (const RegionView &view, const std::string &id, double x)
{
    return std::any_of (view.Players.begin (), view.Players.end (), [&] (const auto &player) {
        return player.PlayerId == id && player.X == x;
    });
}

inline void same_region_state (const RegionView &before, const RegionView &after)
{
    require (before.RegionId == after.RegionId && before.Players == after.Players,
             "Region state changed.");
}

inline void region (const std::string &endpoint)
{
    Client p1 (endpoint, "p1"), p2 (endpoint, "p2"), p3 (endpoint, "p3"),
      p4 (endpoint, "p4"), p5 (endpoint, "p5"), p6 (endpoint, "p6");
    const auto first = p1.join ("forest");
    const auto joined = p2.join ("forest");
    const auto both = p1.wait_view ([&] (const auto &view) { return at (view, p2.player_id, 0); });
    require (first.RegionId == "forest-1" && joined.RegionId == first.RegionId
               && both.Players.size () == 2 && at (both, p1.player_id, 0),
             "First region membership mismatch");

    const auto second_first = p3.join ("forest");
    const auto second = p4.join ("forest");
    require (second_first.RegionId == "forest-2" && second.RegionId == second_first.RegionId
               && second.Players.size () == 2 && at (second, p3.player_id, 0)
               && at (second, p4.player_id, 0),
             "Second Region membership mismatch");

    const auto capacity = p5.exchange<EnterRegionRequest, RegionUnavailable> (
      {p5.player_id, "forest"}, [] (const auto &reply) { return reply.Region == "forest"; });
    require (capacity.Reason == Rejection::capacity_exhausted
               && p5.info ().ActorId == p5.player_id,
             "Capacity exhaustion did not retain the Session.");

    const auto unknown = p6.exchange<EnterRegionRequest, RegionUnavailable> (
      {p6.player_id, "unknown"}, [] (const auto &reply) { return reply.Region == "unknown"; });
    const auto denied = p6.exchange<RegionFull, ActionRejected> (
      {Names::region_full, "forest", Names::first_index},
      [] (const auto &reply) { return reply.Reason == Rejection::packet_not_allowed; });
    require (unknown.Reason == Rejection::region_unknown
               && denied == ActionRejected{Rejection::packet_not_allowed, 0}
               && p6.info ().ActorId == p6.player_id,
             "Unknown Region or client packet rejection changed the Session.");

    p1.close ();
    p2.close ();
    p3.close ();
    p4.close ();
    p5.close ();
    p6.close ();
}

inline void movement (const std::string &endpoint)
{
    Client p1 (endpoint, "p1"), p2 (endpoint, "p2"), p3 (endpoint, "p3");
    p1.join ("forest");
    p2.join ("forest");
    p1.wait_view ([&] (const auto &view) { return at (view, p2.player_id, 0); });
    const auto separate = p3.join ("forest");
    require (separate.RegionId == "forest-2", "Third player did not enter the second Region.");

    const auto first_view = p1.mark_views ();
    const auto first_broadcast_view = p2.mark_views ();
    p1.move (Names::max_step, 1);
    p1.move (Names::max_step, 2);
    const auto first = p1.wait_view (first_view, [&] (const auto &view) {
        return at (view, p1.player_id, Names::max_step, 1);
    });
    const auto first_broadcast = p2.wait_view (first_broadcast_view, [&] (const auto &view) {
        return at (view, p1.player_id, Names::max_step, 1);
    });
    same_region_state (first, first_broadcast);
    require (p1.view_count () - first_view == 1,
             "Caller did not receive one direct movement result.");

    const auto before_rejection = p1.view_count ();
    const auto rejected = p1.wait_rejection ();
    require (rejected.Reason == Rejection::move_budget && rejected.RetryAfterMs > 0,
             "Movement budget rejection omitted its server retry time.");
    const auto unchanged = p2.view ([] (const auto &) { return true; });
    same_region_state (first, unchanged);
    require (p1.view_count () == before_rejection,
             "Rejected movement changed state or sent an accepted result.");

    std::this_thread::sleep_for (std::chrono::milliseconds (rejected.RetryAfterMs));

    const auto second_view = p1.mark_views ();
    const auto second_broadcast_view = p2.mark_views ();
    p1.move (Names::max_step, 2);
    const auto second = p1.wait_view (second_view, [&] (const auto &view) {
        return at (view, p1.player_id, Names::max_step * 2, 2);
    });
    const auto second_broadcast = p2.wait_view (second_broadcast_view, [&] (const auto &view) {
        return at (view, p1.player_id, Names::max_step * 2, 2);
    });
    same_region_state (second, second_broadcast);
    require (p1.view_count () - second_view == 1,
             "Caller received a duplicate accepted movement result.");
    p1.no_region_push ();
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
    require (opened.DungeonId == id && opened.NodeId == "instance",
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
        client.move (Names::max_step, 1);
        before = client.wait_view ([&] (const auto &view) {
            return at (view, "p1", Names::max_step, 1);
        });
        identity = client.info ();
        client.close ();
    }
    Client client (endpoint, "p1");
    const auto after = client.join ("forest");
    const auto rebound = client.info ();
    same_actor (identity, rebound);
    require (before.RegionId == after.RegionId && before.Players == after.Players
               && before.NodeId == after.NodeId,
             "Reconnect changed Region state.");
    require (identity.SessionId != rebound.SessionId, "Reconnect reused physical Session.");
    client.close ();
}

inline void update (const std::string &endpoint, World &world)
{
    Client p1 (endpoint, "p1"), p2 (endpoint, "p2");
    p1.join ("forest");
    p2.join ("forest");
    p1.move (Names::max_step, 1);
    const auto before = p1.wait_view ([&] (const auto &view) {
        return at (view, "p1", Names::max_step, 1) && view.Players.size () == 2;
    });
    p2.wait_view ([&] (const auto &view) { return at (view, "p1", Names::max_step, 1); });
    Client p3 (endpoint, "p3");
    const auto isolated = p3.join ("forest");
    require (isolated.RegionId == "forest-2", "Second Region must occupy survivor capacity");
    const auto identity1 = p1.info (), identity2 = p2.info ();
    auto &owner = world.region (before.NodeId);
    owner.command (Control::drain);
    owner.wait_line (Control::requested, Limits::relocation);
    owner.wait_line (Control::relocated, Limits::relocation);
    owner.wait_line (Control::safe, Limits::safe_observation);
    const auto after = p1.view ([&before] (const auto &view) { return view.NodeId != before.NodeId; });
    require (before.RegionId == after.RegionId && before.Players == after.Players
               && before.NodeId != after.NodeId,
             "Relocation changed application state or retained owner");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Relocation changed Session identity");
    world.replace_region (before.NodeId);
    p1.move (1, 2);
    const auto moved = p1.wait_view ([&] (const auto &view) {
        return at (view, "p1", Names::max_step + 1, 2);
    });
    p2.wait_view ([&] (const auto &view) {
        return at (view, "p1", Names::max_step + 1, 2);
    });
    require (moved.NodeId != before.NodeId && identity1 == p1.info ()
               && identity2 == p2.info (),
             "Replacement changed Session identity or failed to accept movement");
    Client fresh (endpoint, "fresh");
    const auto created = fresh.join ("desert");
    require (created.NodeId == before.NodeId,
             "New Region was not accepted by the replacement Region node");
    require (created.Players.size () == 1 && created.Players.front ().PlayerId == "fresh",
             "New Region membership mismatch");
    fresh.close ();
    p3.close ();
    p1.close ();
    p2.close ();
}
}
