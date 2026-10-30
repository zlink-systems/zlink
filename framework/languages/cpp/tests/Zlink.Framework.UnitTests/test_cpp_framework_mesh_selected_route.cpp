/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

// RouteMesh admission follows the Core selected route (#1087 FW01):
// Core ROUTER §10.1 owns which pipe carries a RID; the framework observes the
// selected-route snapshot and admits only that route
// (server 05-transport-liveness §5). A replaced route loses its admission and
// the new route admits through a new handshake, even when Core keeps the old
// pipe as a standby and never reports it disconnected.

#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <gtest/gtest.h>
#include <zlink.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace backend = zlink::framework::detail::backend;

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

mesh::raw_mesh_node_options_t options (const std::string &rid, std::uint64_t lifecycle_generation)
{
    mesh::raw_mesh_node_options_t result;
    result.runtime_failures =
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
    result.descriptor = mesh::service_node_descriptor_t{"selected-route-mesh",
                                                        bytes (rid),
                                                        1,
                                                        1,
                                                        "tcp://127.0.0.1:0",
                                                        {{"alpha", 100}},
                                                        mesh::service_node_state_t::preparing};
    result.descriptor.lifecycle_generation = lifecycle_generation;
    return result;
}

// One host ingress pass observes the Core selected routes and pumps one record.
void progress (mesh::raw_mesh_node_owner_t &owner)
{
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) owner.observe_routes ();
    (void) std::move (owner.pump_one (now)).result ();
}

bool pump_until (const std::vector<mesh::raw_mesh_node_owner_t *> &owners,
                 const std::function<bool ()> &condition,
                 std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (!condition () && std::chrono::steady_clock::now () < deadline) {
        for (auto *owner : owners)
            progress (*owner);
    }
    return condition ();
}

std::uint64_t admitted_generation (mesh::raw_mesh_node_owner_t &owner,
                                   const std::vector<std::uint8_t> &peer)
{
    const auto admitted = owner.topology ().peer (peer);
    return admitted ? admitted->descriptor.lifecycle_generation : 0;
}

TEST (CppFrameworkMeshSelectedRoute, AdmissionFollowsCoreSelectedRouteAcrossHandoverAndPromotion)
{
    const auto target_rid = bytes ("selected-route-target");
    mesh::raw_mesh_node_owner_t source (options ("selected-route-source", 101));
    auto first =
      std::make_unique<mesh::raw_mesh_node_owner_t> (options ("selected-route-target", 201));
    auto second =
      std::make_unique<mesh::raw_mesh_node_owner_t> (options ("selected-route-target", 202));
    source.start ();
    first->start ();
    second->start ();

    // Endpoint-only manual intent: the admitted identity is whatever process
    // answers the handshake on the Core selected route.
    ASSERT_TRUE (source.connect_peer (first->endpoint ()));
    ASSERT_TRUE (pump_until ({&source, first.get (), second.get ()},
                             [&] { return admitted_generation (source, target_rid) == 201; }));

    // Same RID, new process: Core hands the selected route to the new pipe
    // and keeps the first pipe as a standby without a disconnect.
    ASSERT_TRUE (source.connect_peer (second->endpoint ()));
    EXPECT_TRUE (pump_until ({&source, first.get (), second.get ()},
                             [&] { return admitted_generation (source, target_rid) == 202; }))
      << "admitted lifecycle generation stayed " << admitted_generation (source, target_rid);

    // The selected pipe ends; Core promotes the standby to a new route
    // generation. The first process answers the new handshake on it.
    second->close ();
    EXPECT_TRUE (pump_until ({&source, first.get ()},
                             [&] { return admitted_generation (source, target_rid) == 201; }))
      << "admitted lifecycle generation is " << admitted_generation (source, target_rid);

    source.close ();
    first->close ();
}

// Submit §5: the first request precedes service admission; no warm-up is sent.
class CppFrameworkRequestAdmission : public testing::TestWithParam<std::tuple<bool, bool>>
{
};

TEST_P (CppFrameworkRequestAdmission, FirstRequestWaitsForAdmissionAndSendsOnce)
{
    mesh::raw_mesh_node_owner_t source (options ("first-source", 101));
    mesh::raw_mesh_node_owner_t target (options ("first-target", 201));
    source.start ();
    target.start ();
    const auto descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), descriptor));
    using terminal_t = zlink::framework::runtime::foundation::operation_terminal_t;
    auto completion = std::make_shared<zlink::framework::task_completion_source_t<terminal_t>> ();
    auto terminal = completion->task ();
    const auto payload =
      protocol::application_payload_t{"admission.probe", "application/json", {123, 125}};
    const auto [delayed, instance] = GetParam ();
    const auto domain = instance ? mesh::service_mailbox_domain_t::infrastructure
                                 : mesh::service_mailbox_domain_t::application;
    const auto completed = [completion] (auto result, auto) {
        completion->complete (zlink::framework::result_t<terminal_t>::success (result));
    };
    auto submitted = [&] {
        if (!instance)
            return source.request_to_node (descriptor.node_routing_id, payload, 3s, completed);
        const auto local = source.topology ().local_descriptor ();
        const auto expires = static_cast<std::uint64_t> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            (std::chrono::system_clock::now () + 3s).time_since_epoch ())
            .count ());
        protocol::instance_spot_activation_header_t activation{
          {descriptor.node_routing_id, descriptor.lifecycle_generation, "instance-1", "admission",
           "room", "descriptor-1", expires},
          local.lifecycle_generation,
          local.node_routing_id,
          std::string ("source-spot"),
          true,
          {local.lifecycle_generation, 91},
          0,
          false};
        return source.request_instance_spot_activation (
          descriptor.node_routing_id, std::move (activation), std::nullopt, payload, 3s, completed);
    }();
    // The original Node request returns false synchronously at this assertion.
    ASSERT_FALSE (submitted.await_ready ());
    if (delayed) {
        // Only the source progresses: the healthy target has not serviced HELLO.
        for (int turn = 0; turn != 8; ++turn)
            progress (source);
        EXPECT_FALSE (submitted.await_ready ());
        EXPECT_FALSE (terminal.await_ready ());
    }
    ASSERT_TRUE (pump_until ({&source, &target}, [&] { return submitted.await_ready (); }));
    EXPECT_TRUE (submitted.result ().value ());
    ASSERT_TRUE (pump_until ({&source, &target},
                             [&] { return target.mailbox ().pending_messages (domain) != 0; }));
    EXPECT_EQ (1u, target.mailbox ().pending_messages (domain));
    EXPECT_FALSE (terminal.await_ready ());
    auto claim = target.mailbox ().try_claim (domain, 1, 65536);
    ASSERT_TRUE (claim);
    auto request = target.mailbox ().take ({claim, 0});
    ASSERT_TRUE (request);
    EXPECT_TRUE (instance ? target.reply_instance_spot_activation (*request, 0, 0, payload)
                          : target.reply (*request, payload));
    (void) target.mailbox ().release (*claim);
    ASSERT_TRUE (pump_until ({&source, &target}, [&] { return terminal.await_ready (); }));
    EXPECT_EQ (zlink::framework::runtime::foundation::operation_terminal_t::completed,
               terminal.result ().value ());
    for (int turn = 0; turn != 8; ++turn) {
        progress (source);
        progress (target);
    }
    EXPECT_EQ (0u, target.mailbox ().pending_messages (domain));
    source.close ();
    target.close ();
}

INSTANTIATE_TEST_SUITE_P (ImmediateAndDelayedService,
                          CppFrameworkRequestAdmission,
                          testing::Combine (testing::Bool (), testing::Bool ()));

TEST (CppFrameworkMeshSelectedRoute, FailedEndpointCompletesBeforeRequestDeadline)
{
    mesh::raw_mesh_node_owner_t source (options ("failed-source", 101));
    // Releasing the actual listener leaves a valid descriptor and a refused endpoint.
    auto target = std::make_unique<mesh::raw_mesh_node_owner_t> (options ("failed-target", 201));
    source.start ();
    target->start ();
    const auto descriptor = target->topology ().local_descriptor ();
    const auto endpoint = target->endpoint ();
    target->close ();
    target.reset ();
    ASSERT_TRUE (source.connect_peer (endpoint, descriptor));
    using terminal_t = zlink::framework::runtime::foundation::operation_terminal_t;
    auto completion = std::make_shared<zlink::framework::task_completion_source_t<terminal_t>> ();
    auto terminal = completion->task ();
    const auto started = std::chrono::steady_clock::now ();
    auto submitted = source.request_to_node (
      descriptor.node_routing_id,
      protocol::application_payload_t{"admission.probe", "application/json", {123, 125}}, 30s,
      [completion] (auto result, auto) {
          completion->complete (zlink::framework::result_t<terminal_t>::success (result));
      });
    ASSERT_TRUE (pump_until ({&source}, [&] { return terminal.await_ready (); }));
    EXPECT_EQ (zlink::framework::runtime::foundation::operation_terminal_t::route_unavailable,
               terminal.result ().value ());
    EXPECT_TRUE (submitted.await_ready ());
    EXPECT_LT (std::chrono::steady_clock::now () - started, 30s);
    const auto result = submitted.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::unavailable, result.error_kind ());
    source.close ();
}

TEST (CppFrameworkMeshSelectedRoute, ReplacedIntentFailsWaitingRequestBeforeHealthyAdmission)
{
    mesh::raw_mesh_node_owner_t source (options ("replace-source", 101));
    mesh::raw_mesh_node_owner_t target (options ("replace-target", 201));
    source.start ();
    target.start ();
    const auto descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), descriptor));
    // The single observer publishes the original Core row before the wait starts.
    (void) source.observe_routes ();
    using terminal_t = zlink::framework::runtime::foundation::operation_terminal_t;
    auto completion = std::make_shared<zlink::framework::task_completion_source_t<terminal_t>> ();
    auto terminal = completion->task ();
    const auto payload =
      protocol::application_payload_t{"admission.probe", "application/json", {123, 125}};
    auto submitted = source.request_to_node (
      descriptor.node_routing_id, payload, 30s, [completion] (auto result, auto) {
          completion->complete (zlink::framework::result_t<terminal_t>::success (result));
      });
    ASSERT_FALSE (submitted.await_ready ());
    source.disconnect_peer (descriptor.node_routing_id, target.endpoint ());
    ASSERT_TRUE (source.connect_peer (target.endpoint (), descriptor));
    std::size_t received = 0;
    ASSERT_TRUE (pump_until ({&source, &target}, [&] {
        if (auto claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 65536)) {
            if (auto request = target.mailbox ().take ({claim, 0})) {
                ++received;
                (void) target.reply (*request, payload);
            }
            (void) target.mailbox ().release (*claim);
        }
        return terminal.await_ready ();
    }));
    EXPECT_EQ (zlink::framework::runtime::foundation::operation_terminal_t::route_unavailable,
               terminal.result ().value ());
    EXPECT_EQ (0u, received);
    source.close ();
    target.close ();
}

TEST (CppFrameworkMeshSelectedRoute, AdmissionDeadlineExpiresWithoutBindingSubmission)
{
    mesh::raw_mesh_node_owner_t source (options ("deadline-source", 101));
    mesh::raw_mesh_node_owner_t target (options ("deadline-target", 201));
    source.start ();
    target.start ();
    const auto descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), descriptor));
    using terminal_t = zlink::framework::runtime::foundation::operation_terminal_t;
    auto completion = std::make_shared<zlink::framework::task_completion_source_t<terminal_t>> ();
    auto terminal = completion->task ();
    const auto started = std::chrono::steady_clock::now ();
    auto submitted = source.request_to_node (
      descriptor.node_routing_id,
      protocol::application_payload_t{"admission.probe", "application/json", {123, 125}}, 30ms,
      [completion] (auto result, auto) {
          completion->complete (zlink::framework::result_t<terminal_t>::success (result));
      });
    ASSERT_FALSE (submitted.await_ready ());
    ASSERT_TRUE (pump_until ({&source}, [&] { return terminal.await_ready (); }));
    EXPECT_EQ (zlink::framework::runtime::foundation::operation_terminal_t::timed_out,
               terminal.result ().value ());
    EXPECT_GE (std::chrono::steady_clock::now () - started, 30ms);
    ASSERT_TRUE (submitted.await_ready ());
    const auto result = submitted.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::deadline_exceeded, result.error_kind ());
    EXPECT_EQ (0u,
               target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application));
    source.close ();
    target.close ();
}

TEST (CppFrameworkMeshSelectedRoute, ShutdownCompletesUnsubmittedAdmission)
{
    mesh::raw_mesh_node_owner_t source (options ("shutdown-source", 101));
    mesh::raw_mesh_node_owner_t target (options ("shutdown-target", 201));
    source.start ();
    target.start ();
    const auto descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), descriptor));
    using terminal_t = zlink::framework::runtime::foundation::operation_terminal_t;
    auto completion = std::make_shared<zlink::framework::task_completion_source_t<terminal_t>> ();
    auto terminal = completion->task ();
    auto submitted = source.request_to_node (
      descriptor.node_routing_id,
      protocol::application_payload_t{"admission.probe", "application/json", {123, 125}}, 30s,
      [completion] (auto result, auto) {
          completion->complete (zlink::framework::result_t<terminal_t>::success (result));
      });
    ASSERT_FALSE (submitted.await_ready ());
    source.close ();
    ASSERT_TRUE (terminal.await_ready ());
    EXPECT_EQ (zlink::framework::runtime::foundation::operation_terminal_t::shutdown,
               terminal.result ().value ());
    ASSERT_TRUE (submitted.await_ready ());
    const auto result = submitted.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::shutting_down, result.error_kind ());
    EXPECT_EQ (0u,
               target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application));
    target.close ();
}

enum class remote_hello_t
{
    none,
    before_update,
    after_update
};

class CppFrameworkHelloExchange : public testing::TestWithParam<remote_hello_t>
{
};

TEST_P (CppFrameworkHelloExchange, RetainsFirstDescriptorUntilAdmit)
{
    mesh::raw_mesh_node_owner_t source (options ("hello-source", 101));
    source.start ();
    const auto initial = source.topology ().local_descriptor ();
    zlink::router_socket_t remote (source.context ());
    remote.set_routing_id (zlink::routing_id_t::from ("hello-target"));
    remote.options ().linger (0ms);
    remote.bind ("tcp://127.0.0.1:0");
    backend::raw_route_port_t port (remote);
    ASSERT_TRUE (source.connect_peer (remote.options ().last_endpoint ()));
    std::optional<backend::raw_received_t> received;
    const auto receive = [&] {
        received.reset ();
        return pump_until ({&source}, [&] {
            if (!received)
                received = port.try_receive ();
            return received.has_value ();
        });
    };
    ASSERT_TRUE (receive ());
    ASSERT_EQ (1u, received->parts.size ());
    const auto first = received->parts.front ().to_bytes ();
    EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::hello,
                                                      source.topology ().local_descriptor ()),
               first);
    auto descriptor = options ("hello-target", 201).descriptor;
    descriptor.advertised_endpoint = remote.options ().last_endpoint ();
    const auto receive_remote_hello = [&] {
        ASSERT_TRUE (port
                       .send (bytes ("hello-source"), {protocol::encode_route_mesh_admission (
                                                        protocol::command::hello, descriptor)})
                       .result ()
                       .value ());
        ASSERT_TRUE (receive ());
        EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::admit, initial),
                   received->parts.front ().to_bytes ());
        ASSERT_TRUE (source.topology ().peer (descriptor.node_routing_id));
        EXPECT_FALSE (source.topology ().peer (descriptor.node_routing_id)->ready);
    };
    if (GetParam () == remote_hello_t::before_update)
        ASSERT_NO_FATAL_FAILURE (receive_remote_hello ());
    const auto latest = source.publish_descriptor_update ([] (auto &descriptor) {
        ++descriptor.descriptor_revision;
        descriptor.state = mesh::service_node_state_t::serving;
    });
    ASSERT_EQ (initial.descriptor_revision + 1, latest.descriptor_revision);
    // The crossed HELLO must retain our first descriptor after publication too.
    if (GetParam () == remote_hello_t::after_update)
        ASSERT_NO_FATAL_FAILURE (receive_remote_hello ());
    // A successful send has completed; only the remote ADMIT remains withheld.
    const auto next_activity = source.liveness ().next_activity ();
    ASSERT_TRUE (next_activity);
    const auto tick = source.tick_liveness (*next_activity).result ().value ();
    ASSERT_TRUE (receive ());
    EXPECT_EQ (first, received->parts.front ().to_bytes ());
    if (GetParam () != remote_hello_t::none) {
        ASSERT_EQ (1u, tick.probes.size ());
        ASSERT_TRUE (receive ());
        EXPECT_EQ (protocol::encode_liveness (protocol::command::livenessProbe,
                                              tick.probes.front ().probe_id),
                   received->parts.front ().to_bytes ());
        ASSERT_TRUE (port
                       .send (bytes ("hello-source"),
                              {protocol::encode_liveness (protocol::command::livenessAck,
                                                          tick.probes.front ().probe_id)})
                       .result ()
                       .value ());
    }

    ASSERT_TRUE (port
                   .send (bytes ("hello-source"), {protocol::encode_route_mesh_admission (
                                                    protocol::command::admit, descriptor)})
                   .result ()
                   .value ());
    ASSERT_TRUE (receive ());
    EXPECT_EQ (protocol::encode_route_mesh_admission (protocol::command::update, latest),
               received->parts.front ().to_bytes ());
    source.close ();
}

INSTANTIATE_TEST_SUITE_P (WithAndWithoutRemoteHello,
                          CppFrameworkHelloExchange,
                          testing::Values (remote_hello_t::none,
                                           remote_hello_t::before_update,
                                           remote_hello_t::after_update));

TEST (CppFrameworkMeshSelectedRoute, AdmissionRevisionIntegrity)
{
    auto baseline = options ("guard-peer", 201).descriptor;
    baseline.descriptor_revision = 23;
    auto higher = baseline;
    ++higher.descriptor_revision;
    higher.state = mesh::service_node_state_t::serving;
    auto equal_mutated = higher;
    equal_mutated.descriptor_revision = baseline.descriptor_revision;
    auto lower = baseline;
    --lower.descriptor_revision;
    auto immutable = higher;
    immutable.advertised_endpoint = "tcp://127.0.0.1:7071";
    for (const auto &[command, incoming, expected] :
         std::vector<std::tuple<protocol::command, mesh::service_node_descriptor_t,
                                mesh::peer_admission_result_t>>{
           {protocol::command::hello, higher, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::admit, higher, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::update, higher, mesh::peer_admission_result_t::admitted},
           {protocol::command::hello, baseline,
            mesh::peer_admission_result_t::duplicate_connection},
           {protocol::command::hello, equal_mutated,
            mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::hello, lower, mesh::peer_admission_result_t::stale_descriptor},
           {protocol::command::hello, immutable,
            mesh::peer_admission_result_t::stale_descriptor}}) {
        SCOPED_TRACE (static_cast<int> (command));
        zlink::framework::runtime::state_lane_t topology_lane;
        mesh::service_topology_registry_t topology (topology_lane,
                                                    options ("guard-local", 101).descriptor);
        ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (baseline, {1}));
        EXPECT_EQ (expected,
                   topology.admit (incoming, {1},
                                   mesh::service_liveness_registry_t::clock_t::now (), command));
    }
}

TEST (CppFrameworkMeshSelectedRoute, UpdateRequiresAdmittedLifecycle)
{
    const auto baseline = options ("guard-peer", 201).descriptor;
    auto incoming = baseline;
    incoming.lifecycle_generation = 202;
    ++incoming.descriptor_revision;
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    for (const bool has_current : {false, true}) {
        for (const auto command :
             {protocol::command::hello, protocol::command::admit, protocol::command::update}) {
            SCOPED_TRACE (has_current);
            SCOPED_TRACE (static_cast<int> (command));
            zlink::framework::runtime::state_lane_t topology_lane;
            mesh::service_topology_registry_t topology (topology_lane,
                                                        options ("guard-local", 101).descriptor);
            if (has_current)
                ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (baseline, {1}));
            EXPECT_EQ (command == protocol::command::update
                         ? mesh::peer_admission_result_t::stale_descriptor
                         : mesh::peer_admission_result_t::admitted,
                       topology.admit (incoming, {2}, incoming, now, command));
            const auto peer = topology.peer (incoming.node_routing_id);
            if (command == protocol::command::update) {
                EXPECT_EQ (has_current, peer != nullptr);
                if (has_current && peer) {
                    EXPECT_EQ (baseline, peer->descriptor);
                    EXPECT_EQ (std::vector<std::uint8_t> ({1}), peer->connection_id);
                }
            } else {
                ASSERT_TRUE (peer);
                EXPECT_EQ (incoming, peer->descriptor);
            }
        }
    }
}

} // namespace
