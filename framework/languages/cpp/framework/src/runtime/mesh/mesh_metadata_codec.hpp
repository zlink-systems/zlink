/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::detail
{

class mesh_metadata_codec_t
{
  public:
    static bool add_field13 (std::size_t &size, std::string_view key, std::string_view value);

    static std::vector<std::uint8_t> encode (const std::map<std::string, std::string> &metadata);

    static bool decode (const std::vector<std::uint8_t> &encoded,
                        std::map<std::string, std::string> &metadata);
};

} // namespace zlink::framework::detail
