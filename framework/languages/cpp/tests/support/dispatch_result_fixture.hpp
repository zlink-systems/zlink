/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <variant>

namespace zlink::framework::test
{
template <typename T> T await_dispatch_result (std::variant<T, task_t<T>> result)
{
    if (auto ready = std::get_if<T> (&result))
        return std::move (*ready);
    return std::get<task_t<T>> (result).result ().value ();
}
} // namespace zlink::framework::test
