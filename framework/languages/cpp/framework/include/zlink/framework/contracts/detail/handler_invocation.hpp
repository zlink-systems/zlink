/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace zlink::framework::detail
{

// Borrows its callable until the awaited handler invocation completes.
class handler_start_callback_ref_t
{
  public:
    handler_start_callback_ref_t () noexcept = default;

    template <typename TCallback>
        requires (!std::is_same_v<std::remove_cvref_t<TCallback>, handler_start_callback_ref_t>
                  && std::is_invocable_r_v<void, const TCallback &>)
    handler_start_callback_ref_t (const TCallback &callback) noexcept :
        _target (std::addressof (callback)), _invoke (&invoke<TCallback>)
    {
    }

    handler_start_callback_ref_t (const std::function<void ()> &callback) noexcept
    {
        if (callback) {
            _target = std::addressof (callback);
            _invoke = &invoke<std::function<void ()>>;
        }
    }

    explicit operator bool () const noexcept { return _invoke != nullptr; }
    void operator() () const { _invoke (_target); }

  private:
    template <typename TCallback> static void invoke (const void *target)
    {
        std::invoke (*static_cast<const TCallback *> (target));
    }

    const void *_target = nullptr;
    void (*_invoke) (const void *) = nullptr;
};

// Arguments and typed payload are prepared before the target callback starts.
template <typename TMethod, typename TOwner, typename... TArgs>
decltype (auto) invoke_prepared_handler (handler_start_callback_ref_t before_application_handler,
                                         TMethod method,
                                         TOwner &owner,
                                         TArgs &&...args)
{
    if (before_application_handler)
        before_application_handler ();
    return std::invoke (method, owner, std::forward<TArgs> (args)...);
}

template <typename T> struct task_value_type_t
{
};

template <typename T> struct task_value_type_t<task_t<T>>
{
    using type = T;
};

template <typename T>
inline constexpr bool is_task_v = requires { typename task_value_type_t<T>::type; };

template <typename TResult>
task_t<zlink::message_t> serialize_handler_result (TResult &&result,
                                                   serializer_registry_t &serializers)
{
    using result_type = std::remove_cvref_t<TResult>;
    if constexpr (is_task_v<result_type>) {
        return [] (TResult pending, serializer_registry_t &registry) -> task_t<zlink::message_t> {
            using value_type = typename task_value_type_t<result_type>::type;
            if constexpr (std::is_void_v<value_type>) {
                co_await pending;
                co_return result_t<zlink::message_t>::success (zlink::message_t{});
            } else {
                auto value = co_await pending;
                co_return result_t<zlink::message_t>::success (
                  encoded_payload_to_raw (registry.get<value_type> ().serialize (value)));
            }
        }(std::forward<TResult> (result), serializers);
    } else {
        try {
            return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
              encoded_payload_to_raw (serializers.get<result_type> ().serialize (result))));
        }
        catch (...) {
            return task_t<zlink::message_t> (current_exception_result<zlink::message_t> ());
        }
    }
}

template <typename TResult, typename TPlan>
task_t<zlink::message_t> serialize_handler_result (TResult result,
                                                   serializer_registry_t &serializers,
                                                   std::shared_ptr<TPlan> codecs)
{
    if constexpr (is_task_v<TResult>) {
        return [] (TResult pending, serializer_registry_t &registry,
                   std::shared_ptr<TPlan> plan) -> task_t<zlink::message_t> {
            auto value = co_await pending;
            co_return result_t<zlink::message_t>::success (
              encoded_payload_to_raw (plan->serialize (registry, value)));
        }(std::move (result), serializers, std::move (codecs));
    } else {
        try {
            return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
              encoded_payload_to_raw (codecs->serialize (serializers, result))));
        }
        catch (...) {
            return task_t<zlink::message_t> (current_exception_result<zlink::message_t> ());
        }
    }
}

} // namespace zlink::framework::detail
