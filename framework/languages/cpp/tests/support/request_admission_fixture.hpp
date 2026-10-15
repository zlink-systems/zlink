/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once
#include <zlink/framework/contracts/dispatch/request_task.hpp>

namespace zlink::framework::test
{
template <typename T> request_task_t<T> accepted_request_task (task_t<T> reply)
{
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (result_t<void>::success ());
    return request_task_t<T> (std::move (reply), std::move (admitted));
}
}
