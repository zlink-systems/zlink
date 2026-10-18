/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <functional>
#include <type_traits>
#include <utility>

namespace zlink::framework::test
{
// Keep fatal assertions in the original void callback while adapting only its completion type.
template <class Dispatch> auto async_application_dispatch (Dispatch dispatch)
{
    return [dispatch = std::move (dispatch)] (const auto &owner, const auto &record,
                                              auto parts) mutable -> task_t<void> {
        using completion_t =
          std::invoke_result_t<Dispatch &, decltype (owner), decltype (record), decltype (parts)>;
        if constexpr (std::is_convertible_v<completion_t, task_t<void>>) {
            return std::invoke (dispatch, owner, record, std::move (parts));
        } else {
            static_assert (
              !requires { std::declval<completion_t> ().await_ready (); },
              "application dispatch must retain asynchronous completion");
            (void) std::invoke (dispatch, owner, record, std::move (parts));
            return task_t<void> (result_t<void>::success ());
        }
    };
}
}
