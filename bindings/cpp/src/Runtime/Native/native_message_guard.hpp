/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_GUARD_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_MESSAGE_GUARD_HPP_INCLUDED

#include <Runtime/Native/message_access.hpp>

#include <cerrno>

namespace zlink
{
namespace detail
{

class scoped_native_message_t
{
  public:
    scoped_native_message_t () = default;

    scoped_native_message_t (const scoped_native_message_t &) = delete;
    scoped_native_message_t &operator= (const scoped_native_message_t &) = delete;

    ~scoped_native_message_t ()
    {
        if (_initialized) {
            const int saved_errno = errno;
            (void) zlink_msg_close (&_message);
            errno = saved_errno;
        }
    }

    [[nodiscard]] bool init () noexcept
    {
        _initialized = zlink_msg_init (&_message) == 0;
        return _initialized;
    }

    [[nodiscard]] zlink_msg_t *get () noexcept { return &_message; }

    void adopt_into (message_t &message_) { adopt_native_message (message_, &_message); }

  private:
    zlink_msg_t _message;
    bool _initialized = false;
};

} // namespace detail
} // namespace zlink

#endif
