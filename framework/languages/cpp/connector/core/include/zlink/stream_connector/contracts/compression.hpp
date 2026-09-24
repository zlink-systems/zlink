/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace zlink::stream_connector
{

class compression_codec_t
{
  public:
    virtual ~compression_codec_t () = default;

    virtual std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const = 0;
    virtual std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &payload,
                                                  std::size_t max_decompressed_size) const = 0;
};

std::shared_ptr<const compression_codec_t> lz4_compression_codec ();

} // namespace zlink::stream_connector
