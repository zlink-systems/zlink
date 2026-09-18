/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>

#include <optional>
#include <string>
#include <utility>

namespace zlink::stream_connector
{

template <typename T> class result_t
{
  public:
    static result_t success (T value) { return result_t (std::move (value)); }
    static result_t failure (error_code_t code, std::string message)
    {
        return result_t (error_t{code, std::move (message)});
    }

    explicit operator bool () const noexcept { return _value.has_value (); }
    const T &value () const { return *_value; }
    T &value () { return *_value; }
    const std::optional<error_t> &error () const noexcept { return _error; }

    /// Returns the failure code, or an empty optional for a success result.
    ///
    /// A success result carries no error, and the closed error set
    /// (stream-connector §9.1) has no "no error" member to stand in for one, so
    /// the absence is reported as an empty optional instead of reading through
    /// the empty error.
    std::optional<error_code_t> error_code () const noexcept
    {
        if (!_error) {
            return std::nullopt;
        }
        return _error->code;
    }

  private:
    explicit result_t (T value) : _value (std::move (value)) {}
    explicit result_t (error_t error) : _error (std::move (error)) {}

    std::optional<T> _value;
    std::optional<error_t> _error;
};

template <> class result_t<void>
{
  public:
    static result_t success () { return result_t (); }
    static result_t failure (error_code_t code, std::string message)
    {
        return result_t (error_t{code, std::move (message)});
    }

    explicit operator bool () const noexcept { return !_error.has_value (); }
    const std::optional<error_t> &error () const noexcept { return _error; }

    /// Returns the failure code, or an empty optional for a success result.
    std::optional<error_code_t> error_code () const noexcept
    {
        if (!_error) {
            return std::nullopt;
        }
        return _error->code;
    }

  private:
    result_t () = default;
    explicit result_t (error_t error) : _error (std::move (error)) {}

    std::optional<error_t> _error;
};

} // namespace zlink::stream_connector
