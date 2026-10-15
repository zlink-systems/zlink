#pragma once
#include "domain.hpp"
#include "data.hpp"
#include <zlink/framework.hpp>
#include <iostream>
namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;
    const PlayerData &data_;

  public:
    GoldBalance gold;
    Player (fx::actor_context_t context, const PlayerData &data, std::int64_t initial) :
        context_ (std::move (context)), data_ (data), gold (initial)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    void earn (std::int64_t amount)
    {
        const auto next = gold.after_earning (amount);
        data_.save (context_.actor_id ().value (), next);
        gold.restore (next);
    }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (const auto *failed = std::get_if<fx::actor_join_failed_t> (&completion))
            throw fx::framework_exception_t (
              failed->error_kind,
              "Zone join failed: " + std::to_string (static_cast<int> (failed->error_kind)));
        if (std::holds_alternative<fx::actor_join_rejected_t> (completion))
            throw fx::framework_exception_t (fx::framework_error_kind_t::rejected,
                                             "Zone join rejected");
        co_return;
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
    PlayerData data_;
    const NodeConfig &config_;

  public:
    explicit PlayerFactory (const NodeConfig &config) : data_ (config), config_ (config) {}
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        const auto id = std::string (context.actor_id ().value ());
        auto player = std::make_shared<Player> (std::move (context), data_, data_.load (id));
        std::cout << Control::player_created
                  << nlohmann::json{{"actorId", id}, {"nodeId", config_.Id}}.dump () << std::endl;
        co_return player;
    }
};
class PlayerRelocation final : public fx::actor_relocation_adapter_t<Player>
{
  public:
    fx::task_t<std::vector<std::byte>> capture (Player &actor, std::stop_token) override
    {
        const auto payload = zlink::message_t::from_json (actor.gold.value ());
        co_return std::vector<std::byte> (payload.bytes ().begin (), payload.bytes ().end ());
    }
    fx::task_t<void>
    restore (Player &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        actor.gold.restore (zlink::message_t::from (std::span<const std::byte> (payload))
                              .parse_json<std::int64_t> ());
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
    fx::task_t<void>
    enter (Player &actor, fx::message_context_t &, const EnterWorldRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        actor.context ().join_spot (Names::zone_id (Names::first_channel), request).defer ();
        co_return;
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
