/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <vector>

namespace zlink::framework::detail
{
class spot_node_runtime_t;
struct handoff_packet_t;

struct spot_node_runtime_test_access_t
{
    static task_t<void> replay (spot_node_runtime_t &runtime,
                                actor_ref_t actor,
                                std::vector<handoff_packet_t> packets,
                                service_provider_t services);
};
} // namespace zlink::framework::detail
