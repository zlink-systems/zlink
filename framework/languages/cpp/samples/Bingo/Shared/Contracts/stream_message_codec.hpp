/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "messages.hpp"

#include <zlink/codecs/protobuf.hpp>

namespace zlink::samples::bingo
{

inline zlink::message_t encode_authenticate_response (const authenticate_res_t &response)
{
    return zlink::message_t::from (
      zlink::stream_connector::codecs::codec_traits<authenticate_res_t>::encode (response));
}

} // namespace zlink::samples::bingo
