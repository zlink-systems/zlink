/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_CORE_DURATION_CONVERSION_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_CORE_DURATION_CONVERSION_HPP_INCLUDED

#include <zlink/Contracts/Errors/errors.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

namespace zlink
{
namespace detail
{

template <typename Target, typename Rep, typename Period>
Target checked_milliseconds_count (const std::chrono::duration<Rep, Period> &duration_)
{
    const std::chrono::milliseconds ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (duration_);
    const int64_t value = static_cast<int64_t> (ms.count ());
    if (value < static_cast<int64_t> (std::numeric_limits<Target>::min ())
        || value > static_cast<int64_t> (std::numeric_limits<Target>::max ()))
        throw config_error_t (config_result_t::invalid_argument, EINVAL);
    return static_cast<Target> (value);
}

template <typename Rep, typename Period>
uint32_t native_timeout_ms (const std::chrono::duration<Rep, Period> &duration_)
{
    return checked_milliseconds_count<uint32_t> (duration_);
}

template <typename Rep, typename Period>
int native_option_ms (const std::chrono::duration<Rep, Period> &duration_)
{
    return checked_milliseconds_count<int> (duration_);
}

template <typename Rep, typename Period>
long native_poll_timeout_ms (const std::chrono::duration<Rep, Period> &duration_)
{
    return checked_milliseconds_count<long> (duration_);
}

} // namespace detail
} // namespace zlink

#endif
