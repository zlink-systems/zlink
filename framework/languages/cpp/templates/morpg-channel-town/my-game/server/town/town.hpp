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
        for (const auto &[id, members] : state.parties)
            result.parties.push_back ({id, members});
        return result;
    }
    fx::task_t<void> broadcast ()
    {
        const auto snapshot = view ();
        for (const auto &id : snapshot.players)
            co_await actors_.send (fx::actor_id_t (id), snapshot).async ();
    }
    fx::task_t<void> party (Player &actor, const std::string &id, bool create)
    {
        if (!state.party (std::string (actor.context ().actor_id ().value ()), id, create)) {
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
    Town (fx::spot_context_t context,
          const NodeConfig &config,
          fx::actor_client_t &actors,
          fx::spot_manager_t &spots) :
        context_ (std::move (context)), config_ (config), actors_ (actors), spots_ (spots)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ().add_actor_send<&Town::pending_dungeon> ();
        context_.handlers ()
          .add_actor_send<&Town::reconnect> ()
          .add_actor_send<&Town::change> ()
          .add_actor_send<&Town::create_party> ()
          .add_actor_send<&Town::join_party> ()
          .add_actor_send<&Town::enter> ()
          .add_actor_send<&Town::travel> ()
          .add_actor_send<&Town::query> ()
          .add_actor_send<&Town::deliver> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinTownRequest> ();
        if (request.playerId != id || Names::town_id (request.channel) != context_.spot_id ())
            co_return fx::spot_actor_join_result_t::reject ();
        state.players.emplace (id);
        co_return fx::spot_actor_join_result_t::accept ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await broadcast (); }
    fx::task_t<void>
    pending_dungeon (Player &actor, fx::message_context_t &, const DungeonView &message)
    {
        return actor.deliver (message.dungeonId, message);
    }
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
        actor.context ()
          .join_spot (
            Names::town_id (request.channel),
            JoinTownRequest{std::string (actor.context ().actor_id ().value ()), request.channel})
          .defer ();
        co_return;
    }
    fx::task_t<void>
    create_party (Player &actor, fx::message_context_t &, const CreatePartyRequest &request)
    {
        co_await party (actor, request.partyId, true);
    }
    fx::task_t<void>
    join_party (Player &actor, fx::message_context_t &, const JoinPartyRequest &request)
    {
        co_await party (actor, request.partyId, false);
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const EnterDungeonRequest &)
    {
        const auto found = state.led_by (std::string (actor.context ().actor_id ().value ()));
        if (found == state.parties.end ()) {
            co_await actor.context ()
              .bound_session ()
              .send (ActionRejected{Rejection::not_leader})
              .async ();
            co_return;
        }
        const auto party_id = found->first;
        const auto members = found->second;
        const auto id = Names::dungeon_id (party_id);
        const auto created =
          co_await spots_.get_or_create (id, Names::dungeon).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto prepared =
          co_await context_.request_to_spot (id, PrepareDungeon{party_id, members})
            .async<DungeonPrepared> ();
        if (prepared.dungeonId != id)
            throw std::runtime_error ("Dungeon preparation identity mismatch");
        for (const auto &member : members)
            co_await actors_.send (fx::actor_id_t (member), JoinDungeon{id, party_id}).async ();
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const TownView &message)
    {
        return actor.deliver (message.townId, message);
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
