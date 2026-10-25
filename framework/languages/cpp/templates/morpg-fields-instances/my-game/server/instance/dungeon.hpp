#pragma once
#include "server/shared/player.hpp"
#include "domain.hpp"
namespace MyGame
{
class Dungeon final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;

  public:
    DungeonRun state;
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
          .add_actor_send<&Dungeon::deliver> ()
          .add_actor_send<&Dungeon::deliver_party> ()
          .add_actor_send<&Dungeon::enter> ()
          .add_actor_send<&Dungeon::create_party> ()
          .add_actor_send<&Dungeon::join_party> ()
          .add_actor_send<&Dungeon::travel> ();
    }
    fx::task_t<DungeonPrepared> prepare (const PrepareDungeon &request)
    {
        const auto rejection = Names::dungeon_id (request.runId) != context_.spot_id ()
                                ? std::optional<ActionRejected> (
                                    ActionRejected{Rejection::run_mismatch})
                                : state.prepare (request);
        co_return DungeonPrepared{context_.spot_id (), request.runId, rejection};
    }
    DungeonView view () const
    {
        return {context_.spot_id (), config_.Id, state.partyId, state.members,
                state.stage, state.runId, state.phase};
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view actor,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinDungeon> ();
        co_return request.dungeonId == context_.spot_id ()
                       && state.can_join (std::string (actor), request.runId)
                 ? fx::spot_actor_join_result_t::accept ()
                 : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        const auto actor_id = std::string (actor.context ().actor_id ().value ());
        state.arrived (actor_id);
        const auto entered = co_await context_
                               .request_to_spot (state.partyId,
                                                 DungeonMemberEntered{state.runId, actor_id})
                               .async<PartyCommandResult> ();
        if (entered.rejection)
            throw std::runtime_error (entered.rejection->reason);
        if (state.phase == RunPhase::Active) {
            const auto started = co_await context_
                                   .request_to_spot (state.partyId, RunStarted{state.runId})
                                   .async<PartyCommandResult> ();
            if (started.rejection)
                throw std::runtime_error (started.rejection->reason);
            const auto current = view ();
            for (const auto &id : state.members)
                co_await actors_.send (fx::actor_id_t (id), current).async ();
        }
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        co_await show (actor);
    }
    fx::task_t<void> query (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await show (actor);
    }
    fx::task_t<void> show (Player &actor)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const DungeonView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> deliver_party (Player &actor, fx::message_context_t &, const PartyView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const EnterDungeonRequest &)
    {
        co_await show (actor);
    }
    fx::task_t<void> reject_town_request (Player &actor)
    {
        co_await actor.context ().bound_session ()
          .send (ActionRejected{Rejection::run_in_progress})
          .async ();
    }
    fx::task_t<void>
    create_party (Player &actor, fx::message_context_t &, const CreatePartyRequest &)
    {
        co_await reject_town_request (actor);
    }
    fx::task_t<void>
    join_party (Player &actor, fx::message_context_t &, const JoinPartyRequest &)
    {
        co_await reject_town_request (actor);
    }
    fx::task_t<void> travel (Player &actor, fx::message_context_t &, const JoinDungeon &request)
    {
        if (request.dungeonId != context_.spot_id ())
            actor.context ().join_spot (request.dungeonId, request).defer ();
        co_return;
    }
    fx::task_t<void> advance (Player &actor, fx::message_context_t &, const AdvanceRequest &)
    {
        if (auto rejection = state.advance ()) {
            co_await actor.context ().bound_session ().send (*rejection).async ();
            co_return;
        }
        if (state.phase == RunPhase::Completed) {
            const auto completed = co_await context_
                                     .request_to_spot (state.partyId, RunCompleted{state.runId})
                                     .async<PartyCommandResult> ();
            if (completed.rejection)
                throw std::runtime_error (completed.rejection->reason);
        }
        const auto current = view ();
        const auto requester = actor.context ().actor_id ().value ();
        co_await actor.context ().bound_session ().send (current).async ();
        for (const auto &id : state.members)
            if (id != requester)
                co_await actors_.send (fx::actor_id_t (id), current).async ();
    }
};
class DungeonRelocation final : public fx::spot_relocation_adapter_t<Dungeon>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Dungeon &spot, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (spot.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Dungeon &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        spot.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<DungeonRun> ();
        co_return;
    }
};
}
