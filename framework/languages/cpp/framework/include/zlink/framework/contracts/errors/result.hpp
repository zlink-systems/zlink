/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>

#include <optional>
#include <utility>

namespace zlink::framework
{

namespace detail
{
struct result_access_t;
} // namespace detail

template <typename T> class result_t
{
  public:
    static result_t success (T value) { return result_t (std::move (value)); }

    static result_t
    failure (framework_error_kind_t kind, std::string message)
    {
        return result_t (framework_exception_t (kind, std::move (message)));
    }

    bool has_value () const noexcept { return _value.has_value (); }

    explicit operator bool () const noexcept { return has_value (); }

    const T &value () const
    {
        if (!_value) {
            throw *_error;
        }
        return *_value;
    }

    T &value ()
    {
        if (!_value) {
            throw *_error;
        }
        return *_value;
    }

    const framework_exception_t *error () const noexcept { return _error ? &*_error : nullptr; }

    framework_error_kind_t error_kind () const
    {
        if (!_error) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "successful result has no error");
        }
        return _error->kind ();
    }

  private:
    friend struct detail::result_access_t;

    explicit result_t (T value) : _value (std::move (value)) {}
    explicit result_t (framework_exception_t error) : _error (std::move (error)) {}

    std::optional<T> _value;
    std::optional<framework_exception_t> _error;
};

template <> class result_t<void>
{
  public:
    static result_t success () { return result_t (); }

    static result_t
    failure (framework_error_kind_t kind, std::string message)
    {
        return result_t (framework_exception_t (kind, std::move (message)));
    }

    bool has_value () const noexcept { return !_error.has_value (); }

    explicit operator bool () const noexcept { return has_value (); }

    void value () const
    {
        if (_error) {
            throw *_error;
        }
    }

    const framework_exception_t *error () const noexcept { return _error ? &*_error : nullptr; }

    framework_error_kind_t error_kind () const
    {
        if (!_error) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "successful result has no error");
        }
        return _error->kind ();
    }

  private:
    friend struct detail::result_access_t;

    result_t () = default;
    explicit result_t (framework_exception_t error) : _error (std::move (error)) {}

    std::optional<framework_exception_t> _error;
};

namespace detail
{

struct result_access_t
{
    template <typename T> static result_t<T> failure (framework_exception_t error)
    {
        return result_t<T> (std::move (error));
    }
};

template <typename T>
result_t<T> boundary_failure (boundary_error_t state, std::string message)
{
    return result_access_t::failure<T> (
      make_boundary_exception (state, std::move (message)));
}

/* Copies the source failure (kind, message and internal boundary state) into
 * a result of another value type, so awaited-boundary information survives
 * cross-type propagation. */
template <typename T, typename U>
result_t<T> propagate_failure (const result_t<U> &from, std::string fallback_message)
{
    const auto *error = from.error ();
    if (error != nullptr) {
        return result_access_t::failure<T> (*error);
    }
    return result_access_t::failure<T> (framework_exception_t (
      framework_error_kind_t::internal_failure, std::move (fallback_message)));
}

} // namespace detail

} // namespace zlink::framework
