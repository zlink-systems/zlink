/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_replay_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <utility>

namespace zlink::framework::detail
{
task_t<void> spot_node_runtime_test_access_t::replay (spot_node_runtime_t &runtime,
                                                      actor_ref_t actor,
                                                      std::vector<handoff_packet_t> packets,
                                                      service_provider_t services)
{
    return runtime.replay_actor_handoff_batch (std::move (actor), std::move (packets),
                                               std::move (services), "canonical-replay", false, {});
}
} // namespace zlink::framework::detail
