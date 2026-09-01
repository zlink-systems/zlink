/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_FLOW_STATE_FRAME_HPP_INCLUDED__
#define __ZLINK_CORE_FLOW_STATE_FRAME_HPP_INCLUDED__

#include <string.h>

#include "core/msg.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
namespace flow_state
{
//  Absolute receive-flow state of one socket. This is not a counter: applying
//  the same state twice has the same result as applying it once.
enum receive_flow_state_t
{
    receive_flow_running = 0,
    receive_flow_paused = 1
};

//  Wire layout of the Core-internal flow-state frame. It travels as a ZMP
//  command frame on the paired completion lane, so no application receive path
//  can ever observe it.
//
//    offset  size  field
//    0       9     command name "FLOWSTATE"
//    9       1     protocol version
//    10      1     state (0 RUNNING, 1 PAUSED)
//    11      8     flow epoch               (big endian)
//
//  The physical connection that delivered the command is the identity fence;
//  no pair id or connection generation is exposed on the wire.
//  Total frame size is 19 bytes.
static const char frame_name[] = "FLOWSTATE";
static const size_t frame_name_size = sizeof (frame_name) - 1;
static const uint8_t frame_protocol_version = 1;
static const size_t frame_body_size = 1 + 1 + 8;
static const size_t frame_size = frame_name_size + frame_body_size;

struct frame_t
{
    frame_t () :
        version (frame_protocol_version),
        state (receive_flow_running),
        epoch (0)
    {
    }

    uint8_t version;
    uint8_t state;
    uint64_t epoch;
};

enum decode_result_t
{
    //  The frame is not a flow-state frame at all; the caller keeps its own
    //  handling for it.
    decode_not_flow_frame = 0,
    //  A flow-state frame this build cannot interpret. The caller consumes and
    //  drops it instead of handing it to any other frame handler.
    decode_unsupported_version,
    decode_malformed,
    decode_ok
};

static inline void put_uint64_be (unsigned char *target_, uint64_t value_)
{
    for (size_t i = 0; i < 8; ++i)
        target_[i] = static_cast<unsigned char> ((value_ >> (56 - 8 * i)) & 0xffu);
}

static inline uint64_t get_uint64_be (const unsigned char *source_)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<uint64_t> (source_[i]);
    return value;
}

static inline bool version_supported (uint8_t version_)
{
    return version_ == frame_protocol_version;
}

static inline bool state_valid (uint8_t state_)
{
    return state_ == receive_flow_running || state_ == receive_flow_paused;
}

//  Builds the frame into an already initialised message. The message is
//  re-initialised at the required size, so the caller must not hold data of
//  its own in it.
static inline int init_frame (msg_t *msg_, const frame_t &frame_)
{
    if (!msg_) {
        errno = EFAULT;
        return -1;
    }
    if (msg_->init_size (frame_size) != 0)
        return -1;

    unsigned char *data = static_cast<unsigned char *> (msg_->data ());
    memcpy (data, frame_name, frame_name_size);
    data[frame_name_size] = frame_.version;
    data[frame_name_size + 1] = frame_.state;
    put_uint64_be (data + frame_name_size + 2, frame_.epoch);
    msg_->set_flags (msg_t::command);
    return 0;
}

static inline decode_result_t decode_frame (const msg_t &msg_, frame_t *out_)
{
    msg_t &msg = const_cast<msg_t &> (msg_);
    if (!(msg.flags () & msg_t::command))
        return decode_not_flow_frame;
    if (msg.size () < frame_name_size)
        return decode_not_flow_frame;
    const unsigned char *data = static_cast<const unsigned char *> (msg.data ());
    if (memcmp (data, frame_name, frame_name_size) != 0)
        return decode_not_flow_frame;

    //  From here on the frame is ours: it is consumed even when it cannot be
    //  applied, so it never reaches another frame handler.
    if (msg.size () < frame_name_size + 1)
        return decode_malformed;
    const uint8_t version = data[frame_name_size];
    if (!version_supported (version))
        return decode_unsupported_version;
    if (msg.size () != frame_size)
        return decode_malformed;

    frame_t frame;
    frame.version = version;
    frame.state = data[frame_name_size + 1];
    frame.epoch = get_uint64_be (data + frame_name_size + 2);
    if (!state_valid (frame.state) || frame.epoch == 0)
        return decode_malformed;

    if (out_)
        *out_ = frame;
    return decode_ok;
}
}
}

#endif
