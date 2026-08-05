/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/compression/lz4_compression_codec.hpp"

#ifdef ZLINK_STREAM_CONNECTOR_WITH_LZ4
#define ZLINK_LZ4_PICKLE_WITH_LZ4
#endif
#include "runtime/protocol/compression/lz4_pickle.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace zlink::stream_connector::detail
{

bool lz4_compression_codec_t::available () noexcept
{
#ifdef ZLINK_STREAM_CONNECTOR_WITH_LZ4
    return true;
#else
    return false;
#endif
}

zlink::message_t lz4_compression_codec_t::compress (const zlink::message_t &payload) const
{
    return zlink::message_t::from (zlink::detail::lz4_pickle::pickle (payload.bytes ()));
}

zlink::message_t
lz4_compression_codec_t::decompress (const zlink::message_t &payload,
                                     std::size_t max_decompressed_size) const
{
    return zlink::message_t::from (
      zlink::detail::lz4_pickle::unpickle (payload.bytes (), max_decompressed_size));
}

} // namespace zlink::stream_connector::detail

namespace zlink::stream_connector
{

std::shared_ptr<const compression_codec_t> lz4_compression_codec ()
{
    static const auto codec = std::make_shared<const detail::lz4_compression_codec_t> ();
    return codec;
}

} // namespace zlink::stream_connector
