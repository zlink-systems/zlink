/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <functional>

namespace zlink::framework::detail
{

// Called once after receive-turn publication and lane scopes have returned.
// The dispatch owner decides
// whether this resource can join the existing completion dispatcher as a worker.
bool dispatch_completion_receive_handoff (const std::function<bool ()> &handoff);

} // namespace zlink::framework::detail
