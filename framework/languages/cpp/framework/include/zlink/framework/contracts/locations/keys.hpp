/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace zlink::framework
{

struct location_page_request_t
{
    int page_size = 100;
    std::optional<std::string> continuation_token;
};

template <typename T> struct location_page_t
{
    std::vector<T> items;
    std::optional<std::string> continuation_token;
};

} // namespace zlink::framework
