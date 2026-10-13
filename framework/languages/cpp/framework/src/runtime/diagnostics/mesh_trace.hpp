/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <cstdlib>
#include <string_view>

namespace zlink::framework::detail
{
inline bool mesh_trace_enabled () noexcept
{
    static const bool enabled = [] {
        const char *value = std::getenv ("ZLINK_CPP_MESH_TRACE");
        return value != nullptr && *value != '\0' && std::string_view (value) != "0";
    }();
    return enabled;
}
} // namespace zlink::framework::detail
