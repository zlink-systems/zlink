/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void verify_management_does_not_wait_for_spot_lane ()
{
    zlink::framework::completion_owner_t completion_owner;
    auto registration = make_node ("tcp://127.0.0.1:0", "management-pump-node");
    std::promise<void> lane_entered;
    std::promise<void> release_lane;
    auto release = release_lane.get_future ().share ();
    assert (registration->spot_state->lane.try_post ([&] {
        lane_entered.set_value ();
        release.wait ();
    }));
    lane_entered.get_future ().wait ();
    auto activity = [&] {
#ifndef NDEBUG
        zlink::framework::runtime::infrastructure_wait_guard::mesh_receive_scope_t scope (
          registration.get ());
#endif
        return zlink::framework::detail::spot_node_runtime_t (registration->spot_state)
          .advance_management_async ();
    }();
    const bool completed_before_release = activity.await_ready ();
    release_lane.set_value ();
    assert (!completed_before_release);
    (void) activity.result ().value ();
}
} // namespace framework_mesh_node_vertical_tests
