#pragma once
#include "domain.hpp"
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
    PlayerState state;
    Player (fx::actor_context_t context, const NodeConfig &config) :
        context_ (std::move (context)), config_ (config)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    void move (const EnterZone &destination)
    {
        context_.join_spot (Names::zone_id (destination.x), destination).defer ();
    }
    fx::task_t<void> send_state ()
    {
        co_await context_.bound_session ()
          .send (WorldState{Names::zone_id (state.x), config_.Id, state.x, state.sequence})
          .async ();
    }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (!std::holds_alternative<fx::actor_join_accepted_t> (completion))
            throw std::runtime_error ("Zone join failed");
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
        const auto payload = zlink::message_t::from_json (actor.state);
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Player &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        actor.state =
          zlink::message_t::from (std::span<const std::byte> (payload)).parse_json<PlayerState> ();
        co_return;
    }
};
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
        actor.move (EnterZone{request.x, 0});
        co_return;
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
