/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstddef>
#include <functional>

namespace zlink::framework::detail
{

// Called once after receive-turn publication and lane scopes have returned.
// The dispatch owner decides
// whether this resource can join the existing completion dispatcher as a worker.
bool dispatch_completion_receive_handoff (const std::function<bool ()> &handoff,
                                          const completion_dispatcher_ref_t &dispatcher = {});

// Internal occupancy query for deterministic dispatcher resource tests.
std::size_t completion_dispatcher_worker_count () noexcept;
// Joins cleanup transferred by the last worker-owned reference. No live owner
// may remain when a deterministic lifecycle test calls this barrier.
void wait_for_completion_dispatcher_shutdown ();

} // namespace zlink::framework::detail
