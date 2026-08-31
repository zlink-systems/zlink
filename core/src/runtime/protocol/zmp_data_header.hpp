/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_DATA_HEADER_HPP_INCLUDED__
#define __ZLINK_ZMP_DATA_HEADER_HPP_INCLUDED__

#include "core/msg.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_protocol.hpp"

#include <cerrno>
#include <limits>

namespace zlink
{
//  Build the complete header for one ZMP data frame in caller-owned storage.
//  Ordinary messages use 8 bytes; request-reply messages append an 8-byte
//  sequence. The application payload is never copied by this helper.
inline bool build_zmp_data_header (const msg_t &msg_,
                                   unsigned char *buffer_,
                                   size_t buffer_size_,
                                   size_t &header_size_,
                                   bool &has_request_reply_out_)
{
    header_size_ = 0;
    has_request_reply_out_ = false;

    const size_t payload_size = msg_.size ();
    if (payload_size
        > static_cast<size_t> (std::numeric_limits<uint32_t>::max ())) {
        errno = EMSGSIZE;
        return false;
    }

    unsigned char kind = zmp_kind_data;
    uint64_t sequence = 0;
    const bool has_request_reply =
      msg_.get_request_reply_metadata (&kind, &sequence);
    if (has_request_reply
        && (!zmp_is_request_reply_kind (kind) || sequence == 0)) {
        errno = EINVAL;
        return false;
    }

    const size_t required = has_request_reply ? zmp_request_reply_header_size
                                              : zmp_header_size;
    if (!buffer_ || buffer_size_ < required) {
        errno = EINVAL;
        return false;
    }

    const unsigned char msg_flags = msg_.flags ();
    unsigned char flags = 0;
    if (msg_flags & msg_t::more)
        flags |= zmp_flag_more;
    if (msg_flags & msg_t::command)
        flags |= zmp_flag_control;
    if (msg_flags & msg_t::routing_id)
        flags |= zmp_flag_identity;

    const unsigned char cmd_type = msg_flags & CMD_TYPE_MASK;
    if (cmd_type == msg_t::subscribe)
        flags |= zmp_flag_subscribe;
    else if (cmd_type == msg_t::cancel)
        flags |= zmp_flag_cancel;

    if (has_request_reply && zmp_is_special_frame (flags)) {
        errno = EINVAL;
        return false;
    }

    buffer_[0] = zmp_magic;
    buffer_[1] = zmp_version;
    buffer_[2] = flags;
    buffer_[3] = kind;
    put_uint32 (buffer_ + 4, static_cast<uint32_t> (payload_size));
    if (has_request_reply)
        put_uint64 (buffer_ + zmp_header_size, sequence);

    header_size_ = required;
    has_request_reply_out_ = has_request_reply;
    return true;
}
}

#endif
