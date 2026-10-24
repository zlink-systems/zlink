#pragma once
#include "domain.hpp"
#include "server/shared/player.hpp"

namespace MyGame
{
class Dungeon final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    DungeonView view () const
    {
        return {context_.spot_id (), config_.Id, state.partyId, state.members, state.stage};
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
        context_.handlers ()
          .add_handler<&Dungeon::prepare> ()
          .add_actor_send<&Dungeon::reconnect> ()
          .add_actor_send<&Dungeon::query> ()
          .add_actor_send<&Dungeon::advance> ()
          .add_actor_send<&Dungeon::deliver> ();
    }
    fx::task_t<DungeonPrepared> prepare (const PrepareDungeon &request)
    {
        state.prepare (context_.spot_id (), request);
        co_return DungeonPrepared{context_.spot_id ()};
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        co_return state.accepts (id, context_.spot_id (), payload.decode<JoinDungeon> ())
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        co_await actors_.send (actor.context ().actor_id (), view ()).async ();
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
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
    fx::task_t<void> advance (Player &, fx::message_context_t &, const AdvanceRequest &)
    {
        state.advance ();
        const auto snapshot = view ();
        for (const auto &id : snapshot.members)
            co_await actors_.send (fx::actor_id_t (id), snapshot).async ();
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const DungeonView &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
};
class DungeonRelocation final : public fx::spot_relocation_adapter_t<Dungeon>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Dungeon &dungeon, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (dungeon.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Dungeon &dungeon, std::vector<std::byte> payload, std::stop_token) override
    {
        dungeon.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<DungeonState> ();
        co_return;
    }
};
}
