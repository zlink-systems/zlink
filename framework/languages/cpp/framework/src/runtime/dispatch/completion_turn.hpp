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
                                          const completion_dispatcher_borrow_t &dispatcher = {});

// Internal occupancy query for deterministic dispatcher resource tests.
std::size_t completion_dispatcher_worker_count () noexcept;
std::weak_ptr<completion_dispatcher_t> observe_completion_dispatcher ();

} // namespace zlink::framework::detail
