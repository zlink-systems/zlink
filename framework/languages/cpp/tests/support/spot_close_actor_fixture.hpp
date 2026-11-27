/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework.hpp>

namespace zlink::framework::test::spot_close
{
namespace zf = zlink::framework;

class member_actor_t final : public zf::actor_t
{
  public:
    explicit member_actor_t (zf::actor_context_t context) : _context (std::move (context)) {}
    zf::actor_context_t &context () noexcept override { return _context; }
    const zf::actor_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_join_completed (const zf::actor_join_completion_t &completion) override;

  private:
    zf::actor_context_t _context;
};
} // namespace zlink::framework::test::spot_close
