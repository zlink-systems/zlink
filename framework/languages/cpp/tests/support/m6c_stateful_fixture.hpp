/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/relocation_authority_payload.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <cstdint>
#include <string>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

object_ref_t
create_actor (stateful_object_runtime_t &runtime, std::string key, std::string node = "node-a");
object_ref_t create_spot (stateful_object_runtime_t &runtime, object_kind_t kind, std::string key);
inventory_digest_t digest_with (std::uint8_t value);

} // namespace m6c_test
