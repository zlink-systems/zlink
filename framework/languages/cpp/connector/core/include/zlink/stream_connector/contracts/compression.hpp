/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <cstddef>
#include <memory>

namespace zlink::stream_connector
{

class compression_codec_t
{
  public:
    virtual ~compression_codec_t () = default;

    virtual zlink::message_t compress (const zlink::message_t &payload) const = 0;
    virtual zlink::message_t decompress (const zlink::message_t &payload,
                                         std::size_t max_decompressed_size) const = 0;
};

std::shared_ptr<const compression_codec_t> lz4_compression_codec ();

} // namespace zlink::stream_connector
