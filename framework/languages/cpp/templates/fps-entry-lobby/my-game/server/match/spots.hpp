#pragma once
#include "player.hpp"
#include "matchmaking.hpp"
#include "server/shared/config.hpp"
namespace MyGame
{
class Matchmaking final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    fx::spot_manager_t &spots_;
    fx::actor_client_t &actors_;
    fx::task_t<void> match ()
    {
        const auto pair = state.pair ();
        if (pair.empty ())
            co_return;
        const auto room = state.allocate_room ();
        const auto created =
          co_await spots_.get_or_create (room, Names::room).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Room creation rejected");
        state.remove (pair);
        for (const auto &p : pair)
            co_await actors_.send (fx::actor_id_t (p.playerId), RoomAssignment{room}).async ();
    }

  public:
    MatchmakingState state;
    Matchmaking (fx::spot_context_t context,
                 fx::spot_manager_t &spots,
                 fx::actor_client_t &actors) :
        context_ (std::move (context)), spots_ (spots), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Matchmaking::queue> ()
          .add_actor_send<&Matchmaking::assignment> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<QueueRequest> ();
        if (request.playerId != id)
            throw std::invalid_argument ("Queue identity mismatch");
        state.enqueue (request);
        co_return fx::spot_actor_join_result_t::accept ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await match (); }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> queue (Player &actor, fx::message_context_t &, const QueueRequest &request)
    {
        check_identity (actor, request);
        state.enqueue (request);
        co_await match ();
    }
    fx::task_t<void>
    assignment (Player &actor, fx::message_context_t &, const RoomAssignment &request)
    {
        actor.context ()
          .join_spot (
            request.roomId,
            RoomAdmission{std::string (actor.context ().actor_id ().value ()), request.roomId})
          .defer ();
        co_return;
    }
};
class Room final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    RoomView view () const
    {
        return {std::string (context_.spot_id ()), config_.Id, state.view (), state.phase (),
                state.winner ()};
    }
    template <class T> fx::task_t<void> broadcast (const T &message)
    {
        for (const auto &p : state.players ())
            co_await actors_.send (fx::actor_id_t (p.playerId), message).async ();
    }

  public:
    RoomState state;
    Room (fx::spot_context_t context, const NodeConfig &config, fx::actor_client_t &actors) :
        context_ (std::move (context)), config_ (config), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Room::queue> ()
          .add_actor_send<&Room::send_view> ()
          .add_actor_send<&Room::move> ()
          .add_actor_send<&Room::shoot> ()
          .add_actor_send<&Room::deliver_view> ()
          .add_actor_send<&Room::deliver_found> ()
          .add_actor_send<&Room::deliver_ended> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<RoomAdmission> ();
        if (request.playerId != id || request.roomId != context_.spot_id ())
            throw std::invalid_argument ("Room admission identity mismatch");
        co_return state.admit (request.playerId) ? fx::spot_actor_join_result_t::accept ()
                                                 : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        co_await actors_
          .send (actor.context ().actor_id (), MatchFound{std::string (context_.spot_id ())})
          .async ();
        co_await broadcast (view ());
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> queue (Player &actor, fx::message_context_t &, const QueueRequest &request)
    {
        check_identity (actor, request);
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> send_view (Player &actor, fx::message_context_t &, const RoomViewRequest &)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        if (auto reason = state.move (actor.context ().actor_id ().value (), request))
            co_await actor.context ().bound_session ().send (*reason).async ();
        else {
            const auto player = std::find_if (state.players ().begin (), state.players ().end (),
                                              [&] (const auto &candidate) {
                                                  return candidate.playerId
                                                         == actor.context ().actor_id ().value ();
                                              });
            co_await actor.context ().bound_session ()
              .send (MoveResult{player->position, request.sequence})
              .async ();
            co_await broadcast (view ());
        }
    }
    fx::task_t<void> shoot (Player &actor, fx::message_context_t &, const ShootRequest &request)
    {
        const auto decision = state.shoot (actor.context ().actor_id ().value (), request);
        if (decision.rejected) {
            co_await actor.context ().bound_session ().send (*decision.rejected).async ();
            co_return;
        }
        co_await actor.context ().bound_session ().send (*decision.result).async ();
        co_await broadcast (view ());
        if (state.winner ())
            co_await broadcast (MatchEnded{*state.winner ()});
    }
    fx::task_t<void> deliver_view (Player &actor, fx::message_context_t &, const RoomView &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void>
    deliver_found (Player &actor, fx::message_context_t &, const MatchFound &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void>
    deliver_ended (Player &actor, fx::message_context_t &, const MatchEnded &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
};
// Only application state crosses this adapter; Framework restores membership and routing.
template <class Spot, class State>
class StateRelocation final : public fx::spot_relocation_adapter_t<Spot>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Spot &spot, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (spot.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Spot &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        spot.state = zlink::message_t::from (std::span<const std::byte> (payload))
                       .template parse_json<State> ();
        co_return;
    }
};
using MatchmakingRelocation = StateRelocation<Matchmaking, MatchmakingState>;
class RoomRelocation final : public fx::spot_relocation_adapter_t<Room>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Room &spot, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (spot.state.capture ());
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Room &spot, std::vector<std::byte> payload, std::stop_token) override
    {
        const auto snapshot = zlink::message_t::from (std::span<const std::byte> (payload))
                                .template parse_json<RoomSnapshot> ();
        spot.state.restore (snapshot);
        co_return;
    }
};
}
