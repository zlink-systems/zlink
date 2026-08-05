/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <string_view>

namespace zlink::http_client::detail
{

bool starts_with (const std::string &value, const char *prefix);
bool iequals (std::string_view left, std::string_view right);
std::string trim (std::string_view value);

} // namespace zlink::http_client::detail
