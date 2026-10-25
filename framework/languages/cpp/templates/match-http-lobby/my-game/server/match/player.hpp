#pragma once
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>
namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;

  public:
    explicit Player (fx::actor_context_t context) : context_ (std::move (context)) {}
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (std::holds_alternative<fx::actor_join_rejected_t> (completion))
            co_await context_.bound_session ()
              .send (ActionRejected{Rejection::admission_rejected})
              .async ();
        else if (!std::holds_alternative<fx::actor_join_accepted_t> (completion))
            throw std::runtime_error ("Room join failed");
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
  public:
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context));
    }
};
// Actor identity, membership and binding are owned by the Framework.
class PlayerRelocation final : public fx::actor_relocation_adapter_t<Player>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Player &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    fx::task_t<void> restore (Player &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};
class Entry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;

  public:
    explicit Entry (fx::entry_spot_context_t context) : context_ (std::move (context)) {}
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override { context_.handlers ().add_actor_send<&Entry::join> (); }
    fx::task_t<void> join (Player &actor, fx::message_context_t &, const JoinRoomRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value () || request.roomId.empty ())
            throw std::invalid_argument ("Join identity mismatch");
        actor.context ().join_spot (request.roomId, request).defer ();
        co_return;
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
