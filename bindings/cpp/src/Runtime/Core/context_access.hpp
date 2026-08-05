/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_CORE_CONTEXT_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_CORE_CONTEXT_ACCESS_HPP_INCLUDED

#include <zlink/Contracts/Core/context.hpp>

namespace zlink
{
namespace detail
{

struct context_access_t
{
    static void *native_handle (context_t &ctx_) noexcept;
    static const void *native_handle (const context_t &ctx_) noexcept;
};

inline void *native_handle (context_t &ctx_) noexcept
{
    return context_access_t::native_handle (ctx_);
}

inline const void *native_handle (const context_t &ctx_) noexcept
{
    return context_access_t::native_handle (ctx_);
}

} // namespace detail
} // namespace zlink

#endif
