/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink::stream_connector_throwing
{
class stream_connector_error : public std::runtime_error
{
  public:
    stream_connector_error (zlink::stream_connector::error_code_t code, std::string message) :
        std::runtime_error (message), _code (code)
    {
    }
    zlink::stream_connector::error_code_t code () const noexcept { return _code; }

  private:
    zlink::stream_connector::error_code_t _code;
};

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
namespace detail
{
/* Failure conversion belongs here; futures may keep their configured message
 * while retaining the same operation error code. */
template <typename T>
T value_or_throw (zlink::stream_connector::result_t<T> result, const std::string *failure_message)
{
    if (!result) {
        const auto &error = result.error ();
        throw stream_connector_error (error ? error->code
                                            : zlink::stream_connector::error_code_t::disconnected,
                                      failure_message ? *failure_message
                                      : error         ? error->message
                                                      : "stream connector operation failed");
    }
    if constexpr (!std::is_void_v<T>)
        return std::move (result.value ());
}
} // namespace detail

template <typename T> T value_or_throw (zlink::stream_connector::result_t<T> result)
{
    return detail::value_or_throw (std::move (result), nullptr);
}

inline void value_or_throw (zlink::stream_connector::result_t<void> result)
{
    detail::value_or_throw (std::move (result), nullptr);
}
#endif
} // namespace zlink::stream_connector_throwing
