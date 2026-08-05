/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include <exception>
#include <type_traits>

namespace zlink::framework::detail
{

inline result_t<zlink::message_t> current_exception_to_message_result (const char *fallback_message)
{
    try {
        throw;
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (const std::exception &error) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                    error.what ());
    }
    catch (...) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                    fallback_message);
    }
}

template <typename T> struct task_value_type_t
{
};

template <typename T> struct task_value_type_t<task_t<T>>
{
    using type = T;
};

template <typename T> inline constexpr bool is_task_v = requires
{
    typename task_value_type_t<T>::type;
};

template <typename TResult>
task_t<zlink::message_t> serialize_handler_result (TResult &&result,
                                                   serializer_registry_t &serializers)
{
    using result_type = std::remove_cvref_t<TResult>;
    if constexpr (is_task_v<result_type>) {
        using value_type = typename task_value_type_t<result_type>::type;
        if constexpr (std::is_void_v<value_type>) {
            co_await result;
            co_return result_t<zlink::message_t>::success (zlink::message_t{});
        } else {
            auto value = co_await result;
            co_return result_t<zlink::message_t>::success (
              encoded_payload_to_raw (serializers.get<value_type> ().serialize (value)));
        }
    } else {
        co_return result_t<zlink::message_t>::success (
          encoded_payload_to_raw (serializers.get<result_type> ().serialize (result)));
    }
}

} // namespace zlink::framework::detail
