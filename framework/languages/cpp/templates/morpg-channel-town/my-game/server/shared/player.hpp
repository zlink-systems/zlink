#pragma once
#include "config.hpp"
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
    // Decide synchronously because source IDs may be temporary name values.
    template <class Message>
    fx::task_t<void> deliver (const std::string &sourceSpotId, const Message &message)
    {
        if (context_.spot_id () != sourceSpotId)
            return fx::task_t<void> (fx::result_t<void>::success ());
        return context_.bound_session ().send (message).async ();
    }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (std::holds_alternative<fx::actor_join_rejected_t> (completion))
            co_await context_.bound_session ()
              .send (ActionRejected{Rejection::join_rejected})
              .async ();
        else if (!std::holds_alternative<fx::actor_join_accepted_t> (completion))
            throw std::runtime_error (
              "Actor join failed: "
              + std::to_string (
                static_cast<int> (std::get<fx::actor_join_failed_t> (completion).error_kind)));
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
// Identity, routes and membership belong to Framework; Player has no application state.
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
class TownEntry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;

  public:
    explicit TownEntry (fx::entry_spot_context_t context) : context_ (std::move (context)) {}
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ()
          .add_actor_send<&TownEntry::enter> ()
          .add_actor_send<&TownEntry::pending> ();
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const JoinTownRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        actor.context ().join_spot (Names::town_id (request.channel), request).defer ();
        co_return;
    }
    fx::task_t<void> pending (Player &actor, fx::message_context_t &, const TownView &message)
    {
        return actor.deliver (message.townId, message);
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
