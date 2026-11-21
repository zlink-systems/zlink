/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "host_lifecycle_object_message.hpp"
#include <zlink/framework.hpp>
#include <atomic>

namespace zlink::framework::test::host_lifecycle
{
class degraded_object_spot_t;

struct degraded_object_timer_handler_t
{
    zlink::framework::task_t<void> handle (degraded_object_spot_t &,
                                           const zlink::framework::timer_tick_t &)
    {
        timer_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::atomic_int timer_count{0};
};

class degraded_object_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit degraded_object_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&degraded_object_spot_t::on_message> ();
        (void) _context.add_timer<degraded_object_timer_handler_t> ("degraded-object-timer",
                                                                    std::chrono::milliseconds (5));
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override { co_return; }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    void on_message (const degraded_object_message_t &message)
    {
        message_value.store (message.value, std::memory_order_release);
    }

    static inline std::atomic_int factory_count{0};
    static inline std::atomic_int message_value{0};

  private:
    zlink::framework::spot_context_t _context;
};
}
