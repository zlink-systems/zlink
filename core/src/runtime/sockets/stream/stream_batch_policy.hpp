/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_STREAM_BATCH_POLICY_HPP_INCLUDED__
#define __ZLINK_STREAM_BATCH_POLICY_HPP_INCLUDED__

#include "utils/env.hpp"

namespace zlink
{
namespace stream_batch_policy
{
inline int minimum_send_batch_size ()
{
    return env::positive_int ("ZLINK_ASIO_STREAM_BATCH_SIZE", 4096);
}

inline int read_headroom_bytes ()
{
    return env::non_negative_int ("ZLINK_ASIO_STREAM_BATCH_HEADROOM", 64);
}

inline int apply_read_headroom (int base_, int headroom_)
{
    if (headroom_ <= 0 || base_ > INT_MAX - headroom_)
        return base_;
    return base_ + headroom_;
}
}
}

#endif
