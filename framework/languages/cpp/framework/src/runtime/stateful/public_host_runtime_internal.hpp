/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime::host::public_host_runtime_detail
{
std::pair<std::uint32_t, std::uint32_t> stateful_failure_pair (stateful::stateful_error_t failure);
void trace_mesh_host_enabled (std::string_view stage, std::string_view detail);
bool is_actor_application (record_kind_t kind);
bool is_stateful_application (record_kind_t kind);
local_application_dispatch_t local_application_query (const local_application_dispatch_t &original);
std::tuple<std::uint64_t, std::uint64_t, std::string>
session_relocation_key (const protocol::session_relocation_route_t &route);
} // namespace zlink::framework::runtime::host::public_host_runtime_detail

// Gate argument evaluation too: diagnostic snapshots can enter another lane.
#define trace_mesh_host(stage, detail)                                                             \
    do {                                                                                           \
        if (mesh_trace_enabled ())                                                                 \
            trace_mesh_host_enabled (stage, detail);                                               \
    } while (false)
