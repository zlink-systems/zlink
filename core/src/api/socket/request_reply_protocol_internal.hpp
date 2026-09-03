/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_REPLY_PROTOCOL_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_REPLY_PROTOCOL_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include "core/msg.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_protocol.hpp"
#include "zlink.h"

namespace zlink
{
namespace request_reply
{
enum : uint8_t
{
    request_type = zlink::zmp_kind_request,
    reply_type = zlink::zmp_kind_reply,
    error_reply_type = zlink::zmp_kind_error_reply
};

const uint32_t default_timeout_ms = 5000;

inline uint32_t resolve_timeout_ms (uint32_t per_call_timeout_ms_,
                                    uint32_t default_timeout_ms_,
                                    uint32_t socket_timeout_ms_ = 0)
{
    if (per_call_timeout_ms_ != 0)
        return per_call_timeout_ms_;
    if (socket_timeout_ms_ != 0)
        return socket_timeout_ms_;
    return default_timeout_ms_ != 0 ? default_timeout_ms_ : default_timeout_ms;
}

inline bool read_request_reply_metadata (const zlink_msg_t *part_,
                                         uint8_t *message_type_out_,
                                         uint64_t *request_seq_out_)
{
    if (!part_ || !message_type_out_ || !request_seq_out_)
        return false;
    const zlink::msg_t *msg = reinterpret_cast<const zlink::msg_t *> (part_);
    if (!msg->check ())
        return false;
    unsigned char kind = zlink::zmp_kind_data;
    uint64_t sequence = 0;
    if (!msg->get_request_reply_metadata (&kind, &sequence))
        return false;
    *message_type_out_ = kind;
    *request_seq_out_ = sequence;
    return true;
}

inline void clear_request_reply_metadata (zlink_msg_t *part_)
{
    if (!part_)
        return;
    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (msg->check ())
        msg->reset_request_reply_metadata ();
}

inline int decode_reply_completion (uint8_t message_type_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    int *reply_errno_out_,
                                    zlink_msg_t **reply_parts_out_,
                                    size_t *reply_part_count_out_)
{
    if (!reply_errno_out_ || !reply_parts_out_ || !reply_part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    *reply_errno_out_ = 0;
    *reply_parts_out_ = parts_;
    *reply_part_count_out_ = part_count_;

    if (message_type_ != error_reply_type)
        return 0;

    if (part_count_ == 0) {
        *reply_errno_out_ = EPROTO;
        *reply_parts_out_ = NULL;
        *reply_part_count_out_ = 0;
        return 0;
    }

    zlink::msg_t *errno_part = reinterpret_cast<zlink::msg_t *> (&parts_[0]);
    if (!errno_part->check () || errno_part->size () != 4) {
        *reply_errno_out_ = EPROTO;
        *reply_parts_out_ = NULL;
        *reply_part_count_out_ = 0;
        return 0;
    }

    const unsigned char *errbuf = static_cast<const unsigned char *> (errno_part->data ());
    *reply_errno_out_ = static_cast<int> (zlink::get_uint32 (errbuf));
    if (*reply_errno_out_ == 0) {
        *reply_errno_out_ = EPROTO;
        *reply_parts_out_ = NULL;
        *reply_part_count_out_ = 0;
        return 0;
    }
    *reply_parts_out_ = part_count_ > 1 ? parts_ + 1 : NULL;
    *reply_part_count_out_ = part_count_ > 0 ? part_count_ - 1 : 0;
    return 0;
}

inline void consume_send_frame (zlink_msg_t *part_)
{
    const int saved_errno = errno;
    if (!part_)
        return;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check ())
        return;

    const int close_rc = msg->close ();
    errno_assert (close_rc == 0);
    const int init_rc = msg->init ();
    errno_assert (init_rc == 0);
    errno = saved_errno;
}

inline void consume_send_frames_from (zlink_msg_t *parts_, size_t start_index_, size_t part_count_)
{
    if (!parts_)
        return;

    for (size_t i = start_index_; i < part_count_; ++i)
        consume_send_frame (&parts_[i]);
}

}
}

#endif
