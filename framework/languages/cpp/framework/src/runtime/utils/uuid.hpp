/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/detail/binary_text_codec.hpp>

#include <string>
#include <cstddef>

namespace zlink::framework::detail
{


// Returns a lowercase canonical RFC 4122 version 4 UUID.
std::string new_uuid_v4 ();

} // namespace zlink::framework::detail
