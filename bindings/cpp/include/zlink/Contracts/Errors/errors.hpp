/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/capability.hpp"
#include "../Core/routing_id.hpp"
#include "../Messaging/request_result.hpp"
#include "../Sockets/results.hpp"
#include "results.hpp"

#include <cerrno>
#include <zlink_errno.h>
#include <sstream>

namespace zlink
{

namespace detail
{
int current_errno () noexcept;
} // namespace detail

/// @brief Base class for all exceptions thrown by the zlink C++ bindings.
class binding_error_t : public std::runtime_error
{
  public:
    int code () const noexcept { return _code; }
    int internal_errno () const noexcept { return _internal_errno; }

    const char *what () const noexcept override { return _message.c_str (); }

  protected:
    binding_error_t (int code_, int internal_errno_) :
        std::runtime_error (build_message (code_, internal_errno_)),
        _code (code_),
        _internal_errno (internal_errno_),
        _message (build_message (code_, internal_errno_))
    {
    }

    binding_error_t (const binding_error_t &) = default;
    binding_error_t &operator= (const binding_error_t &) = default;

  private:
    static std::string build_message (int code_, int internal_errno_)
    {
        std::ostringstream stream;
        stream << error_text (code_);
        if (internal_errno_ != 0)
            stream << " (errno=" << internal_errno_ << ")";
        return stream.str ();
    }

    int _code;
    int _internal_errno;
    std::string _message;
};

/// @brief General zlink error carrying a native result code.
class error_t : public binding_error_t
{
  public:
    explicit error_t (int code_) : error_t (code_, code_) {}

    error_t (int code_, int internal_errno_) : binding_error_t (code_, internal_errno_) {}
};

/// @brief Thrown when submitting a send or publish fails.
class submit_error_t : public binding_error_t
{
  public:
    explicit submit_error_t (submit_result_t result_) :
        submit_error_t (result_, detail::current_errno ())
    {
    }

    submit_error_t (submit_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    submit_result_t result () const noexcept { return _result; }

  private:
    submit_result_t _result;
};

/// @brief Thrown when a request fails or its reply reports an error.
class request_error_t : public binding_error_t
{
  public:
    explicit request_error_t (request_result_t result_) :
        request_error_t (result_, detail::current_errno ())
    {
    }

    request_error_t (request_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    request_result_t result () const noexcept { return _result; }

  private:
    request_result_t _result;
};

/// @brief Thrown when receiving a message fails.
class recv_error_t : public binding_error_t
{
  public:
    explicit recv_error_t (recv_result_t result_) : recv_error_t (result_, detail::current_errno ())
    {
    }

    recv_error_t (recv_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    recv_result_t result () const noexcept { return _result; }

  private:
    recv_result_t _result;
};

/// @brief Thrown when registering or running a callback handler fails.
class handler_error_t : public binding_error_t
{
  public:
    explicit handler_error_t (handler_result_t result_) :
        handler_error_t (result_, detail::current_errno ())
    {
    }

    handler_error_t (handler_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    handler_result_t result () const noexcept { return _result; }

  private:
    handler_result_t _result;
};

/// @brief Thrown when closing a socket or resource fails.
class close_error_t : public binding_error_t
{
  public:
    explicit close_error_t (close_result_t result_) :
        close_error_t (result_, detail::current_errno ())
    {
    }

    close_error_t (close_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    close_result_t result () const noexcept { return _result; }

  private:
    close_result_t _result;
};

/// @brief Thrown when binding a socket to an endpoint fails.
class bind_error_t : public binding_error_t
{
  public:
    explicit bind_error_t (bind_result_t result_) : bind_error_t (result_, detail::current_errno ())
    {
    }

    bind_error_t (bind_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    bind_result_t result () const noexcept { return _result; }

  private:
    bind_result_t _result;
};

/// @brief Thrown when connecting a socket to an endpoint fails.
class connect_error_t : public binding_error_t
{
  public:
    explicit connect_error_t (connect_result_t result_) :
        connect_error_t (result_, detail::current_errno ())
    {
    }

    connect_error_t (connect_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    connect_result_t result () const noexcept { return _result; }

  private:
    connect_result_t _result;
};

/// @brief Thrown when reading or applying a configuration option fails.
class config_error_t : public binding_error_t
{
  public:
    explicit config_error_t (config_result_t result_) :
        config_error_t (result_, detail::current_errno ())
    {
    }

    config_error_t (config_result_t result_, int internal_errno_) :
        binding_error_t (static_cast<int> (result_), internal_errno_), _result (result_)
    {
    }

    config_result_t result () const noexcept { return _result; }

  private:
    config_result_t _result;
};

namespace detail
{

inline bool is_invalid_handle_errno (int err_)
{
    return err_ == EFAULT || err_ == ENOTSOCK || err_ == EBADF || err_ == ESHUTDOWN;
}

inline config_result_t config_result_from_errno (int err_)
{
    if (err_ == ENOTSUP)
        return config_result_t::not_supported;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    if (err_ == EOPNOTSUPP)
        return config_result_t::not_supported;
#endif
    if (is_invalid_handle_errno (err_))
        return config_result_t::invalid_handle;
    return config_result_t::invalid_argument;
}

inline handler_result_t handler_result_from_errno (int err_)
{
    if (err_ == EBUSY)
        return handler_result_t::busy;
    if (err_ == EDEADLK)
        return handler_result_t::deadlock;
    if (err_ == ENOTSUP)
        return handler_result_t::not_supported;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    if (err_ == EOPNOTSUPP)
        return handler_result_t::not_supported;
#endif
    if (is_invalid_handle_errno (err_))
        return handler_result_t::invalid_handle;
    return handler_result_t::invalid_argument;
}

inline submit_result_t submit_result_from_errno (int err_)
{
    switch (err_) {
        case EAGAIN:
            return submit_result_t::backpressured;
        case ENOTCONN:
            return submit_result_t::not_connected;
        case ENOENT:
            return submit_result_t::not_found;
        case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
#endif
            return submit_result_t::not_supported;
        case EBUSY:
            return submit_result_t::invalid_state;
        case EPERM:
            return submit_result_t::thread_violation;
        case ENOMEM:
            return submit_result_t::out_of_memory;
        default:
            break;
    }

    if (is_invalid_handle_errno (err_))
        return submit_result_t::invalid_handle;
    return submit_result_t::invalid_argument;
}

template <typename ErrorT, typename ResultT> inline void throw_if_failed (ResultT result_)
{
    if (static_cast<int> (result_) != 0)
        throw ErrorT (result_);
}

template <typename ErrorT, typename ResultT>
inline void throw_if_failed (ResultT result_, int internal_errno_)
{
    if (static_cast<int> (result_) != 0)
        throw ErrorT (result_, internal_errno_);
}

inline void throw_on_error (int rc_)
{
    if (rc_ != 0)
        throw error_t (current_errno (), current_errno ());
}

inline error_t last_error ()
{
    return error_t (current_errno (), current_errno ());
}

inline void throw_if_reply_flags_unsupported (send_flags_t flags_)
{
    if (flags_ != send_flags_t::none)
        throw submit_error_t (submit_result_t::not_supported, ENOTSUP);
}

} // namespace detail

using detail::last_error;
using detail::throw_on_error;

} // namespace zlink
