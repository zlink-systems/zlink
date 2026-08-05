/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

struct await_actor_t : zlink::framework::actor_t
{
    explicit await_actor_t (zlink::framework::actor_context_t value) :
        actor_id (value.actor_ref ().actor_id ().value ()),
        actor_ref (value.actor_ref ()),
        _actor_context (std::move (value))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override
    { return _actor_context; }
    const zlink::framework::actor_context_t &context () const noexcept override
    { return _actor_context; }

    std::string actor_id;
    std::string join_request_id;
    zlink::framework::actor_ref_t actor_ref;
    zlink::framework::actor_context_t _actor_context;
};

struct await_actor_factory_t final
    : zlink::framework::actor_factory_t<await_actor_t>
{
    zlink::framework::task_t<std::shared_ptr<await_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<await_actor_t> (
          std::move (context));
    }
};

class await_probe_spot_t;

struct await_timer_handler_t
{
    zlink::framework::task_t<void>
    handle (await_probe_spot_t &spot, const zlink::framework::timer_tick_t &tick) const;
};

struct await_timer_state_t
{
    std::string request_id;
    std::string timer_name;
    std::string mode;
    int delay_ms = 0;
    std::uint64_t tick_count = 0;
    bool active = true;
    zlink::framework::timer_t timer;
};

struct await_timer_tick_state_t
{
    std::string request_id;
    std::string timer_name;
    std::string mode;
    int delay_ms = 0;
    std::uint64_t tick_number = 0;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
