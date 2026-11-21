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
#include "../support/mesh_node_vertical_location_store_fixture.hpp"
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void LocalPlacementIgnoresStaleStoreResult ()
{
    namespace fw = zlink::framework;
    // The positive control must retain availability; the zero-weight case must
    // retain CapacityExceeded before and after the old Store result arrives.
    for (const int weight : {100, 0}) {
        auto registration = make_node ("tcp://127.0.0.1:0", "stale-store-placement");
        registration->placement_weight = 100;
        auto node = std::make_shared<fw::detail::mesh_node_runtime_t> (registration);
        node->start ().result ().value ();
        const auto status = node->status ();
        fw::mesh_node_descriptor_t stale;
        stale.mesh_name = "vertical-mesh";
        stale.rid = status.routing_id ();
        stale.lifecycle_generation = status.lifecycle_generation ();
        stale.descriptor_revision = 1;
        stale.object_role = fw::object_role_t::server;
        stale.state = fw::framework_runtime_state_t::serving;
        stale.placement_weight = 100;
        delayed_monitoring_mesh_store_t store (stale);
        auto read = store.first_read ();
        fw::runtime::route_mesh_runtime_service_t runtime ({node}, nullptr, &store);
        std::jthread starter ([&] { runtime.start (); });
        read.wait ();

        std::mutex event_mutex;
        std::condition_variable event_ready;
        std::vector<fw::observed_status_t<fw::mesh_node_snapshot_t>> received;
        auto existing = runtime.observe ("vertical-mesh", 16, [&] (const auto &observed) {
            {
                std::lock_guard lock (event_mutex);
                received.push_back (observed);
            }
            event_ready.notify_one ();
        });
        {
            std::unique_lock lock (event_mutex);
            assert (event_ready.wait_for (lock, 2s, [&] { return !received.empty (); }));
            assert (received.back ().status.placement.is_available);
        }
        fw::runtime::route_mesh_runtime_options_service_t options ({node});
        options.mesh ("vertical-mesh").placement_weight (weight);
        const auto assert_placement = [weight] (const fw::mesh_node_snapshot_t &snapshot) {
            assert (snapshot.placement.is_available == (weight > 0));
            assert (snapshot.placement.unavailable_reason
                    == (weight > 0 ? std::nullopt
                                   : std::optional (fw::topology_reason_t::capacity_exceeded)));
        };
        assert_placement (runtime.snapshot ("vertical-mesh"));
        {
            std::unique_lock lock (event_mutex);
            assert (event_ready.wait_for (lock, 2s, [&] {
                return !received.empty ()
                       && received.back ().status.placement.is_available == (weight > 0);
            }));
            assert_placement (received.back ().status);
        }

        std::fprintf (stderr,
                      "LocalPlacementIgnoresStaleStoreResult weight=%d before release passed\n",
                      weight);
        store.release ();
        starter.join ();
        std::fprintf (
          stderr, "LocalPlacementIgnoresStaleStoreResult weight=%d stale result applied\n", weight);
        assert_placement (runtime.snapshot ("vertical-mesh"));
        {
            std::lock_guard lock (event_mutex);
            assert_placement (received.back ().status);
        }
        std::promise<fw::observed_status_t<fw::mesh_node_snapshot_t>> first;
        std::once_flag first_delivery;
        auto fresh = runtime.observe ("vertical-mesh", 1, [&] (const auto &observed) {
            std::call_once (first_delivery, [&] { first.set_value (observed); });
        });
        const auto observed = first.get_future ().get ();
        assert_placement (observed.status);
        assert (observed.loss.coalesced_count == 0);
        assert (observed.loss.discarded_terminal_count == 0);
        fresh->close ();
        existing->close ();
        runtime.stop ();
        node->stop ();
        std::fprintf (stderr, "LocalPlacementIgnoresStaleStoreResult weight=%d passed\n", weight);
    }
}

void verify_location_store_blocks_placement ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "placement-store-node");
    registration->placement_weight = 100;
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();

    const auto node_status = node->status ();
    zlink::framework::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "vertical-mesh";
    descriptor.rid = node_status.routing_id ();
    descriptor.lifecycle_generation = node_status.lifecycle_generation ();
    descriptor.object_role = zlink::framework::object_role_t::server;
    descriptor.state = zlink::framework::framework_runtime_state_t::serving;
    descriptor.placement_weight = 100;
    monitoring_mesh_store_t store;
    store.set_local (descriptor);
    monitoring_location_query_t location_query;
    zlink::framework::runtime::route_mesh_runtime_service_t runtime ({node}, &location_query,
                                                                     &store);
    runtime.start ();

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<zlink::framework::mesh_node_snapshot_t> received;
    auto observation = runtime.observe (
      "vertical-mesh", 1,
      [&] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t>
             &observed) {
          {
              std::lock_guard lock (mutex);
              received.push_back (observed.status);
          }
          changed.notify_one ();
      });
    const auto wait_for = [&] (const auto &predicate) {
        std::unique_lock lock (mutex);
        return changed.wait_for (
          lock, 2s, [&] { return !received.empty () && predicate (received.back ()); });
    };

    assert (wait_for ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::ready
               && snapshot.placement.is_available;
    }));
    location_query.set_store_healthy (false);
    assert (wait_for ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::degraded
               && !snapshot.placement.is_available
               && snapshot.placement.unavailable_reason
                    == zlink::framework::topology_reason_t::location_unavailable;
    }));
    location_query.set_store_healthy (true);
    assert (wait_for ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::ready
               && snapshot.placement.is_available;
    }));

    observation->close ();
    runtime.stop ();
    node->stop ();
}
} // namespace framework_mesh_node_vertical_tests
