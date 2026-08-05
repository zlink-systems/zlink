/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_RECONNECT_INTERVAL_HPP_INCLUDED
#define ZLINK_ASIO_RECONNECT_INTERVAL_HPP_INCLUDED

#include "core/options.hpp"
#include "utils/random.hpp"

#include <limits>

namespace zlink
{
inline int next_asio_reconnect_interval (const options_t &options_,
                                         int *current_reconnect_ivl_)
{
    if (options_.reconnect_ivl_max > 0) {
        int candidate_interval = 0;
        if (*current_reconnect_ivl_ == -1)
            candidate_interval = options_.reconnect_ivl;
        else if (*current_reconnect_ivl_ > std::numeric_limits<int>::max () / 2)
            candidate_interval = std::numeric_limits<int>::max ();
        else
            candidate_interval = *current_reconnect_ivl_ * 2;

        if (candidate_interval > options_.reconnect_ivl_max)
            *current_reconnect_ivl_ = options_.reconnect_ivl_max;
        else
            *current_reconnect_ivl_ = candidate_interval;
        return *current_reconnect_ivl_;
    }

    if (*current_reconnect_ivl_ == -1)
        *current_reconnect_ivl_ = options_.reconnect_ivl;
    const int random_jitter = generate_random () % options_.reconnect_ivl;
    const int interval =
      *current_reconnect_ivl_ < std::numeric_limits<int>::max () - random_jitter
        ? *current_reconnect_ivl_ + random_jitter
        : std::numeric_limits<int>::max ();
    return interval;
}
}

#endif
