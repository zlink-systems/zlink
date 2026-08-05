/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/compression.hpp>

#include <cstddef>

namespace zlink::stream_connector::detail
{

class lz4_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    static bool available () noexcept;
    zlink::message_t compress (const zlink::message_t &payload) const override;
    zlink::message_t decompress (const zlink::message_t &payload,
                                 std::size_t max_decompressed_size) const override;
};

} // namespace zlink::stream_connector::detail
