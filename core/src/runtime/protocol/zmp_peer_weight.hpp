/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_PEER_WEIGHT_HPP_INCLUDED__
#define __ZLINK_ZMP_PEER_WEIGHT_HPP_INCLUDED__

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "core/msg.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
namespace zmp_peer_weight
{
static const char command_name[] = "WEIGHT";
static const size_t command_name_size = sizeof (command_name) - 1;
static const size_t command_size = command_name_size + 4;

enum decode_result_t
{
    decode_not_weight_command = 0,
    decode_malformed,
    decode_ok
};

inline int init_command (msg_t *msg_, uint32_t weight_)
{
    if (!msg_) {
        errno = EFAULT;
        return -1;
    }
    if (msg_->init_size (command_size) != 0)
        return -1;

    unsigned char *data = static_cast<unsigned char *> (msg_->data ());
    memcpy (data, command_name, command_name_size);
    data[command_name_size + 0] =
      static_cast<unsigned char> ((weight_ >> 24) & 0xffu);
    data[command_name_size + 1] =
      static_cast<unsigned char> ((weight_ >> 16) & 0xffu);
    data[command_name_size + 2] =
      static_cast<unsigned char> ((weight_ >> 8) & 0xffu);
    data[command_name_size + 3] =
      static_cast<unsigned char> (weight_ & 0xffu);
    msg_->set_flags (msg_t::command);
    return 0;
}

inline decode_result_t decode_command (const msg_t &msg_,
                                       uint32_t *weight_out_)
{
    msg_t &msg = const_cast<msg_t &> (msg_);
    if (!(msg.flags () & msg_t::command) || msg.size () < command_name_size)
        return decode_not_weight_command;
    const unsigned char *data =
      static_cast<const unsigned char *> (msg.data ());
    if (memcmp (data, command_name, command_name_size) != 0)
        return decode_not_weight_command;

    // From this point the command name belongs to WEIGHT. A type-specific
    // shape error is consumed by this codec and never offered to another
    // command handler or the Application receive path.
    if (msg.size () != command_size)
        return decode_malformed;

    const unsigned char *payload = data + command_name_size;
    const uint32_t weight =
      (static_cast<uint32_t> (payload[0]) << 24)
      | (static_cast<uint32_t> (payload[1]) << 16)
      | (static_cast<uint32_t> (payload[2]) << 8)
      | static_cast<uint32_t> (payload[3]);
    if (weight_out_)
        *weight_out_ = weight;
    return decode_ok;
}
}
}

#endif
