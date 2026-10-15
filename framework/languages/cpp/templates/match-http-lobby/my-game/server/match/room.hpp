#pragma once
#include "domain.hpp"
#include "player.hpp"
namespace MyGame
{
class Room final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    RoomView view () const { return {context_.spot_id (), config_.Id, state.view ()}; }
    template <class T> fx::task_t<void> broadcast (const T &message)
    {
        for (const auto &p : state.players)
            co_await actors_.send (fx::actor_id_t (p.view.playerId), message).async ();
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
          .add_actor_send<&Room::reconnect> ()
          .add_actor_send<&Room::get_view> ()
          .add_actor_send<&Room::move> ()
          .add_actor_send<&Room::shoot> ()
          .add_actor_send<&Room::deliver_view> ()
          .add_actor_send<&Room::deliver_end> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        const auto request = payload.decode<JoinRoomRequest> ();
        if (request.playerId != id || request.roomId != context_.spot_id ())
            throw std::invalid_argument ("Room admission identity mismatch");
        co_return state.admit (std::string (id)) ? fx::spot_actor_join_result_t::accept ()
                                                 : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_await broadcast (view ()); }
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
        const auto reason =
          state.move (std::string (actor.context ().actor_id ().value ()), request);
        if (!reason.empty ())
            co_await actor.context ().bound_session ().send (ActionRejected{reason}).async ();
        else
            co_await broadcast (view ());
    }
    fx::task_t<void> shoot (Player &actor, fx::message_context_t &, const ShootRequest &request)
    {
        const auto reason =
          state.shoot (std::string (actor.context ().actor_id ().value ()), request);
        if (!reason.empty ()) {
            co_await actor.context ().bound_session ().send (ShotRejected{reason}).async ();
            co_return;
        }
        co_await broadcast (view ());
        if (state.winner)
            co_await broadcast (MatchEnded{*state.winner});
    }
    fx::task_t<void> deliver_view (Player &actor, fx::message_context_t &, const RoomView &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
    fx::task_t<void> deliver_end (Player &actor, fx::message_context_t &, const MatchEnded &message)
    {
        co_await actor.context ().bound_session ().send (message).async ();
    }
};
class RoomRelocation final : public fx::spot_relocation_adapter_t<Room>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Room &room, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (room.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void> restore (Room &room, std::vector<std::byte> payload, std::stop_token) override
    {
        room.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<RoomState> ();
        co_return;
    }
};
}
