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
inline void same_dungeon (const DungeonView &before, const DungeonView &after)
{
    require (before.dungeonId == after.dungeonId && before.partyId == after.partyId
               && before.stage == after.stage && before.members == after.members,
             "Dungeon state changed");
}
inline void field (const std::string &endpoint)
{
    Client player (endpoint, "field-p1-" + unique_id ()),
      observer (endpoint, "field-p2-" + unique_id ());
    const auto observed = observer.join (199), origin = player.join (198);
    require (origin.fieldId == "field-2" && origin.nodeId == "fieldA"
               && observed.nodeId == "fieldA",
             "Field crossing must start on fieldA");
    const auto identity = player.info ();
    auto wait = observer.border (player.player_id);
    auto visible =
      std::async (std::launch::async, [&] { return checked (wait.submit ()).payload; });
    try {
        const auto moved = player.move (200, 1);
        require (moved.fieldId == "field-3" && moved.nodeId == "fieldB" && moved.x == 200
                   && moved.sequence == 1,
                 "Field relocation failed");
        const auto border = visible.get ();
        require (border.observerFieldId == "field-2" && border.sourceFieldId == "field-3"
                   && border.x == 200,
                 "Adjacent field did not receive border publication");
        require (identity == player.info (), "Field relocation changed Session");
    }
    catch (...) {
        if (visible.valid ())
            visible.wait ();
        throw;
    }
    player.close ();
    observer.close ();
}
inline DungeonView enter (Client &leader, Client &member, const std::string &party)
{
    const std::vector<std::string> leaderMembers{leader.player_id};
    const std::vector<std::string> partyMembers{leader.player_id, member.player_id};
    auto party_has = [&] (std::size_t count) {
        return [&, count] (const FieldView &view) {
            return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &p) {
                return p.partyId == party
                       && p.members == (count == 1 ? leaderMembers : partyMembers);
            });
        };
    };
    leader.send (CreatePartyRequest{party});
    leader.receive<FieldView> (party_has (1));
    member.send (JoinPartyRequest{party});
    member.receive<FieldView> (party_has (2));
    leader.receive<FieldView> (party_has (2));
    leader.send (EnterDungeonRequest{});
    const auto a =
      leader.receive<DungeonView> ([&] (const auto &view) { return view.partyId == party; });
    const auto b =
      member.receive<DungeonView> ([&] (const auto &view) { return view.partyId == party; });
    same_dungeon (a, b);
    require (a.nodeId == b.nodeId && a.dungeonId == Names::dungeon_id (party) && a.stage == 0
               && a.members == std::vector<std::string>{leader.player_id, member.player_id},
             "Party dungeon differs");
    // Queries from the current Spot confirm completion of both deferred joins.
    same_dungeon (a, leader.dungeon ());
    same_dungeon (b, member.dungeon ());
    return a;
}
inline void dungeon_scenario (const std::string &endpoint, Scenario scenario, World &world)
{
    Client leader (endpoint, "leader-" + unique_id ());
    const auto memberId = "member-" + unique_id ();
    auto member = std::make_unique<Client> (endpoint, memberId);
    leader.join (250);
    member->join (250);
    const auto identity1 = leader.info (), identity2 = member->info ();
    const auto initial = enter (leader, *member, "party-" + unique_id ());
    leader.send (AdvanceRequest{});
    const auto advanced =
      leader.receive<DungeonView> ([] (const auto &view) { return view.stage == 1; });
    same_dungeon (advanced,
                  member->receive<DungeonView> ([] (const auto &view) { return view.stage == 1; }));
    require (initial.nodeId == advanced.nodeId && identity1 == leader.info ()
               && identity2 == member->info (),
             "Dungeon entry or advance changed identity");
    if (scenario == Scenario::reconnect) {
        member->close ();
        member.reset ();
        member = std::make_unique<Client> (endpoint, memberId);
        const auto restored = member->reconnect_dungeon ();
        same_dungeon (advanced, restored);
        require (restored.nodeId == advanced.nodeId, "Reconnect changed dungeon host");
        const auto rebound = member->info ();
        same_actor (identity2, rebound);
        require (identity2.sessionId != rebound.sessionId, "Reconnect reused physical session");
    }
    if (scenario == Scenario::update) {
        auto &source = world.instance (advanced.nodeId);
        source.command (Control::drain);
        source.wait_line (Control::requested, Limits::relocation);
        source.wait_line (Control::relocated, Limits::relocation);
        source.wait_line (Control::safe, Limits::safe_shutdown);
        const auto relocated = leader.dungeon (advanced.nodeId),
                   second = member->dungeon (advanced.nodeId);
        same_dungeon (advanced, relocated);
        same_dungeon (relocated, second);
        require (relocated.nodeId != advanced.nodeId && relocated.nodeId == second.nodeId,
                 "Dungeon did not relocate to other instance");
        require (identity1 == leader.info () && identity2 == member->info (),
                 "Drain changed Session");
        world.replace_instance (advanced.nodeId);
        Client fresh1 (endpoint, "replacement1-" + unique_id ()),
          fresh2 (endpoint, "replacement2-" + unique_id ());
        fresh1.join (250);
        fresh2.join (250);
        const auto fresh = enter (fresh1, fresh2, "replacement-party-" + unique_id ());
        require (fresh.nodeId == advanced.nodeId, "Replacement instance did not accept new work");
        same_dungeon (relocated, leader.dungeon ());
        require (identity1 == leader.info () && identity2 == member->info (),
                 "Replacement changed Session");
        fresh1.close ();
        fresh2.close ();
    }
    leader.close ();
    member->close ();
}
}
