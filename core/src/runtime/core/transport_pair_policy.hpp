/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_TRANSPORT_PAIR_POLICY_HPP_INCLUDED__
#define __ZLINK_CORE_TRANSPORT_PAIR_POLICY_HPP_INCLUDED__

#include <algorithm>
#include <stdint.h>

namespace zlink
{
namespace transport_pair_policy
{
static const int completion_socket_buffer_bytes = 64 * 1024;

static inline int completion_socket_buffer (int configured_)
{
    return configured_ < 0
             ? completion_socket_buffer_bytes
             : std::min (configured_, completion_socket_buffer_bytes);
}
}
}

#endif
