/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace zlink::framework
{

enum class framework_error_kind_t
{
    not_found = 0,
    already_exists = 1,
    type_mismatch = 2,
    not_configured = 3,
    rejected = 4,
    unavailable = 5,
    capacity_exceeded = 6,
    deadline_exceeded = 7,
    shutting_down = 8,
    protocol_error = 9,
    invalid_operation = 10,
    data_lost = 11,
    internal_failure = 12
};

namespace detail
{

/* Lifecycle/transport boundary conditions demoted from the public error enum
 * (shared implementation-gap §5.2). They surface at the awaited boundary as
 * the C++ standard convention — std::system_error with the mapped errc — while
 * the internal runtime keeps the precise state. */
enum class boundary_error_t
{
    none = 0,
    timed_out = 1,
    shutdown = 2,
    disconnected = 3,
    closed = 4,
    cancelled = 5,
    stale_generation = 6
};

enum class failure_origin_t
{
    none,
    payload_encode,
    payload_decode,
    actor_transfer_in_progress
};

/* Who produced the failure. `framework` marks errors the framework runtime
 * generated on its own (route resolution failure, sealed admission, dispatch
 * rejection); replies for such errors carry the `zlink.origin=framework`
 * metadata marker. `application` marks errors decoded from a peer error reply
 * that did not carry the marker — the remote application handler produced
 * them, so route caches must not read them as a stale-route signal. */
enum class error_origin_t
{
    unspecified = 0,
    framework = 1,
    application = 2
};

inline std::error_code boundary_error_code (boundary_error_t state) noexcept
{
    switch (state) {
        case boundary_error_t::timed_out:
            return std::make_error_code (std::errc::timed_out);
        case boundary_error_t::shutdown:
        case boundary_error_t::cancelled:
            return std::make_error_code (std::errc::operation_canceled);
        case boundary_error_t::disconnected:
            return std::make_error_code (std::errc::not_connected);
        case boundary_error_t::closed:
            return std::make_error_code (std::errc::connection_aborted);
        case boundary_error_t::stale_generation:
            return std::make_error_code (std::errc::operation_not_permitted);
        case boundary_error_t::none:
            break;
    }
    return {};
}

} // namespace detail

class framework_exception_t : public std::exception
{
  public:
    framework_exception_t (framework_error_kind_t kind,
                           std::string message) :
        _kind (kind),
        _message (std::move (message))
    {
    }

    framework_error_kind_t kind () const noexcept { return _kind; }

    /* Boundary conditions demoted from the public kind enum surface through
     * the standard error-code facet: an empty code means a plain framework
     * error, otherwise the mapped std::errc identifies the boundary. */
    std::error_code code () const noexcept { return detail::boundary_error_code (_boundary); }

    const char *what () const noexcept override { return _message.c_str (); }

  private:
    friend framework_exception_t detail_make_boundary_exception (detail::boundary_error_t state,
                                                                 std::string message);
    friend detail::boundary_error_t
    detail_boundary_state (const framework_exception_t &error) noexcept;
    friend framework_exception_t detail_make_origin_exception (
      framework_error_kind_t kind,
      detail::failure_origin_t origin,
      std::string message);
    friend detail::failure_origin_t
    detail_failure_origin (const framework_exception_t &error) noexcept;
    friend framework_exception_t detail_with_error_origin (
      framework_exception_t error, detail::error_origin_t origin) noexcept;
    friend detail::error_origin_t
    detail_error_origin (const framework_exception_t &error) noexcept;

    framework_error_kind_t _kind;
    std::string _message;
    detail::boundary_error_t _boundary = detail::boundary_error_t::none;
    detail::failure_origin_t _origin = detail::failure_origin_t::none;
    detail::error_origin_t _error_origin = detail::error_origin_t::unspecified;
};

inline framework_exception_t detail_make_boundary_exception (detail::boundary_error_t state,
                                                             std::string message)
{
    const auto kind = state == detail::boundary_error_t::timed_out
                        ? framework_error_kind_t::deadline_exceeded
                      : state == detail::boundary_error_t::shutdown
                        ? framework_error_kind_t::shutting_down
                      : state == detail::boundary_error_t::disconnected
                          || state == detail::boundary_error_t::closed
                          || state == detail::boundary_error_t::stale_generation
                        ? framework_error_kind_t::unavailable
                      : state == detail::boundary_error_t::cancelled
                        ? framework_error_kind_t::invalid_operation
                        : framework_error_kind_t::internal_failure;
    framework_exception_t error (kind, std::move (message));
    error._boundary = state;
    return error;
}

inline detail::boundary_error_t
detail_boundary_state (const framework_exception_t &error) noexcept
{
    return error._boundary;
}

inline framework_exception_t detail_make_origin_exception (
  framework_error_kind_t kind,
  detail::failure_origin_t origin,
  std::string message)
{
    framework_exception_t error (kind, std::move (message));
    error._origin = origin;
    return error;
}

inline detail::failure_origin_t
detail_failure_origin (const framework_exception_t &error) noexcept
{
    return error._origin;
}

inline framework_exception_t detail_with_error_origin (framework_exception_t error,
                                                       detail::error_origin_t origin) noexcept
{
    error._error_origin = origin;
    return error;
}

inline detail::error_origin_t
detail_error_origin (const framework_exception_t &error) noexcept
{
    return error._error_origin;
}

namespace detail
{

inline framework_exception_t
make_boundary_exception (boundary_error_t state, std::string message)
{
    return detail_make_boundary_exception (state, std::move (message));
}

inline framework_exception_t
with_error_origin (framework_exception_t error, error_origin_t origin) noexcept
{
    return detail_with_error_origin (std::move (error), origin);
}

/* Framework-generated failure (route resolution, sealed admission, dispatch
 * rejection): its error reply carries the `zlink.origin=framework` marker. */
inline framework_exception_t
make_framework_origin_exception (framework_error_kind_t kind, std::string message)
{
    return detail_with_error_origin (framework_exception_t (kind, std::move (message)),
                                     error_origin_t::framework);
}

inline error_origin_t error_origin (const framework_exception_t &error) noexcept
{
    return detail_error_origin (error);
}

inline bool is_transient_error (framework_error_kind_t kind) noexcept
{
    return kind == framework_error_kind_t::unavailable
           || kind == framework_error_kind_t::deadline_exceeded
           || kind == framework_error_kind_t::capacity_exceeded;
}

inline boundary_error_t boundary_state (const framework_exception_t &error) noexcept
{
    return detail_boundary_state (error);
}

inline framework_exception_t
make_origin_exception (framework_error_kind_t kind,
                       failure_origin_t origin,
                       std::string message)
{
    return detail_make_origin_exception (kind, origin, std::move (message));
}

inline failure_origin_t failure_origin (const framework_exception_t &error) noexcept
{
    return detail_failure_origin (error);
}

} // namespace detail

} // namespace zlink::framework
