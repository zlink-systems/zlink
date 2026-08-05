/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/http_client/contracts/client.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace zlink::http_client::detail
{

std::optional<std::string> find_header (const std::map<std::string, std::string> &headers,
                                        std::string_view name);
void erase_header (std::map<std::string, std::string> &headers, std::string_view name);
std::string gunzip (const std::string &compressed, std::size_t decoded_limit);
std::string inflate_deflate (const std::string &compressed, std::size_t decoded_limit);

} // namespace zlink::http_client::detail
