/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_PROTOCOL_HPP_INCLUDED__
#define __ZLINK_ZMP_PROTOCOL_HPP_INCLUDED__

#include <stddef.h>
#include <stdint.h>

namespace zlink
{
const unsigned char zmp_magic = 0x5a;
const unsigned char zmp_version = 0x01;

const size_t zmp_header_size = 8;
const size_t zmp_request_sequence_size = 8;
const size_t zmp_request_reply_header_size =
  zmp_header_size + zmp_request_sequence_size;
const uint64_t zmp_max_body_size = 0xffffffffULL;
// CONTROL frames bypass the negotiated Application payload limit, but retain
// a small independent allocation bound. This covers READY metadata and all
// current post-handshake commands (WEIGHT/FLOWSTATE) without turning CONTROL
// into a max-message-size bypass for arbitrary peer allocations.
const uint32_t zmp_max_control_body_size = 4096;

//  DATA frame kinds. Request-reply kinds carry one 64-bit big-endian
//  sequence extension immediately after the common 8-byte header.
enum zmp_message_kind_t
{
    zmp_kind_data = 0x00,
    zmp_kind_request = 0x01,
    zmp_kind_reply = 0x02,
    zmp_kind_error_reply = 0x03
};

inline bool zmp_is_request_reply_kind (unsigned char kind_)
{
    return kind_ == zmp_kind_request || kind_ == zmp_kind_reply
           || kind_ == zmp_kind_error_reply;
}

//  FLAGS bits
const unsigned char zmp_flag_more = 0x01;
const unsigned char zmp_flag_control = 0x02;
const unsigned char zmp_flag_identity = 0x04;
const unsigned char zmp_flag_subscribe = 0x08;
const unsigned char zmp_flag_cancel = 0x10;
const unsigned char zmp_flag_mask = 0x1f;
const unsigned char zmp_special_frame_flags =
  zmp_flag_control | zmp_flag_identity | zmp_flag_subscribe
  | zmp_flag_cancel;

inline bool zmp_is_special_frame (unsigned char flags_)
{
    return (flags_ & zmp_special_frame_flags) != 0;
}

//  Control Frame Types
const unsigned char zmp_control_hello = 0x01;
const unsigned char zmp_control_ready = 0x04;
const unsigned char zmp_control_error = 0x05;

//  Error Codes
const uint8_t zmp_error_invalid_magic = 0x01;
const uint8_t zmp_error_version_mismatch = 0x02;
const uint8_t zmp_error_flags_invalid = 0x03;
const uint8_t zmp_error_body_too_large = 0x04;
const uint8_t zmp_error_socket_type_mismatch = 0x05;
const uint8_t zmp_error_handshake_timeout = 0x06;
const uint8_t zmp_error_kind_invalid = 0x07;
const uint8_t zmp_error_sequence_invalid = 0x08;
const uint8_t zmp_error_multipart_invalid = 0x09;
const uint8_t zmp_error_frame_incomplete = 0x0a;
const uint8_t zmp_error_internal = 0x7f;

inline const char *zmp_error_reason (uint8_t code_)
{
    switch (code_) {
        case zmp_error_invalid_magic:
            return "invalid magic";
        case zmp_error_version_mismatch:
            return "version mismatch";
        case zmp_error_flags_invalid:
            return "flags invalid";
        case zmp_error_body_too_large:
            return "body too large";
        case zmp_error_socket_type_mismatch:
            return "socket type mismatch";
        case zmp_error_handshake_timeout:
            return "handshake timeout";
        case zmp_error_kind_invalid:
            return "message kind invalid";
        case zmp_error_sequence_invalid:
            return "request sequence invalid";
        case zmp_error_multipart_invalid:
            return "multipart frame invalid";
        case zmp_error_frame_incomplete:
            return "frame incomplete";
        case zmp_error_internal:
            return "internal error";
        default:
            return "unknown error";
    }
}

} // namespace zlink

#endif
