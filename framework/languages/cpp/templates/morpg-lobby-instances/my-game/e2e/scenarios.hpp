#pragma once
#include "client.hpp"
#include <regex>
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
               && a.stage == b.stage && a.phase == b.phase && a.runId == b.runId,
             "Dungeon state changed");
}
inline DungeonView enter (Client &p1, Client &p2)
{
    p1.join ();
    p2.join ();
    const auto identity1 = p1.info (), identity2 = p2.info ();
    const auto unknown = p1.exchange<JoinPartyRequest, ActionRejected>({new_id("party-")});
    require(unknown.reason == Rejection::invalid_party && unknown.retryAfterMs == 0, "Unknown party allowed");
    p1.send(CreatePartyRequest{});
    const auto created = p1.receive<PartyView>([](const auto &) { return true; });
    const auto party = created.partyId;
    require(std::regex_match(party, std::regex("party-[0-9a-f]{32}")) && created.phase == RunPhase::Forming && !created.runId, "Party identity");
    require (created.members == std::vector<std::string>{p1.player_id},
             "Party leader mismatch");
    p2.send (JoinPartyRequest{party});
    const auto joined = p1.party_view (party, 2, 0);
    require (joined.members == std::vector<std::string>{p1.player_id, p2.player_id},
             "Party members mismatch");
    require (p2.party_view (party, 2, 0).members == joined.members, "Party push mismatch");
    const auto rejected = p1.exchange<EnterDungeonRequest, ActionRejected> ({});
    require (rejected.reason == Rejection::not_ready, "Unready party admitted");
    p1.send(ViewRequest{});
    const auto unchanged = p1.receive<PartyView>([](const auto &) { return true; });
    require(nlohmann::json(unchanged) == nlohmann::json(joined), "Rejected entry changed state");
    const auto denied = p2.exchange<EnterDungeonRequest, ActionRejected>({});
    require(denied.reason == Rejection::not_leader && denied.retryAfterMs == 0, "Non-leader allowed");
    p2.send(ViewRequest{});
    require(nlohmann::json(p2.receive<PartyView>([](const auto &) { return true; })) == nlohmann::json(joined), "Non-leader changed state");
    const auto internal = p2.exchange<JoinDungeon, ActionRejected>({"dungeon-forged", "run-forged"});
    require(internal.reason == Rejection::packet_not_allowed && internal.retryAfterMs == 0, "Internal packet allowed");
    p1.send (ReadyRequest{});
    require (p1.party_view (party, 2, 1).ready == std::vector<bool> ({true, false}),
             "Leader readiness mismatch");
    require (p2.party_view (party, 2, 1).ready == std::vector<bool> ({true, false}),
             "Member readiness push mismatch");
    p2.send (ReadyRequest{});
    p1.party_view (party, 2, 2);
    p2.party_view (party, 2, 2);
    p1.send (EnterDungeonRequest{});
    p1.send (EnterDungeonRequest{});
    const auto first = p1.dungeon_view (party, 0), second = p2.dungeon_view (party, 0);
    same_dungeon (first, second);
    require (first.runId && std::regex_match(*first.runId, std::regex("run-[0-9a-f]{32}")) && first.dungeonId == Names::dungeon_id(*first.runId) && first.members == joined.members
               && first.nodeId == second.nodeId
               && (first.nodeId == "instanceA" || first.nodeId == "instanceB"),
             "Dungeon placement mismatch");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Dungeon entry changed Session binding");
    return first;
}
inline void complete(Client &p1, Client &p2, const std::string &party, std::int64_t next_stage)
{
    DungeonView completed;
    for (auto stage = next_stage; stage <= Names::final_stage; ++stage) {
        p1.send(AdvanceRequest{});
        completed = p1.dungeon_view(party, stage);
        same_dungeon(completed, p2.dungeon_view(party, stage));
    }
    require(completed.phase == RunPhase::Completed, "Run did not complete");
    const auto denied = p1.exchange<AdvanceRequest, ActionRejected>({});
    require(denied.reason == Rejection::run_not_active && denied.retryAfterMs == 0, "Completed advance allowed");
    same_dungeon(completed, p1.view([](const auto &v) { return v.stage == Names::final_stage; }));
}
inline void party (const std::string &endpoint)
{
    const auto id = "party-" + unique_id ();
    Client p1 (endpoint, "p1-" + id), p2 (endpoint, "p2-" + id);
    const auto first = enter(p1, p2);
    p1.send(AdvanceRequest{});
    same_dungeon(p1.dungeon_view(first.partyId, 1), p2.dungeon_view(first.partyId, 1));
    Client p3(endpoint, "p3-" + id), p4(endpoint, "p4-" + id);
    const auto second = enter(p3, p4);
    require(first.dungeonId != second.dungeonId && first.runId != second.runId, "Two parties shared run");
    complete(p1, p2, first.partyId, 2);
    p3.close(); p4.close();
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
        const auto initial = enter (p1, p2);
        p1.send (AdvanceRequest{});
        before = p1.dungeon_view (initial.partyId, 1);
        same_dungeon (before, p2.dungeon_view (before.partyId, 1));
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
    const auto advanced = p1.dungeon_view (before.partyId, 2);
    same_dungeon (advanced, p2.dungeon_view (before.partyId, 2));
    complete(p1, p2, before.partyId, 3);
    p1.close ();
    p2.close ();
}
inline void update (const std::string &endpoint, World &world)
{
    const auto id = "update-" + unique_id ();
    Client p1 (endpoint, "p1-" + id), p2 (endpoint, "p2-" + id);
    const auto placed = enter (p1, p2);
    const auto identity1 = p1.info (), identity2 = p2.info ();
    p1.send (AdvanceRequest{});
    const auto before = p1.dungeon_view (placed.partyId, 1);
    same_dungeon (before, p2.dungeon_view (before.partyId, 1));
    auto &source = world.instance (placed.nodeId);
    source.command (Control::drain);
    source.wait_line (Control::requested, Limits::relocation);
    source.wait_line (Control::relocated, Limits::relocation);
    source.wait_line (Control::safe, Limits::safe);
    const auto relocated = p1.view ([&before] (const auto &view) {
        return view.stage == before.stage && view.nodeId != before.nodeId;
    });
    same_dungeon (before, relocated);
    const auto member = p2.view ([&relocated] (const auto &view) {
        return view.stage == relocated.stage && view.nodeId == relocated.nodeId;
    });
    same_dungeon (before, member);
    require (member.nodeId == relocated.nodeId, "Relocated members have different hosts");
    require (relocated.nodeId != before.nodeId, "Dungeon did not relocate");
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Relocation changed Session binding");
    p2.send (AdvanceRequest{});
    const auto advanced = p1.dungeon_view (before.partyId, 2);
    same_dungeon (advanced, p2.dungeon_view (before.partyId, 2));
    world.replace_instance (placed.nodeId);
    const auto current_dungeon = [&advanced] (const auto &view) {
        return view.stage == advanced.stage && view.nodeId == advanced.nodeId;
    };
    same_dungeon (advanced, p1.view (current_dungeon));
    same_dungeon (advanced, p2.view (current_dungeon));
    // Framework selects placement. Live clients exercise new work after
    // replacement readiness.
    const auto fresh_id = "replacement-" + unique_id ();
    Client fresh1 (endpoint, "p1-" + fresh_id), fresh2 (endpoint, "p2-" + fresh_id);
    const auto fresh = enter(fresh1, fresh2);
    require(fresh.nodeId == placed.nodeId && fresh.dungeonId != placed.dungeonId && fresh.runId != placed.runId, "Replacement instance did not accept distinct work");
    fresh1.send (AdvanceRequest{});
    same_dungeon (fresh1.dungeon_view (fresh.partyId, 1), fresh2.dungeon_view (fresh.partyId, 1));
    fresh1.close ();
    fresh2.close ();
    require (identity1 == p1.info () && identity2 == p2.info (),
             "Replacement changed Session binding");
    complete(p1, p2, before.partyId, 3);
    p1.close ();
    p2.close ();
}
}
