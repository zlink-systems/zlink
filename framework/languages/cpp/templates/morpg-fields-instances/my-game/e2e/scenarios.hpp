#pragma once
#include "client.hpp"
#include "../server/field/party-domain.hpp"
#include <future>
#include <thread>

namespace MyGame::E2E
{
inline void same_actor (const SessionInfo &before, const SessionInfo &after)
{
    require (before.actorId == after.actorId && before.objectGeneration == after.objectGeneration
               && before.objectGeneration != 0,
             "Actor identity changed");
}
inline bool matches_dungeon (const DungeonView &before, const DungeonView &after)
{
    return before.dungeonId == after.dungeonId && before.partyId == after.partyId
           && before.stage == after.stage && before.members == after.members
           && before.runId == after.runId && before.phase == after.phase;
}
inline void same_dungeon (const DungeonView &before, const DungeonView &after)
{
    require (matches_dungeon (before, after), "Dungeon state changed");
}
inline bool same_members (const PartyView &view,
                          const std::string &party_id,
                          const std::vector<std::string> &members)
{
    return view.partyId == party_id && view.members == members;
}
template <class Request>
ActionRejected reject (Client &client, const Request &request, const std::string &reason)
{
    return client.exchange<Request, ActionRejected> (
      request, [&] (const auto &reply) { return reply.reason == reason; });
}
inline DungeonView enter (Client &leader, Client &member)
{
    const std::vector<std::string> leader_members{leader.player_id};
    const std::vector<std::string> party_members{leader.player_id, member.player_id};
    const auto created = leader.exchange<CreatePartyRequest, PartyView> (
      {}, [&] (const auto &view) { return view.members == leader_members; });
    const auto party_id = created.partyId;
    require (PartyRun::valid_id (party_id, "party-"), "Party ID was not issued by the server");
    const auto joined = member.exchange<JoinPartyRequest, PartyView> (
      {party_id}, [&] (const auto &view) { return same_members (view, party_id, party_members); });
    const auto leader_joined = leader.receive<PartyView> (
      [&] (const auto &view) { return same_members (view, party_id, party_members); });
    require (joined.phase == RunPhase::Forming && leader_joined.phase == RunPhase::Forming
               && joined.runId == std::nullopt,
             "Party join changed the initial run state");
    const auto non_leader = reject (member, EnterDungeonRequest{}, Rejection::not_leader);
    require (non_leader.retryAfterMs == 0, "Leadership rejection has a cooldown");
    const auto unchanged = member.party ();
    require (unchanged.members == joined.members && unchanged.phase == joined.phase
               && unchanged.runId == std::nullopt,
             "Rejected entry changed party state");

    leader.send (EnterDungeonRequest{});
    leader.send (EnterDungeonRequest{});
    const auto leader_view = leader.receive<DungeonView> (
      [&] (const auto &view) { return view.partyId == party_id && view.phase == RunPhase::Active; });
    const auto member_view = member.receive<DungeonView> (
      [&] (const auto &view) { return view.partyId == party_id && view.phase == RunPhase::Active; });
    same_dungeon (leader_view, member_view);
    require (leader_view.dungeonId == Names::dungeon_id (leader_view.runId)
               && leader_view.nodeId == member_view.nodeId && leader_view.stage == 0
               && PartyRun::valid_id (leader_view.runId, "run-")
               && leader_view.members == party_members,
             "Party did not enter one fresh Dungeon");
    const auto party_active = leader.receive<PartyView> ([&] (const auto &view) {
        return view.partyId == party_id && view.phase == RunPhase::Active;
    });
    require (party_active.members == party_members && party_active.leaderId == leader.player_id
               && party_active.runId == leader_view.runId,
             "Party run did not become active after all members entered");

    same_dungeon (leader_view, leader.dungeon (leader_view));
    same_dungeon (member_view, member.dungeon (member_view));
    const auto enter_again = leader.exchange<EnterDungeonRequest, DungeonView> (
      {}, [expected = leader_view] (const auto &view) {
          return matches_dungeon (expected, view);
      });
    same_dungeon (leader_view, enter_again);
    const auto create_in_dungeon = reject (leader, CreatePartyRequest{}, Rejection::run_in_progress);
    const auto join_in_dungeon = reject (
      leader, JoinPartyRequest{"party-" + std::string (32, '0')}, Rejection::run_in_progress);
    require (create_in_dungeon.retryAfterMs == 0 && join_in_dungeon.retryAfterMs == 0,
             "Town-only request rejection has a cooldown");
    return leader_view;
}
inline void field (const std::string &endpoint)
{
    Client player (endpoint, "field-p1-" + unique_id ()),
      observer (endpoint, "field-p2-" + unique_id ());
    const auto observed = observer.join (199), origin = player.join (198);
    require (origin.fieldId == "field-2" && origin.nodeId == "fieldA"
               && observed.nodeId == "fieldA",
             "Field crossing must start on fieldA");
    const auto missing_party = reject (
      player, JoinPartyRequest{"party-" + std::string (32, '0')}, Rejection::invalid_party);
    require (missing_party.retryAfterMs == 0, "Missing-party rejection has a cooldown");
    const auto party = player.exchange<CreatePartyRequest, PartyView> (
      {}, [&] (const auto &view) { return view.members == std::vector<std::string>{player.player_id}; });
    require (PartyRun::valid_id (party.partyId, "party-") && party.leaderId == player.player_id
               && party.phase == RunPhase::Forming && !party.runId,
             "Party creation did not issue a fresh party ID");
    const auto joined = observer.exchange<JoinPartyRequest, PartyView> (
      {party.partyId}, [&] (const auto &view) { return view.members.size () == 2; });
    const auto leader_joined = player.receive<PartyView> (
      [&] (const auto &view) { return view.members.size () == 2; });
    require (joined.partyId == party.partyId && leader_joined.partyId == party.partyId,
             "Party membership was not delivered to both players");
    const auto disallowed = player.exchange<JoinDungeon, ActionRejected> (
      {"internal", "internal"}, [] (const auto &reply) {
          return reply.reason == Rejection::packet_not_allowed;
      });
    require (disallowed.retryAfterMs == 0, "Packet allowlist rejection has a cooldown");
    const auto identity = player.info ();
    auto wait = observer.border (player.player_id);
    auto visible = std::async (std::launch::async, [&] { return checked (wait.submit ()).payload; });
    try {
        const auto moved = player.move (2, 1, 200);
        require (moved.fieldId == "field-3" && moved.nodeId == "fieldB" && moved.x == 200
                   && moved.sequence == 1,
                 "Field relocation failed");
        const auto border = visible.get ();
        require (border.observerFieldId == "field-2" && border.sourceFieldId == "field-3"
                   && border.x == 200,
                 "Adjacent field did not receive border publication");
        require (identity == player.info (), "Field relocation changed Session");
        const auto player_party = player.party (), observer_party = observer.party ();
        const std::vector<std::string> members{player.player_id, observer.player_id};
        require (player_party.partyId == party.partyId && observer_party.partyId == party.partyId
                   && player_party.members == members && observer_party.members == members
                   && player_party.leaderId == player.player_id
                   && observer_party.leaderId == player.player_id,
                 "Cross-field movement changed party membership or leadership");

        for (const auto &[request, reason] :
             std::vector<std::pair<MoveRequest, std::string>>{
               {{11, 2}, Rejection::step_too_large},
               {{0, Names::max_safe_integer + 1}, Rejection::integer_out_of_range},
               {{0, 1}, Rejection::stale_sequence}}) {
            const auto rejected = reject (player, request, reason);
            require (rejected.retryAfterMs == 0, "Movement rejection has a cooldown");
            const auto state = player.exchange<ViewRequest, WorldState> (
              {}, [] (const auto &) { return true; });
            require (state.x == moved.x && state.sequence == moved.sequence,
                     "Rejected movement changed state");
        }

        auto accepted = player.receive_async<WorldState> (
          [] (const auto &view) { return view.sequence == 2; });
        auto budget = player.receive_async<ActionRejected> (
          [] (const auto &rejection) { return rejection.reason == Rejection::move_budget; });
        player.send (MoveRequest{0, 2});
        player.send (MoveRequest{10, 3});
        const auto accepted_state = accepted.get ();
        const auto budget_rejection = budget.get ();
        const auto budget_state = player.exchange<ViewRequest, WorldState> (
          {}, [] (const auto &view) { return view.sequence == 2; });
        require (accepted_state.x == moved.x && budget_state.x == accepted_state.x
                   && budget_state.sequence == accepted_state.sequence
                   && budget_rejection.retryAfterMs > 0,
                 "Movement budget rejection changed state");
        std::this_thread::sleep_for (std::chrono::milliseconds (budget_rejection.retryAfterMs));
        const auto retried = player.move (10, 3, moved.x + 10);
        require (retried.sequence == 3, "Movement was not accepted after server retry delay");
    }
    catch (...) {
        if (visible.valid ())
            visible.wait ();
        throw;
    }
    player.close ();
    observer.close ();
}
inline void dungeon_scenario (const std::string &endpoint, Scenario scenario, World &world)
{
    Client leader (endpoint, "leader-" + unique_id ());
    const auto member_id = "member-" + unique_id ();
    auto member = std::make_unique<Client> (endpoint, member_id);
    leader.join (250);
    member->join (250);
    const auto identity1 = leader.info (), identity2 = member->info ();
    const auto initial = enter (leader, *member);
    leader.send (AdvanceRequest{});
    const auto advanced = leader.receive<DungeonView> (
      [] (const auto &view) { return view.stage == 1; });
    const auto member_advanced = member->receive<DungeonView> (
      [] (const auto &view) { return view.stage == 1; });
    same_dungeon (advanced, member_advanced);
    require (initial.nodeId == advanced.nodeId && initial.runId == advanced.runId
               && identity1 == leader.info () && identity2 == member->info (),
             "Dungeon entry or advance changed identity");

    if (scenario == Scenario::dungeon) {
        auto current = advanced;
        for (int stage = 2; stage <= Names::final_stage; ++stage)
            current = leader.exchange<AdvanceRequest, DungeonView> (
              {}, [stage] (const auto &view) { return view.stage == stage; });
        require (current.phase == RunPhase::Completed, "Final stage did not complete run");
        const auto completed_party = leader.receive<PartyView> (
          [&] (const auto &view) { return view.runId == current.runId
                                      && view.phase == RunPhase::Completed; });
        require (completed_party.phase == RunPhase::Completed,
                 "Dungeon completion did not reach the party owner");
        const auto after_completion = reject (leader, AdvanceRequest{}, Rejection::run_not_active);
        require (after_completion.retryAfterMs == 0, "Inactive-run rejection has a cooldown");
        same_dungeon (current, leader.dungeon (current));

        Client other_leader (endpoint, "other-leader-" + unique_id ()),
          other_member (endpoint, "other-member-" + unique_id ());
        other_leader.join (250);
        other_member.join (250);
        const auto other = enter (other_leader, other_member);
        require (other.dungeonId != initial.dungeonId && other.runId != initial.runId,
                 "Different parties shared a Dungeon run");
        other_leader.close ();
        other_member.close ();
    }
    else if (scenario == Scenario::reconnect) {
        member->close ();
        member.reset ();
        member = std::make_unique<Client> (endpoint, member_id);
        const auto restored = member->reconnect_dungeon ();
        same_dungeon (advanced, restored);
        require (restored.nodeId == advanced.nodeId, "Reconnect changed the Dungeon host");
        same_actor (identity2, member->info ());
        require (identity2.sessionId != member->info ().sessionId,
                 "Reconnect reused the physical Session");
    }
    else if (scenario == Scenario::update) {
        auto &source = world.instance (advanced.nodeId);
        source.command (Control::drain);
        source.wait_line (Control::requested, Limits::relocation);
        source.wait_line (Control::relocated, Limits::relocation);
        source.wait_line (Control::safe, Limits::safe_shutdown);
        const auto relocated = leader.dungeon (advanced, advanced.nodeId);
        const auto relocated_member = member->dungeon (advanced, advanced.nodeId);
        same_dungeon (advanced, relocated);
        same_dungeon (relocated, relocated_member);
        require (relocated.nodeId != advanced.nodeId && relocated.nodeId == relocated_member.nodeId,
                 "Dungeon did not relocate to the other Instance");
        require (identity1 == leader.info () && identity2 == member->info (),
                 "Drain changed Session identity");
        world.replace_instance (advanced.nodeId);
        Client fresh1 (endpoint, "replacement1-" + unique_id ()),
          fresh2 (endpoint, "replacement2-" + unique_id ());
        fresh1.join (250);
        fresh2.join (250);
        const auto fresh = enter (fresh1, fresh2);
        require (fresh.nodeId == advanced.nodeId && fresh.dungeonId != advanced.dungeonId
                   && fresh.runId != advanced.runId,
                 "Replacement Instance did not accept new work");
        same_dungeon (relocated, leader.dungeon (relocated));
        require (identity1 == leader.info () && identity2 == member->info (),
                 "Replacement changed Session identity");
        fresh1.close ();
        fresh2.close ();
    }
    leader.close ();
    member->close ();
}
}
