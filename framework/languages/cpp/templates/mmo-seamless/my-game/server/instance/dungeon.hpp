#pragma once
#include "server/shared/config.hpp"
#include "server/zone/player.hpp"
namespace MyGame
{
class Dungeon final : public fx::spot_t<Player>
{
    fx::spot_context_t context_;
    const NodeConfig &config_;

  public:
    Dungeon (fx::spot_context_t context, const NodeConfig &config) :
        context_ (std::move (context)), config_ (config)
    {
    }
    fx::spot_context_t &context () noexcept override { return context_; }
    const fx::spot_context_t &context () const noexcept override { return context_; }
    void configure () override { context_.handlers ().add_handler<&Dungeon::open> (); }
    fx::task_t<DungeonOpened> open (const OpenDungeonRequest &request)
    {
        co_return DungeonOpened{request.dungeonId, config_.Id};
    }
    fx::task_t<fx::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fx::message_t &) override
    {
        co_return fx::spot_actor_join_result_t::reject ();
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
