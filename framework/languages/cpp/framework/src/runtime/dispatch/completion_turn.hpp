/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstddef>
namespace zlink::framework::detail
{

completion_dispatcher_borrow_t try_borrow_completion_dispatcher ();

class lane_completion_waiter_t
{
  public:
    task_t<void> task () const;
    void complete ();

  private:
    const completion_dispatcher_borrow_t _dispatcher = borrow_completion_dispatcher ();
    task_completion_source_t<void> _completion;
};

// Internal occupancy query for deterministic dispatcher resource tests.
std::size_t completion_dispatcher_worker_count () noexcept;
std::weak_ptr<completion_dispatcher_t> observe_completion_dispatcher ();

} // namespace zlink::framework::detail
