/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_CORE_ROUTING_ID_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_CORE_ROUTING_ID_ACCESS_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>

#include <zlink.h>

#include <cstring>

namespace zlink::detail
{

struct routing_id_access_t
{
    static routing_id_t from_native (const zlink_routing_id_t &native_) noexcept
    {
        routing_id_t out;
        out.assign_unchecked (native_.data, native_.size);
        return out;
    }

    static void assign_native (routing_id_t &routing_id_,
                               const zlink_routing_id_t &native_) noexcept
    {
        routing_id_.assign_unchecked (native_.data, native_.size);
    }

    static zlink_routing_id_t to_native (const routing_id_t &routing_id_) noexcept
    {
        zlink_routing_id_t native;
        std::memset (&native, 0, sizeof (native));
        native.size = static_cast<uint8_t> (routing_id_._size);
        if (routing_id_._size > 0)
            std::memcpy (native.data, routing_id_._data.data (), routing_id_._size);
        return native;
    }
};

inline routing_id_t native_routing_id (const zlink_routing_id_t &native_) noexcept
{
    return routing_id_access_t::from_native (native_);
}

inline void assign_routing_id_native (routing_id_t &routing_id_,
                                      const zlink_routing_id_t &native_) noexcept
{
    routing_id_access_t::assign_native (routing_id_, native_);
}

inline zlink_routing_id_t routing_id_native_value (const routing_id_t &routing_id_) noexcept
{
    return routing_id_access_t::to_native (routing_id_);
}

inline const zlink_routing_id_t *routing_id_native (const routing_id_t &routing_id_) noexcept
{
    // Returned pointers share a small thread-local ring and are only for immediate
    // single-call native handoff. Use routing_id_native_value() when a call needs
    // more than one native routing id pointer at the same time.
    constexpr size_t native_ring_capacity = 8u;
    thread_local zlink_routing_id_t native[native_ring_capacity];
    thread_local size_t index = 0;
    zlink_routing_id_t &slot = native[index++ % native_ring_capacity];
    slot = routing_id_access_t::to_native (routing_id_);
    return &slot;
}

inline zlink_routing_id_t *routing_id_native (routing_id_t &routing_id_) noexcept
{
    return const_cast<zlink_routing_id_t *> (
      routing_id_native (static_cast<const routing_id_t &> (routing_id_)));
}

routing_id_t routing_id_from_native_pointer (const void *native_) noexcept;

} // namespace zlink::detail

#endif
