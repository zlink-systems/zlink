/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_task.hpp"

#include <utility>

namespace m6c_test
{

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

} // namespace m6c_test
