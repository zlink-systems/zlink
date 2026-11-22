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
#include "runtime/actors/actor_ref_access.hpp"
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
#include "../support/mesh_node_vertical_location_resolvers.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void verify_host_shutdown_seal_reaches_raw_mesh ()
{
    namespace mesh = zlink::framework::runtime::mesh;
    auto registration = make_node ("tcp://127.0.0.1:0", "seal-owner");
    auto seal = std::make_shared<std::atomic_bool> (false);
    zlink::framework::detail::spot_node_runtime_t (registration->spot_state).bind_drain_flag (seal);
    zlink::framework::detail::mesh_node_runtime_t node (registration);
    node.start ().result ().value ();
    auto remote_options = mesh::raw_mesh_node_options_t{
      .descriptor = node.native_node ().transport ().topology ().local_descriptor (),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()};
    remote_options.descriptor.node_routing_id = {'s', 'i', 'l', 'e', 'n', 't'};
    remote_options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    remote_options.descriptor.state = mesh::service_node_state_t::preparing;
    remote_options.shutdown_admission_seal = seal;
    mesh::raw_mesh_node_owner_t remote (std::move (remote_options));
    remote.start ();
    seal->store (true, std::memory_order_release);
    auto &transport = node.native_node ().transport ();
    assert (transport.connect_peer (remote.endpoint (), remote.topology ().local_descriptor ()));
    std::size_t ready = 0;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (ready == 0 && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        transport.observe_routes ();
        ready += remote.observe_routes ();
        std::this_thread::sleep_for (1ms);
    }
    assert (ready != 0);
    const auto quiet_until = std::chrono::steady_clock::now () + 200ms;
    while (std::chrono::steady_clock::now () < quiet_until) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        transport.observe_routes ();
        assert (remote.pump_one (now).result ().value () == mesh::raw_mesh_pump_result_t::no_data);
        std::this_thread::sleep_for (1ms);
    }
    node.stop ();
    remote.close ();
}

void verify_fixed_drain_callback_barrier ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "drain-barrier");
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::runtime::mesh_node_host_service_t service (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), {registration},
      serializers);
    const auto node = service.nodes ().front ();

    node->application_work_enqueued ();
    node->application_work_started ();
    service.seal_application_dispatch ();
    assert (!service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 20ms));

    std::thread completion ([node] {
        std::this_thread::sleep_for (20ms);
        node->application_work_finished ();
    });
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    completion.join ();
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);
}

void verify_accepted_handler_push_after_host_seal (bool shutdown)
{
    zlink::framework::completion_owner_t completion_owner;
    using namespace zlink::framework;
    auto registration = make_named_node ("accepted-send-mesh", "accepted-send-node");
    serializer_registry_t serializers;
    service_collection_t services;
    auto owned_store = std::make_unique<runtime::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<runtime::location_runtime_t> (
      std::make_unique<runtime::location_runtime_t> (store));
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    register_mesh_location_resolvers (services);
    auto provider = services.build_provider ();
    auto &locations = provider.get_required<runtime::location_runtime_t> ();
    locations.start (*registration->routing_id);
    runtime::mesh_node_host_service_t service (
      std::make_shared<runtime::runtime_failure_collector_t> (), {registration}, serializers);
    service.start (provider).result ().value ();
    const auto node = service.nodes ().front ();
    auto gateway_state = std::make_shared<detail::actor_gateway_state_t> ();
    detail::actor_gateway_runtime_t gateway (gateway_state);
    const auto actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("accepted-send-node"), "player", "accepted-handler", 1);
    std::atomic_int sends{0};
    gateway.bind_session_sink (actor,
                               [&sends] (std::string, stream_codec_t, const zlink::message_t &) {
                                   ++sends;
                                   return task_t<void> (result_t<void>::success ());
                               });
    std::promise<void> started;
    std::promise<void> release;
    auto latch = release.get_future ().share ();
    node->application_work_enqueued ();
    auto handler = std::async (std::launch::async, [&] {
        node->application_work_started ();
        started.set_value ();
        latch.wait ();
        const auto submitted = gateway.dispatch_bound_session_send (
          actor, "AcceptedPush", stream_codec_t::raw, zlink::message_t::from ("push"));
        node->application_work_finished ();
        return submitted;
    });
    assert (started.get_future ().wait_for (1s) == std::future_status::ready);
    assert (service.publish_descriptor_state (shutdown ? framework_runtime_state_t::draining
                                                       : framework_runtime_state_t::relocating));
    service.seal_application_dispatch ();
    assert (service.submit_local_node_send (node, {}).result ().value ()
            == zlink::submit_result_t::terminated);
    assert (!service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now ()));
    release.set_value ();
    assert (handler.wait_for (1s) == std::future_status::ready);
    assert (handler.get ());
    assert (sends.load () == 1);
    assert (service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 1s));
    service.stop ();
    locations.stop ();
}
} // namespace framework_mesh_node_vertical_tests
