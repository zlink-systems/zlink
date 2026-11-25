/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstddef>
namespace zlink::framework::detail
{

completion_dispatcher_borrow_t try_borrow_completion_dispatcher ();

// Internal occupancy query for deterministic dispatcher resource tests.
std::size_t completion_dispatcher_worker_count () noexcept;
std::weak_ptr<completion_dispatcher_t> observe_completion_dispatcher ();

} // namespace zlink::framework::detail
