/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <chrono>

namespace zlink::framework::runtime::configuration
{

inline constexpr std::chrono::milliseconds default_request_timeout{std::chrono::seconds (30)};

} // namespace zlink::framework::runtime::configuration
