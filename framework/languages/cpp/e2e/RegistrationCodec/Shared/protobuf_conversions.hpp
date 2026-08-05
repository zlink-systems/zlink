/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Protobuf codec은 payload를 protobuf wire로 싣는다. 도메인 타입은 아래 변환을 거쳐
 * `registration_codec.proto`가 만든 message로 옮겨진다. */

#include "registration_codec_contracts.hpp"

#include "registration_codec.pb.h"

namespace zlink::framework::e2e::registration_codec
{

namespace pb = ::zlink::e2e::registrationcodec;

inline void to_protobuf (const protobuf_roundtrip_req_t &value, pb::ProtobufRoundtripReq &message)
{
    message.set_value (value.value);
}

inline void from_protobuf (const pb::ProtobufRoundtripReq &message, protobuf_roundtrip_req_t &value)
{
    value.value = message.value ();
}

inline void to_protobuf (const protobuf_roundtrip_res_t &value, pb::ProtobufRoundtripRes &message)
{
    message.set_value (value.value);
    message.set_content_type (value.content_type);
}

inline void from_protobuf (const pb::ProtobufRoundtripRes &message, protobuf_roundtrip_res_t &value)
{
    value.value = message.value ();
    value.content_type = message.content_type ();
}

inline void to_protobuf (const protobuf_codec_msg_t &value, pb::ProtobufCodecMsg &message)
{
    message.set_value (value.value);
}

inline void from_protobuf (const pb::ProtobufCodecMsg &message, protobuf_codec_msg_t &value)
{
    value.value = message.value ();
}

} // namespace zlink::framework::e2e::registration_codec
