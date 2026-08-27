/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_API_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <mutex>

#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_public_handle.hpp"
#include "zlink.h"

class socket_handle_t
{
  public:
    socket_handle_t () : socket (NULL), _public_handle (NULL) {}

    socket_handle_t (const socket_handle_t &other_) :
        socket (other_.socket), _public_handle (other_._public_handle)
    {
        if (_public_handle)
            _public_handle->add_ref ();
    }

    socket_handle_t &operator= (const socket_handle_t &other_)
    {
        if (this == &other_)
            return *this;
        if (other_._public_handle)
            other_._public_handle->add_ref ();
        if (_public_handle)
            _public_handle->release ();
        socket = other_.socket;
        _public_handle = other_._public_handle;
        return *this;
    }

    ~socket_handle_t ()
    {
        if (_public_handle)
            _public_handle->release ();
    }

    bool begin_close ()
    {
        return _public_handle && _public_handle->begin_close ();
    }

    void cancel_close ()
    {
        if (_public_handle)
            _public_handle->cancel_close ();
    }

    zlink::socket_base_t *socket;

  private:
    friend socket_handle_t make_socket_handle (zlink::socket_base_t *);
    friend socket_handle_t as_socket_handle (void *);

    socket_handle_t (zlink::socket_base_t *socket_,
                     zlink::socket_public_handle_t *public_handle_) :
        socket (socket_), _public_handle (public_handle_)
    {
    }

    zlink::socket_public_handle_t *_public_handle;
};

inline socket_handle_t make_socket_handle (zlink::socket_base_t *socket_)
{
    return socket_handle_t (socket_, NULL);
}

inline socket_handle_t as_socket_handle (void *s_)
{
    if (!s_) {
        errno = EFAULT;
        return socket_handle_t ();
    }

    zlink::socket_public_handle_t *public_handle =
      static_cast<zlink::socket_public_handle_t *> (s_);
    if (!public_handle->check_tag ()) {
        errno = EFAULT;
        return socket_handle_t ();
    }

    zlink::socket_base_t *socket = NULL;
    if (!public_handle->acquire (&socket))
        return socket_handle_t ();
    if (!socket->check_tag ()) {
        public_handle->release ();
        errno = EFAULT;
        return socket_handle_t ();
    }

    return socket_handle_t (socket, public_handle);
}

static inline bool is_stream_type (const socket_handle_t &handle_)
{
    return handle_.socket && handle_.socket->socket_type () == ZLINK_CORE_SOCKET_STREAM;
}

static inline int socket_type (const socket_handle_t &handle_)
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
    explicit stream_api_lock_t (const socket_handle_t &handle_) : _lock ()
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
