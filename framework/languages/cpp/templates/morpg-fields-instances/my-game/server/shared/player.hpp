#pragma once
#include <optional>
#include <string_view>
#include <type_traits>
#pragma once
#include "player-state.hpp"
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;
    const NodeConfig &config_;

  public:
    template <class Message> fx::task_t<void> deliver (const Message &message, bool visible = true)
    {
        std::optional<std::string_view> source;
        if constexpr (std::is_same_v<Message, BorderVisible>)
            source = message.observerFieldId;

        else if constexpr (std::is_same_v<Message, DungeonView>)
            source = message.dungeonId;
        if (source && (context_.spot_id () != *source || !visible))
            return fx::task_t<void> (fx::result_t<void>::success ());
        return context_.bound_session ().send (message).async ();
    }
    PlayerState state;
    Player (fx::actor_context_t context, const NodeConfig &config) :
        context_ (std::move (context)), config_ (config)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    void move (const EnterField &destination)
    {
        context_.join_spot (Names::field_id (destination.x), destination).defer ();
    }
    fx::task_t<void> send_state ()
    {
        co_await context_.bound_session ()
          .send (WorldState{Names::field_id (state.x), config_.Id, state.x, state.sequence})
          .async ();
    }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (!std::holds_alternative<fx::actor_join_accepted_t> (completion))
            throw std::runtime_error ("Field join failed");
        if (context_.spot_id () == Names::field_id (state.x))
            co_await send_state ();
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
    const NodeConfig &config_;

  public:
    explicit PlayerFactory (const NodeConfig &config) : config_ (config) {}
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context), config_);
    }
};
// This adapter serializes application state, never Framework routing or membership.
class PlayerRelocation final : public fx::actor_relocation_adapter_t<Player>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Player &actor, std::stop_token) override
    {
        const auto payload = fx::serializer_registry_t{}.get<PlayerState> ().serialize (actor.state);
        const auto bytes = payload.bytes ();
        co_return std::vector<std::byte> (bytes.begin (), bytes.end ());
    }
    fx::task_t<void>
    restore (Player &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        actor.state = fx::serializer_registry_t{}.get<PlayerState> ().deserialize (
          fx::encoded_payload_t::from_bytes (std::span<const std::byte> (payload)));
        co_return;
    }
};
}
