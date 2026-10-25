#pragma once
#include "client.hpp"
#include "server/instance/domain.hpp"

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
    require (same_dungeon_state (before, after), "Dungeon state changed");
}
inline bool party_members (const TownView &view, const std::string &id,
                           const std::vector<std::string> &members)
{
    return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &party) {
        return party.partyId == id && party.members == members;
    });
}
inline const PartyView &party_for (const TownView &view, const std::vector<std::string> &members)
{
    const auto found = std::find_if (view.parties.begin (), view.parties.end (),
                                     [&] (const auto &party) { return party.members == members; });
    require (found != view.parties.end (), "Party was not present in town view");
    return *found;
}
inline bool same_town (const TownView &a, const TownView &b)
{
    return nlohmann::json (a) == nlohmann::json (b);
}

inline void channel (const std::string &endpoint)
{
    Client player (endpoint, Deployment::leader_id);
    const auto before = player.join (1);
    require (before.townId == Names::town_id (1) && before.nodeId == Deployment::town_a,
             "Incorrect initial town");
    const auto identity = player.info ();
    Client observer (endpoint, "channel-observer");
    observer.join (1);
    const auto forming = player.exchange<TownView> (CreatePartyRequest{}, [&] (const auto &view) {
        return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &party) {
            return party.members == std::vector<std::string>{player.player_id};
        });
    });
    const auto party_id = party_for (forming, {player.player_id}).partyId;
    require (DungeonState::valid_id (party_id, "party-"), "Party ID format");
    const auto &party = party_for (forming, {player.player_id});
    require (party.phase == RunPhase::Forming && !party.runId, "Created party state");
    const auto rejected = player.exchange<ActionRejected> (
      ChangeChannelRequest{Names::maximum_integer + 1});
    require (rejected.reason == "integer-out-of-range", "Channel range rejection");
    const auto unchanged = player.exchange<TownView> (ViewRequest{});
    require (same_town (forming, unchanged), "Rejected channel changed town");
    const auto observer_from = observer.mark ();
    const auto changed = player.exchange<TownView> (
      ChangeChannelRequest{2}, [] (const auto &view) { return view.townId == Names::town_id (2); });
    const auto source_left = observer.wait<TownView> (observer_from, [&] (const auto &view) {
        return std::find (view.players.begin (), view.players.end (), player.player_id)
                 == view.players.end ()
               && std::none_of (view.parties.begin (), view.parties.end (),
                                [&] (const auto &item) { return item.partyId == party_id; });
    });
    (void)source_left;
    require (changed.nodeId == Deployment::town_b
               && std::find (changed.players.begin (), changed.players.end (), player.player_id)
                    != changed.players.end ()
               && std::none_of (changed.parties.begin (), changed.parties.end (),
                                [&] (const auto &item) { return item.partyId == party_id; }),
             "Channel relocation or forming-party leave failed");
    require (identity == player.info (), "Channel relocation changed physical Session or Actor");
    observer.close ();
    player.close ();
}

inline DungeonView enter (Client &leader, Client &member)
{
    const std::vector<std::string> members{leader.player_id, member.player_id};
    const auto created = leader.exchange<TownView> (CreatePartyRequest{}, [&] (const auto &view) {
        return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &party) {
            return party.members == std::vector<std::string>{leader.player_id};
        });
    });
    const auto party_id = party_for (created, {leader.player_id}).partyId;
    require (DungeonState::valid_id (party_id, "party-"), "Party ID format");
    const auto &forming = party_for (created, {leader.player_id});
    require (forming.phase == RunPhase::Forming && !forming.runId, "Party did not start Forming");
    const auto leader_from = leader.mark ();
    const auto member_joined = member.exchange<TownView> (
      JoinPartyRequest{party_id}, [&] (const auto &view) {
          return party_members (view, party_id, members);
      });
    const auto leader_joined = leader.wait<TownView> (
      leader_from, [&] (const auto &view) { return party_members (view, party_id, members); });
    (void)member_joined;
    const auto nonleader = member.exchange<ActionRejected> (EnterDungeonRequest{});
    require (nonleader.reason == "not-leader", "Nonleader was not rejected");
    const auto unchanged = member.exchange<TownView> (ViewRequest{});
    const auto &unchanged_party = party_for (unchanged, members);
    require (unchanged_party.phase == RunPhase::Forming && !unchanged_party.runId,
             "Rejected entry changed party");
    const auto internal_rejection =
      member.exchange<ActionRejected> (JoinDungeon{"forged", "forged"});
    require (internal_rejection.reason == "packet-not-allowed", "Session relayed internal packet");
    const auto still_unchanged = member.exchange<TownView> (ViewRequest{});
    require (same_town (unchanged, still_unchanged), "Internal rejection changed party");

    const auto leader_dungeon_from = leader.mark ();
    const auto member_dungeon_from = member.mark ();
    leader.send (EnterDungeonRequest{});
    leader.send (EnterDungeonRequest{});
    const auto leader_view = leader.wait<DungeonView> (
      leader_dungeon_from, [&] (const auto &view) {
          return view.partyId == party_id && view.phase == RunPhase::Active;
      });
    const auto member_view = member.wait<DungeonView> (
      member_dungeon_from, [&] (const auto &view) {
          return view.partyId == party_id && view.phase == RunPhase::Active;
      });
    same_dungeon (leader_view, member_view);
    require (DungeonState::valid_id (leader_view.runId, "run-")
               && leader_view.dungeonId == Names::dungeon_id (leader_view.runId)
               && leader_view.nodeId == member_view.nodeId && leader_view.stage == 0
               && leader_view.members == members,
             "Party did not enter the same fresh dungeon");
    same_dungeon (leader_view, leader.dungeon (leader_view));
    same_dungeon (member_view, member.dungeon (member_view));
    (void)leader_joined;
    return leader_view;
}

inline DungeonView second_party (const std::string &endpoint, std::int64_t channel,
                                 const DungeonView &first)
{
    Client p3 (endpoint, "p3-" + unique_id ());
    p3.join (channel);
    Client p4 (endpoint, "p4-" + unique_id ());
    p4.join (channel);
    const auto fresh = enter (p3, p4);
    require (fresh.dungeonId != first.dungeonId && fresh.runId != first.runId
               && fresh.partyId != first.partyId,
             "Two town parties shared an identity");
    p4.close ();
    p3.close ();
    return fresh;
}

class PartyRun
{
    std::string endpoint_;
    std::unique_ptr<Client> leader_, member_;
    DungeonView advanced_;

  public:
    explicit PartyRun (const std::string &endpoint) : endpoint_ (endpoint)
    {
        leader_ = std::make_unique<Client> (endpoint, Deployment::leader_id);
        member_ = std::make_unique<Client> (endpoint, Deployment::member_id);
        leader_->join (2);
        member_->join (2);
        const auto identity1 = leader_->info (), identity2 = member_->info ();
        const auto initial = enter (*leader_, *member_);
        const auto channel_rejected = leader_->exchange<ActionRejected> (ChangeChannelRequest{1});
        require (channel_rejected.reason == "run-in-progress", "Running member changed channel");
        same_dungeon (initial, leader_->dungeon (initial));
        for (std::int64_t stage = 1; stage <= Names::final_stage; ++stage) {
            const auto leader_from = leader_->mark ();
            const auto member_from = member_->mark ();
            leader_->send (AdvanceRequest{});
            advanced_ = leader_->wait<DungeonView> (
              leader_from, [stage] (const auto &view) { return view.stage == stage; });
            const auto other = member_->wait<DungeonView> (
              member_from, [stage] (const auto &view) { return view.stage == stage; });
            same_dungeon (advanced_, other);
        }
        require (advanced_.phase == RunPhase::Completed, "Final stage did not complete run");
        const auto completed_party = leader_->exchange<TownView> (
          ViewRequest{}, [&] (const auto &view) {
              return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &item) {
                  return item.partyId == advanced_.partyId && item.phase == RunPhase::Completed
                         && item.runId == advanced_.runId;
              });
          });
        require (party_for (completed_party, {leader_->player_id, member_->player_id}).phase
                   == RunPhase::Completed,
                 "Party owner did not complete the run");
        const auto completed_rejection = leader_->exchange<ActionRejected> (AdvanceRequest{});
        require (completed_rejection.reason == "run-not-active", "Completed advance accepted");
        same_dungeon (advanced_, leader_->dungeon (advanced_));
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Session identity changed while entering dungeon");
    }
    void reconnect ()
    {
        const auto identity = member_->info ();
        const auto id = member_->player_id;
        member_->close ();
        member_ = std::make_unique<Client> (endpoint_, id);
        const auto restored = member_->reconnect ();
        same_dungeon (advanced_, restored);
        require (restored.nodeId == advanced_.nodeId, "Reconnect changed host");
        const auto rebound = member_->info ();
        same_actor (identity, rebound);
        require (identity.sessionId != rebound.sessionId, "Reconnect reused physical Session");
    }
    void distinct_party () { (void)second_party (endpoint_, 1, advanced_); }
    void update (World &world)
    {
        const auto identity1 = leader_->info (), identity2 = member_->info ();
        auto &source = world.instance (advanced_.nodeId);
        source.command (Control::drain);
        source.wait_line (Control::requested, Limits::relocation);
        source.wait_line (Control::relocated, Limits::relocation);
        source.wait_line (Control::safe, Limits::safe);
        const auto relocated = leader_->dungeon (advanced_, true);
        const auto other = member_->dungeon (advanced_, true);
        same_dungeon (advanced_, relocated);
        same_dungeon (relocated, other);
        require (relocated.nodeId != advanced_.nodeId && relocated.nodeId == other.nodeId
                   && (relocated.nodeId == Deployment::instance_a
                       || relocated.nodeId == Deployment::instance_b),
                 "Dungeon did not relocate to other instance");
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Drain changed Session identity");
        world.replace_instance (advanced_.nodeId);
        const auto fresh = second_party (endpoint_, 2, advanced_);
        require (fresh.nodeId == advanced_.nodeId && fresh.dungeonId != advanced_.dungeonId
                   && fresh.runId != advanced_.runId,
                 "Replacement instance did not accept new work");
        same_dungeon (relocated, leader_->dungeon (relocated));
        same_dungeon (relocated, member_->dungeon (relocated));
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Replacement interrupted Session");
    }
    void close ()
    {
        member_->close ();
        leader_->close ();
    }
};
}
