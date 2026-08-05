/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "routing_id.hpp"

#include <string>

namespace zlink
{

void version (int &major_, int &minor_, int &patch_);
const char *error_text (int errnum_) noexcept;
bool has (const std::string &capability_);

} // namespace zlink
