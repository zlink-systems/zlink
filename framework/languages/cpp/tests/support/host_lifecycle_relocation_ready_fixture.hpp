/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework.hpp>
#include <atomic>
#include <memory>

namespace zlink::framework::test::host_lifecycle
{
struct relocation_ready_message_t
{
    static constexpr const char *packet_name = "relocation-ready";
    int value = 0;
};
}

template <>
struct zlink::json::dto_traits<zlink::framework::test::host_lifecycle::relocation_ready_message_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::host_lifecycle::relocation_ready_message_t::value)};
};

namespace zlink::framework::test::host_lifecycle
{
class relocation_ready_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit relocation_ready_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&relocation_ready_spot_t::on_ready> ();
    }

    zlink::framework::task_t<void> on_ready (const relocation_ready_message_t &)
    {
        if (readiness_release) {
            readiness_entered->complete (zlink::framework::result_t<void>::success ());
            co_await readiness_release->task ();
        }
        _context.relocation_ready ().defer ();
        co_return;
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

    zlink::framework::task_t<void> on_relocation_ready_completed (
      const zlink::framework::spot_relocation_ready_completion_t &completion) override
    {
        last_outcome.store (static_cast<int> (completion.outcome), std::memory_order_release);
        completion_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>>
      readiness_entered;
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>>
      readiness_release;
    static inline std::atomic_int completion_count{0};
    static inline std::atomic_int last_outcome{-1};

  private:
    zlink::framework::spot_context_t _context;
};

}
