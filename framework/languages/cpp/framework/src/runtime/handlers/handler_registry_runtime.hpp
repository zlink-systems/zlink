/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <memory>

namespace zlink::framework::detail
{

std::shared_ptr<runtime::offload_executor_t> handler_invocation_executor ();

} // namespace zlink::framework::detail
