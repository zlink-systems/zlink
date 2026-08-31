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
static const uint64_t request_correlation_work_budget = 32ULL * 1024ULL * 1024ULL;
static const uint64_t request_correlation_linear_charge_bytes = 1024ULL;
static const uint64_t request_correlation_cubic_charge_limit_bytes =
  32ULL * 1024ULL;
static const uint64_t request_correlation_count_budget = 16ULL * 1024ULL;

static inline uint64_t request_correlation_work_charge (
  uint64_t accounted_bytes_)
{
    if (accounted_bytes_ <= request_correlation_linear_charge_bytes)
        return accounted_bytes_;
    // At 32 KiB the cubic charge exactly equals the work budget. Larger
    // requests only need a saturated over-budget charge: the empty-pair
    // exception admits one, and all non-empty pairs reject it. This branch
    // also keeps the multiplications below their uint64_t overflow boundary.
    if (accounted_bytes_ > request_correlation_cubic_charge_limit_bytes)
        return request_correlation_work_budget + 1;
    const uint64_t squared = accounted_bytes_ * accounted_bytes_;
    const uint64_t cubed = squared * accounted_bytes_;
    const uint64_t divisor = request_correlation_linear_charge_bytes
                             * request_correlation_linear_charge_bytes;
    return cubed / divisor + (cubed % divisor != 0);
}

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
