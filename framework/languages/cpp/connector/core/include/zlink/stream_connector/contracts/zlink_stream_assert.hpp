/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace zlink::stream_connector::assertions
{

class failure_t : public std::runtime_error
{
  public:
    explicit failure_t (error_t error) :
        std::runtime_error (error.message), _error (std::move (error))
    {
    }

    const error_t &error () const noexcept { return _error; }

  private:
    error_t _error;
};

/// Fails with the required diagnostic message when condition is false.
inline void ensure (bool condition, std::string_view message)
{
    if (message.empty ()) {
        throw std::invalid_argument ("ensure requires a diagnostic message");
    }
    if (!condition) {
        throw std::runtime_error (std::string (message));
    }
}

/// Executes action and returns its failure, optionally checking the error kind.
template <typename TAction>
error_t expect_failure (TAction &&action,
                        std::optional<error_code_t> expected_kind = std::nullopt)
{
    using action_result_t = std::invoke_result_t<TAction>;
    std::optional<action_result_t> result;
    try {
        result.emplace (std::invoke (std::forward<TAction> (action)));
    } catch (const failure_t &failure) {
        if (expected_kind && failure.error ().code != *expected_kind) {
            throw std::runtime_error ("action failed with an unexpected error kind");
        }
        return failure.error ();
    } catch (const std::exception &exception) {
        error_t error{error_code_t::user_callback_failed, exception.what ()};
        if (expected_kind && error.code != *expected_kind) {
            throw std::runtime_error ("action failed with an unexpected error kind");
        }
        return error;
    } catch (...) {
        error_t error{error_code_t::user_callback_failed,
                      "action failed with a non-standard exception"};
        if (expected_kind && error.code != *expected_kind) {
            throw std::runtime_error ("action failed with an unexpected error kind");
        }
        return error;
    }

    if (*result) {
        throw std::runtime_error ("expected action to fail");
    }
    auto error = result->error ().value_or (
      error_t{result->error_code (), "action failed without an error message"});
    if (expected_kind && error.code != *expected_kind) {
        throw std::runtime_error ("action failed with an unexpected error kind");
    }
    return error;
}

/// Executes action, accepts only timeout failures, and rethrows every other failure.
template <typename TAction> error_t expect_timeout (TAction &&action)
{
    try {
        auto result = std::invoke (std::forward<TAction> (action));
        if (result) {
            throw std::runtime_error ("expected action to time out");
        }
        auto error = result.error ().value_or (
          error_t{result.error_code (), "action failed without an error message"});
        if (error.code == error_code_t::request_timeout
            || error.code == error_code_t::connect_timeout) {
            return error;
        }
        throw failure_t (std::move (error));
    } catch (const failure_t &failure) {
        if (failure.error ().code == error_code_t::request_timeout
            || failure.error ().code == error_code_t::connect_timeout) {
            return failure.error ();
        }
        throw;
    }
}

} // namespace zlink::stream_connector::assertions
