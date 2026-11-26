/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <functional>
#include <memory>
#include <string>

namespace zlink::framework::detail
{
struct spot_context_state_t;
}

namespace m6c_test
{

bool run_serial_turn (const std::shared_ptr<zlink::framework::detail::spot_context_state_t> &state,
                      std::string name,
                      std::function<void ()> work);

} // namespace m6c_test
