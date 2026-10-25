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
                state.stage, state.runId, state.phase};
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
          .add_actor_send<&Dungeon::show> ()
          .add_actor_send<&Dungeon::advance> ()
          .add_actor_send<&Dungeon::deliver> ()
          .add_actor_send<&Dungeon::reject_delivery>()
          .add_actor_send<&Dungeon::party_delivery>()
          .add_actor_send<&Dungeon::enter>()
          .add_actor_send<&Dungeon::travel>();
    }
    fx::task_t<DungeonPrepared> prepare (const PrepareDungeon &request)
    {
        if (Names::dungeon_id(request.runId) != context_.spot_id() || !state.prepare(request).empty()) throw std::invalid_argument(Rejection::run_mismatch);
        co_return DungeonPrepared{std::string(context_.spot_id()), request.runId};
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
        const auto started = state.join(std::string(actor.context().actor_id().value()));
        co_await context_.send_to_spot(state.partyId, DungeonMemberEntered{*state.runId, std::string(actor.context().actor_id().value())}).async();
        if (started) co_await context_.send_to_spot(state.partyId, RunStarted{*state.runId}).async();
        co_await broadcast();
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
    fx::task_t<void> advance (Player &actor, fx::message_context_t &, const AdvanceRequest &)
    {
        const auto reason = state.advance();
        if (!reason.empty()) { co_await actor.context().bound_session().send(ActionRejected{reason}).async(); co_return; }
        if (state.phase == RunPhase::Completed) co_await context_.send_to_spot(state.partyId, RunCompleted{*state.runId}).async();
        co_await broadcast();
    }
    fx::task_t<void> broadcast() {
        const auto update = view();
        for (const auto &id : state.joined)
            co_await actors_.send (fx::actor_id_t (id), update).async ();
    }
    fx::task_t<void> enter(Player &actor, fx::message_context_t &, const EnterDungeonRequest &) { co_await context_.send_to_spot(state.partyId, EnterPartyRun{std::string(actor.context().actor_id().value())}).async(); }
    fx::task_t<void> travel(Player &actor, fx::message_context_t &, const JoinDungeon &r) { actor.context().join_spot(r.dungeonId, r).defer(); co_return; }
    fx::task_t<void> reject_delivery(Player &actor, fx::message_context_t &, const ActionRejected &r) { co_await actor.context().bound_session().send(r).async(); }
    fx::task_t<void> party_delivery(Player &actor, fx::message_context_t &, const PartyView &r) { co_await actor.context().bound_session().send(r).async(); }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const DungeonView &update)
    {
        co_await actor.context ().bound_session ().send (update).async ();
    }
};
using DungeonRelocation = StateRelocation<Dungeon, DungeonState>;
}
