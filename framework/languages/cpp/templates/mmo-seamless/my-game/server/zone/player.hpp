#pragma once
#include <optional>
#include <string_view>
#include <type_traits>
#pragma once
#include "domain.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;

  public:
    template <class Message> fx::task_t<void> deliver (const Message &message, bool visible = true)
    {
        std::optional<std::string_view> source;
        if constexpr (std::is_same_v<Message, PlayerAppeared>)
            source = message.sourceSpotId;
        else if constexpr (std::is_same_v<Message, PlayerMoved>)
            source = message.sourceSpotId;
        else if constexpr (std::is_same_v<Message, PlayerLeft>)
            source = message.sourceSpotId;
        if (source && (context_.spot_id () != *source || !visible))
            return fx::task_t<void> (fx::result_t<void>::success ());
        return context_.bound_session ().send (message).async ();
    }
    explicit Player (fx::actor_context_t context) : context_ (std::move (context)) {}
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    void move (const EnterZone &destination)
    {
        context_.join_spot (Names::zone_id (destination.position.x), destination).defer ();
    }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (const auto *accepted = std::get_if<fx::actor_join_accepted_t> (&completion)) {
            if (!accepted->reply)
                throw std::runtime_error ("Zone admission returned no WorldState");
            const auto state = accepted->reply->decode<WorldState> ();
            co_await context_.bound_session ().send (state).async ();
            co_return;
        }
        if (const auto *rejected = std::get_if<fx::actor_join_rejected_t> (&completion);
            rejected && rejected->reply) {
            const auto reason = rejected->reply->decode<ActionRejected> ();
            co_await context_.bound_session ().send (reason).async ();
            co_return;
        }
        throw std::runtime_error ("Zone join failed");
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
  public:
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context));
    }
};
class WorldEntry final : public fx::entry_spot_t<Player>
{
    fx::entry_spot_context_t context_;

  public:
    explicit WorldEntry (fx::entry_spot_context_t context) : context_ (std::move (context)) {}
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ().add_actor_send<&WorldEntry::notification_PlayerAppeared> ();
        context_.handlers ().add_actor_send<&WorldEntry::notification_PlayerMoved> ();
        context_.handlers ().add_actor_send<&WorldEntry::notification_PlayerLeft> ();
        context_.handlers ().add_actor_send<&WorldEntry::enter> ();
    }
    fx::task_t<void> enter (Player &actor, fx::message_context_t &, const JoinWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player does not match bound Actor");
        const auto location = ZonePositionDomain::resolve_location (request.x);
        if (location.rejection) {
            co_await actor.context ().bound_session ().send (*location.rejection).async ();
            co_return;
        }
        actor.move (EnterZone{Position{request.x}, 0, 0});
        co_return;
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }

    fx::task_t<void> notification_PlayerAppeared (Player &actor,
                                                  fx::message_context_t &,
                                                  const PlayerAppeared &message)
    {
        return actor.deliver (message);
    }

    fx::task_t<void>
    notification_PlayerMoved (Player &actor, fx::message_context_t &, const PlayerMoved &message)
    {
        return actor.deliver (message);
    }

    fx::task_t<void>
    notification_PlayerLeft (Player &actor, fx::message_context_t &, const PlayerLeft &message)
    {
        return actor.deliver (message);
    }
};
}
