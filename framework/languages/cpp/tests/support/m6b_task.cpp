/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6b_task.hpp"

namespace zlink::framework::test::m6b
{
void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}
}
