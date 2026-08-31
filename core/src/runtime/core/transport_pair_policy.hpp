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
static const uint64_t request_correlation_liveness_bytes =
  2ULL * completion_socket_buffer_bytes;

static inline int completion_socket_buffer (int configured_)
{
    // Preserve the public -1 contract: the OS owns its default/autotuned
    // socket buffer. Only an explicit application value is capped for the
    // unbounded completion lane.
    return configured_ < 0 ? configured_
                           : std::min (configured_, completion_socket_buffer_bytes);
}
}
}

#endif
