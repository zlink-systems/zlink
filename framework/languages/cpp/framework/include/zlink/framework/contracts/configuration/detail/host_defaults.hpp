/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <chrono>

namespace zlink::framework::detail
{

inline constexpr std::chrono::milliseconds default_stop_timeout{std::chrono::seconds (30)};

} // namespace zlink::framework::detail
