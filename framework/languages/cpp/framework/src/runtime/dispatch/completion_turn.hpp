/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstddef>
namespace zlink::framework::detail
{

// C1's E5 caller turn may use the existing receive-role handoff. Internal task
// and management completions keep dispatch_completion's independent progress.
void dispatch_request_completion (std::function<void ()> work,
                                  const void *owner,
                                  completion_dispatcher_borrow_t dispatcher = {});

// Internal occupancy query for deterministic dispatcher resource tests.
std::size_t completion_dispatcher_worker_count () noexcept;
std::weak_ptr<completion_dispatcher_t> observe_completion_dispatcher ();

} // namespace zlink::framework::detail
