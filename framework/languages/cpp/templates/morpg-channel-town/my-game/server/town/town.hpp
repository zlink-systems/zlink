#pragma once
#include "domain.hpp"
#include "server/shared/player.hpp"

namespace MyGame
{
class Town final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;

    TownView view () const
    {
        TownView result{
          context_.spot_id (), config_.Id, {state.players.begin (), state.players.end ()}, {}};
        for (const auto &[id, party] : state.parties)
            result.parties.push_back (party.view ());
        return result;
    }
    fx::task_t<void> broadcast ()
    {
        const auto snapshot = view ();
        for (const auto &id : snapshot.players)
            co_await actors_.send (fx::actor_id_t (id), snapshot).async ();
    }
    fx::task_t<void> party (Player &actor, const std::optional<std::string> &party_id, bool create)
    {
        if (!state.party (std::string (actor.context ().actor_id ().value ()), party_id, create)) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::invalid_party})
              .async ();
            co_return;
        }
        co_await broadcast ();
    }

  public:
    TownState state;
    Town (fx::spot_context_t context, const NodeConfig &config, fx::actor_client_t &actors,
          fx::spot_manager_t &spots) :
        context_ (std::move (context)), config_ (config), actors_ (actors), spots_ (spots)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Town::reconnect> ()
          .add_actor_send<&Town::change> ()
          .add_actor_send<&Town::create_party> ()
          .add_actor_send<&Town::join_party> ()
          .add_actor_send<&Town::enter> ()
          .add_actor_send<&Town::travel> ()
          .add_actor_send<&Town::query> ()
          .add_actor_send<&Town::deliver> ()
          .add_actor_send<&Town::deliver_rejection> ()
          .add_handler<&Town::run_started> ()
          .add_handler<&Town::run_completed> ()
          .add_handler<&Town::show_party> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinTownRequest> ();
        if (request.playerId != id || ChannelRules::validate (request.channel)
            || Names::town_id (request.channel) != context_.spot_id ())
            co_return fx::spot_actor_join_result_t::reject ();
        state.players.emplace (id);
        co_return fx::spot_actor_join_result_t::accept ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await broadcast (); }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        state.leave (std::string (actor.context ().actor_id ().value ()));
        co_await broadcast ();
    }
    // Disconnect retains channel and party until an explicit move.
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void> query (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &message, const JoinTownRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await query (actor, message, {});
    }
    fx::task_t<void>
    change (Player &actor, fx::message_context_t &, const ChangeChannelRequest &request)
    {
        auto reason = ChannelRules::validate (request.channel);
        if (!reason) {
            const auto id = std::string (actor.context ().actor_id ().value ());
            const auto party = std::find_if (state.parties.begin (), state.parties.end (),
                                             [&] (const auto &item) {
                                                 return std::find (item.second.members.begin (),
                                                                   item.second.members.end (), id)
                                                        != item.second.members.end ();
                                             });
            if (party != state.parties.end ())
                reason = party->second.change_channel ();
        }
        if (reason) {
            co_await actor.context ().bound_session ().send (ActionRejected{*reason}).async ();
            co_return;
        }
        actor.context ()
          .join_spot (
            Names::town_id (request.channel),
            JoinTownRequest{std::string (actor.context ().actor_id ().value ()), request.channel})
          .defer ();
        co_return;
    }
    fx::task_t<void> create_party (Player &actor, fx::message_context_t &, const CreatePartyRequest &)
    {
        co_await party (actor, std::nullopt, true);
    }
    fx::task_t<void> join_party (Player &actor, fx::message_context_t &, const JoinPartyRequest &request)
    {
        co_await party (actor, request.partyId, false);
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const EnterDungeonRequest &)
    {
        const auto player_id = std::string (actor.context ().actor_id ().value ());
        const auto found = std::find_if (state.parties.begin (), state.parties.end (),
                                         [&] (const auto &item) {
                                             return std::find (item.second.members.begin (),
                                                               item.second.members.end (), player_id)
                                                    != item.second.members.end ();
                                         });
        auto reason = found == state.parties.end ()
                        ? std::optional<std::string> (Rejection::not_leader)
                        : found->second.enter (player_id);
        if (reason) {
            co_await actor.context ().bound_session ().send (ActionRejected{*reason}).async ();
            co_return;
        }
        auto &party = found->second;
        if (party.phase == RunPhase::Active) {
            co_await actor.context ().bound_session ().send (view ()).async ();
            co_return;
        }
        if (!party.runId) {
            co_await actor.context ().bound_session ().send (ActionRejected{"run-mismatch"}).async ();
            co_return;
        }
        const auto run_id = *party.runId;
        const auto party_id = party.partyId;
        const auto members = party.members;
        const auto id = Names::dungeon_id (run_id);
        const auto created =
          co_await spots_.get_or_create (id, Names::dungeon).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto prepared =
          co_await context_
            .request_to_spot (id, PrepareDungeon{run_id, party_id, std::string (context_.spot_id ()),
                                                members})
            .async<DungeonPrepared> ();
        if (prepared.reason || prepared.dungeonId != id || party.prepared (prepared.runId)) {
            co_await actor.context ().bound_session ().send (ActionRejected{"run-mismatch"}).async ();
            co_return;
        }
        for (const auto &member : members)
            if (state.players.contains (member))
                co_await actors_.send (fx::actor_id_t (member), JoinDungeon{id, run_id}).async ();
    }
    fx::task_t<void> run_started (const RunStarted &message)
    {
        const auto party = std::find_if (state.parties.begin (), state.parties.end (),
                                         [&] (const auto &item) {
                                             return item.second.runId
                                                    && *item.second.runId == message.runId;
                                         });
        if (party == state.parties.end ())
            co_return;
        if (party->second.started (message.runId)) {
            if (!party->second.members.empty ())
                co_await actors_
                  .send (fx::actor_id_t (party->second.members.front ()),
                         ActionRejected{"run-mismatch"})
                  .async ();
            co_return;
        }
        co_await broadcast ();
    }
    fx::task_t<void> run_completed (const RunCompleted &message)
    {
        const auto party = std::find_if (state.parties.begin (), state.parties.end (),
                                         [&] (const auto &item) {
                                             return item.second.runId
                                                    && *item.second.runId == message.runId;
                                         });
        if (party == state.parties.end ())
            co_return;
        if (party->second.completed (message.runId)) {
            if (!party->second.members.empty ())
                co_await actors_
                  .send (fx::actor_id_t (party->second.members.front ()),
                         ActionRejected{"run-mismatch"})
                  .async ();
            co_return;
        }
        co_await broadcast ();
    }
    fx::task_t<void> show_party (const PartyViewRequest &request)
    {
        co_await actors_.send (fx::actor_id_t (request.playerId), view ()).async ();
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const TownView &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void> deliver_rejection (Player &actor, fx::message_context_t &,
                                        const ActionRejected &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
};
class TownRelocation final : public fx::spot_relocation_adapter_t<Town>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Town &town, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (town.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Town &town, std::vector<std::byte> payload, std::stop_token) override
    {
        town.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<TownState> ();
        co_return;
    }
};
}
