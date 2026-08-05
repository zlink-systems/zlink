/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_EVENTING_TIMER_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_EVENTING_TIMER_ACCESS_HPP_INCLUDED

#include <zlink/Contracts/Eventing/timers.hpp>

namespace zlink
{
namespace detail
{

struct timer_access_t
{
    static void *native_handle (timer_t &timer_) noexcept;
    static const void *native_handle (const timer_t &timer_) noexcept;
};

inline void *native_handle (timer_t &timer_) noexcept
{
    return timer_access_t::native_handle (timer_);
}

inline const void *native_handle (const timer_t &timer_) noexcept
{
    return timer_access_t::native_handle (timer_);
}

} // namespace detail
} // namespace zlink

#endif
