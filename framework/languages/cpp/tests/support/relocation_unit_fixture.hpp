/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/framework.hpp>

namespace zlink::framework::test
{
class relocation_unit_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit relocation_unit_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override {}

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

  private:
    zlink::framework::spot_context_t _context;
};

}
