/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace zlink::framework
{

namespace detail
{
struct result_access_t;

inline const framework_exception_t *framework_error (const std::exception_ptr &exception) noexcept
{
    if (!exception)
        return nullptr;
    try {
        std::rethrow_exception (exception);
    }
    catch (const framework_exception_t &error) {
        return &error;
    }
    catch (...) {
        return nullptr;
    }
}

inline framework_error_kind_t result_error_kind (const std::exception_ptr &exception)
{
    if (const auto *error = framework_error (exception))
        return error->kind ();
    throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                 "result has no Framework error");
}

inline std::exception_ptr result_exception (std::exception_ptr exception)
{
    const auto *error = framework_error (exception);
    if (error && boundary_state (*error) == boundary_error_t::cancelled)
        return std::make_exception_ptr (std::system_error (error->code (), error->what ()));
    return exception;
}
} // namespace detail

template <typename T> class result_t
{
  public:
    static result_t success (T value) { return result_t (std::in_place, std::move (value)); }

    static result_t failure (framework_error_kind_t kind, std::string message)
    {
        return result_t (framework_exception_t (kind, std::move (message)));
    }

    bool has_value () const noexcept { return _value.has_value (); }

    explicit operator bool () const noexcept { return has_value (); }

    const T &value () const
    {
        if (!_value) {
            std::rethrow_exception (_error);
        }
        return *_value;
    }

    T &value ()
    {
        if (!_value) {
            std::rethrow_exception (_error);
        }
        return *_value;
    }

    const framework_exception_t *error () const noexcept
    {
        return detail::framework_error (_error);
    }

    std::exception_ptr exception () const noexcept { return _error; }

    framework_error_kind_t error_kind () const { return detail::result_error_kind (_error); }

  private:
    friend struct detail::result_access_t;

    explicit result_t (std::in_place_t, T &&value)
    {
        if constexpr (std::is_move_constructible_v<T>)
            _value.emplace (std::move (value));
        else
            _value.emplace (value);
    }
    explicit result_t (framework_exception_t error) :
        result_t (std::make_exception_ptr (std::move (error)))
    {
    }
    explicit result_t (std::exception_ptr error) :
        _error (detail::result_exception (std::move (error)))
    {
    }

    std::optional<T> _value;
    std::exception_ptr _error;
};

template <> class result_t<void>
{
  public:
    static result_t success () { return result_t (); }

    static result_t failure (framework_error_kind_t kind, std::string message)
    {
        return result_t (framework_exception_t (kind, std::move (message)));
    }

    bool has_value () const noexcept { return !_error; }

    explicit operator bool () const noexcept { return has_value (); }

    void value () const
    {
        if (_error) {
            std::rethrow_exception (_error);
        }
    }

    const framework_exception_t *error () const noexcept
    {
        return detail::framework_error (_error);
    }

    std::exception_ptr exception () const noexcept { return _error; }

    framework_error_kind_t error_kind () const { return detail::result_error_kind (_error); }

  private:
    friend struct detail::result_access_t;

    result_t () = default;
    explicit result_t (framework_exception_t error) :
        result_t (std::make_exception_ptr (std::move (error)))
    {
    }
    explicit result_t (std::exception_ptr error) :
        _error (detail::result_exception (std::move (error)))
    {
    }

    std::exception_ptr _error;
};

namespace detail
{

struct result_access_t
{
    template <typename T> static result_t<T> failure (framework_exception_t error)
    {
        return result_t<T> (std::move (error));
    }

    template <typename T> static result_t<T> failure (std::exception_ptr error)
    {
        return result_t<T> (std::move (error));
    }
};

template <typename T> result_t<T> boundary_failure (boundary_error_t state, std::string message)
{
    return result_access_t::failure<T> (make_boundary_exception (state, std::move (message)));
}

/* Preserve the original exception across result value types. */
template <typename T, typename U>
result_t<T> propagate_failure (const result_t<U> &from, std::string fallback_message)
{
    if (auto error = from.exception ()) {
        return result_access_t::failure<T> (std::move (error));
    }
    return result_access_t::failure<T> (framework_exception_t (
      framework_error_kind_t::internal_failure, std::move (fallback_message)));
}

} // namespace detail

} // namespace zlink::framework
