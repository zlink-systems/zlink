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
// A shutdown can seal a node while an earlier startup/Serving Store read is pending.
void LocalLifecycleIgnoresStaleStoreResult ()
{
    namespace fw = zlink::framework;
    for (const auto stale_state :
         {fw::framework_runtime_state_t::serving, fw::framework_runtime_state_t::preparing}) {
        auto registration = make_node ("tcp://127.0.0.1:0", "stale-store-lifecycle");
        auto node = std::make_shared<fw::detail::mesh_node_runtime_t> (registration);
        node->start ().result ().value ();
        const auto status = node->status ();
        fw::mesh_node_descriptor_t stale;
        stale.mesh_name = "vertical-mesh";
        stale.rid = status.routing_id ();
        stale.lifecycle_generation = status.lifecycle_generation ();
        stale.descriptor_revision = 1;
        stale.object_role = fw::object_role_t::server;
        stale.state = stale_state;
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
            assert (received.back ().status.is_ready);
        }
        node->request_stop ();
        node->native_node ().transport ().publish_draining ().result ().value ();
        const auto assert_draining = [] (const fw::mesh_node_snapshot_t &snapshot) {
            assert (snapshot.state == fw::topology_state_t::stopping);
            assert (!snapshot.is_ready);
            assert (!snapshot.placement.is_available);
            assert (snapshot.placement.unavailable_reason == fw::topology_reason_t::draining);
            assert (std::all_of (snapshot.channels.begin (), snapshot.channels.end (),
                                 [] (const auto &channel) { return !channel.is_ready; }));
        };
        assert_draining (runtime.snapshot ("vertical-mesh"));
        {
            std::unique_lock lock (event_mutex);
            assert (event_ready.wait_for (lock, 2s, [&] {
                return received.back ().status.state == fw::topology_state_t::stopping;
            }));
            assert_draining (received.back ().status);
        }
        std::fprintf (stderr,
                      "LocalLifecycleIgnoresStaleStoreResult old=%d before release passed\n",
                      static_cast<int> (stale_state));
        store.release ();
        starter.join ();
        std::fprintf (stderr, "LocalLifecycleIgnoresStaleStoreResult old=%d stale result applied\n",
                      static_cast<int> (stale_state));
        assert_draining (runtime.snapshot ("vertical-mesh"));
        {
            std::lock_guard lock (event_mutex);
            assert_draining (received.back ().status);
        }
        std::promise<fw::observed_status_t<fw::mesh_node_snapshot_t>> first;
        std::once_flag first_delivery;
        auto fresh = runtime.observe ("vertical-mesh", 1, [&] (const auto &observed) {
            std::call_once (first_delivery, [&] { first.set_value (observed); });
        });
        const auto observed = first.get_future ().get ();
        assert_draining (observed.status);
        assert (observed.loss.coalesced_count == 0);
        assert (observed.loss.discarded_terminal_count == 0);
        fresh->close ();
        existing->close ();
        runtime.stop ();
        node->stop ();
        std::fprintf (stderr, "LocalLifecycleIgnoresStaleStoreResult old=%d passed\n",
                      static_cast<int> (stale_state));
    }
}

// Host lifecycle is visible before descriptor publication changes the native node.
void verify_local_host_lifecycle_uses_runtime_owner ()
{
    namespace fw = zlink::framework;
    auto registration = make_node ("tcp://127.0.0.1:0", "local-host-lifecycle");
    auto node = std::make_shared<fw::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();
    std::atomic host_state{fw::framework_runtime_state_t::serving};
    fw::runtime::route_mesh_runtime_service_t runtime ({node}, nullptr, nullptr, nullptr,
                                                       [&] { return host_state.load (); });
    runtime.start ();
    assert (runtime.snapshot ("vertical-mesh").placement.is_available);

    // The host admission seal precedes native Draining publication (host relocation §14).
    host_state.store (fw::framework_runtime_state_t::draining);
    const auto snapshot = runtime.snapshot ("vertical-mesh");
    assert (snapshot.state == fw::topology_state_t::stopping);
    assert (!snapshot.is_ready);
    assert (!snapshot.placement.is_available);
    assert (snapshot.placement.unavailable_reason == fw::topology_reason_t::draining);
    assert (node->native_node ().transport ().topology ().local_descriptor ().state
            == fw::runtime::mesh::service_node_state_t::serving);
    runtime.stop ();
    node->stop ();
    std::fprintf (stderr, "Local host Draining before native publication passed\n");
}
} // namespace framework_mesh_node_vertical_tests
