#pragma once
#include "player.hpp"
namespace MyGame
{
class Zone final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;
    fx::actor_client_t &actors_;
    PlayerView view (const Player &actor) const
    {
        return {config_.WorldId, Names::channel_of (context_.spot_id ()), config_.Id,
                actor.gold.value ()};
    }
    fx::task_t<void> show (Player &actor)
    {
        co_await actor.context ().bound_session ().send (view (actor)).async ();
    }

  public:
    Zone (fx::spot_context_t context, NodeConfig &config, fx::actor_client_t &actors) :
        context_ (std::move (context)), config_ (config), actors_ (actors)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&Zone::enter> ()
          .add_actor_send<&Zone::change> ()
          .add_actor_send<&Zone::earn> ()
          .add_actor_send<&Zone::read> ()
          .add_actor_send<&Zone::deliver> ();
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fx::message_t &payload) override
    {
        co_return payload.decode<EnterWorldRequest> ().playerId == id
          ? fx::spot_actor_join_result_t::accept ()
          : fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &actor) override
    {
        co_await actors_.send (actor.context ().actor_id (), view (actor)).async ();
    }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
    fx::task_t<void> on_disconnect_actor (Player &) override { co_return; }
    fx::task_t<void>
    enter (Player &actor, fx::message_context_t &, const EnterWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await show (actor);
    }
    fx::task_t<void>
    change (Player &actor, fx::message_context_t &, const ChangeChannelRequest &request)
    {
        actor.context ()
          .join_spot (Names::zone_id (request.channel),
                      EnterWorldRequest{std::string (actor.context ().actor_id ().value ())})
          .defer ();
        co_return;
    }
    fx::task_t<void> earn (Player &actor, fx::message_context_t &, const EarnRequest &request)
    {
        actor.earn (request.amount);
        co_await show (actor);
    }
    fx::task_t<void> read (Player &actor, fx::message_context_t &, const ViewRequest &)
    {
        co_await show (actor);
    }
    fx::task_t<void> deliver (Player &actor, fx::message_context_t &, const PlayerView &event)
    {
        co_await actor.context ().bound_session ().send (event).async ();
    }
};
// Framework owns membership and Spot identity. Zone has no application state to transfer.
class ZoneRelocation final : public fx::spot_relocation_adapter_t<Zone>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Zone &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    fx::task_t<void> restore (Zone &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};
}
