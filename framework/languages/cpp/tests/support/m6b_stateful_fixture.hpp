/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/stateful_object_runtime.hpp"

namespace zlink::framework::test::m6b
{
namespace stateful = zlink::framework::runtime::stateful;

stateful::object_ref_t create_ready (stateful::stateful_object_runtime_t &runtime,
                                     stateful::create_request_t request);
}
