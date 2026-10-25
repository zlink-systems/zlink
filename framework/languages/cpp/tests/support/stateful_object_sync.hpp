/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/stateful/stateful_object_runtime.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test
{
inline runtime::stateful::create_result_t
begin_reserved_object (runtime::stateful::stateful_object_runtime_t &objects,
                       const runtime::stateful::object_ref_t &reserved,
                       const std::string &stable_type,
                       std::vector<std::uint8_t> creation_request)
{
    return objects.begin_reserved_object_async (reserved, stable_type, std::move (creation_request))
      .result ()
      .value ();
}
} // namespace zlink::framework::test
