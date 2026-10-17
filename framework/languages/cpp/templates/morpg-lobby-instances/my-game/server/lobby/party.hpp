#pragma once
#include "domain.hpp"
#include "server/shared/relocation.hpp"
namespace MyGame
{
class Party final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::spot_manager_t &spots_;
    fx::actor_client_t &actors_;
    fx::task_t<void> broadcast ()
    {
        const auto view = state.view ();
        for (const auto &id : view.members)
            co_await actors_.send (fx::actor_id_t (id), view).async ();
    }

  public:
    PartyState state;
    Party (fx::spot_context_t context, fx::spot_manager_t &spots, fx::actor_client_t &actors) :
        context_ (std::move (context)), spots_ (spots), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ().add_actor_send<&Party::pending_dungeon> ();
        context_.handlers ()
          .add_actor_send<&Party::reconnect> ()
          .add_actor_send<&Party::show> ()
          .add_actor_send<&Party::ready> ()
          .add_actor_send<&Party::enter> ()
          .add_actor_send<&Party::travel> ()
          .add_actor_send<&Party::deliver> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<PartyJoin> ();
        co_return Names::party_prefix + request.partyId == context_.spot_id ()
            && state.join (std::string (id), request)
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await broadcast (); }
    fx::task_t<void>
    departedDungeonView (Player &actor, fx::message_context_t &, const DungeonView &message)
    {
        return actor.deliver (message.dungeonId, message);
    }
    fx::task_t<void>
    pending_dungeon (Player &actor, fx::message_context_t &, const DungeonView &message)
    {
        return actor.deliver (message.dungeonId, message);
    }
    fx::task_t<void> on_leave_actor (Player &actor) override
    {
        state.leave (std::string (actor.context ().actor_id ().value ()));
        co_return;
    }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &ctx, const JoinLobbyRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await show (actor, ctx, ViewRequest{});
    }
    fx::task_t<void> show (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await actor.context ().bound_session ().send (state.view ()).async ();
    }
    fx::task_t<void> ready (Player &actor, fx::message_context_t &, const ReadyRequest &)
    {
        state.ready.insert (std::string (actor.context ().actor_id ().value ()));
        co_await broadcast ();
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const EnterDungeonRequest &)
    {
        const auto reason =
          state.entry_rejection (std::string (actor.context ().actor_id ().value ()));
        if (!reason.empty ()) {
            co_await actor.context ().bound_session ().send (ActionRejected{reason}).async ();
            co_return;
        }
        const auto party = state.partyId;
        const auto members = state.members;
        const auto id = Names::dungeon_prefix + party;
        const auto result =
          co_await spots_.get_or_create (id, Names::dungeon).in_mesh (Names::mesh).async ();
        if (result.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Dungeon creation rejected");
        const auto prepared = co_await context_.request_to_spot (id, PrepareDungeon{party, members})
                                .async<DungeonPrepared> ();
        if (prepared.dungeonId != id)
            throw std::runtime_error ("Dungeon preparation identity mismatch");
        for (const auto &member : members)
            co_await actors_.send (fx::actor_id_t (member), JoinDungeon{id, party}).async ();
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const PartyView &view)
    {
        return actor.deliver (std::string (Names::party_prefix) + view.partyId, view);
    }
};
using PartyRelocation = StateRelocation<Party, PartyState>;
}
