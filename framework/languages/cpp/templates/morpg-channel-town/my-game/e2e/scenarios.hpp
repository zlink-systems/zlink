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
inline void same_dungeon (const DungeonView &before, const DungeonView &after)
{
    require (same_dungeon_state (before, after), "Dungeon state changed");
}
inline void channel (const std::string &endpoint)
{
    Client client (endpoint, Deployment::leader_id);
    const auto before = client.join (1);
    require (before.townId == Names::town_id (1) && before.nodeId == Deployment::town_a,
             "Incorrect initial town");
    const auto identity = client.info ();
    const auto changed = client.exchange<TownView> (
      ChangeChannelRequest{2}, [] (const auto &view) { return view.townId == Names::town_id (2); });
    require (changed.nodeId == Deployment::town_b
               && std::find (changed.players.begin (), changed.players.end (), client.player_id)
                    != changed.players.end (),
             "Channel relocation failed");
    require (identity == client.info (), "Channel relocation changed physical Session or Actor");
    client.close ();
}
inline bool
party_members (const TownView &view, const std::string &id, const std::vector<std::string> &members)
{
    return std::any_of (view.parties.begin (), view.parties.end (), [&] (const auto &party) {
        return party.partyId == id && party.members == members;
    });
}
inline DungeonView enter (Client &leader, Client &member, const std::string &party)
{
    const std::vector<std::string> members{leader.player_id, member.player_id};
    leader.exchange<TownView> (CreatePartyRequest{party}, [&] (const auto &view) {
        return party_members (view, party, {leader.player_id});
    });
    member.exchange<TownView> (JoinPartyRequest{party}, [&] (const auto &view) {
        return party_members (view, party, members);
    });
    leader.receive<TownView> (
      [&] (const auto &view) { return party_members (view, party, members); });
    const auto first = leader.exchange<DungeonView> (
      EnterDungeonRequest{}, [&] (const auto &view) { return view.partyId == party; });
    const auto second =
      member.receive<DungeonView> ([&] (const auto &view) { return view.partyId == party; });
    same_dungeon (first, second);
    require (first.nodeId == second.nodeId
               && (first.nodeId == Deployment::instance_a || first.nodeId == Deployment::instance_b)
               && first.dungeonId == Names::dungeon_id (party) && first.members == members
               && first.stage == 0,
             "Party did not enter the same fresh dungeon");
    // Current-Spot replies confirm both deferred joins completed before advancing.
    same_dungeon (first, leader.dungeon (first));
    same_dungeon (second, member.dungeon (second));
    return first;
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
        const auto initial = enter (*leader_, *member_, "party-" + unique_id ());
        advanced_ = leader_->exchange<DungeonView> (
          AdvanceRequest{}, [] (const auto &view) { return view.stage == 1; });
        const auto other =
          member_->receive<DungeonView> ([] (const auto &view) { return view.stage == 1; });
        same_dungeon (advanced_, other);
        require (advanced_.nodeId == initial.nodeId && other.nodeId == initial.nodeId,
                 "Advance changed host");
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Dungeon join changed Session identity");
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
    void update (World &world)
    {
        const auto identity1 = leader_->info (), identity2 = member_->info ();
        auto &source = world.instance (advanced_.nodeId);
        source.command (Control::drain);
        source.wait_line (Control::requested, Limits::relocation);
        source.wait_line (Control::relocated, Limits::relocation);
        source.wait_line (Control::safe, Limits::safe);
        const auto relocated = leader_->dungeon (advanced_, true),
                   other = member_->dungeon (advanced_, true);
        same_dungeon (advanced_, relocated);
        same_dungeon (relocated, other);
        require (relocated.nodeId != advanced_.nodeId && relocated.nodeId == other.nodeId
                   && (relocated.nodeId == Deployment::instance_a
                       || relocated.nodeId == Deployment::instance_b),
                 "Dungeon did not relocate to other instance");
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Drain changed Session identity");
        world.replace_instance (advanced_.nodeId);
        Client fresh1 (endpoint_, "fresh-leader-" + unique_id ()),
          fresh2 (endpoint_, "fresh-member-" + unique_id ());
        fresh1.join (2);
        fresh2.join (2);
        const auto fresh = enter (fresh1, fresh2, "fresh-party-" + unique_id ());
        require (fresh.nodeId == advanced_.nodeId,
                 "Replacement instance did not accept fresh dungeon");
        same_dungeon (relocated, leader_->dungeon (relocated));
        same_dungeon (relocated, member_->dungeon (relocated));
        require (identity1 == leader_->info () && identity2 == member_->info (),
                 "Replacement interrupted Session");
        fresh1.close ();
        fresh2.close ();
    }
    void close ()
    {
        member_->close ();
        leader_->close ();
    }
};
}
