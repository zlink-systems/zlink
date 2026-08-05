/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_API_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <mutex>

#include "sockets/common/socket_base.hpp"
#include "zlink.h"

struct socket_handle_t
{
    zlink::socket_base_t *socket;
};

static inline socket_handle_t make_socket_handle (zlink::socket_base_t *socket_)
{
    socket_handle_t handle;
    handle.socket = socket_;
    return handle;
}

static inline zlink::socket_base_t *try_as_socket (void *s_)
{
    if (!s_)
        return NULL;

    zlink::socket_base_t *socket = static_cast<zlink::socket_base_t *> (s_);
    return socket->check_tag () ? socket : NULL;
}

static inline socket_handle_t as_socket_handle (void *s_)
{
    socket_handle_t handle = make_socket_handle (NULL);

    if (!s_) {
        errno = EFAULT;
        return handle;
    }

    zlink::socket_base_t *s = try_as_socket (s_);
    if (!s) {
        errno = EFAULT;
        return handle;
    }

    return make_socket_handle (s);
}

static inline bool is_stream_type (socket_handle_t handle_)
{
    return handle_.socket && handle_.socket->socket_type () == ZLINK_CORE_SOCKET_STREAM;
}

static inline int socket_type (socket_handle_t handle_)
{
    return handle_.socket ? handle_.socket->socket_type () : -1;
}

static inline int validate_recv_flags (int flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static inline int core_socket_type_from_public_type (zlink_socket_type_t type_)
{
    switch (type_) {
        case ZLINK_SOCKET_PAIR:
            return ZLINK_CORE_SOCKET_PAIR;
        case ZLINK_SOCKET_PUB:
            return ZLINK_CORE_SOCKET_PUB;
        case ZLINK_SOCKET_SUB:
            return ZLINK_CORE_SOCKET_SUB;
        case ZLINK_SOCKET_DEALER:
            return ZLINK_CORE_SOCKET_DEALER;
        case ZLINK_SOCKET_ROUTER:
            return ZLINK_CORE_SOCKET_ROUTER;
        case ZLINK_SOCKET_XPUB:
            return ZLINK_CORE_SOCKET_XPUB;
        case ZLINK_SOCKET_XSUB:
            return ZLINK_CORE_SOCKET_XSUB;
        case ZLINK_SOCKET_STREAM:
            return ZLINK_CORE_SOCKET_STREAM;
        default:
            return -1;
    }
}

static inline bool is_send_only_socket_type (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PUB;
}

class stream_api_lock_t
{
  public:
    explicit stream_api_lock_t (socket_handle_t handle_) : _lock ()
    {
        if (!handle_.socket)
            return;

        std::recursive_mutex *mutex = handle_.socket->api_sync_mutex ();
        if (mutex)
            _lock = std::unique_lock<std::recursive_mutex> (*mutex);
    }

  private:
    std::unique_lock<std::recursive_mutex> _lock;
};

#endif
