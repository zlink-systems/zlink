/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_CONTROL_HPP_INCLUDED__
#define __ZLINK_ZMP_CONTROL_HPP_INCLUDED__

#include "core/msg.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_metadata.hpp"
#include "utils/err.hpp"

#include <errno.h>
#include <algorithm>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace zlink
{
namespace zmp_control
{
const size_t hello_min_body_size = 3;
const size_t hello_max_body_size = 3 + 255;

enum command_message_kind_t
{
    command_message_invalid = 0,
    command_message_ready,
    command_message_error,
    command_message_data_after_ready
};

struct hello_parse_result_t
{
    hello_parse_result_t () :
        error_code (zmp_error_internal),
        error_reason (NULL),
        malformed_hello_event (false),
        peer_type (0),
        identity (NULL),
        identity_len (0)
    {
    }

    uint8_t error_code;
    const char *error_reason;
    bool malformed_hello_event;
    int peer_type;
    const unsigned char *identity;
    size_t identity_len;
};

enum hello_receive_status_t
{
    hello_receive_incomplete = 0,
    hello_receive_ready,
    hello_receive_error
};

struct hello_receive_result_t
{
    hello_receive_result_t () :
        status (hello_receive_incomplete),
        error_code (zmp_error_internal),
        error_reason (NULL)
    {
    }

    hello_receive_status_t status;
    uint8_t error_code;
    const char *error_reason;
};

inline void build_hello_frame (const options_t &options_,
                               unsigned char *hello_send_,
                               size_t hello_send_capacity_,
                               size_t *hello_send_size_out_)
{
    zlink_assert (hello_send_);
    zlink_assert (hello_send_size_out_);
    zlink_assert (hello_send_capacity_ >= zmp_header_size + hello_max_body_size);

    const size_t identity_len =
      std::min (static_cast<size_t> (options_.routing_id_size), static_cast<size_t> (255));
    const size_t body_len = hello_min_body_size + identity_len;
    hello_send_[0] = zmp_magic;
    hello_send_[1] = zmp_version;
    hello_send_[2] = zmp_flag_control;
    hello_send_[3] = 0;
    put_uint32 (hello_send_ + 4, static_cast<uint32_t> (body_len));
    hello_send_[zmp_header_size + 0] = zmp_control_hello;
    hello_send_[zmp_header_size + 1] = static_cast<unsigned char> (options_.type);
    hello_send_[zmp_header_size + 2] = static_cast<unsigned char> (identity_len);
    if (identity_len > 0)
        memcpy (hello_send_ + zmp_header_size + hello_min_body_size, options_.routing_id,
                identity_len);

    *hello_send_size_out_ = zmp_header_size + body_len;
}

inline void build_ready_frame (const options_t &options_,
                               std::vector<unsigned char> &ready_frame_)
{
    std::vector<unsigned char> ready_body;
    ready_body.push_back (zmp_control_ready);
    if (options_.zmp_metadata)
        zmp_metadata::add_basic_properties (options_, ready_body);

    ready_frame_.resize (zmp_header_size + ready_body.size ());
    ready_frame_[0] = zmp_magic;
    ready_frame_[1] = zmp_version;
    ready_frame_[2] = zmp_flag_control;
    ready_frame_[3] = 0;
    put_uint32 (&ready_frame_[4], static_cast<uint32_t> (ready_body.size ()));
    memcpy (&ready_frame_[zmp_header_size], &ready_body[0], ready_body.size ());
}

inline void build_hello_ready_frames (const options_t &options_,
                                      unsigned char *hello_send_,
                                      size_t hello_send_capacity_,
                                      size_t *hello_send_size_out_,
                                      std::vector<unsigned char> &ready_send_)
{
    build_hello_frame (
      options_, hello_send_, hello_send_capacity_, hello_send_size_out_);
    std::vector<unsigned char> ready_frame;
    build_ready_frame (options_, ready_frame);
    ready_send_.clear ();
    ready_send_.reserve (*hello_send_size_out_ + ready_frame.size ());
    ready_send_.insert (
      ready_send_.end (), hello_send_, hello_send_ + *hello_send_size_out_);
    ready_send_.insert (ready_send_.end (), ready_frame.begin (), ready_frame.end ());
}

inline hello_receive_result_t receive_hello_bytes (unsigned char *&inpos_,
                                                   size_t &insize_,
                                                   unsigned char *hello_recv_,
                                                   size_t &hello_header_bytes_,
                                                   size_t &hello_body_bytes_,
                                                   uint32_t &hello_body_len_)
{
    zlink_assert (hello_recv_);
    hello_receive_result_t result;

    if (hello_header_bytes_ < zmp_header_size) {
        const size_t to_copy = std::min (insize_, zmp_header_size - hello_header_bytes_);
        memcpy (hello_recv_ + hello_header_bytes_, inpos_, to_copy);
        hello_header_bytes_ += to_copy;
        inpos_ += to_copy;
        insize_ -= to_copy;
        if (hello_header_bytes_ < zmp_header_size)
            return result;

        hello_body_len_ = get_uint32 (hello_recv_ + 4);
        if (hello_body_len_ < hello_min_body_size) {
            result.status = hello_receive_error;
            result.error_code = zmp_error_internal;
            result.error_reason = "hello too short";
            errno = EPROTO;
            return result;
        }
        if (hello_body_len_ > hello_max_body_size) {
            result.status = hello_receive_error;
            result.error_code = zmp_error_body_too_large;
            result.error_reason = "hello too large";
            errno = EPROTO;
            return result;
        }
    }

    if (hello_body_bytes_ < hello_body_len_) {
        const size_t to_copy =
          std::min (insize_, static_cast<size_t> (hello_body_len_) - hello_body_bytes_);
        memcpy (hello_recv_ + zmp_header_size + hello_body_bytes_, inpos_, to_copy);
        hello_body_bytes_ += to_copy;
        inpos_ += to_copy;
        insize_ -= to_copy;
        if (hello_body_bytes_ < hello_body_len_)
            return result;
    }

    result.status = hello_receive_ready;
    return result;
}

inline bool socket_types_compatible (int local_type_, int peer_type_)
{
    switch (local_type_) {
        case ZLINK_CORE_SOCKET_DEALER:
        case ZLINK_CORE_SOCKET_ROUTER:
            return peer_type_ == ZLINK_CORE_SOCKET_DEALER || peer_type_ == ZLINK_CORE_SOCKET_ROUTER;
        case ZLINK_CORE_SOCKET_PUB:
            return peer_type_ == ZLINK_CORE_SOCKET_SUB || peer_type_ == ZLINK_CORE_SOCKET_XSUB;
        case ZLINK_CORE_SOCKET_SUB:
            return peer_type_ == ZLINK_CORE_SOCKET_PUB || peer_type_ == ZLINK_CORE_SOCKET_XPUB;
        case ZLINK_CORE_SOCKET_XPUB:
            return peer_type_ == ZLINK_CORE_SOCKET_SUB || peer_type_ == ZLINK_CORE_SOCKET_XSUB;
        case ZLINK_CORE_SOCKET_XSUB:
            return peer_type_ == ZLINK_CORE_SOCKET_PUB || peer_type_ == ZLINK_CORE_SOCKET_XPUB;
        case ZLINK_CORE_SOCKET_PAIR:
            return peer_type_ == ZLINK_CORE_SOCKET_PAIR;
        default:
            return false;
    }
}

inline int parse_hello_frame (const unsigned char *data_,
                              size_t size_,
                              int local_type_,
                              hello_parse_result_t *result_out_)
{
    if (!data_ || !result_out_) {
        errno = EFAULT;
        return -1;
    }
    *result_out_ = hello_parse_result_t ();

    if (size_ < zmp_header_size + hello_min_body_size) {
        result_out_->error_reason = "hello too short";
        errno = EPROTO;
        return -1;
    }

    if (data_[0] != zmp_magic) {
        result_out_->error_code = zmp_error_invalid_magic;
        result_out_->malformed_hello_event = true;
        errno = EPROTO;
        return -1;
    }

    if (data_[1] != zmp_version) {
        result_out_->error_code = zmp_error_version_mismatch;
        result_out_->malformed_hello_event = true;
        errno = EPROTO;
        return -1;
    }

    const unsigned char flags = data_[2];
    if (data_[3] != 0 || flags != zmp_flag_control) {
        result_out_->error_code = zmp_error_flags_invalid;
        result_out_->malformed_hello_event = true;
        errno = EPROTO;
        return -1;
    }

    const uint32_t body_len = get_uint32 (data_ + 4);
    if (body_len + zmp_header_size != size_) {
        result_out_->error_reason = "hello length mismatch";
        errno = EPROTO;
        return -1;
    }

    const unsigned char *body = data_ + zmp_header_size;
    if (body[0] != zmp_control_hello) {
        result_out_->error_reason = "missing hello";
        errno = EPROTO;
        return -1;
    }

    const int peer_type = body[1];
    if (!socket_types_compatible (local_type_, peer_type)) {
        result_out_->error_code = zmp_error_socket_type_mismatch;
        errno = EPROTO;
        return -1;
    }

    const unsigned char identity_len = body[2];
    if (body_len != static_cast<uint32_t> (hello_min_body_size + identity_len)) {
        result_out_->error_reason = "hello identity mismatch";
        errno = EPROTO;
        return -1;
    }

    result_out_->peer_type = peer_type;
    result_out_->identity = body + hello_min_body_size;
    result_out_->identity_len = identity_len;
    return 0;
}

inline int
parse_ready_metadata (msg_t *msg_, metadata_t::dict_t *properties_, const char **error_reason_out_)
{
    if (error_reason_out_)
        *error_reason_out_ = NULL;
    if (!msg_ || !properties_) {
        errno = EFAULT;
        return -1;
    }

    const size_t size = msg_->size ();
    if (size < 1) {
        if (error_reason_out_)
            *error_reason_out_ = "ready too short";
        errno = EPROTO;
        return -1;
    }

    if (size > 1) {
        metadata_t::dict_t peer_props;
        const unsigned char *data = static_cast<const unsigned char *> (msg_->data ());
        if (zmp_metadata::parse (data + 1, size - 1, peer_props) == -1) {
            if (error_reason_out_)
                *error_reason_out_ = "ready metadata invalid";
            return -1;
        }
        properties_->insert (peer_props.begin (), peer_props.end ());
    }

    return 0;
}

inline int accept_ready_message (msg_t *msg_,
                                 bool &ready_received_,
                                 metadata_t *&metadata_,
                                 metadata_t::dict_t &properties_,
                                 const char **error_reason_out_)
{
    if (error_reason_out_)
        *error_reason_out_ = NULL;
    if (ready_received_) {
        if (error_reason_out_)
            *error_reason_out_ = "duplicate ready";
        errno = EPROTO;
        return -1;
    }

    if (parse_ready_metadata (msg_, &properties_, error_reason_out_) != 0)
        return -1;

    if (!properties_.empty ()) {
        metadata_ = new (std::nothrow) metadata_t (properties_);
        alloc_assert (metadata_);
    }

    ready_received_ = true;
    return 0;
}

inline command_message_kind_t classify_command_message (msg_t *msg_,
                                                        bool ready_received_,
                                                        const char **error_reason_out_)
{
    if (error_reason_out_)
        *error_reason_out_ = NULL;
    if (!msg_ || msg_->size () < 1) {
        errno = EPROTO;
        return command_message_invalid;
    }

    const uint8_t type = *(static_cast<const uint8_t *> (msg_->data ()));
    if (type == zmp_control_ready)
        return command_message_ready;
    if (type == zmp_control_error)
        return command_message_error;
    if (ready_received_)
        return command_message_data_after_ready;

    if (error_reason_out_)
        *error_reason_out_ = "unknown control";
    errno = EPROTO;
    return command_message_invalid;
}

inline int parse_error_frame (msg_t *msg_, uint8_t *code_out_, const char **error_reason_out_)
{
    if (error_reason_out_)
        *error_reason_out_ = NULL;
    if (!msg_ || !code_out_) {
        errno = EFAULT;
        return -1;
    }

    const size_t size = msg_->size ();
    if (size < 3) {
        if (error_reason_out_)
            *error_reason_out_ = "error frame too short";
        errno = EPROTO;
        return -1;
    }

    const unsigned char *data = static_cast<const unsigned char *> (msg_->data ());
    const uint8_t code = data[1];
    const size_t reason_len = data[2];
    if (size < 3 + reason_len) {
        if (error_reason_out_)
            *error_reason_out_ = "error frame invalid";
        errno = EPROTO;
        return -1;
    }

    *code_out_ = code;
    errno = EPROTO;
    return -1;
}

}
}

#endif
