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
const uint64_t zmp_max_body_size = 0xffffffffULL;

//  FLAGS bits
const unsigned char zmp_flag_more = 0x01;
const unsigned char zmp_flag_control = 0x02;
const unsigned char zmp_flag_identity = 0x04;
const unsigned char zmp_flag_subscribe = 0x08;
const unsigned char zmp_flag_cancel = 0x10;
const unsigned char zmp_flag_mask = 0x1f;

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
        case zmp_error_internal:
            return "internal error";
        default:
            return "unknown error";
    }
}

} // namespace zlink

#endif
