/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "host_lifecycle_actor_fixture.hpp"
#include "host_lifecycle_object_message.hpp"
#include "host_lifecycle_relocation_ready_fixture.hpp"

namespace zlink::framework::test::host_lifecycle
{
class remote_create_entry_spot_t final
    : public zlink::framework::entry_spot_t<configuration_actor_t>
{
  public:
    explicit remote_create_entry_spot_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_send<&remote_create_entry_spot_t::on_probe> (
          "remote-create-probe");
        _context.handlers ().add_actor_request<&remote_create_entry_spot_t::on_echo> ();
        _context.handlers ().add_actor_send<&remote_create_entry_spot_t::on_relocation_join> ();
    }

    zlink::framework::task_t<zlink::framework::actor_create_response_t>
    on_create_actor (configuration_actor_t &, const zlink::framework::message_t &) override
    {
        created_count.fetch_add (1, std::memory_order_acq_rel);
        co_return zlink::framework::actor_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_actor_joined (configuration_actor_t &) override
    {
        joined_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (configuration_actor_t &) override { co_return; }

    zlink::framework::task_t<void> on_disconnect_actor (configuration_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_probe (configuration_actor_t &,
                                             zlink::framework::message_context_t &,
                                             const relocation_ready_message_t &)
    {
        co_return;
    }

    degraded_object_message_t on_echo (configuration_actor_t &,
                                       zlink::framework::message_context_t &,
                                       const degraded_object_message_t &request)
    {
        return degraded_object_message_t{request.value + 1};
    }

    void on_relocation_join (configuration_actor_t &actor,
                             zlink::framework::message_context_t &,
                             const relocation_ready_message_t &)
    {
        actor.context ().join_spot (zlink::framework::spot_id_t ("relocated-room")).defer ();
    }

    static inline std::atomic_int created_count{0};
    static inline std::atomic_int joined_count{0};

  private:
    zlink::framework::entry_spot_context_t _context;
};
}
