/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/streams/stream.hpp>

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_LZ4
#define ZLINK_LZ4_PICKLE_WITH_LZ4
#endif
#include "../../../../connector/core/src/runtime/protocol/compression/lz4_pickle.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework
{

namespace
{

class lz4_stream_compression_codec_t final : public stream_compression_codec_t
{
  public:
    zlink::message_t compress (const zlink::message_t &payload) const override
    {
        return zlink::message_t::from (zlink::detail::lz4_pickle::pickle (payload.bytes ()));
    }

    zlink::message_t decompress (const zlink::message_t &payload,
                                 std::size_t max_decompressed_size) const override
    {
        return zlink::message_t::from (
          zlink::detail::lz4_pickle::unpickle (payload.bytes (), max_decompressed_size));
    }
};

} // namespace

std::shared_ptr<const stream_compression_codec_t> lz4_stream_compression_codec ()
{
    static const auto codec = std::make_shared<const lz4_stream_compression_codec_t> ();
    return codec;
}

} // namespace zlink::framework
