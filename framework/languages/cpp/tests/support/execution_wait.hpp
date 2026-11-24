/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <functional>

namespace zlink::framework::test::execution
{
bool wait_until (const std::function<bool ()> &predicate);
}
