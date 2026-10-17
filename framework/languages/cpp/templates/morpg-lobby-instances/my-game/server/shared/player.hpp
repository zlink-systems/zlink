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
        else if (const auto *failed = std::get_if<fx::actor_join_failed_t> (&completion))
            throw std::runtime_error ("Actor join failed: "
                                      + std::to_string (static_cast<int> (failed->error_kind)));
        co_return;
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
}
