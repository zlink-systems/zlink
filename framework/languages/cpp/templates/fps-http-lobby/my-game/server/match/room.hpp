#pragma once
#include "domain.hpp"
#include "player.hpp"
#include "roster.hpp"
namespace MyGame
{
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
        for (const auto &player : state.players ())
            co_await actors_.send (fx::actor_id_t (player.playerId), message).async ();
    }

  public:
    RoomState state;
    RoomRoster roster;
    Room (fx::spot_context_t context, const NodeConfig &config, fx::actor_client_t &actors) :
        context_ (std::move (context)), config_ (config), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_handler<&Room::prepare> ()
          .add_actor_send<&Room::reconnect> ()
          .add_actor_send<&Room::get_view> ()
          .add_actor_send<&Room::move> ()
          .add_actor_send<&Room::shoot> ()
          .add_actor_send<&Room::deliver_view> ()
          .add_actor_send<&Room::deliver_found> ()
          .add_actor_send<&Room::deliver_ended> ();
    }
    fx::task_t<RoomPrepared> prepare (const PrepareRoom &request)
    {
        co_return roster.prepare (context_.spot_id (), request);
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinRoomRequest> ();
        if (request.playerId != id || request.roomId != context_.spot_id ())
            throw std::invalid_argument ("Room admission identity mismatch");
        co_return roster.can_admit (id) && state.admit (std::string (id))
          ? fx::spot_actor_join_result_t::accept ()
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
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void>
    reconnect (Player &actor, fx::message_context_t &, const JoinRoomRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ()
            || request.roomId != context_.spot_id ())
            throw std::invalid_argument ("Reconnect identity mismatch");
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> get_view (Player &actor, fx::message_context_t &, const RoomViewRequest &)
    {
        co_await actor.context ().bound_session ().send (view ()).async ();
    }
    fx::task_t<void> move (Player &actor, fx::message_context_t &, const MoveRequest &request)
    {
        const auto id = std::string (actor.context ().actor_id ().value ());
        if (auto reason = state.move (id, request))
            co_await actor.context ().bound_session ().send (*reason).async ();
        else {
            const auto found = std::find_if (state.players ().begin (), state.players ().end (),
                                             [&] (const auto &player) {
                                                 return player.playerId == id;
                                             });
            co_await actor.context ().bound_session ()
              .send (MoveResult{found->position, request.sequence})
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
    fx::task_t<void> deliver_found (Player &actor, fx::message_context_t &, const MatchFound &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void>
    deliver_ended (Player &actor, fx::message_context_t &, const MatchEnded &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
};

class RoomRelocation final : public fx::spot_relocation_adapter_t<Room>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Room &room, std::stop_token) override
    {
        const HttpRoomSnapshot snapshot{room.state.capture (), room.roster.capture ()};
        const auto payload = zlink::message_t::from_json (snapshot);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Room &room, std::vector<std::byte> payload, std::stop_token) override
    {
        const auto snapshot = zlink::message_t::from (std::span<const std::byte> (payload))
                                .template parse_json<HttpRoomSnapshot> ();
        room.roster.restore (snapshot.roster);
        room.state.restore (snapshot.game);
        co_return;
    }
};
}
