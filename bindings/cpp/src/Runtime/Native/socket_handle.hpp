/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_SOCKET_HANDLE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>

#include <zlink/Contracts/Errors/errors.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace zlink
{

namespace detail
{
struct socket_runtime_state_t;
class socket_handle_t;
inline void *native_handle (socket_handle_t &socket_) noexcept;
inline const void *native_handle (const socket_handle_t &socket_) noexcept;

inline void require_submit_open (bool open_)
{
    if (!open_)
        throw submit_error_t (submit_result_t::terminated, ESHUTDOWN);
}

class socket_handle_t
{
  public:
    socket_handle_t () noexcept : _socket (nullptr), _state (closed), _own (false) {}

    explicit socket_handle_t (void *socket_, bool own_ = true) noexcept :
        _socket (socket_), _state (socket_ ? 0 : closed), _own (own_)
    {
    }

    ~socket_handle_t () { (void) close (); }

    socket_handle_t (const socket_handle_t &) = delete;
    socket_handle_t &operator= (const socket_handle_t &) = delete;

    bool valid () const noexcept
    {
        return (_state.load (std::memory_order_acquire) & closed) == 0;
    }

    // Bound once before the socket is published; never changed during use.
    std::weak_ptr<socket_runtime_state_t> runtime;

    bool close_in_progress () const noexcept
    {
        return (_state.load (std::memory_order_acquire) & closing) != 0;
    }

    bool reserve () noexcept
    {
        auto state = _state.load (std::memory_order_acquire);
        for (;;) {
            if (state & closed)
                return false;
            if (state & closing) {
                // Acquire observes close's release-published outcome before retry;
                // only closed is terminal, because a rejected close leaves it open.
                _state.wait (state, std::memory_order_acquire);
                state = _state.load (std::memory_order_acquire);
                continue;
            }
            if (_state.compare_exchange_weak (state, state + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                return true;
        }
    }

    void release () noexcept { _state.fetch_sub (1, std::memory_order_release); }

    [[nodiscard]] int close () noexcept
    {
        uint64_t expected = 0;
        if (!_state.compare_exchange_strong (expected, closing, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (expected & closed) return 0;
            errno = EBUSY;
            return ZLINK_CLOSE_BUSY;
        }

        const int rc = _own ? zlink_close (_socket) : 0;
        _state.store (rc == 0 || rc == ZLINK_CLOSE_SHUTDOWN ? closed : 0, std::memory_order_release);
        _state.notify_all ();
        return rc;
    }

  private:
    friend void *native_handle (socket_handle_t &socket_) noexcept;
    friend const void *native_handle (const socket_handle_t &socket_) noexcept;

    static constexpr uint64_t closing = uint64_t{1} << 63;
    static constexpr uint64_t closed = uint64_t{1} << 62;
    void *const _socket;
    // The low bits count native calls; close can claim only an idle owner.
    // One CAS orders reservation against close without serializing records.
    std::atomic<uint64_t> _state;
    const bool _own;
};

inline void *native_handle (socket_handle_t &socket_) noexcept
{
    return socket_._socket;
}

inline const void *native_handle (const socket_handle_t &socket_) noexcept
{
    return socket_._socket;
}

inline socket_handle_t &submit_handle (const std::shared_ptr<socket_handle_t> &socket_)
{
    require_submit_open (static_cast<bool> (socket_));
    return *socket_;
}

// The caller retains the socket member, operation handle, or runtime/completion
// owner until this borrowed reservation releases the native lifetime.
class socket_call_t
{
  public:
    explicit socket_call_t (socket_handle_t &socket_) :
        _socket (socket_)
    {
        require_submit_open (_socket.reserve ());
    }
    ~socket_call_t () { _socket.release (); }
    socket_call_t (const socket_call_t &) = delete;
    socket_call_t &operator= (const socket_call_t &) = delete;

    void *handle () const noexcept { return native_handle (_socket); }

  private:
    socket_handle_t &_socket;
};

} // namespace detail

} // namespace zlink

#endif
