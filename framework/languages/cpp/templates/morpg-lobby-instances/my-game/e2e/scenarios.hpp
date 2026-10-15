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
inline void same_dungeon (const DungeonView &a, const DungeonView &b)
{
    require (a.dungeonId == b.dungeonId && a.partyId == b.partyId && a.members == b.members
               && a.stage == b.stage,
             "Dungeon state changed");
}
inline DungeonView enter (Client &p1, Client &p2, const std::string &party)
{
    p1.join ();
    p2.join ();
    const auto identity1 = p1.info (), identity2 = p2.info ();
    p1.send (CreatePartyRequest{party});
    require (p1.party_view (party, 1, 0).members == std::vector<std::string>{p1.player_id},
             "Party leader mismatch");
    p2.send (JoinPartyRequest{party});
    const auto joined = p1.party_view (party, 2, 0);
    require (joined.members == std::vector<std::string>{p1.player_id, p2.player_id},
             "Party members mismatch");
    require (p2.party_view (party, 2, 0).members == joined.members, "Party push mismatch");
    const auto rejected = p1.exchange<EnterDungeonRequest, ActionRejected> ({});
    require (rejected.reason == Rejection::not_ready, "Unready party admitted");
    p1.send (ReadyRequest{});
    require (p1.party_view (party, 2, 1).ready == std::vector<bool> ({true, false}),
             "Leader readiness mismatch");
    require (p2.party_view (party, 2, 1).ready == std::vector<bool> ({true, false}),
             "Member readiness push mismatch");
    p2.send (ReadyRequest{});
    p1.party_view (party, 2, 2);
    p2.party_view (party, 2, 2);
    p1.send (EnterDungeonRequest{});
    const auto first = p1.dungeon_view (party, 0), second = p2.dungeon_view (party, 0);
    same_dungeon (first, second);
    require (first.dungeonId == Names::dungeon_prefix + party && first.members == joined.members
               && first.nodeId == second.nodeId
               && (first.nodeId == "instanceA" || first.nodeId == "instanceB"),
             "Dungeon placement mismatch");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Dungeon entry changed Session binding");
    return first;
}
inline void party (const std::string &endpoint)
{
    const auto id = "party-" + unique_id ();
    Client p1 (endpoint, "p1-" + id), p2 (endpoint, "p2-" + id);
    enter (p1, p2, id);
    p1.close ();
    p2.close ();
}
inline void reconnect (const std::string &endpoint)
{
    const auto id = "reconnect-" + unique_id ();
    Client p1 (endpoint, "p1-" + id);
    SessionInfo identity;
    DungeonView before;
    {
        Client p2 (endpoint, "p2-" + id);
        enter (p1, p2, id);
        p1.send (AdvanceRequest{});
        before = p1.dungeon_view (id, 1);
        same_dungeon (before, p2.dungeon_view (id, 1));
        identity = p2.info ();
        p2.close ();
    }
    Client p2 (endpoint, "p2-" + id);
    const auto after = p2.exchange<JoinLobbyRequest, DungeonView> ({p2.player_id});
    same_dungeon (before, after);
    require (before.nodeId == after.nodeId, "Reconnect changed Dungeon host");
    const auto rebound = p2.info ();
    same_actor (identity, rebound);
    require (identity.sessionId != rebound.sessionId, "Reconnect reused physical Session");
    p2.send (AdvanceRequest{});
    const auto advanced = p1.dungeon_view (id, 2);
    same_dungeon (advanced, p2.dungeon_view (id, 2));
    p1.close ();
    p2.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    const auto id = "update-" + unique_id ();
    Client p1 (endpoint, "p1-" + id), p2 (endpoint, "p2-" + id);
    const auto placed = enter (p1, p2, id);
    const auto identity1 = p1.info (), identity2 = p2.info ();
    p1.send (AdvanceRequest{});
    const auto before = p1.dungeon_view (id, 1);
    same_dungeon (before, p2.dungeon_view (id, 1));
    auto &source = world.instance (placed.nodeId);
    source.command (Control::drain);
    source.wait_line (Control::requested, Limits::relocation);
    source.wait_line (Control::relocated, Limits::relocation);
    source.wait_line (Control::safe, Limits::relocation);
    const auto relocated = p1.view ();
    same_dungeon (before, relocated);
    const auto member = p2.view ();
    same_dungeon (before, member);
    require (member.nodeId == relocated.nodeId, "Relocated members have different hosts");
    require (relocated.nodeId != before.nodeId, "Dungeon did not relocate");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Relocation changed Session binding");
    p2.send (AdvanceRequest{});
    const auto advanced = p1.dungeon_view (id, 2);
    same_dungeon (advanced, p2.dungeon_view (id, 2));
    world.replace_instance (placed.nodeId);
    same_dungeon (advanced, p1.view ());
    same_dungeon (advanced, p2.view ());
    // Framework selects placement. Live clients exercise new work after
    // replacement readiness.
    const auto fresh_id = "replacement-" + unique_id ();
    Client fresh1 (endpoint, "p1-" + fresh_id), fresh2 (endpoint, "p2-" + fresh_id);
    require (enter (fresh1, fresh2, fresh_id).nodeId == placed.nodeId,
             "Replacement instance did not accept new work");
    fresh1.send (AdvanceRequest{});
    same_dungeon (fresh1.dungeon_view (fresh_id, 1), fresh2.dungeon_view (fresh_id, 1));
    fresh1.close ();
    fresh2.close ();
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Replacement changed Session binding");
    p1.close ();
    p2.close ();
}
}
