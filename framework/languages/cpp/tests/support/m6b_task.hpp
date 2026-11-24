/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <utility>

namespace zlink::framework::test::m6b
{
template <class T> T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (task_t<void> task);
}
