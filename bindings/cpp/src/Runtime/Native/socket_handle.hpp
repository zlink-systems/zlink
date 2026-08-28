/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>

#include <atomic>

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
    socket_handle_t () noexcept : _socket (nullptr), _open (false), _own (false) {}

    explicit socket_handle_t (void *socket_, bool own_ = true) noexcept :
        _socket (socket_), _open (socket_ != nullptr), _own (own_)
    {
    }

    ~socket_handle_t () { (void) close (); }

    socket_handle_t (socket_handle_t &&other) noexcept :
        _socket (other._socket),
        _open (other._open.load (std::memory_order_acquire)),
        _own (other._own)
    {
        other._socket = nullptr;
        other._open.store (false, std::memory_order_release);
        other._own = false;
    }

    socket_handle_t &operator= (socket_handle_t &&other) noexcept
    {
        if (this == &other)
            return *this;

        (void) close ();
        _socket = other._socket;
        _open.store (other._open.load (std::memory_order_acquire),
                     std::memory_order_release);
        _own = other._own;
        other._socket = nullptr;
        other._open.store (false, std::memory_order_release);
        other._own = false;
        return *this;
    }

    socket_handle_t (const socket_handle_t &) = delete;
    socket_handle_t &operator= (const socket_handle_t &) = delete;

    bool valid () const noexcept
    {
        return _socket != nullptr && _open.load (std::memory_order_acquire);
    }

    [[nodiscard]] int close () noexcept
    {
        if (!_socket || !_open.load (std::memory_order_acquire))
            return 0;

        if (!_own) {
            _open.store (false, std::memory_order_release);
            return 0;
        }

        const int rc = zlink_close (_socket);
        if (rc == 0 || rc == ZLINK_CLOSE_SHUTDOWN)
            _open.store (false, std::memory_order_release);
        return rc;
    }

  protected:
    void *handle () noexcept { return _socket; }
    const void *handle () const noexcept { return _socket; }

    void reset_handle (void *socket_, bool own_) noexcept
    {
        _socket = socket_;
        _open.store (socket_ != nullptr, std::memory_order_release);
        _own = own_;
    }

  private:
    friend void *native_handle (socket_handle_t &socket_) noexcept;
    friend const void *native_handle (const socket_handle_t &socket_) noexcept;

    void *_socket;
    std::atomic<bool> _open;
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
