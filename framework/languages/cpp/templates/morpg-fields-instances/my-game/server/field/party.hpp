#pragma once
#include "party-domain.hpp"
#include "server/shared/player.hpp"

namespace MyGame
{
class PartySpot final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::actor_client_t &actors_;
    fx::spot_manager_t &spots_;

    fx::task_t<void> broadcast ()
    {
        const auto view = state.view (context_.spot_id ());
        for (const auto &id : state.members)
            co_await actors_.send (fx::actor_id_t (id), view).async ();
    }
    fx::task_t<PartyCommandResult> progress (const std::string &run_id,
                                             const std::optional<std::string> &actor_id,
                                             bool completed)
    {
        const auto rejection = actor_id ? state.arrived (run_id, *actor_id)
                                        : completed ? state.completed (run_id)
                                                    : state.started (run_id);
        if (rejection)
            co_return PartyCommandResult{std::nullopt, rejection};
        co_await broadcast ();
        co_return PartyCommandResult{state.view (context_.spot_id ()), std::nullopt};
    }

  public:
    PartyRun state;
    PartySpot (fx::spot_context_t context, fx::actor_client_t &actors, fx::spot_manager_t &spots) :
        context_ (std::move (context)), actors_ (actors), spots_ (spots)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_handler<&PartySpot::command> ()
          .add_handler<&PartySpot::member_entered> ()
          .add_handler<&PartySpot::run_started> ()
          .add_handler<&PartySpot::run_completed> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fx::message_t &) override
    {
        co_return fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }

    fx::task_t<PartyCommandResult> command (const PartyCommand &request)
    {
        auto rejection = request.join ? state.join (request.actorId)
                        : request.enter ? state.enter (request.actorId)
                        : std::find (state.members.begin (), state.members.end (), request.actorId)
                                != state.members.end ()
                          ? std::optional<ActionRejected>{}
                          : std::optional<ActionRejected>{ActionRejected{Rejection::invalid_party}};
        if (rejection)
            co_return PartyCommandResult{std::nullopt, rejection};
        if (request.enter && state.phase != RunPhase::Active) {
            const auto run_id = *state.runId;
            const auto dungeon_id = Names::dungeon_id (run_id);
            const auto created = co_await spots_.get_or_create (dungeon_id, Names::dungeon)
                                   .in_mesh (Names::mesh)
                                   .async ();
            if (created.state == fx::spot_create_state_t::rejected)
                throw std::runtime_error ("Dungeon creation rejected");
            const auto prepared = co_await context_.request_to_spot (
                                    dungeon_id,
                                    PrepareDungeon{run_id, context_.spot_id (), state.members})
                                    .async<DungeonPrepared> ();
            if (prepared.rejection)
                co_return PartyCommandResult{std::nullopt, prepared.rejection};
            if (prepared.dungeonId != dungeon_id)
                throw std::runtime_error ("Dungeon identity mismatch");
            if (auto failure = state.prepared (prepared.runId))
                co_return PartyCommandResult{std::nullopt, failure};
            for (const auto &id : state.members) {
                if (std::find (state.entered.begin (), state.entered.end (), id) != state.entered.end ())
                    continue;
                co_await actors_.send (fx::actor_id_t (id), JoinDungeon{dungeon_id, run_id}).async ();
            }
        }
        if (request.join || request.enter)
            co_await broadcast ();
        co_return PartyCommandResult{state.view (context_.spot_id ()), std::nullopt};
    }

    fx::task_t<PartyCommandResult> member_entered (const DungeonMemberEntered &request)
    {
        co_return co_await progress (request.runId, request.actorId, false);
    }
    fx::task_t<PartyCommandResult> run_started (const RunStarted &request)
    {
        co_return co_await progress (request.runId, std::nullopt, false);
    }
    fx::task_t<PartyCommandResult> run_completed (const RunCompleted &request)
    {
        co_return co_await progress (request.runId, std::nullopt, true);
    }

    std::vector<std::byte> capture () const
    {
        const auto payload = zlink::message_t::from_json (state);
        return {payload.bytes ().begin (), payload.bytes ().end ()};
    }
    void restore (std::vector<std::byte> payload)
    {
        state = zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<PartyRun> ();
    }
};
class PartyRelocation final : public fx::spot_relocation_adapter_t<PartySpot>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (PartySpot &spot, std::stop_token) override
    {
        co_return spot.capture ();
    }
    fx::task_t<void> restore (PartySpot &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        spot.restore (std::move (payload));
        co_return;
    }
};
}
