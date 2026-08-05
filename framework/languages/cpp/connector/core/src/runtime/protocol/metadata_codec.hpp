/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zlink::stream_connector::detail
{

inline constexpr std::size_t max_metadata_size = 1024;

class metadata_codec_t
{
  public:
    static std::size_t encoded_size (const metadata_t &metadata);
    static result_t<std::vector<std::uint8_t>> encode (const metadata_t &metadata);
    static result_t<metadata_t> decode (const std::vector<std::uint8_t> &bytes);
};

} // namespace zlink::stream_connector::detail
