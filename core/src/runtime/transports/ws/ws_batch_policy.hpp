/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__
#define __ZLINK_WS_BATCH_POLICY_HPP_INCLUDED__

#include <stddef.h>

namespace zlink
{
namespace ws_batch_policy
{
// One encoder batch becomes one Beast binary write. Keep the established
// small-connection footprint, then let the existing encoder target policy
// grow sustained full batches up to this bounded maximum.
inline int zmp_send_batch_size ()
{
    return 16 * 1024;
}

inline size_t zmp_send_batch_max_size ()
{
    return 128 * 1024;
}

// A large body may occupy the zero-copy middle of a bounded batch. Its header
// must fit the current copy target, and the complete operation must remain
// below the WS hard maximum.
inline bool use_pointer_body_in_batch (size_t batch_size_,
                                       size_t header_size_,
                                       size_t body_size_,
                                       size_t target_size_,
                                       size_t max_size_)
{
    if (batch_size_ >= target_size_ || batch_size_ >= max_size_)
        return false;
    if (header_size_ > target_size_ - batch_size_
        || header_size_ > max_size_ - batch_size_)
        return false;

    const size_t body_room_in_max = max_size_ - batch_size_ - header_size_;
    return body_size_ <= body_room_in_max;
}

inline size_t pointer_batch_size (size_t copy_size_, size_t body_size_)
{
    return copy_size_ > static_cast<size_t> (-1) - body_size_
             ? static_cast<size_t> (-1)
             : copy_size_ + body_size_;
}

inline size_t write_buffer_initial_size ()
{
    return 64 * 1024;
}
}
}

#endif
