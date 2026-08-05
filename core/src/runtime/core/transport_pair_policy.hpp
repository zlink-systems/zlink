/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_TRANSPORT_PAIR_POLICY_HPP_INCLUDED__
#define __ZLINK_CORE_TRANSPORT_PAIR_POLICY_HPP_INCLUDED__

#include <algorithm>
#include <stdint.h>

namespace zlink
{
namespace transport_pair_policy
{
static const uint64_t completion_hwm_bytes = 256u * 1024u;
static const int completion_socket_buffer_bytes = 64 * 1024;

//  The completion pair carries handshake, liveness and reply frames, never
//  application traffic. An application lowers its high water mark to throttle
//  what it sends, not to make the connection unusable, so the pair keeps its
//  own budget instead of following the configured one down.
static inline uint64_t completion_hwm (uint64_t /*configured_*/)
{
    return completion_hwm_bytes;
}

static inline int completion_socket_buffer (int configured_)
{
    return configured_ < 0
             ? completion_socket_buffer_bytes
             : std::min (configured_, completion_socket_buffer_bytes);
}
}
}

#endif
