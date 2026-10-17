/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <cstddef>

namespace zlink::framework::runtime
{

namespace location_store_limits
{
inline constexpr std::size_t key_bytes = 1024;
inline constexpr std::size_t write_keys = 2048;
inline constexpr std::size_t value_bytes = 1024u * 1024u;
inline constexpr std::size_t write_bytes = 4u * 1024u * 1024u;
}

}
