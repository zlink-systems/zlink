/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <chrono>

namespace zlink::framework::runtime
{
inline constexpr auto auto_connect_wait = std::chrono::seconds (5);
} // namespace zlink::framework::runtime
