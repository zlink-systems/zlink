/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <chrono>

#include <cmath>
#include <cstdint>
#include <limits>

namespace zlink::framework::runtime::messaging
{

// Preserve the source operation's deadline across the steady/system clock boundary.
inline std::uint64_t request_deadline_unix_ms (std::chrono::steady_clock::time_point at)
{
    using milliseconds_t = std::chrono::duration<long double, std::milli>;
    const auto steady_now = std::chrono::steady_clock::now ();
    const auto unix_now = std::chrono::system_clock::now ();
    const auto unix_ms = std::ceil (milliseconds_t (unix_now.time_since_epoch ()).count ()
                                    + milliseconds_t (at.time_since_epoch ()).count ()
                                    - milliseconds_t (steady_now.time_since_epoch ()).count ());
    if (unix_ms <= 1)
        return 1;
    if (unix_ms >= static_cast<long double> (std::numeric_limits<std::uint64_t>::max ()))
        return std::numeric_limits<std::uint64_t>::max ();
    return static_cast<std::uint64_t> (unix_ms);
}

inline std::chrono::steady_clock::time_point request_deadline_from_unix_ms (std::uint64_t unix_ms)
{
    using clock_t = std::chrono::steady_clock;
    using milliseconds_t = std::chrono::duration<long double, std::milli>;
    const auto now = clock_t::now ();
    const auto remaining =
      static_cast<long double> (unix_ms)
      - milliseconds_t (std::chrono::system_clock::now ().time_since_epoch ()).count ();
    if (remaining <= 0)
        return now;
    const auto available = milliseconds_t (clock_t::time_point::max ().time_since_epoch ()).count ()
                           - milliseconds_t (now.time_since_epoch ()).count ();
    if (remaining >= available)
        return clock_t::time_point::max ();
    return now + std::chrono::duration_cast<clock_t::duration> (milliseconds_t{remaining});
}

} // namespace zlink::framework::runtime::messaging
