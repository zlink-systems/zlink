/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>

#include <zlink.h>

namespace zlink
{

namespace detail
{
class socket_handle_t;
inline void *native_handle (socket_handle_t &socket_) noexcept;
inline const void *native_handle (const socket_handle_t &socket_) noexcept;

class socket_handle_t
{
  public:
    socket_handle_t () noexcept : _socket (nullptr), _own (false) {}

    explicit socket_handle_t (void *socket_, bool own_ = true) noexcept :
        _socket (socket_), _own (own_)
    {
    }

    ~socket_handle_t () { (void) close (); }

    socket_handle_t (socket_handle_t &&other) noexcept : _socket (other._socket), _own (other._own)
    {
        other._socket = nullptr;
        other._own = false;
    }

    socket_handle_t &operator= (socket_handle_t &&other) noexcept
    {
        if (this == &other)
            return *this;

        (void) close ();
        _socket = other._socket;
        _own = other._own;
        other._socket = nullptr;
        other._own = false;
        return *this;
    }

    socket_handle_t (const socket_handle_t &) = delete;
    socket_handle_t &operator= (const socket_handle_t &) = delete;

    bool valid () const noexcept { return _socket != nullptr; }

    [[nodiscard]] int close () noexcept
    {
        if (!_socket) {
            _own = false;
            return 0;
        }

        if (!_own) {
            _socket = nullptr;
            return 0;
        }

        void *socket = _socket;
        const int rc = zlink_close (socket);
        if (rc == 0) {
            _socket = nullptr;
            _own = false;
        }
        return rc;
    }

  protected:
    void *handle () noexcept { return _socket; }
    const void *handle () const noexcept { return _socket; }

    void reset_handle (void *socket_, bool own_) noexcept
    {
        _socket = socket_;
        _own = own_;
    }

  private:
    friend void *native_handle (socket_handle_t &socket_) noexcept;
    friend const void *native_handle (const socket_handle_t &socket_) noexcept;

    void *_socket;
    bool _own;
};

inline void *native_handle (socket_handle_t &socket_) noexcept
{
    return socket_._socket;
}

inline const void *native_handle (const socket_handle_t &socket_) noexcept
{
    return socket_._socket;
}

} // namespace detail

} // namespace zlink

#endif
