/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::framework::detail
{

// Returns a lowercase canonical RFC 4122 version 4 UUID.
std::string new_uuid_v4 ();

} // namespace zlink::framework::detail
