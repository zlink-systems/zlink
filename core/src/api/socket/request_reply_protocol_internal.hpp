/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_REPLY_PROTOCOL_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_REPLY_PROTOCOL_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <vector>

#include "api/message/request_result_internal.hpp"
#include "core/msg.hpp"
#include "zlink.h"

namespace zlink
{
namespace request_reply
{
enum : uint8_t
{
    protocol_id = 0x01,
    version = 0x01,
    request_type = 0x01,
    reply_type = 0x02,
    error_reply_type = 0x03,
    completion_control_type = 0x04
};

const uint32_t default_timeout_ms = 5000;
const size_t control_part_count = 4;

enum envelope_control_index_t
{
    envelope_protocol_index = 0,
    envelope_version_index = 1,
    envelope_type_index = 2,
    envelope_sequence_index = 3
};

struct parsed_envelope_t
{
    uint8_t message_type;
    uint64_t request_seq;
    zlink_msg_t *payload_parts;
    size_t payload_part_count;
};

struct envelope_control_data_t
{
    unsigned char protocol_id;
    unsigned char version;
    unsigned char message_type;
    unsigned char request_seq[8];
};

inline void encode_u64_be (uint64_t value_, unsigned char *out_)
{
    for (int i = 7; i >= 0; --i) {
        out_[i] = static_cast<unsigned char> (value_ & 0xffu);
        value_ >>= 8;
    }
}

inline uint64_t decode_u64_be (const unsigned char *in_)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | in_[i];
    return value;
}

inline void encode_u32_be (uint32_t value_, unsigned char *out_)
{
    for (int i = 3; i >= 0; --i) {
        out_[i] = static_cast<unsigned char> (value_ & 0xffu);
        value_ >>= 8;
    }
}

inline uint32_t decode_u32_be (const unsigned char *in_)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value = (value << 8) | in_[i];
    return value;
}

inline uint32_t resolve_timeout_ms (uint32_t per_call_timeout_ms_,
                                    uint32_t default_timeout_ms,
                                    uint32_t socket_timeout_ms_ = 0)
{
    if (per_call_timeout_ms_ != 0)
        return per_call_timeout_ms_;
    if (socket_timeout_ms_ != 0)
        return socket_timeout_ms_;
    return default_timeout_ms;
}

inline bool is_valid_message_type (uint8_t message_type_)
{
    return message_type_ == request_type || message_type_ == reply_type
           || message_type_ == error_reply_type
           || message_type_ == completion_control_type;
}

inline int encode_envelope_control_data (uint8_t message_type_,
                                         uint64_t request_seq_,
                                         envelope_control_data_t *out_)
{
    const bool valid_sequence = message_type_ == completion_control_type
                                  ? request_seq_ == 0
                                  : request_seq_ != 0;
    if (!out_ || !is_valid_message_type (message_type_) || !valid_sequence) {
        errno = EINVAL;
        return -1;
    }

    out_->protocol_id = protocol_id;
    out_->version = version;
    out_->message_type = message_type_;
    encode_u64_be (request_seq_, out_->request_seq);
    return 0;
}

inline bool frame_is_single_byte_value (zlink_msg_t *part_, uint8_t value_)
{
    if (!part_)
        return false;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check () || msg->size () != 1)
        return false;

    const unsigned char *data = static_cast<const unsigned char *> (msg->data ());
    return data[0] == value_;
}

inline bool parse_envelope (zlink_msg_t *parts_, size_t part_count_, parsed_envelope_t *out_)
{
    if (!parts_ || !out_ || part_count_ < control_part_count)
        return false;

    zlink::msg_t *protocol_msg = reinterpret_cast<zlink::msg_t *> (&parts_[0]);
    if (!protocol_msg->check () || !frame_is_single_byte_value (&parts_[0], protocol_id)
        || !frame_is_single_byte_value (&parts_[1], version)) {
        return false;
    }

    zlink::msg_t *type_msg = reinterpret_cast<zlink::msg_t *> (&parts_[2]);
    zlink::msg_t *seq_msg = reinterpret_cast<zlink::msg_t *> (&parts_[3]);
    if (!type_msg->check () || !seq_msg->check () || type_msg->size () != 1
        || seq_msg->size () != 8) {
        return false;
    }

    const unsigned char *type_data = static_cast<const unsigned char *> (type_msg->data ());
    if (!is_valid_message_type (type_data[0])) {
        return false;
    }

    out_->message_type = type_data[0];
    out_->request_seq = decode_u64_be (static_cast<const unsigned char *> (seq_msg->data ()));
    if ((out_->message_type == completion_control_type) != (out_->request_seq == 0))
        return false;

    out_->payload_parts = parts_ + control_part_count;
    out_->payload_part_count = part_count_ - control_part_count;
    return true;
}

inline int decode_reply_completion (uint8_t message_type_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    int *callback_errno_out_,
                                    zlink_msg_t **callback_parts_out_,
                                    size_t *callback_part_count_out_)
{
    if (!callback_errno_out_ || !callback_parts_out_ || !callback_part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    *callback_errno_out_ = 0;
    *callback_parts_out_ = parts_;
    *callback_part_count_out_ = part_count_;

    if (message_type_ != error_reply_type)
        return 0;

    if (part_count_ == 0) {
        *callback_errno_out_ = EPROTO;
        *callback_parts_out_ = NULL;
        *callback_part_count_out_ = 0;
        return 0;
    }

    zlink::msg_t *errno_part = reinterpret_cast<zlink::msg_t *> (&parts_[0]);
    if (!errno_part->check () || errno_part->size () != 4) {
        *callback_errno_out_ = EPROTO;
        *callback_parts_out_ = NULL;
        *callback_part_count_out_ = 0;
        return 0;
    }

    const unsigned char *errbuf = static_cast<const unsigned char *> (errno_part->data ());
    *callback_errno_out_ = static_cast<int> (decode_u32_be (errbuf));
    *callback_parts_out_ = part_count_ > 1 ? parts_ + 1 : NULL;
    *callback_part_count_out_ = part_count_ > 0 ? part_count_ - 1 : 0;
    return 0;
}

inline int init_error_reply_errno_part (zlink_msg_t *part_, int errnum_)
{
    if (!part_) {
        errno = EFAULT;
        return -1;
    }

    unsigned char errbuf[4];
    encode_u32_be (static_cast<uint32_t> (errnum_), errbuf);
    if (zlink_msg_init_size (part_, sizeof (errbuf)) != 0)
        return -1;
    memcpy (zlink_msg_data (part_), errbuf, sizeof (errbuf));
    return 0;
}

inline int init_error_reply_result_part (zlink_msg_t *part_, zlink_request_result_t result_)
{
    return init_error_reply_errno_part (part_, zlink::request_result_internal::to_errno (result_));
}

inline int init_control_part (zlink_msg_t *part_, const void *data_, size_t size_)
{
    if (!part_) {
        errno = EFAULT;
        return -1;
    }

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check ()) {
        errno = EFAULT;
        return -1;
    }
    if (msg->init_size (size_) != 0)
        return -1;
    if (size_ > 0)
        memcpy (msg->data (), data_, size_);
    return 0;
}

inline int init_envelope_control_parts (zlink_msg_t *parts_,
                                        uint8_t message_type_,
                                        uint64_t request_seq_)
{
    if (!parts_) {
        errno = EFAULT;
        return -1;
    }

    envelope_control_data_t control;
    if (encode_envelope_control_data (message_type_, request_seq_, &control) != 0)
        return -1;

    if (init_control_part (&parts_[envelope_protocol_index], &control.protocol_id, 1) != 0
        || init_control_part (&parts_[envelope_version_index], &control.version, 1) != 0
        || init_control_part (&parts_[envelope_type_index], &control.message_type, 1) != 0
        || init_control_part (&parts_[envelope_sequence_index], control.request_seq,
                              sizeof (control.request_seq))
             != 0) {
        return -1;
    }
    return 0;
}

template <typename SendFrameFn>
inline int send_envelope_control_frames (uint8_t message_type_,
                                         uint64_t request_seq_,
                                         SendFrameFn send_frame_)
{
    envelope_control_data_t control;
    if (encode_envelope_control_data (message_type_, request_seq_, &control) != 0)
        return -1;

    if (send_frame_ (envelope_protocol_index, &control.protocol_id, 1) != 0
        || send_frame_ (envelope_version_index, &control.version, 1) != 0
        || send_frame_ (envelope_type_index, &control.message_type, 1) != 0
        || send_frame_ (envelope_sequence_index, control.request_seq,
                        sizeof (control.request_seq))
             != 0) {
        return -1;
    }
    return 0;
}

inline void consume_send_frame (zlink_msg_t *part_)
{
    if (!part_)
        return;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check ())
        return;

    const int close_rc = msg->close ();
    errno_assert (close_rc == 0);
    const int init_rc = msg->init ();
    errno_assert (init_rc == 0);
}

inline void consume_send_frames_from (zlink_msg_t *parts_, size_t start_index_, size_t part_count_)
{
    if (!parts_)
        return;

    for (size_t i = start_index_; i < part_count_; ++i)
        consume_send_frame (&parts_[i]);
}

inline void close_built_parts (std::vector<zlink_msg_t> *parts_)
{
    if (!parts_)
        return;

    for (size_t i = 0; i < parts_->size (); ++i)
        consume_send_frame (&(*parts_)[i]);
}

inline void close_built_parts (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return;

    for (size_t i = 0; i < part_count_; ++i)
        consume_send_frame (&parts_[i]);
}

inline void close_request_reply_parts (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return;
    zlink_multipart_close (parts_, part_count_);
}

inline void complete_reply_callback (zlink_reply_handler_fn handler_,
                                     int errnum_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     void *userdata_)
{
    if (handler_)
        handler_ (zlink::request_result_internal::from_errno (errnum_), parts_, part_count_,
                  userdata_);
}
}
}

#endif
