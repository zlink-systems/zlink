#pragma once
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>
#include <limits>
namespace MyGame
{
namespace fx = zlink::framework;
class Player final : public fx::actor_t
{
    fx::actor_context_t context_;
    fx::actor_client_t &actors_;

  public:
    Player (fx::actor_context_t context, fx::actor_client_t &actors) :
        context_ (std::move (context)), actors_ (actors)
    {
    }
    fx::actor_context_t &context () noexcept override { return context_; }
    const fx::actor_context_t &context () const noexcept override { return context_; }
    fx::task_t<void> on_join_completed (const fx::actor_join_completion_t &completion) override
    {
        if (const auto *rejected = std::get_if<fx::actor_join_rejected_t> (&completion)) {
            if (!rejected->reply)
                throw std::runtime_error ("Region rejection omitted reply");
            co_await actors_.send (context_.actor_id (), rejected->reply->decode<RegionFull> ())
              .async ();
        } else if (std::holds_alternative<fx::actor_join_failed_t> (completion))
            throw std::runtime_error ("Region join failed");
    }
};
class PlayerFactory final : public fx::actor_factory_t<Player>
{
    fx::service_provider_t services_;

  public:
    explicit PlayerFactory (fx::service_provider_t services) : services_ (std::move (services)) {}
    fx::task_t<std::shared_ptr<Player>> create (fx::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context),
                                            services_.get_required<fx::actor_client_t> ());
    }
};
// Player has no application state. Framework preserves identity, membership and binding.
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
    fx::spot_manager_t &spots_;
    fx::task_t<void> select (Player &actor, const std::string &region, int index)
    {
        const auto id = Names::region_id (region, index);
        const auto created =
          co_await spots_.get_or_create (id, Names::region).in_mesh (Names::mesh).async ();
        if (created.state == fx::spot_create_state_t::rejected)
            throw std::runtime_error ("Region creation rejected");
        actor.context ().join_spot (id, JoinRegion{region, index}).defer ();
    }

  public:
    Entry (fx::entry_spot_context_t context, fx::spot_manager_t &spots) :
        context_ (std::move (context)), spots_ (spots)
    {
    }
    fx::entry_spot_context_t &context () noexcept override { return context_; }
    const fx::entry_spot_context_t &context () const noexcept override { return context_; }
    void configure () override
    {
        context_.handlers ().add_actor_send<&Entry::enter> ().add_actor_send<&Entry::next> ();
    }
    fx::task_t<void>
    enter (Player &actor, fx::message_context_t &, const EnterRegionRequest &request)
    {
        if (request.playerId != actor.context ().actor_id ().value ())
            throw std::invalid_argument ("Player identity mismatch");
        co_await select (actor, request.region, Names::first_index);
    }
    fx::task_t<void> next (Player &actor, fx::message_context_t &, const RegionFull &reply)
    {
        if (reply.reason != Names::region_full || reply.index == std::numeric_limits<int>::max ())
            throw std::runtime_error ("Invalid region rejection");
        co_await select (actor, reply.region, reply.index + 1);
    }
    fx::task_t<void> on_actor_joined (Player &) override { co_return; }
    fx::task_t<void> on_leave_actor (Player &) override { co_return; }
};
}
