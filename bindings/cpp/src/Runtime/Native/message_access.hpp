/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_ACCESS_HPP_INCLUDED

#include <zlink/Contracts/Messaging/message.hpp>

#include <zlink.h>

namespace zlink
{
namespace detail
{

struct message_access_t
{
    static zlink_msg_t *native (message_t &message_) noexcept
    {
        return reinterpret_cast<zlink_msg_t *> (message_._storage.data ());
    }

    static const zlink_msg_t *native (const message_t &message_) noexcept
    {
        return reinterpret_cast<const zlink_msg_t *> (message_._storage.data ());
    }

    static bool &valid (message_t &message_) noexcept { return message_._valid; }

    static bool &has_payload (message_t &message_) noexcept { return message_._has_payload; }

    static const bool &valid (const message_t &message_) noexcept { return message_._valid; }

    static const bool &has_payload (const message_t &message_) noexcept
    {
        return message_._has_payload;
    }

    static void close_noexcept (message_t &message_) noexcept { message_.close_noexcept (); }
};

static_assert (sizeof (zlink_msg_t) <= 64, "message_t opaque storage is too small for zlink_msg_t");
static_assert (alignof (zlink_msg_t) <= alignof (message_t),
               "message_t opaque storage is under-aligned for zlink_msg_t");

inline zlink_msg_t *native_handle (message_t &message_) noexcept
{
    return message_access_t::native (message_);
}

inline const zlink_msg_t *native_handle (const message_t &message_) noexcept
{
    return message_access_t::native (message_);
}

inline void adopt_native_message (message_t &message_, zlink_msg_t *src_)
{
    if (!src_)
        return;
    message_access_t::close_noexcept (message_);
    *message_access_t::native (message_) = *src_;
    if (zlink_msg_init (src_) != 0)
        return;
    message_access_t::valid (message_) = true;
    message_access_t::has_payload (message_) = zlink_msg_size (
      message_access_t::native (message_)) > 0;
}

inline void move_to_native (message_t &message_, zlink_msg_t *dest_)
{
    if (!dest_ || !message_.valid ())
        return;
    *dest_ = *message_access_t::native (message_);
    message_access_t::valid (message_) = false;
    message_access_t::has_payload (message_) = false;
}

inline void mark_sent (message_t &message_) noexcept
{
    message_access_t::valid (message_) = false;
    message_access_t::has_payload (message_) = false;
}

inline bool has_payload (const message_t &message_) noexcept
{
    return message_access_t::has_payload (message_);
}

inline void refresh_payload_presence (message_t &message_) noexcept
{
    message_access_t::has_payload (message_) = message_.valid ()
      && zlink_msg_size (native_handle (message_)) > 0;
}

} // namespace detail
} // namespace zlink

#endif
