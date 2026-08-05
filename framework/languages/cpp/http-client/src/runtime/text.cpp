/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/text.hpp"

#include <algorithm>
#include <cctype>

namespace zlink::http_client::detail
{

bool starts_with (const std::string &value, const char *prefix)
{
    return value.rfind (prefix, 0) == 0;
}

bool iequals (std::string_view left, std::string_view right)
{
    return left.size () == right.size ()
           && std::equal (left.begin (), left.end (), right.begin (), [] (char a, char b) {
                  return std::tolower (static_cast<unsigned char> (a))
                         == std::tolower (static_cast<unsigned char> (b));
              });
}

std::string trim (std::string_view value)
{
    const auto begin = value.find_first_not_of (" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of (" \t");
    return std::string (value.substr (begin, end - begin + 1));
}

} // namespace zlink::http_client::detail
