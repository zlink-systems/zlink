#pragma once
#include "player.hpp"
namespace MyGame
{
class WorldEntry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;

  public:
    explicit WorldEntry (fx::entry_spot_context_t context) : context_ (std::move (context)) {}
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override { context_.handlers ().add_actor_send<&WorldEntry::enter> (); }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        actor.move (EnterField{request.x, 0});
        co_return;
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
