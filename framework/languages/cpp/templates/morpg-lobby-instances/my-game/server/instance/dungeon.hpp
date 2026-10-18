#pragma once
#include "domain.hpp"
#include "server/shared/relocation.hpp"
namespace MyGame
{
class Dungeon final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    DungeonView view () const
    {
        return {std::string (context_.spot_id ()), config_.Id, state.partyId, state.members,
                state.stage};
    }

  public:
    DungeonState state;
    Dungeon (fx::spot_context_t context, const NodeConfig &config, fx::actor_client_t &actors) :
        context_ (std::move (context)), config_ (config), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ().add_actor_send<&Dungeon::departed_party_view> ();
        context_.handlers ()
          .add_handler<&Dungeon::prepare> ()
          .add_actor_send<&Dungeon::reconnect> ()
          .add_actor_send<&Dungeon::show> ()
          .add_actor_send<&Dungeon::advance> ()
          .add_actor_send<&Dungeon::deliver> ();
    }
    fx::task_t<DungeonPrepared> prepare (const PrepareDungeon &request)
    {
        state.prepare (request, context_.spot_id ());
        co_return DungeonPrepared{std::string (context_.spot_id ())};
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinDungeon> ();
        co_return state.admits (id, request, context_.spot_id ())
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        co_await actors_.send (actor.context ().actor_id (), view ()).async ();
    }
    fx::task_t<void>
    departed_party_view (Player &actor, fx::message_context_t &, const PartyView &message)
    {
        return actor.deliver (std::string (Names::party_prefix) + message.partyId, message);
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &ctx, const JoinLobbyRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await show (actor, ctx, ViewRequest{});
    }
    fx::task_t<void> show (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> advance (Player &, fx::message_context_t &, const AdvanceRequest &)
    {
        state.advance ();
        const auto update = view ();
        for (const auto &id : state.members)
            co_await actors_.send (fx::actor_id_t (id), update).async ();
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const DungeonView &update)
    {
        return actor.deliver (update.dungeonId, update);
    }
};
using DungeonRelocation = StateRelocation<Dungeon, DungeonState>;
}
