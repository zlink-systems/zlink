/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

class mesh_metadata_codec_t
{
  public:
    static std::vector<std::uint8_t>
    encode (const std::map<std::string, std::string> &metadata);

    static bool decode (const std::vector<std::uint8_t> &encoded,
                        std::map<std::string, std::string> &metadata);
};

} // namespace zlink::framework::detail
