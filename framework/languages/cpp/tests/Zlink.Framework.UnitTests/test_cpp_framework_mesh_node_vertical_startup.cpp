/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "../support/runtime_failure_fixture.hpp"
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
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
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
#include <regex>
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
void verify_mesh_start_and_rollback_from_completion_turn ()
{
    using namespace zlink::framework;
    completion_owner_t completion_owner;
    // Plain provider continuations always resume on completion turns. This
    // makes the pre-fix lifecycle violation independent of lane timing.
    runtime::configure_handler_coroutine_executor (1);
    runtime::shutdown_handler_coroutine_executor ();
    for (const bool fail_second : {false, true}) {
        auto first = make_named_node ("completion-start", "completion-start-first");
        auto second = make_named_node ("completion-start", "completion-start-second");
        if (fail_second)
            second->socket.receive_timeout = std::chrono::milliseconds::zero ();
        serializer_registry_t serializers;
        service_collection_t services;
        auto provider = services.build_provider ();
        auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
        runtime::mesh_node_host_service_t service (failures, {first, second}, serializers);
        auto completion = std::make_shared<task_completion_source_t<void>> ();
        auto started = completion->task ();
        detail::dispatch_completion ([&service, &provider, completion] {
            assert (detail::current_completion_owner () != nullptr);
            bool rejected = false;
            try {
                completion_owner_t forbidden;
            }
            catch (const framework_exception_t &error) {
                rejected = error.kind () == framework_error_kind_t::invalid_operation;
            }
            assert (rejected);
            auto pending = service.start (provider);
            detail::observe_task_completion (pending, [completion] (const result_t<void> &result) {
                completion->complete (result);
            });
        });
        const auto result = started.result ();
        if (fail_second) {
            assert (!result);
            assert (std::string_view (result.error ()->what ()).find ("receive timeout")
                    != std::string_view::npos);
            // Failure is returned only after the already-started first node is closed.
            bool stopped = false;
            try {
                (void) service.nodes ().front ()->native_node ();
            }
            catch (const framework_exception_t &) {
                stopped = true;
            }
            assert (stopped);
        } else {
            result.value ();
            assert (
              !service.nodes ().front ()->native_node ().status ().local_endpoint ().empty ());
        }
        service.stop ();
        failures->rethrow_if_failed ();
    }
}

void verify_automatic_identity_and_port_builder ()
{
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("automatic-builder-mesh");
    mesh.set_bind_host ("127.0.0.1")
      .listen (std::uint16_t{0})
      .set_automatic_routing_id_prefix ("play.node");

    const auto registrations =
      zlink::framework::detail::mesh_node_runtime_t::registrations (builder);
    assert (registrations.size () == 1);
    const auto &state = registrations.front ();
    assert (!state->spot_state->snapshot.spot_route_channel_name);
    assert (state->spot_state->snapshot.accepted_route_channels.empty ());
    assert (zlink::framework::detail::spot_node_runtime_t (state->spot_state)
              .actor_route_transport_name ()
            == "automatic-builder-mesh");
    assert (state->listen_endpoint == "tcp://127.0.0.1:0");
    assert (state->routing_id);
    assert (std::regex_match (
      state->routing_id->to_string (),
      std::regex (
        R"(play\.node-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})")));
    assert (state->spot_state->snapshot.routing_id
            && state->spot_state->snapshot.routing_id->to_string ()
                 == state->routing_id->to_string ());

    bool rejected_fixed_id = false;
    try {
        mesh.set_routing_id (zlink::routing_id_t::from ("fixed"));
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_fixed_id = true;
    }
    assert (rejected_fixed_id);

    zlink::framework::zlink_builder_t invalid_builder =
      zlink::framework::test::runtime_failure_builder ();
    auto invalid_mesh = invalid_builder.add_route_mesh ("invalid-builder-mesh");
    bool rejected_prefix = false;
    try {
        invalid_mesh.set_automatic_routing_id_prefix ("bad prefix");
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_prefix = true;
    }
    assert (rejected_prefix);
}
} // namespace framework_mesh_node_vertical_tests
